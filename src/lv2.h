/* The LV2 core plugin interface (https://lv2plug.in/ns/lv2core), written from the specification: the
 * part GxPlugins' wrappers use (they include <lv2.h> and need only the descriptor, handle and feature types).
 * Nothing is exported: each pedal's lv2_descriptor is renamed and kept inside the plugin (tools/gen_models.py). */
#ifndef LV2_H_INCLUDED
#define LV2_H_INCLUDED
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *LV2_Handle;

typedef struct {
    const char *URI;
    void *data;
} LV2_Feature;

typedef struct LV2_Descriptor {
    const char *URI;
    LV2_Handle (*instantiate)(const struct LV2_Descriptor *descriptor, double sample_rate, const char *bundle_path,
                              const LV2_Feature *const *features);
    void (*connect_port)(LV2_Handle instance, uint32_t port, void *data_location);
    void (*activate)(LV2_Handle instance);
    void (*run)(LV2_Handle instance, uint32_t sample_count);
    void (*deactivate)(LV2_Handle instance);
    void (*cleanup)(LV2_Handle instance);
    const void *(*extension_data)(const char *uri);
} LV2_Descriptor;

#define LV2_SYMBOL_EXPORT

#ifdef __cplusplus
}
#endif
#endif
