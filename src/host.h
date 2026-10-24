/* One loaded upstream module: a guitarix PluginDef or a GxPlugins LV2 pedal, behind the same interface.
 * Controls are listed with their real ranges; set() takes a real value. run() is in place. */
#pragma once
#include <string>
#include <vector>
#include "models.h"

struct mod_param {
    std::string id, name;
    float lo = 0, hi = 1, def = 0, step = 0;
    enum { CONT, BOOL, ENUM } type = CONT;
    bool log = false;
    std::vector<std::string> labels;   /* ENUM: one per value from lo */
    float *f = nullptr; int *i = nullptr; bool *b = nullptr;   /* where the module reads it */
    float value = 0;
};

/* a model written here (builtins.cpp): the noise gate, the cabinet */
struct Builtin {
    virtual ~Builtin() {}
    virtual void run(int n, float *x) = 0;
    virtual void clear() = 0;
    virtual void changed() {}   /* a control moved */
    std::vector<mod_param> params;
};
Builtin *make_builtin(const model_t *m, int rate);

class Module {
public:
    static Module *create(const model_t *m, int rate);
    ~Module();
    const model_t *model() const { return m_; }
    bool stereo() const { return stereo_; }
    int nparams() const { return (int)params_.size(); }
    const mod_param &param(int k) const { return params_[k]; }
    int find(const char *id) const;
    void set(int k, float v);
    void clear();
    /* n <= 128 frames; r is ignored for a mono module */
    void run(int n, float *l, float *r);

private:
    Module() = default;
    const model_t *m_ = nullptr;
    bool stereo_ = false;
    std::vector<mod_param> params_;
    PluginDef *pd_ = nullptr;
    Builtin *bi_ = nullptr;
    const LV2_Descriptor *d_ = nullptr;
    LV2_Handle h_ = nullptr;
    float ports_[64] = {};
    float tmp_[2][128] = {};
    friend struct Reg;
};
