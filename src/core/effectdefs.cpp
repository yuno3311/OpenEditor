#include "effectdefs.h"

static ParamDef num(const char *id, const char *name, double def, double smin, double smax, double step = 1, const char *unit = "")
{
    ParamDef p; p.id = id; p.name = name; p.def = def; p.softMin = smin; p.softMax = smax; p.step = step; p.unit = unit;
    return p;
}
static ParamDef lim(ParamDef p, double mn, double mx) { p.min = mn; p.max = mx; return p; }
static ParamDef pct(const char *id, const char *name, double def, double smin = 0, double smax = 100)
{
    ParamDef p = num(id, name, def, smin, smax, 1, "%"); p.kind = ParamKind::Percent; return p;
}
static ParamDef ang(const char *id, const char *name, double def)
{
    ParamDef p = num(id, name, def, -360, 360, 1, "\xC2\xB0"); p.kind = ParamKind::Angle; return p;
}
static ParamDef tog(const char *id, const char *name, bool def)
{
    ParamDef p; p.id = id; p.name = name; p.kind = ParamKind::Toggle; p.def = def; p.softMin = 0; p.softMax = 1; p.animatable = false;
    return p;
}
static ParamDef cho(const char *id, const char *name, int def, QStringList c)
{
    ParamDef p; p.id = id; p.name = name; p.kind = ParamKind::Choice; p.def = def; p.choices = c; p.animatable = false;
    return p;
}
static ParamDef col(const char *id, const char *name, QRgb def)
{
    ParamDef p; p.id = id; p.name = name; p.kind = ParamKind::Color; p.def = double(def & 0xFFFFFF); p.animatable = false;
    return p;
}
static ParamDef db(const char *id, const char *name, double def, double smin, double smax)
{
    ParamDef p = num(id, name, def, smin, smax, .1, " dB"); p.kind = ParamKind::Db; return p;
}
static ParamDef ptx(const char *id, const char *name, double def)
{
    ParamDef p = num(id, name, def, -4000, 8000, 1); p.kind = ParamKind::PointX; return p;
}
static ParamDef pty(const char *id, double def)
{
    ParamDef p = num(id, "", def, -4000, 8000, 1); p.kind = ParamKind::PointY; return p;
}

static QVector<EffectDef> build()
{
    QVector<EffectDef> v;
    auto add = [&](const char *id, const char *name, const char *cat, QVector<ParamDef> ps, bool audio = false, bool intr = false) {
        EffectDef d; d.id = id; d.name = name; d.category = cat; d.params = ps; d.audio = audio; d.intrinsic = intr;
        v << d;
    };
    // --- Intrinsic
    add("motion", "Motion", "", {
        ptx("posX", "Position", 960), pty("posY", 540),
        lim(pct("scale", "Scale", 100, 0, 600), 0, 10000),
        lim(pct("scaleW", "Scale Width", 100, 0, 600), 0, 10000),
        tog("uniform", "Uniform Scale", true),
        ang("rot", "Rotation", 0),
        ptx("ancX", "Anchor Point", 960), pty("ancY", 540),
    }, false, true);
    add("opacity", "Opacity", "", {
        lim(pct("opacity", "Opacity", 100), 0, 100),
        cho("blend", "Blend Mode", 0, {"Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "Color Dodge",
                                        "Color Burn", "Hard Light", "Soft Light", "Difference", "Exclusion", "Add"}),
    }, false, true);
    add("volume", "Volume", "", {tog("bypass", "Bypass", false), db("level", "Level", 0, -60, 15)}, true, true);
    add("panner", "Panner", "", {num("balance", "Balance", 0, -100, 100)}, true, true);

    // --- Video: Adjust
    add("brightcontrast", "Brightness & Contrast", "Adjust", {num("bright", "Brightness", 0, -100, 100), num("contrast", "Contrast", 0, -100, 100)});
    add("levels", "Levels", "Adjust", {num("inB", "Input Black", 0, 0, 255), num("inW", "Input White", 255, 0, 255),
        num("gamma", "Gamma", 1, .1, 5, .01), num("outB", "Output Black", 0, 0, 255), num("outW", "Output White", 255, 0, 255)});
    add("shadowhigh", "Shadow/Highlight", "Adjust", {num("shadow", "Shadow Amount", 0, 0, 100), num("high", "Highlight Amount", 0, 0, 100)});
    add("exposure", "Exposure", "Adjust", {num("ev", "Exposure", 0, -5, 5, .01), num("offset", "Offset", 0, -.5, .5, .01), num("gamma", "Gamma Correction", 1, .1, 3, .01)});
    add("tint", "Tint", "Adjust", {col("black", "Map Black To", 0x000000), col("white", "Map White To", 0xFFFFFF), pct("amount", "Amount to Tint", 100)});
    // --- Video: Color
    add("colorgrade", "Color Grade", "Color Correction", {
        num("temp", "Temperature", 0, -100, 100), num("tint", "Tint", 0, -100, 100),
        num("exposure", "Exposure", 0, -5, 5, .01), num("contrast", "Contrast", 0, -100, 100),
        num("highlights", "Highlights", 0, -100, 100), num("shadows", "Shadows", 0, -100, 100),
        num("whites", "Whites", 0, -100, 100), num("blacks", "Blacks", 0, -100, 100),
        num("sat", "Saturation", 100, 0, 200), num("vibrance", "Vibrance", 0, -100, 100),
        num("faded", "Faded Film", 0, 0, 100), num("sharpen", "Sharpen", 0, 0, 100),
        num("liftY", "Shadow Level", 0, -1, 1, .01), num("liftR", "Shadow R", 0, -1, 1, .01), num("liftG", "Shadow G", 0, -1, 1, .01), num("liftB", "Shadow B", 0, -1, 1, .01),
        num("gammaY", "Midtone Level", 0, -1, 1, .01), num("gammaR", "Midtone R", 0, -1, 1, .01), num("gammaG", "Midtone G", 0, -1, 1, .01), num("gammaB", "Midtone B", 0, -1, 1, .01),
        num("gainY", "Highlight Level", 0, -1, 1, .01), num("gainR", "Highlight R", 0, -1, 1, .01), num("gainG", "Highlight G", 0, -1, 1, .01), num("gainB", "Highlight B", 0, -1, 1, .01),
        num("vigAmount", "Vignette Amount", 0, -5, 5, .01), num("vigMid", "Vignette Midpoint", 50, 0, 100),
        num("vigRound", "Vignette Roundness", 0, -100, 100), num("vigFeather", "Vignette Feather", 50, 0, 100),
    });
    add("hsl", "Hue/Saturation", "Color Correction", {ang("hue", "Hue", 0), num("sat", "Saturation", 0, -100, 100), num("light", "Lightness", 0, -100, 100)});
    add("colorbalance", "Color Balance", "Color Correction", {
        num("sr", "Shadow Red", 0, -100, 100), num("sg", "Shadow Green", 0, -100, 100), num("sb", "Shadow Blue", 0, -100, 100),
        num("mr", "Midtone Red", 0, -100, 100), num("mg", "Midtone Green", 0, -100, 100), num("mb", "Midtone Blue", 0, -100, 100),
        num("hr", "Highlight Red", 0, -100, 100), num("hg", "Highlight Green", 0, -100, 100), num("hb", "Highlight Blue", 0, -100, 100)});
    add("blackwhite", "Black & White", "Color Correction", {pct("amount", "Amount", 100)});
    add("sepia", "Sepia", "Color Correction", {pct("amount", "Amount", 100)});
    add("invert", "Invert", "Channel", {pct("amount", "Blend With Original", 0)});
    add("timeremap", "Time Remapping", "Time", {lim(pct("speed", "Speed", 100, 0, 400), 0, 2000)});
    // --- Blur
    add("gaussblur", "Gaussian Blur", "Blur & Sharpen", {lim(num("radius", "Blurriness", 10, 0, 200), 0, 1000)});
    add("dirblur", "Directional Blur", "Blur & Sharpen", {ang("angle", "Direction", 0), lim(num("len", "Blur Length", 20, 0, 200), 0, 500)});
    add("sharpen", "Sharpen", "Blur & Sharpen", {num("amount", "Sharpen Amount", 50, 0, 300)});
    // --- Stylize
    add("glow", "Glow", "Stylize", {num("thresh", "Threshold", 70, 0, 100), lim(num("radius", "Radius", 20, 0, 150), 0, 500), num("amount", "Intensity", 60, 0, 200)});
    add("mosaic", "Mosaic", "Stylize", {lim(num("h", "Horizontal Blocks", 40, 1, 400), 1, 4000), lim(num("v", "Vertical Blocks", 24, 1, 400), 1, 4000)});
    add("posterize", "Posterize", "Stylize", {lim(num("levels", "Level", 6, 2, 32), 2, 256)});
    add("noise", "Noise", "Noise & Grain", {pct("amount", "Amount of Noise", 15), tog("color", "Use Color Noise", false)});
    add("vignette", "Vignette", "Stylize", {num("amount", "Amount", 50, -100, 100), num("mid", "Midpoint", 50, 0, 100), num("feather", "Feather", 50, 0, 100)});
    add("dropshadow", "Drop Shadow", "Perspective", {col("color", "Shadow Color", 0), pct("opacity", "Opacity", 50), ang("dir", "Direction", 135),
        num("dist", "Distance", 8, 0, 120), num("soft", "Softness", 8, 0, 100)});
    // --- Distort
    add("crop", "Crop", "Transform", {pct("left", "Left", 0), pct("top", "Top", 0), pct("right", "Right", 0), pct("bottom", "Bottom", 0), pct("feather", "Edge Feather", 0, 0, 100)});
    add("hflip", "Horizontal Flip", "Transform", {});
    add("vflip", "Vertical Flip", "Transform", {});
    add("mirror", "Mirror", "Distort", {cho("axis", "Reflection", 0, {"Left to Right", "Right to Left", "Top to Bottom", "Bottom to Top"})});
    add("wave", "Wave Warp", "Distort", {num("height", "Wave Height", 20, 0, 200), num("width", "Wave Width", 80, 1, 500), num("speed", "Wave Speed", 1, 0, 10, .1), ang("dir", "Direction", 0)});
    add("lens", "Lens Distortion", "Distort", {num("curv", "Curvature", -30, -100, 100)});
    // --- Keying
    add("chromakey", "Chroma Key", "Keying", {col("key", "Key Color", 0x00C800), num("tol", "Tolerance", 40, 0, 100), num("soft", "Softness", 15, 0, 100),
        num("spill", "Spill Suppression", 60, 0, 100), num("choke", "Choke", 0, -100, 100)});
    add("colorkey", "Color Key", "Keying", {col("key", "Key Color", 0x0000FF), num("tol", "Color Tolerance", 30, 0, 100), num("feather", "Edge Feather", 10, 0, 100)});
    // --- New in 2.0
    add("stabilize", "Warp Stabilizer", "Distort", {num("smooth", "Smoothness", 50, 0, 100, 1, "%"), tog("autoscale", "Auto-scale", true),
        lim(pct("extra", "Additional Scale", 100, 100, 150), 100, 300)});
    add("leavecolor", "Leave Color", "Color Correction", {col("color", "Color To Leave", 0xD03030), num("tol", "Tolerance", 25, 0, 100), num("soft", "Edge Softness", 10, 0, 100),
        pct("amount", "Amount to Decolor", 100)});
    add("findedges", "Find Edges", "Stylize", {tog("invert", "Invert", true), pct("blend", "Blend With Original", 0)});
    add("emboss", "Emboss", "Stylize", {ang("dir", "Direction", 45), num("relief", "Relief", 3, 0, 20, .1), pct("blend", "Blend With Original", 0)});
    add("twirl", "Twirl", "Distort", {ang("angle", "Angle", 90), num("radius", "Twirl Radius", 40, 1, 100, 1, "%")});
    add("spherize", "Spherize", "Distort", {num("amount", "Amount", 50, -100, 100), num("radius", "Radius", 50, 1, 100, 1, "%")});
    add("replicate", "Replicate", "Stylize", {lim(num("count", "Count", 2, 2, 16), 2, 16)});
    add("linearwipe", "Linear Wipe", "Transition", {pct("done", "Transition Completion", 0), ang("angle", "Wipe Angle", 90), num("feather", "Feather", 0, 0, 500)});
    add("ramp", "Gradient Ramp", "Generate", {col("start", "Start Color", 0x000000), col("end", "End Color", 0xFFFFFF), cho("shape", "Ramp Shape", 0, {"Linear", "Radial"}),
        ang("angle", "Angle", 180), pct("blend", "Blend With Original", 0)});
    add("strobe", "Strobe Light", "Stylize", {col("color", "Strobe Color", 0xFFFFFF), pct("blend", "Blend With Original", 0),
        num("period", "Strobe Period", .5, .05, 5, .01, " s"), num("dur", "Strobe Duration", .1, .01, 5, .01, " s")});
    add("lumakey", "Luma Key", "Keying", {num("thresh", "Threshold", 20, 0, 100), num("cutoff", "Softness", 10, 0, 100), tog("invert", "Invert", false)});
    // --- Audio
    add("a_gain", "Amplify", "Amplitude and Compression", {db("gain", "Gain", 0, -60, 30)}, true);
    add("a_comp", "Dynamics Compressor", "Amplitude and Compression", {db("thresh", "Threshold", -18, -60, 0), num("ratio", "Ratio", 4, 1, 20, .1),
        num("attack", "Attack", 10, 1, 200, 1, " ms"), num("release", "Release", 120, 10, 1000, 1, " ms"), db("makeup", "Make-up Gain", 0, 0, 30)}, true);
    add("a_limit", "Hard Limiter", "Amplitude and Compression", {db("ceil", "Maximum Amplitude", -1, -30, 0), db("input", "Input Boost", 0, 0, 30)}, true);
    add("a_hp", "High-Pass", "Filter and EQ", {lim(num("freq", "Cutoff", 120, 20, 2000, 1, " Hz"), 20, 20000)}, true);
    add("a_lp", "Low-Pass", "Filter and EQ", {lim(num("freq", "Cutoff", 8000, 200, 20000, 1, " Hz"), 20, 22000)}, true);
    add("a_eq", "3-Band Parametric EQ", "Filter and EQ", {db("low", "Low Shelf", 0, -18, 18), num("lowF", "Low Freq", 200, 40, 800, 1, " Hz"),
        db("mid", "Mid Gain", 0, -18, 18), num("midF", "Mid Freq", 1000, 200, 8000, 1, " Hz"), num("midQ", "Mid Q", 1, .1, 10, .1),
        db("high", "High Shelf", 0, -18, 18), num("highF", "High Freq", 6000, 1500, 16000, 1, " Hz")}, true);
    add("a_delay", "Delay", "Delay and Echo", {num("time", "Delay Time", 300, 1, 2000, 1, " ms"), pct("fb", "Feedback", 30), pct("mix", "Mix", 30)}, true);
    add("a_reverb", "Studio Reverb", "Reverb", {pct("size", "Room Size", 50), pct("damp", "Damping", 50), pct("mix", "Wet", 25)}, true);
    add("a_enhance", "Enhance Speech", "Essential Sound", {pct("amount", "Amount", 70), tog("level", "Level Loudness", true)}, true);
    add("a_denoise", "DeNoise", "Noise Reduction/Restoration", {num("reduce", "Reduction", 18, 0, 40, .1, " dB"), num("sens", "Sensitivity", 2, 1, 4, .1)}, true);
    add("a_deess", "DeEsser", "Noise Reduction/Restoration", {db("thresh", "Threshold", -30, -60, 0), num("freq", "Frequency", 6000, 3000, 12000, 10, " Hz"),
        db("cut", "Maximum Reduction", 12, 0, 30)}, true);
    add("a_dehum", "DeHum", "Noise Reduction/Restoration", {cho("freq", "Frequency", 1, {"50 Hz", "60 Hz"}), lim(num("harm", "Harmonics", 4, 1, 8), 1, 8),
        num("q", "Notch Width (Q)", 30, 5, 100)}, true);
    add("a_gate", "Noise Gate", "Amplitude and Compression", {db("thresh", "Threshold", -45, -80, 0), num("attack", "Attack", 2, .1, 50, .1, " ms"),
        num("hold", "Hold", 50, 0, 500, 1, " ms"), num("release", "Release", 150, 5, 2000, 1, " ms"), db("range", "Range", -60, -96, 0)}, true);
    add("a_chorus", "Chorus/Flanger", "Modulation", {cho("mode", "Mode", 0, {"Chorus", "Flanger"}), num("rate", "Rate", 1.2, .05, 10, .01, " Hz"),
        pct("depth", "Depth", 50), pct("mix", "Mix", 50), pct("fb", "Feedback", 30)}, true);
    add("a_invert", "Invert Polarity", "Special", {}, true);
    add("a_swap", "Swap Channels", "Special", {}, true);
    add("a_mono", "Fill Stereo From Left", "Special", {}, true);
    return v;
}

namespace EffectRegistry {
static QVector<EffectDef> &mutableAll()
{
    static QVector<EffectDef> v = build();
    return v;
}
const QVector<EffectDef> &all() { return mutableAll(); }
void add(const EffectDef &d) { mutableAll() << d; }
const EffectDef *find(const QString &id)
{
    for (auto &d : all()) if (d.id == id) return &d;
    return nullptr;
}
const QVector<TransitionDef> &transitions()
{
    static const QVector<TransitionDef> v = {
        {"xdissolve", "Cross Dissolve", "Dissolve"}, {"filmdissolve", "Film Dissolve", "Dissolve"},
        {"additive", "Additive Dissolve", "Dissolve"}, {"dipblack", "Dip to Black", "Dissolve"},
        {"dipwhite", "Dip to White", "Dissolve"}, {"wipeL", "Wipe", "Wipe"}, {"wipeD", "Wipe Down", "Wipe"},
        {"barn", "Barn Doors", "Wipe"}, {"clock", "Clock Wipe", "Wipe"}, {"irisround", "Iris Round", "Iris"},
        {"irisbox", "Iris Box", "Iris"}, {"irisdiamond", "Iris Diamond", "Iris"}, {"push", "Push", "Slide"},
        {"slide", "Slide", "Slide"}, {"zoom", "Cross Zoom", "Zoom"},
        {"a_cpower", "Constant Power", "Crossfade", true}, {"a_cgain", "Constant Gain", "Crossfade", true},
        {"a_expo", "Exponential Fade", "Crossfade", true}};
    return v;
}
const TransitionDef *findTransition(const QString &id)
{
    for (auto &d : transitions()) if (d.id == id) return &d;
    return nullptr;
}
Effect make(const QString &id)
{
    Effect e;
    e.type = id;
    if (auto d = find(id))
        for (auto &pd : d->params) {
            Param p; p.id = pd.id; p.value = pd.def;
            e.params << p;
        }
    return e;
}
void defaultsFor(Clip &c)
{
    if (c.kind == MediaKind::Audio) { c.fx << make("volume") << make("panner"); }
    else { c.fx << make("motion") << make("opacity"); }
}
}
