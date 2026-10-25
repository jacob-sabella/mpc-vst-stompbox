/* The models written here: the noise gate; the cabinet, the preamp and the presence (guitarix's built-in impulse
 * responses and its own tone shapers for them, run by a convolver of our own); and the IR loader (the impulse response
 * files guitarix ships, as its convolver plays them: a partitioned FFT convolver here). */
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
struct PreDesc { int ir_count; int ir_sr; float ir_data[]; };
template <int tab_size> struct PreDesc_imp {
    int ir_count; int ir_sr; float ir_data[tab_size];
    operator PreDesc &() { return *(PreDesc *)this; }
};
#include "gx_preamp_data.cc"
#include "preamp_impulse_former.h"
#include "preamp_impulse_former.cc"
#include "presence_level.h"
#include "presence_level.cc"

static CabDesc *cab_by_name(const char *n) {
#define C(x) if (!strcmp(n, #x)) return &static_cast<CabDesc &>(x);
    C(cab_data_4x12) C(cab_data_2x12) C(cab_data_1x12) C(cab_data_4x10) C(cab_data_2x10) C(cab_data_HighGain)
    C(cab_data_Twin) C(cab_data_Bassman) C(cab_data_Marshall) C(cab_data_AC30) C(cab_data_Princeton) C(cab_data_A2)
    C(cab_data_1x15) C(cab_data_mesa) C(cab_data_briliant) C(cab_data_vitalize) C(cab_data_charisma)
#undef C
    return nullptr;
}

static PreDesc *pre_by_name(const char *n) {
#define C(x) if (!strcmp(n, #x)) return &static_cast<PreDesc &>(x);
    C(pre_data_ac30) C(pre_data_bassman) C(pre_data_tube) C(pre_data_fender) C(pre_data_jcm800) C(pre_data_jtm45)
    C(pre_data_mesaboogie) C(pre_data_boutique) C(pre_data_ampeg) C(pre_data_rectifier)
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

/* a short impulse response in direct form: hist holds the last N inputs, twice over, so a block reads one straight run */
struct Direct {
    std::vector<float> ir, hist;
    int pos = 0, N = 0;
    void set(std::vector<float> h) {
        ir = std::move(h);
        N = (int)ir.size();
        std::reverse(ir.begin(), ir.end());
        hist.assign(2 * (N + 128), 0.f);
        pos = N - 1;   /* N - 1 zeros of history before the first sample */
    }
    void clear() { std::fill(hist.begin(), hist.end(), 0.f); pos = N - 1; }
    void run(int n, const float *in, float *out) {   /* in and out may be the same */
        /* when the write position reaches the end, the last N - 1 samples move to the front */
        int cap = (int)hist.size();
        if (pos + n > cap) {
            memmove(&hist[0], &hist[pos - (N - 1)], (N - 1) * sizeof(float));
            pos = N - 1;
        }
        memcpy(&hist[pos], in, n * sizeof(float));
        const float *h = ir.data();
        for (int i = 0; i < n; i++)
            out[i] = dot(h, &hist[pos + i - (N - 1)], N);
        pos += n;
    }
};

/* the tone shapers' controls, as their register_par() hands them over ("<unit>.Level", ".bass", ".treble") */
static float *g_shaper_ptr[3];
static float *reg_f(const char *id, const char *, const char *, const char *, float *var, float val, float, float, float,
                    const value_pair *) {
    *var = val;
    const char *d = strrchr(id, '.');
    if (d && !strcmp(d, ".Level")) g_shaper_ptr[0] = var;
    else if (d && !strcmp(d, ".bass")) g_shaper_ptr[1] = var;
    else if (d && !strcmp(d, ".treble")) g_shaper_ptr[2] = var;
    return var;
}

/* the cabinet and the preamp: an impulse response, then guitarix's level / bass / treble shaper for it */
template <class Former> struct ShortIR : Builtin {
    Direct conv;
    Former impf;
    float *ctl[3] = {};
    ShortIR(const float *data, int count, int sr, int rate, float level_lo, float level_hi) {
        conv.set(resample_ir(data, count, sr, rate));
        impf.init(rate);
        ParamReg reg = { nullptr, reg_f, nullptr, nullptr };
        static std::mutex m; std::lock_guard<std::mutex> g(m);
        g_shaper_ptr[0] = g_shaper_ptr[1] = g_shaper_ptr[2] = nullptr;
        impf.register_par(reg);
        for (int i = 0; i < 3; i++) ctl[i] = g_shaper_ptr[i];
        params = { P("level", "Level", level_lo, level_hi, 1.f), P("bass", "Bass", -10, 10, 0), P("treble", "Treble", -10, 10, 0) };
        for (auto &p : params) p.step = 0.5f;
        changed();
    }
    void changed() override { for (int i = 0; i < 3; i++) if (ctl[i]) *ctl[i] = params[i].value; }
    void clear() override { conv.clear(); impf.clear_state_f(); }
    void run(int n, float *x) override {
        conv.run(n, x, x);
        impf.compute(n, x, x);
    }
};

/* guitarix's presence ("contrast convolver"): its impulse response scaled by the level, crossfaded with the dry
 * signal over the level's range (gx_internal_plugins.cpp, run_contrast) */
struct Presence : Builtin {
    Direct conv;
    presence_level::Dsp presl;
    float level = 1, *lv = nullptr;
    int rate;
    Presence(int r) : rate(r) {
        presl.init(contrast_ir_desc.ir_sr);
        ParamReg reg = { nullptr, reg_f, nullptr, nullptr };
        static std::mutex m; std::lock_guard<std::mutex> g(m);
        g_shaper_ptr[0] = nullptr;
        presl.register_par(reg);
        lv = g_shaper_ptr[0];
        params = { P("level", "Level", 0.5f, 5.f, 1.f) };
        params[0].step = 0.1f;
        changed();
    }
    void changed() override {
        level = params[0].value;
        if (lv) *lv = level;
        float h[192];
        presl.compute(contrast_ir_desc.ir_count, contrast_ir_desc.ir_data, h);
        std::vector<float> old = conv.hist;
        int pos = conv.pos;
        conv.set(resample_ir(h, contrast_ir_desc.ir_count, contrast_ir_desc.ir_sr, rate));
        if (old.size() == conv.hist.size()) { conv.hist = old; conv.pos = pos; }   /* same length: keep the history */
    }
    void clear() override { conv.clear(); }
    void run(int n, float *x) override {
        float t = fminf(level * 0.2f, 1.f), mix = t * t * (3.f - 2.f * t);
        float a = cosf(1.57079632679f * mix), b = sinf(1.57079632679f * mix) / 2;
        float wet[128];
        conv.run(n, x, wet);
        for (int i = 0; i < n; i++) x[i] = a * x[i] + b * wet[i];
    }
};

/* ---- the IR loader: a WAV file's first channel, as guitarix's convolver plays it (gain when gain correction is on,
 * offset, length and delay in the file's samples), mixed with the dry signal by wet/dry and gain (jconv_post_mono).
 * The convolver: the first B taps in direct form, the rest in partitions of B taps, uniformly partitioned overlap-save
 * on 2B-point FFTs computed once per B input samples whatever the block size, so it adds no latency. */
static const int PB = 128, PN = 256, PK = 132;   /* partition, FFT size, bins (129, padded for NEON) */

struct FFT {
    float cs[PN / 2], sn[PN / 2];
    int rev[PN];
    FFT() {
        for (int i = 0; i < PN / 2; i++) { cs[i] = (float)cos(2 * M_PI * i / PN); sn[i] = (float)-sin(2 * M_PI * i / PN); }
        int bits = 0;
        while ((1 << bits) < PN) bits++;
        for (int i = 0; i < PN; i++) {
            int r = 0;
            for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[i] = r;
        }
    }
    void run(float *re, float *im, bool inverse) const {   /* in place, unscaled */
        for (int i = 0; i < PN; i++) if (rev[i] > i) { std::swap(re[i], re[rev[i]]); std::swap(im[i], im[rev[i]]); }
        for (int len = 2; len <= PN; len <<= 1) {
            int half = len / 2, step = PN / len;
            for (int i = 0; i < PN; i += len)
                for (int j = 0; j < half; j++) {
                    float wr = cs[j * step], wi = inverse ? -sn[j * step] : sn[j * step];
                    int a = i + j, b = a + half;
                    float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
        }
    }
};
static const FFT &fft() { static const FFT f; return f; }

/* acc += x * h, complex, over PK bins */
static inline void cmac(float *ar, float *ai, const float *xr, const float *xi, const float *hr, const float *hi) {
    int k = 0;
#ifdef __ARM_NEON
    for (; k <= PK - 4; k += 4) {
        float32x4_t a = vld1q_f32(xr + k), b = vld1q_f32(xi + k), c = vld1q_f32(hr + k), d = vld1q_f32(hi + k);
        float32x4_t r = vld1q_f32(ar + k), i = vld1q_f32(ai + k);
        r = vmlaq_f32(r, a, c); r = vmlsq_f32(r, b, d);
        i = vmlaq_f32(i, a, d); i = vmlaq_f32(i, b, c);
        vst1q_f32(ar + k, r); vst1q_f32(ai + k, i);
    }
#endif
    for (; k < PK; k++) {
        ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
        ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
    }
}

struct PartConv {
    Direct head;                       /* taps 0 .. PB-1 */
    int P = 0;                         /* partitions after the head */
    std::vector<float> Hr, Hi;         /* their spectra, P x PK */
    std::vector<float> Xr, Xi;         /* the last P input spectra, a ring */
    int xpos = 0, fill = 0;
    float in[PN] = {}, tail[PB] = {};  /* the last two input blocks; the tail's output for the block filling now */
    void set(const std::vector<float> &h) {
        std::vector<float> hd(h.begin(), h.begin() + (std::min)((size_t)PB, h.size()));
        if (hd.empty()) hd.push_back(0.f);
        head.set(hd);
        P = h.size() > (size_t)PB ? (int)((h.size() - PB + PB - 1) / PB) : 0;
        Hr.assign((size_t)P * PK, 0.f); Hi.assign((size_t)P * PK, 0.f);
        Xr.assign((size_t)P * PK, 0.f); Xi.assign((size_t)P * PK, 0.f);
        float re[PN], im[PN];
        for (int q = 0; q < P; q++) {
            std::fill(re, re + PN, 0.f); std::fill(im, im + PN, 0.f);
            for (int k = 0; k < PB; k++) {
                size_t t = (size_t)(q + 1) * PB + k;
                if (t < h.size()) re[k] = h[t];
            }
            fft().run(re, im, false);
            for (int k = 0; k <= PN / 2; k++) { Hr[(size_t)q * PK + k] = re[k] / PN; Hi[(size_t)q * PK + k] = im[k] / PN; }
        }
        clear();
    }
    void clear() {
        head.clear();
        std::fill(Xr.begin(), Xr.end(), 0.f); std::fill(Xi.begin(), Xi.end(), 0.f);
        std::fill(in, in + PN, 0.f); std::fill(tail, tail + PB, 0.f);
        xpos = fill = 0;
    }
    void block() {   /* a block of PB inputs is in: its spectrum, and the tail's output for the next block */
        float re[PN], im[PN];
        memcpy(re, in, sizeof re);
        std::fill(im, im + PN, 0.f);
        fft().run(re, im, false);
        float *xr = &Xr[(size_t)xpos * PK], *xi = &Xi[(size_t)xpos * PK];
        for (int k = 0; k <= PN / 2; k++) { xr[k] = re[k]; xi[k] = im[k]; }
        float ar[PK] = {}, ai[PK] = {};
        for (int q = 0; q < P; q++) {
            int j = (xpos - q + P) % P;
            cmac(ar, ai, &Xr[(size_t)j * PK], &Xi[(size_t)j * PK], &Hr[(size_t)q * PK], &Hi[(size_t)q * PK]);
        }
        for (int k = 0; k <= PN / 2; k++) { re[k] = ar[k]; im[k] = ai[k]; }
        for (int k = 1; k < PN / 2; k++) { re[PN - k] = ar[k]; im[PN - k] = -ai[k]; }   /* a real signal's spectrum */
        fft().run(re, im, true);
        memcpy(tail, re + PB, sizeof tail);
        memmove(in, in + PB, PB * sizeof(float));
        xpos = (xpos + 1) % P;
    }
    void run(int n, const float *x, float *y) {
        head.run(n, x, y);
        if (!P) return;
        for (int i = 0; i < n; i++) {
            y[i] += tail[fill];
            in[PB + fill] = x[i];
            if (++fill == PB) { fill = 0; block(); }
        }
    }
};

/* a WAV file's first channel (16/24/32-bit PCM or 32-bit float) -> samples; false if it isn't one */
static bool wav_read(const unsigned char *d, size_t n, std::vector<float> &out, int &sr) {
    auto u16 = [&](size_t o) { return (unsigned)d[o] | (unsigned)d[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
    unsigned fmt = 0, ch = 0, bits = 0;
    size_t o = 12;
    sr = 0;
    while (o + 8 <= n) {
        size_t len = u32(o + 4), b = o + 8;
        if (b + len > n) len = n - b;
        if (!memcmp(d + o, "fmt ", 4) && len >= 16) {
            fmt = u16(b); ch = u16(b + 2); sr = (int)u32(b + 4); bits = u16(b + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = u16(b + 24);   /* extensible: the subformat's first two bytes */
        } else if (!memcmp(d + o, "data", 4) && ch && bits && sr) {
            unsigned bps = bits / 8, frame = bps * ch;
            size_t frames = len / frame;
            out.resize(frames);
            for (size_t i = 0; i < frames; i++) {
                const unsigned char *s = d + b + i * frame;
                float v = 0;
                if (fmt == 3 && bits == 32) { uint32_t w = u32(s - d); memcpy(&v, &w, 4); }
                else if (fmt == 1 && bits == 16) v = (int16_t)u16(s - d) / 32768.f;
                else if (fmt == 1 && bits == 24) v = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) / 2147483648.f;
                else if (fmt == 1 && bits == 32) v = (int32_t)u32(s - d) / 2147483648.f;
                else return false;
                out[i] = v;
            }
            return frames > 0;
        }
        o = b + len + (len & 1);
    }
    return false;
}

struct IRLoader : Builtin {
    PartConv conv;
    int rate, loaded[5] = {-1, -1, -1, -1, -1};
    float g = 0;   /* the wet gain, smoothed as jconv_post_mono does */
    IRLoader(int r) : rate(r) {
        mod_param f = P("file", "IR", 0, (float)(std::max)(0, NIR_FILES - 1), 0);
        f.type = mod_param::ENUM;
        f.step = 1;
        for (int i = 0; i < NIR_FILES; i++) { f.labels.push_back(IR_FILES[i].label); f.keys.push_back(IR_FILES[i].id); }
        params = { f, P("wet_dry", "Dry/Wet", 0, 100, 100), P("gain", "Gain", -20, 20, 0),
                   P("ir_gain", "IR Gain", 0, 10, 1), P("offset", "Offset", 0, 1e6f, 0), P("length", "Length", 0, 1e7f, 0),
                   P("delay", "Delay", 0, 1e6f, 0) };
        params[1].step = 1; params[2].step = 0.1f;
        shown = 3;
        changed();
    }
    void changed() override {
        int want[5] = { (int)params[0].value, (int)lrintf(params[3].value * 1e4f), (int)params[4].value,
                        (int)params[5].value, (int)params[6].value };
        if (!memcmp(want, loaded, sizeof want)) return;
        memcpy(loaded, want, sizeof want);
        std::vector<float> h;
        int sr = rate, k = want[0];
        if (k >= 0 && k < NIR_FILES && wav_read(IR_FILES[k].data, IR_FILES[k].size, h, sr)) {
            size_t off = (std::min)(h.size(), (size_t)want[2]);
            h.erase(h.begin(), h.begin() + off);
            if (want[3] > 0 && (size_t)want[3] < h.size()) h.resize(want[3]);
            h.insert(h.begin(), (size_t)want[4], 0.f);
            for (auto &v : h) v *= params[3].value;
            h = resample_ir(h.data(), (int)h.size(), sr, rate);
        }
        if (h.empty()) h.push_back(0.f);
        conv.set(h);
    }
    void clear() override { conv.clear(); }
    void run(int n, float *x) override {
        float wet[128];
        conv.run(n, x, wet);
        float wd = params[1].value * 0.01f, tg = 0.001f * powf(10.f, 0.05f * params[2].value);
        for (int i = 0; i < n; i++) {
            g = tg + 0.999f * g;
            x[i] = (1.f - wd) * x[i] + wd * wet[i] * g;
        }
    }
};

/* for tests/rack_test.cpp: the partitioned convolver against plain direct-form convolution, on a random impulse
 * response and random block sizes; -> the largest difference */
float stompbox_conv_selftest() {
    std::vector<float> h(1000), x(20000);
    unsigned s = 1;
    auto rnd = [&] { s = s * 1103515245u + 12345u; return ((s >> 8) & 0xFFFF) / 32768.f - 1.f; };
    for (auto &v : h) v = rnd() * 0.05f;
    for (auto &v : x) v = rnd();
    PartConv c;
    c.set(h);
    float worst = 0;
    size_t i = 0;
    while (i < x.size()) {
        int n = (std::min)((int)(x.size() - i), 1 + (int)((rnd() + 1) * 64));
        float y[128];
        c.run(n, &x[i], y);
        for (int k = 0; k < n; k++) {
            double ref = 0;
            for (size_t j = 0; j < h.size() && j <= i + k; j++) ref += h[j] * x[i + k - j];
            worst = (std::max)(worst, (float)fabs(ref - y[k]));
        }
        i += n;
    }
    return worst;
}

Builtin *make_builtin(const model_t *m, int rate) {
    if (m->builtin == BI_GATE) return new Gate(rate);
    if (m->builtin == BI_CAB) {
        const CabDesc *d = m->arg ? cab_by_name(m->arg) : nullptr;
        return d ? new ShortIR<cabinet_impulse_former::Dsp>(d->ir_data, d->ir_count, d->ir_sr, rate, 0.5f, 5.f) : nullptr;
    }
    if (m->builtin == BI_PRE) {
        const PreDesc *d = m->arg ? pre_by_name(m->arg) : nullptr;
        return d ? new ShortIR<preamp_impulse_former::Dsp>(d->ir_data, d->ir_count, d->ir_sr, rate, 0.1f, 2.1f) : nullptr;
    }
    if (m->builtin == BI_PRESENCE) return new Presence(rate);
    if (m->builtin == BI_IR) return new IRLoader(rate);
    return nullptr;
}
