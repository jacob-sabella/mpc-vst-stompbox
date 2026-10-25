/* Stompbox: a guitar rack as one MPC insert effect, hosting guitarix and GxPlugins modules (models.json).
 *
 *   input gain -> [mono: gate, comp, wah, pedals, amp, tone stack, power amp, cabinet]
 *              -> [stereo: mod, delay, reverb, EQ] -> output gain
 *
 * Each slot has a model picker, an on switch and generic knobs (<slot>_k1..kN, 0..1) that take the names, ranges and
 * value text of the chosen model's controls (get_param "<key>_name" / "<key>_display"). A model is loaded the first
 * time it is picked and kept, with its own settings, so switching back finds it as it was. The two input channels are
 * summed into the mono part; a mono model in the stereo part runs once per side. Switching a slot on or off, or its
 * model, crossfades over 5 ms. State ("state"): every slot's model and settings by upstream control id.
 *
 * A paged section (models.json "paged": the pedals) holds any number of slots, up to its maximum: "<section>_add"
 * appends one, "<frame>_up" / "_down" / "_remove" move or drop the one in a frame, and its parameter frames
 * ("pedal1".."pedal3") show a page of them ("<section>_prev" / "_next"), so a frame's keys address the slot on the
 * page shown. A removed slot fades out before it is dropped.
 *
 * Loading a model (allocation, tables, an impulse response) takes far longer than an audio block, and every engine
 * call runs under the wrapper's lock, so a model picked on the screen or a Q-Link is built by a loader thread: the slot
 * plays its old model until the new one is ready, then crossfades, and "display_rev" moves so the wrapper re-reads the
 * knob names (HAS_DISPLAY_REV). Creating the rack and restoring state load in the caller's thread. Clearing a model
 * (its delay lines, before it fades in again) is the loader's job too: a slot that has faded out asks for it, and a slot
 * fades in only from a clean model. */
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
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
    int section, tab;
    std::string key, name;       /* in the state ("pedal4") and on the screen ("Pedal 4") */
    std::vector<int> models;     /* indices into MODELS */
    int cur = 0, want = 0;       /* index into models: playing, picked */
    int on = 0;
    float mix = 0;               /* 0 = out .. 1 = in */
    int pending_clear = -1;      /* audio thread: an entry to hand to the loader for clearing (the queue was busy) */
    bool gone = false;           /* removed: it fades out, then reap() drops it */
    std::unique_ptr<std::atomic<Loaded *>[]> loaded;   /* per entry of models: null until built (by either thread) */
    ~Slot() {
        for (size_t k = 0; k < models.size(); k++)
            if (Loaded *l = loaded[k].load()) { delete l->a; delete l->b; delete l; }
    }
};
typedef std::shared_ptr<Slot> SlotP;   /* the loader's queue holds slots too, so a removed one outlives its last job */

struct Group {                   /* a section's slots, in chain order (removed ones until they have faded out) */
    std::vector<SlotP> list;
    int page = 0;                /* paged: the page of frames shown */
};

struct Rack {
    std::vector<Group> groups;   /* per section */
    std::vector<SlotP> chain;    /* every group's slots in signal order: what the audio thread runs */
    float input_db = 0, output_db = 0, in_g = 1, out_g = 1;
    /* the loader thread: (slot, model entry) requests to build; (slot, -1 - entry) to clear */
    std::thread loader;
    std::mutex lk;
    std::condition_variable cv;
    std::vector<std::pair<SlotP, int>> queue;
    bool stop = false;
    std::atomic<int> rev{0};
    SlotP br;                    /* the model browser: the slot it picks for (null: closed) */
    int br_page = 0;             /* the page shown */
    std::string trig;            /* the last trigger and when (a repeat within REPEAT_MS is dropped) */
    std::chrono::steady_clock::time_point trig_t{};
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

static void request(Rack *r, const SlotP &s, int k) {   /* engine thread: ask the loader for entry k, never wait */
    if (ready(*s, k)) return;
    { std::lock_guard<std::mutex> g(r->lk); r->queue.push_back({s, k}); }
    r->cv.notify_one();
}

/* audio thread: hand an idle entry to the loader for clearing, without ever waiting for the lock */
static bool post_clear(Rack *r, const SlotP &s, int k) {
    std::unique_lock<std::mutex> g(r->lk, std::try_to_lock);
    if (!g.owns_lock()) return false;
    r->queue.push_back({s, -1 - k});
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
        Slot &s = *job.first;
        if (job.second >= 0) {
            build(s, job.second);
            r->rev.fetch_add(1, std::memory_order_release);
        } else if (Loaded *l = ready(s, -1 - job.second)) {   /* idle: the audio thread won't run it until clean */
            if (l->a) l->a->clear();
            if (l->b) l->b->clear();
            l->clearing.store(false, std::memory_order_relaxed);
            l->clean.store(true, std::memory_order_release);
        }
        job.first.reset();   /* a removed slot's last reference may be this one: free it outside the lock */
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

/* ---- the slots: groups, pages, frames */
static bool paged(int sec) { return SECTIONS[sec].paged > 0; }

/* a new slot of section sec, with the defaults of parameter frame d (an index into SLOT_KEYS) */
static SlotP new_slot(int sec, int d) {
    SlotP s = std::make_shared<Slot>();
    s->section = sec;
    s->tab = SLOT_TAB[d];
    s->key = SLOT_KEYS[d];
    s->name = SLOT_NAMES[d];
    for (int m = 0; m < NMODELS; m++) if (MODELS[m].section == sec) s->models.push_back(m);
    s->loaded.reset(new std::atomic<Loaded *>[s->models.size()]);
    for (size_t k = 0; k < s->models.size(); k++) s->loaded[k].store(nullptr);
    s->cur = s->want = SLOT_DEFAULT_MODEL[d];
    s->on = SLOT_DEFAULT_ON[d];
    return s;
}

static std::vector<SlotP> live(const Group &g) {
    std::vector<SlotP> v;
    for (auto &s : g.list) if (!s->gone) v.push_back(s);
    return v;
}
static int frames(int sec) { return SECTIONS[sec].nslots; }
static int pages(const Rack *r, int sec) {
    int n = (int)live(r->groups[sec]).size(), f = frames(sec);
    return n <= f ? 1 : (n + f - 1) / f;
}

/* after a change to a paged group: number its slots by position ("pedal3", "Pedal 3"), keep its page in range */
static void renumber(Rack *r, int sec) {
    Group &g = r->groups[sec];
    auto v = live(g);
    for (size_t i = 0; i < v.size(); i++) {
        v[i]->key = std::string(SECTIONS[sec].key) + std::to_string(i + 1);
        v[i]->name = std::string(SECTIONS[sec].item) + " " + std::to_string(i + 1);
    }
    int np = pages(r, sec);
    if (g.page >= np) g.page = np - 1;
    if (g.page < 0) g.page = 0;
}

static void relink(Rack *r) {   /* engine calls only: the audio thread never runs at the same time */
    r->chain.clear();
    for (auto &g : r->groups) for (auto &s : g.list) r->chain.push_back(s);
}

/* drop removed slots once they have faded out */
static void reap(Rack *r) {
    bool any = false;
    for (auto &g : r->groups)
        for (size_t i = 0; i < g.list.size();)
            if (g.list[i]->gone && g.list[i]->mix <= 0.f) { g.list.erase(g.list.begin() + i); any = true; }
            else i++;
    if (any) relink(r);
}

/* parameter frame i (an index into SLOT_KEYS) -> the slot it shows (null: an empty frame of a paged section) */
static SlotP framed(Rack *r, int i) {
    int sec = SLOT_SECTION[i], j = i - SECTIONS[sec].slot;
    Group &g = r->groups[sec];
    if (!paged(sec)) return g.list[j];
    auto v = live(g);
    size_t k = (size_t)(g.page * frames(sec) + j);
    return k < v.size() ? v[k] : nullptr;
}

/* "<frame>_<rest>" -> frame index, rest */
static int slot_of(const char *key, const char **rest) {
    for (int i = 0; i < NSLOTS; i++) {
        size_t n = strlen(SLOT_KEYS[i]);
        if (!strncmp(key, SLOT_KEYS[i], n) && key[n] == '_') { *rest = key + n + 1; return i; }
    }
    return -1;
}

/* "<section>_<rest>" of a paged section -> section index, rest */
static int group_of(const char *key, const char **rest) {
    for (int s = 0; s < NSECTIONS; s++) {
        size_t n = strlen(SECTIONS[s].key);
        if (paged(s) && !strncmp(key, SECTIONS[s].key, n) && key[n] == '_') { *rest = key + n + 1; return s; }
    }
    return -1;
}

static int model_by_id(const Slot &s, const char *id) {
    for (int k = 0; k < (int)s.models.size(); k++) if (!strcmp(MODELS[s.models[k]].id, id)) return k;
    return -1;
}

/* MPC delivers one tap on a trigger button twice (traced on a Key 37, 2026-10-24: "browse_next=1" twice, no 0 between),
 * which turned a two-page list forward and straight back, and would add two pedals. Opening and closing don't mind; a
 * page turn, an add, a move or a remove drops a repeat of the same trigger. */
static const int REPEAT_MS = 250;

static bool fresh(Rack *r, const char *key) {
    auto now = std::chrono::steady_clock::now();
    if (r->trig == key && now - r->trig_t < std::chrono::milliseconds(REPEAT_MS)) return false;
    r->trig = key;
    r->trig_t = now;
    return true;
}

static void touched(Rack *r) { r->rev.fetch_add(1, std::memory_order_release); }

/* ---- a paged section's own actions */
static void add_slot(Rack *r, int sec) {
    Group &g = r->groups[sec];
    if ((int)live(g).size() >= SECTIONS[sec].paged) return;
    SlotP s = new_slot(sec, SECTIONS[sec].slot);
    s->on = 1;   /* added to be heard: it fades in once the loader has built it */
    g.list.push_back(s);
    request(r, s, s->want);
    relink(r);
    renumber(r, sec);
    g.page = pages(r, sec) - 1;   /* where the new one is */
    touched(r);
}

static void remove_slot(Rack *r, int sec, const SlotP &s) {
    s->gone = true;
    s->on = 0;
    if (r->br == s) r->br.reset();
    renumber(r, sec);
    touched(r);
}

static void move_slot(Rack *r, int sec, const SlotP &s, int by) {   /* by -1: earlier in the chain, +1: later */
    Group &g = r->groups[sec];
    auto v = live(g);
    int i = 0;
    while (i < (int)v.size() && v[i] != s) i++;
    int j = i + by;
    if (i >= (int)v.size() || j < 0 || j >= (int)v.size()) return;
    size_t a = 0, b = 0;
    for (size_t k = 0; k < g.list.size(); k++) { if (g.list[k] == v[i]) a = k; if (g.list[k] == v[j]) b = k; }
    std::swap(g.list[a], g.list[b]);
    relink(r);
    renumber(r, sec);
    g.page = j / frames(sec);   /* follow it */
    touched(r);
}

static void set_state(Rack *r, const char *txt);

static void *rk_create(const char *) {
    Rack *r = new Rack();
    r->queue.reserve(256);
    r->groups.resize(NSECTIONS);
    for (int i = 0; i < NSLOTS; i++) {
        SlotP s = new_slot(SLOT_SECTION[i], i);
        build(*s, s->want);
        playing(*s);
        r->groups[s->section].list.push_back(s);
    }
    for (int sec = 0; sec < NSECTIONS; sec++) if (paged(sec)) renumber(r, sec);
    relink(r);
#ifdef STOMPBOX_BENCH
    /* bench builds only (tools/bench_rig.sh): start from a saved rig, since the bench can't set parameters */
    if (FILE *f = fopen("/tmp/stompbox_bench.state", "r")) {
        static char txt[16384];
        size_t n = fread(txt, 1, sizeof txt - 1, f);
        fclose(f);
        txt[n] = 0;
        set_state(r, txt);
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
    delete r;
}

static void rk_midi(void *, const uint8_t *, int) {}

/* ---- the model browser: a slot's model field ("<frame>_browse") opens it on that slot's page, BROWSE_ROWS names a
 * page ("brow_<j>": a name, "brow_<j>_on": the slot's model), any number of pages; a tap on a name picks it and closes
 * it. "browse_<page key>" is On while it is open on that page (the skin hides the page's own controls then), and
 * "browse_<page key>_paged" while it is with more than one page (the skin shows the arrows). Every change moves
 * display_rev, so the wrapper re-reads it all. */
static int br_pages(const Rack *r) {
    return !r->br ? 1 : ((int)r->br->models.size() + BROWSE_ROWS - 1) / BROWSE_ROWS;
}

static void browser(Rack *r, const SlotP &s, int page) {   /* page wraps around: < on the first page goes to the last */
    r->br = s;
    int np = br_pages(r);
    r->br_page = !s ? 0 : ((page % np) + np) % np;
    touched(r);
}

/* "brow_<j>[_on]" -> the model entry it shows (-1: none, or not a row key); *on: the "_on" suffix */
static int br_row(const Rack *r, const char *key, bool *on) {
    if (strncmp(key, "brow_", 5)) return -1;
    char *e;
    int j = (int)strtol(key + 5, &e, 10) - 1;
    *on = !strcmp(e, "_on");
    if ((*e && !*on) || j < 0 || j >= BROWSE_ROWS || !r->br) return -1;
    int k = r->br_page * BROWSE_ROWS + j;
    return k < (int)r->br->models.size() ? k : -1;
}

static bool br_open_on(const Rack *r, int tab) { return r->br && r->br->tab == tab; }

static void rk_set(void *p, const char *key, const char *val) {
    Rack *r = (Rack *)p;
    if (!val) return;
    reap(r);
    if (!strcmp(key, "state")) { set_state(r, val); return; }
    if (!strcmp(key, "input")) { r->input_db = (float)atof(val); return; }
    if (!strcmp(key, "output")) { r->output_db = (float)atof(val); return; }
    bool on, press = atof(val) >= 0.5;
    if (!strncmp(key, "brow_", 5)) {   /* a tap on a name, either toggle direction: pick it */
        int k = br_row(r, key, &on);
        if (k >= 0 && !on) {
            r->br->want = k;
            request(r, r->br, k);
            browser(r, nullptr, 0);
        }
        return;
    }
    if (!strncmp(key, "browse_", 7)) {   /* triggers act on press; "browse_<page>" and the arrows' states are ours */
        if (!press) return;
        if (!strcmp(key + 7, "close")) browser(r, nullptr, 0);
        else if (!strcmp(key + 7, "prev") && r->br && fresh(r, key)) browser(r, r->br, r->br_page - 1);
        else if (!strcmp(key + 7, "next") && r->br && fresh(r, key)) browser(r, r->br, r->br_page + 1);
        return;
    }
    const char *rest;
    int sec = group_of(key, &rest);
    if (sec >= 0) {   /* a paged section: add, page < > */
        if (!press || !fresh(r, key)) return;
        Group &g = r->groups[sec];
        int np = pages(r, sec);
        if (!strcmp(rest, "add")) add_slot(r, sec);
        else if (!strcmp(rest, "prev")) { g.page = (g.page + np - 1) % np; touched(r); }
        else if (!strcmp(rest, "next")) { g.page = (g.page + 1) % np; touched(r); }
        return;
    }
    int i = slot_of(key, &rest);
    if (i < 0) return;
    SlotP sp = framed(r, i);
    if (!sp) return;
    Slot &s = *sp;
    if (paged(s.section) && (!strcmp(rest, "up") || !strcmp(rest, "down") || !strcmp(rest, "remove"))) {
        if (!press || !fresh(r, key)) return;
        if (!strcmp(rest, "remove")) remove_slot(r, s.section, sp);
        else move_slot(r, s.section, sp, rest[0] == 'u' ? -1 : 1);
    } else if (!strcmp(rest, "browse")) {
        if (press) browser(r, sp, s.want / BROWSE_ROWS);
    } else if (!strcmp(rest, "model")) {
        int k = atoi(val);
        if (k >= 0 && k < (int)s.models.size()) { s.want = k; request(r, sp, k); }
    } else if (!strcmp(rest, "on")) {
        s.on = atoi(val) ? 1 : 0;
        if (s.on) request(r, sp, s.want);
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
    reap(r);
    if (!strcmp(key, "state")) return get_state(r, buf, len);
    if (!strcmp(key, "input")) return snprintf(buf, len, "%g", r->input_db);
    if (!strcmp(key, "output")) return snprintf(buf, len, "%g", r->output_db);
    if (!strcmp(key, "display_rev")) return snprintf(buf, len, "%d", r->rev.load(std::memory_order_acquire));
    bool on;
    if (!strncmp(key, "brow_", 5)) {
        int k = br_row(r, key, &on);
        if (on) return snprintf(buf, len, "%d", k >= 0 && k == r->br->want);
        /* past the last model: a space, since an empty answer makes the wrapper show the row's own name */
        return snprintf(buf, len, "%s", k >= 0 ? MODELS[r->br->models[k]].name : " ");
    }
    if (!strncmp(key, "browse_", 7)) {
        const char *b = key + 7;
        if (!strcmp(b, "title")) {
            if (!r->br) return snprintf(buf, len, "%s", " ");
            int n = (int)r->br->models.size(), np = br_pages(r);
            return np > 1 ? snprintf(buf, len, "%s  \xc2\xb7  %d models  \xc2\xb7  page %d of %d", r->br->name.c_str(), n,
                                     r->br_page + 1, np)
                          : snprintf(buf, len, "%s  \xc2\xb7  %d models", r->br->name.c_str(), n);
        }
        if (!strcmp(b, "close") || !strcmp(b, "prev") || !strcmp(b, "next")) return snprintf(buf, len, "0");
        for (int t = 0; t < NTABS; t++) {   /* "browse_<page>": open on that page; "browse_<page>_paged": with arrows */
            size_t n = strlen(TAB_KEYS[t]);
            if (strncmp(b, TAB_KEYS[t], n) || (b[n] && strcmp(b + n, "_paged"))) continue;
            return snprintf(buf, len, "%d", br_open_on(r, t) && (!b[n] || br_pages(r) > 1));
        }
        return 0;
    }
    const char *rest;
    int sec = group_of(key, &rest);
    if (sec >= 0) {
        int n = (int)live(r->groups[sec]).size(), f = frames(sec), pg = r->groups[sec].page;
        if (!strcmp(rest, "title")) {   /* "Pedals 4-6 of 7" */
            if (!n) return snprintf(buf, len, "No %s", SECTIONS[sec].name);
            int a = pg * f + 1, z = std::min(n, a + f - 1);
            return a == z ? snprintf(buf, len, "%s %d of %d", SECTIONS[sec].item, a, n)
                          : snprintf(buf, len, "%s %d\xe2\x80\x93%d of %d", SECTIONS[sec].name, a, z, n);
        }
        if (!strcmp(rest, "paged"))   /* the arrows: more than one page, and the page's browser closed */
            return snprintf(buf, len, "%d", pages(r, sec) > 1 && !br_open_on(r, SLOT_TAB[SECTIONS[sec].slot]));
        return snprintf(buf, len, "0");   /* the triggers */
    }
    int i = slot_of(key, &rest);
    if (i < 0) return 0;
    SlotP sp = framed(r, i);
    if (paged(SLOT_SECTION[i])) {
        if (!strcmp(rest, "view"))   /* 0: hidden (the page's browser is open), 1: a slot, 2: empty */
            return snprintf(buf, len, "%d", br_open_on(r, SLOT_TAB[i]) ? 0 : sp ? 1 : 2);
        if (!strcmp(rest, "title")) {   /* shown by the skin whatever the view (see gen_models.py), so blank unless a slot is */
            if (!sp || br_open_on(r, SLOT_TAB[i])) return snprintf(buf, len, "%s", " ");
            std::string t = sp->name;
            for (auto &c : t) c = (char)toupper((unsigned char)c);
            return snprintf(buf, len, "%s", t.c_str());
        }
        if (!strcmp(rest, "up") || !strcmp(rest, "down") || !strcmp(rest, "remove")) return snprintf(buf, len, "0");
    }
    if (!strcmp(rest, "browse")) return snprintf(buf, len, "0");
    if (!sp) {   /* an empty frame: nothing to show, and never the wrapper's fallback names */
        if (rest[0] == 'k' && (strstr(rest, "_name") || strstr(rest, "_display"))) return snprintf(buf, len, "%s", " ");
        return snprintf(buf, len, "0");
    }
    Slot &s = *sp;
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

/* ---- state: key=value lines. "<section>_count=<n>" per paged section, then per slot "<slot>_model=<model id>",
 * "<slot>_on=0|1", then "<slot>.<model id>.<control id>=<real value>": each slot's picked model first, then the other
 * models it has loaded, as far as they fit (the wrapper's chunk is 8 KB) */
static int get_state(Rack *r, char *buf, int len) {
    int o = 0;
    bool full = false;
    auto put = [&](const char *fmt, auto... a) {   /* whole lines only */
        if (full) return;
        int w = snprintf(buf + o, len - o, fmt, a...);
        if (w < 0 || w >= len - o) { buf[o] = 0; full = true; return; }
        o += w;
    };
    put("v=1\ninput=%g\noutput=%g\n", r->input_db, r->output_db);
    for (int sec = 0; sec < NSECTIONS; sec++)
        if (paged(sec)) put("%s_count=%d\n", SECTIONS[sec].key, (int)live(r->groups[sec]).size());
    std::vector<SlotP> all;
    for (auto &s : r->chain) if (!s->gone) all.push_back(s);
    for (auto &s : all) put("%s_model=%s\n%s_on=%d\n", s->key.c_str(), MODELS[s->models[s->want]].id, s->key.c_str(), s->on);
    for (int pass = 0; pass < 2; pass++) {   /* the picked models, then the others */
        for (auto &sp : all) {
            Slot &s = *sp;
            for (size_t e = 0; e < s.models.size(); e++) {
                if (((int)e == s.want) != (pass == 0)) continue;
                Loaded *l = ready(s, (int)e);
                Module *m = l ? l->a : nullptr;
                if (!m) continue;
                for (int k = 0; k < m->nparams(); k++) {
                    const mod_param &p = m->param(k);   /* a knob swept and set back lands a hair off its default */
                    if (fabsf(p.value - p.def) > 1e-5f * fabsf(p.hi - p.lo))
                        put("%s.%s.%s=%.9g\n", s.key.c_str(), MODELS[s.models[e]].id, p.id.c_str(), p.value);
                }
            }
        }
    }
    return o;
}

static SlotP state_slot(Rack *r, const std::string &key) {
    for (auto &s : r->chain) if (!s->gone && s->key == key) return s;
    return nullptr;
}

static void set_state(Rack *r, const char *txt) {
    std::vector<std::pair<std::string, std::string>> kv;
    std::string line;
    for (const char *t = txt;; t++) {
        if (*t && *t != '\n') { line += *t; continue; }
        size_t eq = line.find('=');
        if (eq != std::string::npos) kv.push_back({line.substr(0, eq), line.substr(eq + 1)});
        line.clear();
        if (!*t) break;
    }
    /* paged sections start over with the count saved (a state from before they were paged: their frame count) */
    r->br.reset();
    for (int sec = 0; sec < NSECTIONS; sec++) {
        if (!paged(sec)) continue;
        int n = frames(sec);
        for (auto &x : kv)
            if (x.first == std::string(SECTIONS[sec].key) + "_count") n = atoi(x.second.c_str());
        n = std::max(0, std::min(n, SECTIONS[sec].paged));
        Group &g = r->groups[sec];
        g.list.clear();
        g.page = 0;
        for (int i = 0; i < n; i++) g.list.push_back(new_slot(sec, SECTIONS[sec].slot + std::min(i, frames(sec) - 1)));
        renumber(r, sec);
    }
    relink(r);
    for (auto &x : kv) {
        const std::string &k = x.first, &v = x.second;
        size_t d1 = k.find('.');
        if (d1 != std::string::npos) {   /* <slot>.<model>.<control> */
            size_t d2 = k.find('.', d1 + 1);
            SlotP s = d2 == std::string::npos ? nullptr : state_slot(r, k.substr(0, d1));
            if (!s) continue;
            int mk = model_by_id(*s, k.substr(d1 + 1, d2 - d1 - 1).c_str());
            if (mk < 0) continue;
            Loaded *l = build(*s, mk);   /* restoring state may wait: a project is loading */
            int pk = l->a ? l->a->find(k.substr(d2 + 1).c_str()) : -1;
            if (pk >= 0) set_both(l, pk, (float)atof(v.c_str()));
        } else if (k == "input") r->input_db = (float)atof(v.c_str());
        else if (k == "output") r->output_db = (float)atof(v.c_str());
        else if (k.size() > 6 && !k.compare(k.size() - 6, 6, "_model")) {
            SlotP s = state_slot(r, k.substr(0, k.size() - 6));
            int mk = s ? model_by_id(*s, v.c_str()) : -1;
            if (mk >= 0) s->want = mk;
        } else if (k.size() > 3 && !k.compare(k.size() - 3, 3, "_on")) {
            SlotP s = state_slot(r, k.substr(0, k.size() - 3));
            if (s) s->on = atoi(v.c_str()) ? 1 : 0;
        }
    }
    for (auto &s : r->chain) { build(*s, s->want); s->cur = s->want; playing(*s); }
    touched(r);
}

/* ---- audio */
static void run_slot(Rack *rk, const SlotP &sp, float *l, float *r, bool stereo, int n) {
    Slot &s = *sp;
    if (s.pending_clear >= 0 && post_clear(rk, sp, s.pending_clear)) s.pending_clear = -1;
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
    if (s.mix <= 0.f && !s.gone) {   /* just faded out: it sits idle now, so have it cleared for next time */
        ld.clean.store(false, std::memory_order_relaxed);
        ld.clearing.store(true, std::memory_order_relaxed);
        if (!post_clear(rk, sp, s.cur)) s.pending_clear = s.cur;
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
    for (size_t i = 0; i < r->chain.size(); i++) {
        const SlotP &s = r->chain[i];
        bool st = SECTIONS[s->section].stereo;
        if (st && !split) { memcpy(rr, l, n * sizeof(float)); split = true; }
#ifdef STOMPBOX_PROFILE
        timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        run_slot(r, s, l, rr, st, n);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (i < 64) {
            stompbox_prof[i] = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
            stompbox_prof_model[i] = s->models[s->cur];
        }
#else
        run_slot(r, s, l, rr, st, n);
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
