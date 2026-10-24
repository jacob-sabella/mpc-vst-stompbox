#include "host.h"
#include <cmath>
#include <cstring>
#include <mutex>
#include "gx_plugin.h"

/* ---- guitarix: a module registers its controls through ParamReg's callbacks, which carry no context of their own,
 * so the module being created is held here for the duration of its register_params() call. */
struct Reg {
    static std::mutex lock;
    static Module *cur;

    static mod_param *existing(const char *id) {
        for (auto &p : cur->params_) if (p.id == id) return &p;
        return nullptr;
    }
    static bool skip(const char *tp) {   /* output meters and the like: not controls */
        return tp && (strchr(tp, 'o') || strchr(tp, 'O') || strchr(tp, 'M'));
    }
    static void labels(mod_param &p, const value_pair *v) {
        for (; v && v->value_id; v++) p.labels.push_back(v->value_label ? v->value_label : v->value_id);
    }
    static float *f(const char *id, const char *name, const char *tp, const char *, float *var, float val, float lo,
                    float up, float step, const value_pair *values) {
        *var = val;
        if (skip(tp)) return var;
        if (mod_param *p = existing(id)) { *var = p->value; return p->f ? p->f : var; }   /* an alias: share the first */
        mod_param p;
        p.id = id; p.name = name ? name : ""; p.f = var; p.def = p.value = val; p.lo = lo; p.hi = up; p.step = step;
        p.log = tp && strchr(tp, 'L');
        if (values) { labels(p, values); p.type = mod_param::ENUM; p.lo = 0; p.hi = (float)p.labels.size() - 1; }
        else if (tp && tp[0] == 'B') { p.type = mod_param::BOOL; p.lo = 0; p.hi = 1; }
        cur->params_.push_back(p);
        return var;
    }
    static int *i(const char *id, const char *name, const char *tp, const char *, int *var, int val, int lo, int up,
                  const value_pair *values) {
        *var = val;
        if (skip(tp)) return var;
        if (mod_param *p = existing(id)) { *var = (int)p->value; return p->i ? p->i : var; }
        mod_param p;
        p.id = id; p.name = name ? name : ""; p.i = var; p.def = p.value = (float)val; p.step = 1;
        p.type = mod_param::ENUM;
        labels(p, values);
        p.lo = p.labels.empty() ? (float)lo : 0;
        p.hi = p.labels.empty() ? (float)up : (float)p.labels.size() - 1;
        if (p.labels.empty())
            for (int v = lo; v <= up && up - lo < 64; v++) p.labels.push_back(std::to_string(v));
        cur->params_.push_back(p);
        return var;
    }
    static bool *b(const char *id, const char *name, const char *tp, const char *, bool *var, bool val) {
        *var = val;
        if (skip(tp)) return var;
        if (mod_param *p = existing(id)) { *var = p->value > 0.5f; return p->b ? p->b : var; }
        mod_param p;
        p.id = id; p.name = name ? name : ""; p.b = var; p.def = p.value = val ? 1.0f : 0.0f;
        p.type = mod_param::BOOL; p.lo = 0; p.hi = 1;
        cur->params_.push_back(p);
        return var;
    }
};
std::mutex Reg::lock;
Module *Reg::cur = nullptr;

/* keep the params named in show (in that order), relabel from names */
static void curate(std::vector<mod_param> &ps, const model_t *m) {
    if (m->show) {
        std::vector<mod_param> out;
        for (const char *const *s = m->show; *s; s++)
            for (auto &p : ps) if (p.id == *s) { out.push_back(p); break; }
        ps.swap(out);
    }
    for (const name_t *n = m->names; n && n->id; n++)
        for (auto &p : ps) if (p.id == n->id) p.name = n->label;
    for (auto &p : ps) if (p.name.empty()) {   /* fall back to the id's last part: "amp2.stage1.Pregain" -> "Pregain" */
        size_t dot = p.id.rfind('.');
        p.name = dot == std::string::npos ? p.id : p.id.substr(dot + 1);
    }
}

Module *Module::create(const model_t *m, int rate) {
    Module *x = new Module();
    x->m_ = m;
    if (m->kind == MK_GX) {
        PluginDef *pd = m->gx();
        if (!pd || (pd->version & PLUGINDEF_VERMAJOR_MASK) != (PLUGINDEF_VERSION & PLUGINDEF_VERMAJOR_MASK)) {
            if (pd && pd->delete_instance) pd->delete_instance(pd);
            delete x; return nullptr;
        }
        x->pd_ = pd;
        x->stereo_ = pd->stereo_audio != nullptr;
        if (pd->register_params) {
            std::lock_guard<std::mutex> g(Reg::lock);
            Reg::cur = x;
            ParamReg reg = { pd, Reg::f, Reg::i, Reg::b };
            pd->register_params(reg);
            Reg::cur = nullptr;
        }
        curate(x->params_, m);
        if (pd->set_samplerate) pd->set_samplerate(rate, pd);
        if (pd->activate_plugin) pd->activate_plugin(true, pd);
        if (pd->clear_state) pd->clear_state(pd);
    } else if (m->kind == MK_LV2) {
        x->d_ = m->lv2(0);
        static const LV2_Feature *const none[] = { nullptr };
        x->h_ = x->d_ ? x->d_->instantiate(x->d_, rate, "/", none) : nullptr;
        if (!x->h_) { delete x; return nullptr; }
        x->stereo_ = m->ain[1] >= 0;
        /* every control input gets its default, shown or not (a bypass port must read "on") */
        for (int k = 0; k < m->nports; k++) {
            const lv2_port_t &pt = m->ports[k];
            if (pt.index < 0 || pt.index >= 64) continue;
            x->ports_[pt.index] = pt.def;
        }
        for (int k = 0; k < 64; k++) x->d_->connect_port(x->h_, k, &x->ports_[k]);   /* audio ports reconnected in run() */
        for (int k = 0; k < m->nports; k++) {
            const lv2_port_t &pt = m->ports[k];
            if (!pt.name) continue;   /* a hidden port (bypass) */
            mod_param p;
            p.id = pt.sym; p.name = pt.name; p.lo = pt.lo; p.hi = pt.hi; p.def = p.value = pt.def;
            p.f = &x->ports_[pt.index]; p.log = pt.logarithmic;
            if (pt.nlabels) { p.type = mod_param::ENUM; for (int l = 0; l < pt.nlabels; l++) p.labels.push_back(pt.labels[l]); }
            else if (pt.toggled) p.type = mod_param::BOOL;
            else if (pt.integer) { p.type = mod_param::ENUM; for (int v = (int)pt.lo; v <= (int)pt.hi; v++) p.labels.push_back(std::to_string(v)); }
            x->params_.push_back(p);
        }
        if (x->d_->activate) x->d_->activate(x->h_);
    } else {
        x->bi_ = make_builtin(m, rate);
        if (!x->bi_) { delete x; return nullptr; }
        x->params_ = x->bi_->params;
        for (auto &p : x->params_) p.f = nullptr;
    }
    return x;
}

Module::~Module() {
    delete bi_;
    if (pd_) {
        if (pd_->activate_plugin) pd_->activate_plugin(false, pd_);
        if (pd_->delete_instance) pd_->delete_instance(pd_);
    }
    if (h_) {
        if (d_->deactivate) d_->deactivate(h_);
        d_->cleanup(h_);
    }
}

int Module::find(const char *id) const {
    for (int k = 0; k < nparams(); k++) if (params_[k].id == id) return k;
    return -1;
}

void Module::set(int k, float v) {
    if (k < 0 || k >= nparams()) return;
    mod_param &p = params_[k];
    v = std::fmin(std::fmax(v, std::fmin(p.lo, p.hi)), std::fmax(p.lo, p.hi));
    if (p.type != mod_param::CONT) v = std::round(v);
    p.value = v;
    if (p.f) *p.f = v;
    if (p.i) *p.i = (int)v;
    if (p.b) *p.b = v > 0.5f;
    if (bi_) { bi_->params[k].value = v; bi_->changed(); }
}

void Module::clear() {
    if (bi_) bi_->clear();
    if (pd_ && pd_->clear_state) pd_->clear_state(pd_);
    if (h_) {   /* LV2 has no clear: restart the plugin's state the way a host does */
        if (d_->deactivate) d_->deactivate(h_);
        if (d_->activate) d_->activate(h_);
    }
}

void Module::run(int n, float *l, float *r) {
    if (n > 128) n = 128;
    if (bi_) { bi_->run(n, l); return; }
    if (pd_) {
        if (stereo_) pd_->stereo_audio(n, l, r, l, r, pd_);
        else if (pd_->mono_audio) pd_->mono_audio(n, l, l, pd_);
        return;
    }
    const model_t *m = m_;
    d_->connect_port(h_, m->ain[0], l);
    d_->connect_port(h_, m->aout[0], tmp_[0]);
    if (stereo_) { d_->connect_port(h_, m->ain[1], r); d_->connect_port(h_, m->aout[1], tmp_[1]); }
    d_->run(h_, n);
    memcpy(l, tmp_[0], n * sizeof(float));
    if (stereo_) memcpy(r, tmp_[1], n * sizeof(float));
}
