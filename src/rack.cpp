/* Stompbox: a guitar rack as one MPC insert effect, hosting guitarix and GxPlugins modules (models.json).
 *
 *   input gain -> [mono: gate, comp, wah, pedal 1-3, amp, tone stack, power amp, cabinet]
 *              -> [stereo: mod, delay, reverb, EQ] -> output gain
 *
 * Each slot has a model picker, an on switch and generic knobs (<slot>_k1..kN, 0..1) that take the names, ranges and
 * value text of the chosen model's controls (get_param "<key>_name" / "<key>_display"). A model is loaded the first
 * time it is picked and kept, with its own settings, so switching back finds it as it was. The two input channels are
 * summed into the mono part; a mono model in the stereo part runs once per side. Switching a slot on or off, or its
 * model, crossfades over 5 ms. State ("state"): every loaded model's settings by upstream control id.
 *
 * Loading a model (allocation, tables, an impulse response) takes far longer than an audio block, and every engine
 * call runs under the wrapper's lock, so a model picked on the screen or a Q-Link is built by a loader thread: the slot
 * plays its old model until the new one is ready, then crossfades, and "display_rev" moves so the wrapper re-reads the
 * knob names (HAS_DISPLAY_REV). Creating the rack and restoring state load in the caller's thread. Clearing a model
 * (its delay lines, before it fades in again) is the loader's job too: a slot that has faded out asks for it, and a slot
 * fades in only from a clean model. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
extern "C" {
#include "engine.h"
}
#include "host.h"

static const int RATE = 44100, MAXN = 128;
static const float FADE = 1.0f / (0.005f * RATE);

struct Loaded {
    Module *a = nullptr, *b = nullptr;   /* b: the right side, for a mono model in the stereo part */
    std::atomic<bool> clean{true};       /* cleared since it last ran: may fade in */
    std::atomic<bool> clearing{false};   /* a clear is queued */
};

struct Slot {
    int section;
    std::vector<int> models;     /* indices into MODELS */
    int cur = 0, want = 0;       /* index into models: playing, picked */
    int on = 0;
    float mix = 0;               /* 0 = out .. 1 = in */
    int pending_clear = -1;      /* audio thread: an entry to hand to the loader for clearing (the queue was busy) */
    std::unique_ptr<std::atomic<Loaded *>[]> loaded;   /* per entry of models: null until built (by either thread) */
};

struct Rack {
    std::vector<Slot> slots;
    float input_db = 0, output_db = 0, in_g = 1, out_g = 1;
    /* the loader thread: (slot, model entry) requests to build; (slot, -1 - entry) to clear */
    std::thread loader;
    std::mutex lk;
    std::condition_variable cv;
    std::vector<std::pair<int, int>> queue;
    bool stop = false;
    std::atomic<int> rev{0};
};

static float db2lin(float db) { return powf(10.f, db / 20.f); }

#if defined(__aarch64__)
static unsigned long fz_on() { unsigned long o, v; __asm__ volatile("mrs %0, fpcr" : "=r"(o)); v = o | (1UL << 24); __asm__ volatile("msr fpcr, %0" ::"r"(v)); return o; }
static void fz_off(unsigned long o) { __asm__ volatile("msr fpcr, %0" ::"r"(o)); }
#elif defined(__arm__)
static unsigned long fz_on() { unsigned long o, v; __asm__ volatile("vmrs %0, fpscr" : "=r"(o)); v = o | (1UL << 24); __asm__ volatile("vmsr fpscr, %0" ::"r"(v)); return o; }
static void fz_off(unsigned long o) { __asm__ volatile("vmsr fpscr, %0" ::"r"(o)); }
#elif defined(__SSE__)   /* the x86 host tests: flush to zero and denormals are zero, as on the device */
#include <xmmintrin.h>
static unsigned long fz_on() { unsigned long o = _mm_getcsr(); _mm_setcsr(o | 0x8040); return o; }
static void fz_off(unsigned long o) { _mm_setcsr((unsigned)o); }
#else
static unsigned long fz_on() { return 0; }
static void fz_off(unsigned long) {}
#endif

/* ---- knob <-> real value */
static float to_real(const mod_param &p, float n) {
    n = fminf(fmaxf(n, 0.f), 1.f);
    if (p.type == mod_param::CONT && p.log && p.lo > 0 && p.hi > p.lo) return p.lo * powf(p.hi / p.lo, n);
    float v = p.lo + n * (p.hi - p.lo);
    return p.type == mod_param::CONT ? v : roundf(v);
}
static float to_norm(const mod_param &p, float v) {
    if (p.hi == p.lo) return 0;
    if (p.type == mod_param::CONT && p.log && p.lo > 0 && p.hi > p.lo && v > 0) return logf(v / p.lo) / logf(p.hi / p.lo);
    return fminf(fmaxf((v - p.lo) / (p.hi - p.lo), 0.f), 1.f);
}
static void value_text(const mod_param &p, char *buf, int len) {
    if (p.type == mod_param::BOOL) { snprintf(buf, len, "%s", p.value > 0.5f ? "On" : "Off"); return; }
    if (p.type == mod_param::ENUM) {
        int i = (int)roundf(p.value - p.lo);
        if (i >= 0 && i < (int)p.labels.size()) { snprintf(buf, len, "%s", p.labels[i].c_str()); return; }
    }
    float a = fabsf(p.hi - p.lo);
    int d = p.step >= 1 || a >= 100 ? 0 : p.step >= 0.1f || a >= 10 ? 1 : 2;
    snprintf(buf, len, "%.*f", d, p.value);
}

static Loaded *ready(Slot &s, int k) { return s.loaded[k].load(std::memory_order_acquire); }

/* build entry k of a slot (any thread); the first to publish wins */
static Loaded *build(Slot &s, int k) {
    if (Loaded *l = ready(s, k)) return l;
    int mi = s.models[k];
    Loaded *l = new Loaded();
    l->a = Module::create(&MODELS[mi], RATE);
    if (l->a && SECTIONS[s.section].stereo && !l->a->stereo()) l->b = Module::create(&MODELS[mi], RATE);
    /* warm it up here, not in the audio thread: a new model's delay lines and tables are fresh memory that page-faults
     * on its first touch, which cost several blocks' worth of time on its first run */
    for (Module *m : {l->a, l->b}) {
        if (!m) continue;
        float wl[MAXN], wr[MAXN];
        for (int k = 0; k < 8; k++) {
            memset(wl, 0, sizeof wl); memset(wr, 0, sizeof wr);
            m->run(MAXN, wl, wr);
        }
        m->clear();
    }
    Loaded *none = nullptr;
    if (!s.loaded[k].compare_exchange_strong(none, l, std::memory_order_acq_rel)) {
        delete l->a; delete l->b; delete l;
        return none;
    }
    return l;
}

static void request(Rack *r, int si, int k) {   /* engine thread: ask the loader for entry k, never wait */
    if (ready(r->slots[si], k)) return;
    { std::lock_guard<std::mutex> g(r->lk); r->queue.push_back({si, k}); }
    r->cv.notify_one();
}

/* audio thread: hand an idle entry to the loader for clearing, without ever waiting for the lock */
static bool post_clear(Rack *r, int si, int k) {
    std::unique_lock<std::mutex> g(r->lk, std::try_to_lock);
    if (!g.owns_lock()) return false;
    r->queue.push_back({si, -1 - k});
    g.unlock();
    r->cv.notify_one();
    return true;
}

static void loader_main(Rack *r) {
    std::unique_lock<std::mutex> g(r->lk);
    for (;;) {
        r->cv.wait(g, [r] { return r->stop || !r->queue.empty(); });
        if (r->stop) return;
        auto job = r->queue.front();
        r->queue.erase(r->queue.begin());
        g.unlock();
        Slot &s = r->slots[job.first];
        if (job.second >= 0) {
            build(s, job.second);
            r->rev.fetch_add(1, std::memory_order_release);
        } else if (Loaded *l = ready(s, -1 - job.second)) {   /* idle: the audio thread won't run it until clean */
            if (l->a) l->a->clear();
            if (l->b) l->b->clear();
            l->clearing.store(false, std::memory_order_relaxed);
            l->clean.store(true, std::memory_order_release);
        }
        g.lock();
    }
}

/* the module the knobs address: the picked model once built, else the one still playing */
static Loaded *shown(Slot &s) { Loaded *l = ready(s, s.want); return l ? l : ready(s, s.cur); }
/* a slot put straight into play (create, state): its model is running, so not clean */
static void playing(Slot &s) {
    Loaded *l = ready(s, s.cur);
    if (l && l->clearing.load(std::memory_order_acquire)) { s.mix = 0; return; }   /* the loader has it: start when clean */
    s.mix = s.on ? 1.f : 0.f;
    if (l && s.on) l->clean.store(false, std::memory_order_relaxed);
}
static Module *current(Slot &s) { Loaded *l = shown(s); return l ? l->a : nullptr; }
static void set_both(Loaded *l, int k, float v) { if (l->a) l->a->set(k, v); if (l->b) l->b->set(k, v); }

static void set_state(Rack *r, const char *txt);

static void *rk_create(const char *) {
    Rack *r = new Rack();
    r->slots.resize(NSLOTS);
    for (int i = 0; i < NSLOTS; i++) {
        Slot &s = r->slots[i];
        s.section = SLOT_SECTION[i];
        for (int m = 0; m < NMODELS; m++) if (MODELS[m].section == s.section) s.models.push_back(m);
        s.loaded.reset(new std::atomic<Loaded *>[s.models.size()]);
        for (size_t k = 0; k < s.models.size(); k++) s.loaded[k].store(nullptr);
        s.cur = s.want = SLOT_DEFAULT_MODEL[i];
        s.on = SLOT_DEFAULT_ON[i];
        build(s, s.want);
        playing(s);
    }
#ifdef STOMPBOX_BENCH
    /* bench builds only (tools/bench_rig.sh): start from a saved rig, since the bench can't set parameters */
    if (FILE *f = fopen("/tmp/stompbox_bench.state", "r")) {
        static char txt[16384];
        size_t n = fread(txt, 1, sizeof txt - 1, f);
        fclose(f);
        txt[n] = 0;
        set_state(r, txt);
        for (auto &s : r->slots) playing(s);
    }
#endif
    r->loader = std::thread(loader_main, r);
    return r;
}

static void rk_destroy(void *p) {
    Rack *r = (Rack *)p;
    { std::lock_guard<std::mutex> g(r->lk); r->stop = true; }
    r->cv.notify_one();
    r->loader.join();
    for (auto &s : r->slots)
        for (size_t k = 0; k < s.models.size(); k++)
            if (Loaded *l = s.loaded[k].load()) { delete l->a; delete l->b; delete l; }
    delete r;
}

static void rk_midi(void *, const uint8_t *, int) {}

/* "<slot>_<rest>" -> slot index, rest */
static int slot_of(const char *key, const char **rest) {
    for (int i = 0; i < NSLOTS; i++) {
        size_t n = strlen(SLOT_KEYS[i]);
        if (!strncmp(key, SLOT_KEYS[i], n) && key[n] == '_') { *rest = key + n + 1; return i; }
    }
    return -1;
}

static int model_by_id(const Slot &s, const char *id) {
    for (int k = 0; k < (int)s.models.size(); k++) if (!strcmp(MODELS[s.models[k]].id, id)) return k;
    return -1;
}

static void set_state(Rack *r, const char *txt);

static void rk_set(void *p, const char *key, const char *val) {
    Rack *r = (Rack *)p;
    if (!val) return;
    if (!strcmp(key, "state")) { set_state(r, val); return; }
    if (!strcmp(key, "input")) { r->input_db = (float)atof(val); return; }
    if (!strcmp(key, "output")) { r->output_db = (float)atof(val); return; }
    const char *rest;
    int si = slot_of(key, &rest);
    if (si < 0) return;
    Slot &s = r->slots[si];
    if (!strcmp(rest, "model")) {
        int k = atoi(val);
        if (k >= 0 && k < (int)s.models.size()) { s.want = k; request(r, si, k); }
    } else if (!strcmp(rest, "on")) {
        s.on = atoi(val) ? 1 : 0;
        if (s.on) request(r, si, s.want);
    } else if (rest[0] == 'k') {
        int k = atoi(rest + 1) - 1;
        Loaded *l = shown(s);
        Module *m = l ? l->a : nullptr;
        if (m && k >= 0 && k < m->nparams()) set_both(l, k, to_real(m->param(k), (float)atof(val)));
    }
}

static int get_state(Rack *r, char *buf, int len);

static int rk_get(void *p, const char *key, char *buf, int len) {
    Rack *r = (Rack *)p;
    if (len <= 0) return 0;
    if (!strcmp(key, "state")) return get_state(r, buf, len);
    if (!strcmp(key, "input")) return snprintf(buf, len, "%g", r->input_db);
    if (!strcmp(key, "output")) return snprintf(buf, len, "%g", r->output_db);
    if (!strcmp(key, "display_rev")) return snprintf(buf, len, "%d", r->rev.load(std::memory_order_acquire));
    const char *rest;
    int si = slot_of(key, &rest);
    if (si < 0) return 0;
    Slot &s = r->slots[si];
    if (!strcmp(rest, "model")) return snprintf(buf, len, "%d", s.want);
    if (!strcmp(rest, "on")) return snprintf(buf, len, "%d", s.on);
    if (rest[0] == 'k') {
        char *e;
        int k = (int)strtol(rest + 1, &e, 10) - 1;
        Module *m = current(s);
        bool has = m && k >= 0 && k < m->nparams();
        // a knob the model doesn't use reads blank: a space, since an empty answer makes the wrapper fall back to
        // the parameter's own name ("Gate 5") and a number
        if (!strcmp(e, "_name")) return snprintf(buf, len, "%s", has ? m->param(k).name.c_str() : " ");
        if (!strcmp(e, "_display")) {
            if (!has) return snprintf(buf, len, "%s", " ");
            value_text(m->param(k), buf, len);
            return (int)strlen(buf) + 1;
        }
        if (!*e) return snprintf(buf, len, "%g", has ? to_norm(m->param(k), m->param(k).value) : 0.f);
    }
    return 0;
}

/* ---- state: key=value lines. "<slot>_model=<model id>", "<slot>_on=0|1", "<slot>.<model id>.<control id>=<real value>" */
static int get_state(Rack *r, char *buf, int len) {
    int o = 0;
    auto put = [&](const char *fmt, auto... a) {
        if (o < 0) return;
        int w = snprintf(buf + o, len - o, fmt, a...);
        o = (w < 0 || w >= len - o) ? -1 : o + w;
    };
    put("v=1\ninput=%g\noutput=%g\n", r->input_db, r->output_db);
    for (int i = 0; i < NSLOTS; i++) {
        Slot &s = r->slots[i];
        put("%s_model=%s\n%s_on=%d\n", SLOT_KEYS[i], MODELS[s.models[s.want]].id, SLOT_KEYS[i], s.on);
        for (size_t e = 0; e < s.models.size(); e++) {
            Loaded *l = ready(s, (int)e);
            Module *m = l ? l->a : nullptr;
            if (!m) continue;
            for (int k = 0; k < m->nparams(); k++) {
                const mod_param &p = m->param(k);   /* a knob swept and set back lands a hair off its default */
                if (fabsf(p.value - p.def) > 1e-5f * fabsf(p.hi - p.lo))
                    put("%s.%s.%s=%.9g\n", SLOT_KEYS[i], MODELS[s.models[e]].id, p.id.c_str(), p.value);
            }
        }
    }
    return o < 0 ? 0 : o;
}

static void set_state(Rack *r, const char *txt) {
    std::string line;
    for (const char *t = txt;; t++) {
        if (*t && *t != '\n') { line += *t; continue; }
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            size_t d1 = k.find('.');
            if (d1 != std::string::npos) {   /* <slot>.<model>.<control> */
                size_t d2 = k.find('.', d1 + 1);
                std::string sk = k.substr(0, d1), mid = d2 == std::string::npos ? "" : k.substr(d1 + 1, d2 - d1 - 1);
                for (int i = 0; i < NSLOTS; i++) {
                    if (sk != SLOT_KEYS[i] || d2 == std::string::npos) continue;
                    Slot &s = r->slots[i];
                    int mk = model_by_id(s, mid.c_str());
                    if (mk < 0) break;
                    Loaded *l = build(s, mk);   /* restoring state may wait: a project is loading */
                    int pk = l->a ? l->a->find(k.substr(d2 + 1).c_str()) : -1;
                    if (pk >= 0) set_both(l, pk, (float)atof(v.c_str()));
                }
            } else if (k.size() > 6 && !k.compare(k.size() - 6, 6, "_model")) {
                const char *rest;
                int si = slot_of(k.c_str(), &rest);
                if (si >= 0) {
                    int mk = model_by_id(r->slots[si], v.c_str());
                    if (mk >= 0) { char b[16]; snprintf(b, sizeof b, "%d", mk); rk_set(r, k.c_str(), b); }
                }
            } else if (k != "v") rk_set(r, k.c_str(), v.c_str());
        }
        line.clear();
        if (!*t) break;
    }
    for (auto &s : r->slots) { build(s, s.want); s.cur = s.want; playing(s); }
    r->rev.fetch_add(1, std::memory_order_release);
}

/* ---- audio */
static void run_slot(Rack *rk, int si, Slot &s, float *l, float *r, bool stereo, int n) {
    if (s.pending_clear >= 0 && post_clear(rk, si, s.pending_clear)) s.pending_clear = -1;
    bool swap = s.cur != s.want && ready(s, s.want);   /* the picked model is built: fade the old one out */
    if (s.mix <= 0.f) {   /* idle: start (or not) from a clean model */
        if (swap) { s.cur = s.want; swap = false; }
        if (!s.on) return;
        Loaded *lp = ready(s, s.cur);
        if (!lp || !lp->a) return;
        if (!lp->clean.exchange(false, std::memory_order_acquire)) {   /* not cleared yet: wait for the loader */
            if (!lp->clearing.load(std::memory_order_relaxed) && s.pending_clear < 0) {
                lp->clearing.store(true, std::memory_order_relaxed);
                s.pending_clear = s.cur;
            }
            return;
        }
    }
    Loaded *lp = ready(s, s.cur);
    if (!lp || !lp->a) return;
    Loaded &ld = *lp;
    float dl[MAXN], dr[MAXN];
    memcpy(dl, l, n * sizeof(float));
    if (stereo) memcpy(dr, r, n * sizeof(float));
    if (!stereo && ld.a->stereo()) {   /* a stereo model in a mono slot: feed both sides, fold back to one */
        float sc[MAXN];
        memcpy(sc, l, n * sizeof(float));
        ld.a->run(n, l, sc);
        for (int i = 0; i < n; i++) l[i] = 0.5f * (l[i] + sc[i]);
    } else if (!stereo) ld.a->run(n, l, nullptr);
    else if (ld.a->stereo()) ld.a->run(n, l, r);
    else { ld.a->run(n, l, nullptr); if (ld.b) ld.b->run(n, r, nullptr); }
    float t = s.on && !swap ? 1.f : 0.f;
    for (int i = 0; i < n; i++) {
        if (s.mix != t) s.mix = t > s.mix ? fminf(t, s.mix + FADE) : fmaxf(t, s.mix - FADE);
        float a = s.mix;
        if (!std::isfinite(l[i])) l[i] = 0;
        l[i] = dl[i] + a * (l[i] - dl[i]);
        if (stereo) { if (!std::isfinite(r[i])) r[i] = 0; r[i] = dr[i] + a * (r[i] - dr[i]); }
    }
    if (s.mix <= 0.f) {   /* just faded out: it sits idle now, so have it cleared for next time */
        ld.clean.store(false, std::memory_order_relaxed);
        ld.clearing.store(true, std::memory_order_relaxed);
        if (!post_clear(rk, si, s.cur)) s.pending_clear = s.cur;
    }
}

#ifdef STOMPBOX_PROFILE   /* offline profiling only: the time each slot took in the last block, and its model */
#include <ctime>
double stompbox_prof[64];
int stompbox_prof_model[64];
#endif

static int16_t s16(float f) {
    f *= 32768.f;
    return f >= 32767.f ? 32767 : f <= -32768.f ? -32768 : (int16_t)lrintf(f);
}

static void rk_process(void *p, const int16_t *in, int16_t *out, int n) {
    Rack *r = (Rack *)p;
    unsigned long fz = fz_on();
    if (n > MAXN) n = MAXN;
    float l[MAXN], rr[MAXN];
    float tin = db2lin(r->input_db), tout = db2lin(r->output_db);
    for (int i = 0; i < n; i++) {
        r->in_g += 0.002f * (tin - r->in_g);
        l[i] = (in[2 * i] + in[2 * i + 1]) * (0.5f / 32768.f) * r->in_g;
    }
    bool split = false;
    for (int i = 0; i < NSLOTS; i++) {
        Slot &s = r->slots[i];
        bool st = SECTIONS[s.section].stereo;
        if (st && !split) { memcpy(rr, l, n * sizeof(float)); split = true; }
#ifdef STOMPBOX_PROFILE
        timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        run_slot(r, i, s, l, rr, st, n);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        stompbox_prof[i] = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
        stompbox_prof_model[i] = s.models[s.cur];
#else
        run_slot(r, i, s, l, rr, st, n);
#endif
    }
    if (!split) memcpy(rr, l, n * sizeof(float));
    for (int i = 0; i < n; i++) {
        r->out_g += 0.002f * (tout - r->out_g);
        out[2 * i] = s16(l[i] * r->out_g);
        out[2 * i + 1] = s16(rr[i] * r->out_g);
    }
    fz_off(fz);
}

static void rk_render(void *, int16_t *out, int n) { memset(out, 0, (size_t)n * 4); }

static const mpc_engine_t ENGINE = { rk_create, rk_destroy, rk_midi, rk_set, rk_get, rk_render, rk_process, nullptr };

extern "C" const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
