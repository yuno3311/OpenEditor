/* Open Editor plugin API, version 1. A plugin is a native DLL placed in the "oe-plugins" folder next
 * to OpenEditor.exe or in %APPDATA%/Open Editor/Open Editor/plugins. Plugins run inside the editor
 * process, so install only ones you trust. See examples/plugin_duotone for a complete plugin.
 *
 * Video process: pixels are BGRA8 premultiplied, process in place. Called from render threads, keep
 * it re-entrant. Params arrive in declaration order, already evaluated at the current frame.
 * Audio process: interleaved stereo float at 48 kHz. *state starts NULL for each clip instance. */
#ifndef OE_PLUGIN_H
#define OE_PLUGIN_H
#ifdef __cplusplus
extern "C" {
#endif
#define OE_PLUGIN_API_VERSION 1
#ifdef _WIN32
#define OE_EXPORT __declspec(dllexport)
#else
#define OE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct OeParamDef {
    const char *id, *name;
    double def, min, max; /* min/max are hard limits and the slider range */
} OeParamDef;

typedef struct OeEffectDef {
    const char *id;       /* unique; the editor stores it as "plugin.<id>" */
    const char *name;
    const char *category;
    int audio;            /* 0 video, 1 audio */
    int paramCount;
    const OeParamDef *params;
} OeEffectDef;

OE_EXPORT int oe_plugin_api_version(void);
OE_EXPORT int oe_plugin_effect_count(void);
OE_EXPORT const OeEffectDef *oe_plugin_effect(int index);
OE_EXPORT void oe_plugin_process_video(const char *id, unsigned char *pixels, int width, int height,
                                       int stride, const double *params, double seconds);
OE_EXPORT void oe_plugin_process_audio(const char *id, float *stereo, int frames, const double *params, void **state);
OE_EXPORT void oe_plugin_free_state(const char *id, void *state);
#ifdef __cplusplus
}
#endif
#endif
