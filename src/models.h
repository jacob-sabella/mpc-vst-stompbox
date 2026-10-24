/* The model table tools/gen_models.py writes from models.json (build/gen/models.cpp): every upstream module the rack
 * can load, which section it goes in and its display name. Guitarix modules describe their own controls at run time
 * (register_params); GxPlugins pedals' controls come from their TTL files, read by the generator. */
#pragma once
#include <stdint.h>
#include "lv2.h"

struct PluginDef;

enum { MK_GX, MK_LV2, MK_BUILTIN };
enum { BI_GATE, BI_CAB };   /* modules written here (rack.cpp), not upstream */

struct lv2_port_t {
    int index;
    const char *sym, *name;
    float def, lo, hi;
    unsigned char toggled, integer, logarithmic;
    int nlabels;
    const char *const *labels;   /* scale point labels, for values lo, lo+1, ... */
};

struct name_t { const char *id, *label; };

struct model_t {
    const char *id, *name;
    int section, kind;
    PluginDef *(*gx)();                              /* MK_GX */
    const LV2_Descriptor *(*lv2)(uint32_t);           /* MK_LV2 */
    const lv2_port_t *ports; int nports;              /* MK_LV2: control inputs shown, in order */
    int ain[2], aout[2];                              /* MK_LV2: audio port indices, [1] = -1 when mono */
    const char *const *show;                          /* MK_GX: parameter ids shown, in order (NULL: all) */
    const name_t *names;                              /* MK_GX: labels for ids (upstream leaves some empty), {0,0} ends */
    int builtin;                                      /* MK_BUILTIN */
    const char *arg;                                  /* MK_BUILTIN: e.g. the cabinet's impulse response */
};

struct section_t {
    const char *key, *name;
    int stereo;      /* runs after the mono part of the chain: stereo models stay stereo, mono ones run per side */
    int nknobs;
    int slot;        /* first slot index of this section */
    int nslots;      /* a section with two slots (two pedals) */
};

extern const model_t MODELS[];
extern const int NMODELS;
extern const section_t SECTIONS[];
extern const int NSECTIONS;
extern const char *const SLOT_KEYS[];    /* per slot: parameter key prefix */
extern const char *const SLOT_NAMES[];
extern const int SLOT_SECTION[];
extern const int NSLOTS;
extern const int SLOT_DEFAULT_MODEL[];   /* index into the section's models (models.json "defaults") */
extern const int SLOT_DEFAULT_ON[];
extern const int SLOT_TAB[];             /* per slot: the skin page it is on (models.json "tabs") */
extern const char *const TAB_KEYS[];     /* per page: "browse_<key>" is On while that page's model browser is open */
extern const int NTABS;
extern const int BROWSE_ROWS;            /* model names per browser page ("brow_1".."brow_<n>") */
