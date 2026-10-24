/* The two models written here: the noise gate, and the cabinet (guitarix's impulse responses and its bass/treble
 * shaper, run by a convolver of our own). */
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#include "host.h"
#define _(s) (s)
#include "gx_faust_support.h"
#include "gx_plugin.h"

static mod_param P(const char *id, const char *name, float lo, float hi, float def) {
    mod_param p;
    p.id = id; p.name = name; p.lo = lo; p.hi = hi; p.def = p.value = def;
    return p;
}

/* ---- noise gate: peak envelope (1 ms attack), opens above THRESHOLD, closes 6 dB under it after HOLD, the gain
 * opens in 1 ms and closes over RELEASE, down to RANGE. */
struct Gate : Builtin {
    float rate, env = 0, g = 0, hold = 0;
    int open = 0;
    Gate(int r) : rate((float)r) {
        params = { P("threshold", "Threshold", -80, -10, -55), P("release", "Release", 5, 500, 80),
                   P("hold", "Hold", 0, 250, 20), P("range", "Range", -90, 0, -90) };
        params[0].step = params[3].step = 1; params[1].step = params[2].step = 1;
    }
    void clear() override { env = g = hold = 0; open = 0; }
    void run(int n, float *x) override {
        float open_th = powf(10.f, params[0].value / 20.f), close_th = open_th * 0.5f;
        float atk = 1.f - expf(-1.f / (0.001f * rate)), env_rel = 1.f - expf(-1.f / (0.050f * rate));
        float rel = 1.f - expf(-1.f / (params[1].value * 0.001f * rate));
        float floor_g = params[3].value <= -89.f ? 0.f : powf(10.f, params[3].value / 20.f);
        for (int i = 0; i < n; i++) {
            float a = fabsf(x[i]);
            env += (a > env ? atk : env_rel) * (a - env);
            if (env > open_th) { open = 1; hold = params[2].value * 0.001f * rate; }
            else if (env < close_th) { if (hold > 0) hold--; else open = 0; }
            float t = open ? 1.f : 0.f;
            g += (t > g ? atk : rel) * (t - g);
            x[i] *= floor_g + (1.f - floor_g) * g;
        }
    }
};

/* ---- cabinet. guitarix's data file declares its impulse responses with these two types (gx_internal_plugins.h) */
struct CabDesc { int ir_count; int ir_sr; float ir_data[]; };
template <int tab_size> struct CabDesc_imp {
    int ir_count; int ir_sr; float ir_data[tab_size];
    operator CabDesc &() { return *(CabDesc *)this; }
};
#include "gx_cabinet_data.cc"
#include "cabinet_impulse_former.h"
#include "cabinet_impulse_former.cc"

static CabDesc *cab_by_name(const char *n) {
#define C(x) if (!strcmp(n, #x)) return &static_cast<CabDesc &>(x);
    C(cab_data_4x12) C(cab_data_2x12) C(cab_data_1x12) C(cab_data_4x10) C(cab_data_2x10) C(cab_data_HighGain)
    C(cab_data_Twin) C(cab_data_Bassman) C(cab_data_Marshall) C(cab_data_AC30) C(cab_data_Princeton) C(cab_data_A2)
    C(cab_data_1x15) C(cab_data_mesa) C(cab_data_briliant) C(cab_data_vitalize) C(cab_data_charisma)
#undef C
    return nullptr;
}

/* windowed-sinc resampling of a short impulse response (the data is 48 kHz, MPC runs at 44.1) */
static std::vector<float> resample_ir(const float *x, int n, double from, double to) {
    if (from == to) return std::vector<float>(x, x + n);
    double ratio = to / from, cutoff = std::fmin(1.0, ratio);
    int m = (int)std::ceil(n * ratio);
    std::vector<float> y(m);
    const int A = 16;
    for (int k = 0; k < m; k++) {
        double t = k / ratio, s = 0;
        int c = (int)std::floor(t);
        for (int j = c - A + 1; j <= c + A; j++) {
            if (j < 0 || j >= n) continue;
            double u = (t - j) * cutoff;
            double sinc = u == 0 ? 1 : std::sin(M_PI * u) / (M_PI * u);
            double w = 0.5 * (1 + std::cos(M_PI * (t - j) / A));
            s += x[j] * sinc * w * cutoff;
        }
        y[k] = (float)s;
    }
    return y;
}

/* the convolver's inner loop: by hand in NEON on the device (gcc only vectorises float sums with -ffast-math,
 * which the rest of the upstream code shouldn't get) */
static inline float dot(const float *a, const float *b, int n) {
    int k = 0;
    float s = 0;
#ifdef __ARM_NEON
    float32x4_t s0 = vdupq_n_f32(0), s1 = s0, s2 = s0, s3 = s0;
    for (; k + 16 <= n; k += 16) {
        s0 = vmlaq_f32(s0, vld1q_f32(a + k), vld1q_f32(b + k));
        s1 = vmlaq_f32(s1, vld1q_f32(a + k + 4), vld1q_f32(b + k + 4));
        s2 = vmlaq_f32(s2, vld1q_f32(a + k + 8), vld1q_f32(b + k + 8));
        s3 = vmlaq_f32(s3, vld1q_f32(a + k + 12), vld1q_f32(b + k + 12));
    }
    float32x4_t t = vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3));
    float32x2_t u = vadd_f32(vget_low_f32(t), vget_high_f32(t));
    s = vget_lane_f32(vpadd_f32(u, u), 0);
#endif
    for (; k < n; k++) s += a[k] * b[k];
    return s;
}

static float *g_cab_ptr[3];   /* the shaper's controls, as its register_par() hands them over */
static float *reg_f(const char *id, const char *, const char *, const char *, float *var, float val, float, float, float,
                    const value_pair *) {
    *var = val;
    if (!strcmp(id, "cab.Level")) g_cab_ptr[0] = var;
    else if (!strcmp(id, "cab.bass")) g_cab_ptr[1] = var;
    else if (!strcmp(id, "cab.treble")) g_cab_ptr[2] = var;
    return var;
}

struct Cab : Builtin {
    std::vector<float> ir, hist;   /* hist: the last N inputs, twice over, so a block reads one straight run */
    int pos = 0, N = 0;
    cabinet_impulse_former::Dsp impf;
    float *ctl[3] = {};
    Cab(const CabDesc *d, int rate) {
        ir = resample_ir(d->ir_data, d->ir_count, d->ir_sr, rate);
        N = (int)ir.size();
        std::reverse(ir.begin(), ir.end());
        hist.assign(2 * (N + 128), 0.f);
        impf.init(rate);
        ParamReg reg = { nullptr, reg_f, nullptr, nullptr };
        static std::mutex m; std::lock_guard<std::mutex> g(m);
        impf.register_par(reg);
        for (int i = 0; i < 3; i++) ctl[i] = g_cab_ptr[i];
        params = { P("level", "Level", 0.5f, 5.f, 1.f), P("bass", "Bass", -10, 10, 0), P("treble", "Treble", -10, 10, 0) };
        for (auto &p : params) p.step = 0.5f;
        changed();
    }
    void changed() override { for (int i = 0; i < 3; i++) if (ctl[i]) *ctl[i] = params[i].value; }
    void clear() override { std::fill(hist.begin(), hist.end(), 0.f); impf.clear_state_f(); }
    void run(int n, float *x) override {
        /* hist holds a sliding window of the input of length N + 128; when the write position reaches the end, the
         * last N - 1 samples move to the front */
        int cap = (int)hist.size();
        if (pos + n > cap) {
            memmove(&hist[0], &hist[pos - (N - 1)], (N - 1) * sizeof(float));
            pos = N - 1;
        }
        memcpy(&hist[pos], x, n * sizeof(float));
        const float *h = ir.data();
        for (int i = 0; i < n; i++)
            x[i] = dot(h, &hist[pos + i - (N - 1)], N);
        pos += n;
        impf.compute(n, x, x);
    }
};

Builtin *make_builtin(const model_t *m, int rate) {
    if (m->builtin == BI_GATE) return new Gate(rate);
    if (m->builtin == BI_CAB) {
        const CabDesc *d = m->arg ? cab_by_name(m->arg) : nullptr;
        if (!d) return nullptr;
        Cab *c = new Cab(d, rate);
        c->pos = c->N - 1;   /* N - 1 zeros of history before the first sample */
        return c;
    }
    return nullptr;
}
