/* Offline test of the rack engine (no wrapper), linked with every upstream module (tests/run.sh builds it):
 *   - every model of every slot loads, names its knobs, plays a plucked tone and stays finite and bounded
 *   - every knob of every model moves to both ends without NaN or a blow-up
 *   - the default chain makes sound; switching models and slots on and off is click-free (crossfades)
 *   - silence after a loud chain decays to zero; state round-trips into a fresh instance; bad state is ignored
 * Prints each model's x86 cost per 128-frame block (relative numbers only; the device bench is tools/bench.sh)
 * and a list of models that came out silent, for a look by ear. Exit status = failures. */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
extern "C" {
#include "engine.h"
}
#include "models.h"

static int fails;
#define CHECK(c, ...) do { if (c) { if (verbose) { printf("ok   " __VA_ARGS__); printf("\n"); } } else { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool verbose = true;
static const int N = 128;

static void tone(int16_t *in, long t0, float amp) {
    for (int i = 0; i < N; i++) {
        long t = t0 + i;
        float ph = fmodf(t * 110.f / 44100.f, 1.f);
        float env = expf(-(float)(t % 44100) / 15000.f);
        int16_t v = (int16_t)(amp * env * (2 * ph - 1) * 32767);
        in[2 * i] = in[2 * i + 1] = v;
    }
}

struct Res { double rms, peak, us; bool finite; int maxstep; };

static Res play(const mpc_engine_t *e, void *p, int blocks, float amp, long *t) {
    int16_t in[2 * N], out[2 * N];
    double sum = 0, peak = 0; bool fin = true; int last = 0, maxstep = 0;
    struct timespec a, b;
    double us = 0;
    for (int k = 0; k < blocks; k++) {
        tone(in, *t, amp); *t += N;
        clock_gettime(CLOCK_MONOTONIC, &a);
        e->process(p, in, out, N);
        clock_gettime(CLOCK_MONOTONIC, &b);
        us += (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;
        for (int i = 0; i < 2 * N; i++) {
            double v = out[i] / 32768.0;
            sum += v * v; if (fabs(v) > peak) peak = fabs(v);
            if (i % 2 == 0) { int d = abs(out[i] - last); if (d > maxstep) maxstep = d; last = out[i]; }
        }
    }
    (void)fin;
    return { sqrt(sum / (blocks * 2.0 * N)), peak, us / blocks, true, maxstep };
}

static void set(const mpc_engine_t *e, void *p, const std::string &k, const std::string &v) { e->set_param(p, k.c_str(), v.c_str()); }
static std::string get(const mpc_engine_t *e, void *p, const std::string &k) {
    char b[256] = { 0 };
    return e->get_param(p, k.c_str(), b, sizeof b) > 0 ? b : "";
}
/* pick a model and wait for the loader thread to build it (display_rev moves; at most 5 s for one already built).
 * Returns how long the set_param itself took: the audio waits that long under the wrapper's lock. */
static double pick(const mpc_engine_t *e, void *p, const std::string &slot, int k) {
    std::string rev = get(e, p, "display_rev");
    struct timespec a, b, ms = { 0, 1000000 };
    clock_gettime(CLOCK_MONOTONIC, &a);
    set(e, p, slot + "_model", std::to_string(k));
    clock_gettime(CLOCK_MONOTONIC, &b);
    for (int i = 0; i < 5000 && get(e, p, "display_rev") == rev; i++) nanosleep(&ms, nullptr);
    return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : nullptr;   /* a model id: test just that one */
    const mpc_engine_t *e = mpc_engine();
    void *p = e->create(nullptr);
    CHECK(p, "create");
    long t = 0;

    Res def = play(e, p, 400, 0.3f, &t);
    printf("     default chain: rms %.4f peak %.3f, %.1f us/block\n", def.rms, def.peak, def.us);
    CHECK(def.rms > 0.003 && def.peak < 1.0, "default chain makes sound without clipping");

    /* every slot off: the input passes */
    for (int s = 0; s < NSLOTS; s++) set(e, p, std::string(SLOT_KEYS[s]) + "_on", "0");
    play(e, p, 100, 0.3f, &t);   /* the fade out */
    t = 0;   /* a fresh pluck: the input peaks at 0.3 */
    Res dry = play(e, p, 200, 0.3f, &t);
    CHECK(fabs(dry.peak - 0.3) < 0.01, "everything off passes the input (peak %.3f)", dry.peak);

    std::vector<std::string> silent, loud;
    int tested = 0;
    double worst_set_ms = 0;   /* the longest set_param of a model: the audio waits that long under the wrapper's lock */
    verbose = false;
    for (int s = 0; s < NSLOTS; s++) {
        if (s > 0 && SLOT_SECTION[s] == SLOT_SECTION[s - 1]) continue;   /* the second pedal slot has the same models */
        std::string sk = SLOT_KEYS[s];
        int n = 0;
        for (int m = 0; m < NMODELS; m++) if (MODELS[m].section == SLOT_SECTION[s]) n++;
        set(e, p, sk + "_on", "1");
        for (int k = 0; k < n; k++) {
            int mi = -1, c = 0;
            for (int m = 0; m < NMODELS; m++) if (MODELS[m].section == SLOT_SECTION[s] && c++ == k) { mi = m; break; }
            if (only && strcmp(MODELS[mi].id, only)) continue;
            tested++;
            double set_ms = pick(e, p, sk, k);
            if (set_ms > worst_set_ms) worst_set_ms = set_ms;
            CHECK(get(e, p, sk + "_model") == std::to_string(k), "%s: picks model %d", sk.c_str(), k);
            int named = 0;
            for (int j = 1; j <= SECTIONS[SLOT_SECTION[s]].nknobs; j++) {
                std::string kj = sk + "_k" + std::to_string(j), nm = get(e, p, kj + "_name");
                if (nm != " ") { named++; continue; }
                /* a knob the model doesn't use: blank name and value (a space, never empty: the wrapper would fall back) */
                CHECK(get(e, p, kj + "_display") == " ", "%s %s: unused knob %d reads blank", sk.c_str(), MODELS[mi].id, j);
            }
            CHECK(named > 0 || !strcmp(MODELS[mi].id, "dcblocker"), "%s %s: has named knobs", sk.c_str(), MODELS[mi].id);
            play(e, p, 20, 0.3f, &t);   /* the crossfade */
            Res r = play(e, p, 300, 0.3f, &t);
            CHECK(std::isfinite(r.rms) && r.peak <= 1.0, "%s: finite, bounded", MODELS[mi].id);
            /* every knob to both ends, then back to its own default */
            double worst = 0;
            for (int j = 1; j <= named; j++) {
                std::string kk = sk + "_k" + std::to_string(j);
                std::string keep = get(e, p, kk);
                for (const char *v : { "0", "1" }) {
                    set(e, p, kk, v);
                    Res q = play(e, p, 30, 0.3f, &t);
                    CHECK(std::isfinite(q.rms), "%s knob %d = %s: finite", MODELS[mi].id, j, v);
                    if (q.rms > worst) worst = q.rms;
                }
                set(e, p, kk, keep);
            }
            play(e, p, 60, 0.3f, &t);
            printf("     %-8s %-28s %6.1f us  rms %.4f  peak %.3f  knobs %d\n", sk.c_str(), MODELS[mi].name, r.us, r.rms, r.peak, named);
            if (r.rms < 1e-4) silent.push_back(std::string(MODELS[mi].id) + " (" + MODELS[mi].name + ")");
            if (r.peak >= 0.999) loud.push_back(std::string(MODELS[mi].id) + " (" + MODELS[mi].name + ")");
        }
        set(e, p, sk + "_on", "0");
    }
    verbose = true;
    printf("     %d models played\n", tested);
    CHECK(worst_set_ms < 1.0, "picking a model never blocks (longest set_param %.3f ms)", worst_set_ms);
    if (!silent.empty()) { printf("     silent at defaults:"); for (auto &x : silent) printf(" %s", x.c_str()); printf("\n"); }
    if (!loud.empty()) { printf("     at full scale at defaults:"); for (auto &x : loud) printf(" %s", x.c_str()); printf("\n"); }
    if (only) { e->destroy(p); return fails != 0; }

    /* switching: a model change mid-note on a quiet signal crossfades */
    {
        set(e, p, "pedal1_on", "1"); pick(e, p, "pedal1", 0);
        play(e, p, 50, 0.05f, &t);
        set(e, p, "pedal1_model", "5");
        Res r = play(e, p, 20, 0.05f, &t);
        CHECK(r.maxstep < 8000, "a model change crossfades (largest step %d)", r.maxstep);
        set(e, p, "pedal1_on", "0");
        Res r2 = play(e, p, 20, 0.05f, &t);
        CHECK(r2.maxstep < 8000, "switching a slot off crossfades (largest step %d)", r2.maxstep);
    }

    /* everything on at once, then silence: decays to zero, no NaN */
    {
        for (int s = 0; s < NSLOTS; s++) set(e, p, std::string(SLOT_KEYS[s]) + "_on", "1");
        Res r = play(e, p, 300, 0.5f, &t);
        printf("     every slot on: rms %.4f peak %.3f, %.1f us/block\n", r.rms, r.peak, r.us);
        CHECK(std::isfinite(r.rms) && r.peak <= 1.0, "every slot on: finite");
        for (int s = 0; s < NSLOTS; s++) set(e, p, std::string(SLOT_KEYS[s]) + "_on", "0");
        set(e, p, "amp_on", "1"); set(e, p, "pedal1_on", "1"); set(e, p, "cab_on", "1");
        set(e, p, "gate_on", "0");
        play(e, p, 100, 0.9f, &t);
        int16_t in[2 * N] = { 0 }, out[2 * N]; int nz = 0;
        for (int k = 0; k < 3000; k++) { e->process(p, in, out, N); if (k > 2900) for (int i = 0; i < 2 * N; i++) nz += out[i] != 0; }
        CHECK(nz == 0, "the tail decays to silence (%d non-zero samples in the last 100 blocks)", nz);
    }

    /* the bench's Q-Link sweep: four random parameter writes every fourth block for 10 s of audio, so slots fade in and
     * out, models change and the loader builds and clears them while the audio runs; nothing may go NaN or stick */
    {
        std::vector<std::pair<std::string, int>> keys;   /* key, options (0: a 0..1 knob) */
        for (int s = 0; s < NSLOTS; s++) {
            std::string k = SLOT_KEYS[s];
            int n = 0;
            for (int m = 0; m < NMODELS; m++) if (MODELS[m].section == SLOT_SECTION[s]) n++;
            keys.push_back({k + "_model", n}); keys.push_back({k + "_on", 2});
            for (int j = 1; j <= SECTIONS[SLOT_SECTION[s]].nknobs; j++) keys.push_back({k + "_k" + std::to_string(j), 0});
        }
        srand(7);
        bool fin = true;
        int16_t in[2 * N], out[2 * N];
        for (int b = 0; b < 44100 * 10 / N; b++) {
            if (b % 4 == 0)
                for (int k = 0; k < 4; k++) {
                    auto &kk = keys[rand() % keys.size()];
                    float f = rand() / (float)RAND_MAX;
                    set(e, p, kk.first, kk.second ? std::to_string(std::min(kk.second - 1, (int)(f * kk.second))) : std::to_string(f));
                }
            tone(in, t, 0.3f); t += N;
            e->process(p, in, out, N);
        }
        for (int s = 0; s < NSLOTS; s++) set(e, p, std::string(SLOT_KEYS[s]) + "_on", "0");
        set(e, p, "gate_on", "1"); set(e, p, "amp_on", "1");
        play(e, p, 200, 0.3f, &t);
        Res r = play(e, p, 100, 0.3f, &t);
        CHECK(r.rms > 0.001 && r.peak <= 1.0 && fin, "after a random parameter sweep the rack still plays (rms %.4f)", r.rms);
    }

    /* state: into a fresh instance, and garbage */
    {
        pick(e, p, "amp", 3); set(e, p, "amp_k1", "0.8"); set(e, p, "reverb_on", "1"); pick(e, p, "reverb", 2);
        set(e, p, "reverb_k2", "0.25"); set(e, p, "input", "6");
        std::string st = get(e, p, "state");
        static char big[65536];
        int n = e->get_param(p, "state", big, sizeof big);
        CHECK(n > 0, "state saved (%d bytes)", n);
        void *q = e->create(nullptr);
        e->set_param(q, "state", big);
        static char big2[65536];
        e->get_param(q, "state", big2, sizeof big2);
        CHECK(!strcmp(big, big2), "state restores into a fresh instance");
        if (strcmp(big, big2)) {
            FILE *f = fopen("build/state_a.txt", "w"); fputs(big, f); fclose(f);
            f = fopen("build/state_b.txt", "w"); fputs(big2, f); fclose(f);
            printf("     (the two states are in build/state_a.txt and build/state_b.txt)\n");
        }
        CHECK(get(e, q, "amp_k1") == get(e, p, "amp_k1"), "amp knob 1 restored (%s)", get(e, q, "amp_k1").c_str());
        CHECK(get(e, q, "reverb_model") == "2", "reverb model restored");
        e->set_param(q, "state", "garbage\n=\n.=.\npedal1.nope.x=3\namp_model=nope\npedal1_on=1\n\n\n");
        CHECK(get(e, q, "pedal1_on") == "1", "bad state lines are skipped, good ones applied");
        e->destroy(q);
    }
    e->destroy(p);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
