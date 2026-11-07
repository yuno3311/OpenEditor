/* Example Open Editor plugin: one video effect and one audio effect. */
#include "oe_plugin.h"
#include <stdlib.h>

static const OeParamDef kDuotone[] = {
    {"shadowHue", "Shadow Blue", 180, 0, 255},
    {"amount", "Amount", 100, 0, 100},
};
static const OeParamDef kTremolo[] = {
    {"rate", "Rate (Hz)", 6, 0.1, 30},
    {"depth", "Depth", 60, 0, 100},
};
static const OeEffectDef kEffects[] = {
    {"example.duotone", "Duotone (Example Plugin)", "Plugins", 0, 2, kDuotone},
    {"example.tremolo", "Tremolo (Example Plugin)", "Plugins", 1, 2, kTremolo},
};

OE_EXPORT int oe_plugin_api_version(void) { return OE_PLUGIN_API_VERSION; }
OE_EXPORT int oe_plugin_effect_count(void) { return 2; }
OE_EXPORT const OeEffectDef *oe_plugin_effect(int i) { return i >= 0 && i < 2 ? &kEffects[i] : 0; }

OE_EXPORT void oe_plugin_process_video(const char *id, unsigned char *px, int w, int h, int stride, const double *p, double seconds)
{
    (void)id; (void)seconds;
    double a = p[1] / 100.0;
    for (int y = 0; y < h; ++y) {
        unsigned char *l = px + y * stride;
        for (int x = 0; x < w; ++x, l += 4) {
            double Y = (l[2] * .2126 + l[1] * .7152 + l[0] * .0722) / 255.0;
            double b = p[0] * (1 - Y) + 255 * Y * .55, g = 40 * (1 - Y) + 230 * Y, r = 20 * (1 - Y) + 255 * Y;
            double m = l[3] / 255.0;
            l[0] = (unsigned char)(l[0] + (b * m - l[0]) * a);
            l[1] = (unsigned char)(l[1] + (g * m - l[1]) * a);
            l[2] = (unsigned char)(l[2] + (r * m - l[2]) * a);
        }
    }
}

OE_EXPORT void oe_plugin_process_audio(const char *id, float *s, int frames, const double *p, void **state)
{
    (void)id;
    if (!*state) *state = calloc(1, sizeof(double));
    double *phase = (double *)*state, depth = p[1] / 100.0, step = p[0] / 48000.0;
    for (int i = 0; i < frames; ++i) {
        double f = *phase - (long)*phase;
        double tri = f < .5 ? f * 2 : 2 - f * 2;
        float g = (float)(1 - depth * tri);
        s[i * 2] *= g; s[i * 2 + 1] *= g;
        *phase += step;
    }
}
OE_EXPORT void oe_plugin_free_state(const char *id, void *state) { (void)id; free(state); }
