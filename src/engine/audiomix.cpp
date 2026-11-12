#include "audiomix.h"
#include "../core/mediacache.h"
#include "audiodsp.h"
#include "pluginhost.h"
#include <QtMath>
#include <cmath>
#include <cstring>

static constexpr double kRate = MediaCache::kRate;
static inline float dbToGain(double db) { return db <= -96 ? 0.f : float(std::pow(10.0, db / 20.0)); }

static void setPeaking(Biquad &q, double f, double gainDb, double Q)
{
    double A = std::pow(10, gainDb / 40), w = 2 * M_PI * f / kRate, al = std::sin(w) / (2 * Q), c = std::cos(w);
    double a0 = 1 + al / A;
    q.b0 = (1 + al * A) / a0; q.b1 = -2 * c / a0; q.b2 = (1 - al * A) / a0; q.a1 = -2 * c / a0; q.a2 = (1 - al / A) / a0;
}
static void setShelf(Biquad &q, double f, double gainDb, bool high)
{
    double A = std::pow(10, gainDb / 40), w = 2 * M_PI * f / kRate, c = std::cos(w), al = std::sin(w) / 2 * std::sqrt(2), sq = 2 * std::sqrt(A) * al;
    double a0, a1, a2, b0, b1, b2;
    if (high) {
        b0 = A * ((A + 1) + (A - 1) * c + sq); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sq);
        a0 = (A + 1) - (A - 1) * c + sq; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sq;
    } else {
        b0 = A * ((A + 1) - (A - 1) * c + sq); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sq);
        a0 = (A + 1) + (A - 1) * c + sq; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sq;
    }
    q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0; q.a1 = a1 / a0; q.a2 = a2 / a0;
}
static void setPass(Biquad &q, double f, bool high)
{
    double w = 2 * M_PI * qBound(10.0, f, 23000.0) / kRate, c = std::cos(w), al = std::sin(w) / (2 * 0.7071), a0 = 1 + al;
    if (high) { q.b0 = (1 + c) / 2 / a0; q.b1 = -(1 + c) / a0; q.b2 = q.b0; }
    else { q.b0 = (1 - c) / 2 / a0; q.b1 = (1 - c) / a0; q.b2 = q.b0; }
    q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
}

static void setNotch(Biquad &q, double f, double Q)
{
    double w = 2 * M_PI * qBound(10.0, f, 23000.0) / kRate, c = std::cos(w), al = std::sin(w) / (2 * Q), a0 = 1 + al;
    q.b0 = 1 / a0; q.b1 = -2 * c / a0; q.b2 = 1 / a0; q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
}
static inline float coef(double ms) { return float(std::exp(-1.0 / (qMax(.01, ms) * .001 * kRate))); }

// Noise reduction for both channels, created on first use so idle effects cost nothing.
static void denoise(FxState &s, float *b, int n, float reductionDb, float sensitivity)
{
    for (auto &d : s.nr) if (!d) d = std::make_shared<SpectralDenoiser>();
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c) b[i * 2 + c] = s.nr[c]->process(b[i * 2 + c], reductionDb, sensitivity);
}

// Splits off the sibilance band and turns only that down while it is loud. The band is the signal
// minus its low-passed self, so subtracting it is exact and nothing is phase-smeared.
static void deEss(FxState &s, Biquad &low, float *b, int n, double freq, double threshDb, double maxCutDb)
{
    setPass(low, freq, false);
    const float at = coef(1), rl = coef(60);
    for (int i = 0; i < n; ++i) {
        float hi[2], lv = 0;
        for (int c = 0; c < 2; ++c) { hi[c] = b[i * 2 + c] - low.run(c, b[i * 2 + c]); lv = qMax(lv, std::fabs(hi[c])); }
        s.env2 = lv > s.env2 ? at * s.env2 + (1 - at) * lv : rl * s.env2 + (1 - rl) * lv;
        double over = 20 * std::log10(qMax(1e-6f, s.env2)) - threshDb;
        float g = over > 0 ? dbToGain(-qMin(maxCutDb, over * .75)) : 1.f;
        for (int c = 0; c < 2; ++c) b[i * 2 + c] -= (1 - g) * hi[c];
    }
}

static void compress(FxState &s, float *b, int n, double th, double ratio, double atMs, double rlMs, double makeupDb)
{
    const float at = coef(atMs), rl = coef(rlMs), mk = dbToGain(makeupDb);
    for (int i = 0; i < n; ++i) {
        float lv = qMax(std::fabs(b[i * 2]), std::fabs(b[i * 2 + 1]));
        s.env = lv > s.env ? at * s.env + (1 - at) * lv : rl * s.env + (1 - rl) * lv;
        double over = 20 * std::log10(qMax(1e-6f, s.env)) - th;
        float g = (over > 0 ? dbToGain(-over * (1 - 1 / ratio)) : 1.f) * mk;
        b[i * 2] *= g; b[i * 2 + 1] *= g;
    }
}

void AudioMixer::setState(std::shared_ptr<const ProjectState> st, int sid)
{
    QMutexLocker l(&mx);
    state = std::move(st);
    seqId = sid;
}
void AudioMixer::reset()
{
    QMutexLocker l(&mx);
    fx.clear();
    stretch.clear();
}

// ---------------------------------------------------------------------- pitch-preserving speed
// WSOLA: the output is built from 40 ms Hann grains every 20 ms. Grain k plays source taken from
// where the clip's speed puts it, shifted by up to 10 ms so its waveform lines up with how the
// previous grain would have continued. That keeps the voice at its own pitch at any speed.
// Every 32nd grain is an anchor with no shift, so a grain's shift depends only on its index and
// any block of output, played or exported, seeked to or not, comes out the same.
namespace {
const int kGrain = 1920, kHop = 960, kSearch = 480, kAnchor = 32;

struct Hann {
    float w[kGrain];
    Hann() { for (int i = 0; i < kGrain; ++i) w[i] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / kGrain)); }
};
const Hann &hann() { static const Hann h; return h; }

inline float mono(const PcmData &p, qint64 i)
{
    return i >= 0 && i < p.frames ? (p.data[i * 2] + p.data[i * 2 + 1]) * (0.5f / 32768.f) : 0.f;
}
inline qint64 floorDiv(qint64 a, qint64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline qint64 grainSource(const StretchCache &s, qint64 k) { return qint64(std::llround(s.base + double(k) * kHop * s.speed)); }

double overlap(const PcmData &p, qint64 a, qint64 b, int step)
{
    double sum = 0;
    for (int j = -kGrain / 2; j < 0; j += step) sum += double(mono(p, a + j)) * mono(p, b + j);
    return sum;
}

int grainShift(const PcmData &p, StretchCache &s, qint64 k)
{
    if (floorDiv(k, kAnchor) * kAnchor == k) return 0;
    if (auto it = s.delta.constFind(k); it != s.delta.constEnd()) return *it;
    // Walk forward from the nearest anchor, or from the furthest grain already known.
    qint64 i = floorDiv(k, kAnchor) * kAnchor;
    while (i + 1 < k && s.delta.contains(i + 1)) ++i;
    int prev = i % kAnchor == 0 ? 0 : s.delta.value(i);
    for (qint64 g = i + 1; g <= k; ++g) {
        const qint64 want = grainSource(s, g - 1) + prev + kHop, at = grainSource(s, g);
        int best = 0;
        double bestV = -1e30;
        for (int d = -kSearch; d <= kSearch; d += 4) {
            const double v = overlap(p, at + d, want, 2);
            if (v > bestV) { bestV = v; best = d; }
        }
        // Refine around the coarse match at full resolution.
        const int coarse = best;
        bestV = -1e30;
        for (int d = qMax(-kSearch, coarse - 3); d <= qMin(kSearch, coarse + 3); ++d) {
            const double v = overlap(p, at + d, want, 1);
            if (v > bestV) { bestV = v; best = d; }
        }
        if (g % kAnchor != 0) s.delta.insert(g, best);
        prev = g % kAnchor == 0 ? 0 : best;
    }
    if (s.delta.size() > 400000) { const int keep = s.delta.value(k); s.delta.clear(); s.delta.insert(k, keep); }
    return s.delta.value(k);
}
} // namespace

// Adds n stereo samples of the stretched clip, starting at clip-local output sample local0.
static void stretchInto(const PcmData &p, StretchCache &s, qint64 local0, int n, float *out)
{
    const Hann &h = hann();
    const qint64 kMin = floorDiv(local0 - kGrain / 2, kHop), kMax = floorDiv(local0 + n + kGrain / 2, kHop);
    for (qint64 k = kMin; k <= kMax; ++k) {
        const qint64 centre = k * kHop, src = grainSource(s, k) + grainShift(p, s, k);
        const qint64 m0 = qMax(local0, centre - kGrain / 2), m1 = qMin(local0 + n, centre + kGrain / 2);
        for (qint64 m = m0; m < m1; ++m) {
            const qint64 j = m - centre, si = src + j;
            if (si < 0 || si >= p.frames) continue;
            const float w = h.w[j + kGrain / 2] / 32768.f;
            out[(m - local0) * 2] += p.data[si * 2] * w;
            out[(m - local0) * 2 + 1] += p.data[si * 2 + 1] * w;
        }
    }
}

void AudioMixer::runFx(const Effect &e, quint64 key, float *b, int n, Frame kt)
{
    const QString &t = e.type;
    auto V = [&](const char *id, double d = 0) { return e.val(id, kt, d); };
    FxState &s = fx[key];
    if (t.startsWith(QLatin1String("plugin."))) { PluginHost::audio(e, b, n, kt, s.plugin); return; }
    if (runAudioFxV3(e, s, b, n, kt)) return;
    if (t == "a_gain") {
        float g = dbToGain(V("gain"));
        for (int i = 0; i < n * 2; ++i) b[i] *= g;
    } else if (t == "a_hp" || t == "a_lp") {
        setPass(s.b[0], V("freq", 1000), t == "a_hp");
        for (int i = 0; i < n; ++i) { b[i * 2] = s.b[0].run(0, b[i * 2]); b[i * 2 + 1] = s.b[0].run(1, b[i * 2 + 1]); }
    } else if (t == "a_eq") {
        setShelf(s.b[0], V("lowF", 200), V("low"), false);
        setPeaking(s.b[1], V("midF", 1000), V("mid"), qMax(.1, V("midQ", 1)));
        setShelf(s.b[2], V("highF", 6000), V("high"), true);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                float x = b[i * 2 + c];
                x = s.b[0].run(c, x); x = s.b[1].run(c, x); x = s.b[2].run(c, x);
                b[i * 2 + c] = x;
            }
    } else if (t == "a_comp") {
        double th = V("thresh", -18), ratio = qMax(1.0, V("ratio", 4));
        float at = std::exp(-1.0 / (V("attack", 10) * .001 * kRate)), rl = std::exp(-1.0 / (V("release", 120) * .001 * kRate));
        float mk = dbToGain(V("makeup"));
        for (int i = 0; i < n; ++i) {
            float lv = qMax(std::fabs(b[i * 2]), std::fabs(b[i * 2 + 1]));
            s.env = lv > s.env ? at * s.env + (1 - at) * lv : rl * s.env + (1 - rl) * lv;
            double db = 20 * std::log10(qMax(1e-6f, s.env));
            double over = db - th;
            float g = over > 0 ? dbToGain(-over * (1 - 1 / ratio)) : 1.f;
            b[i * 2] *= g * mk; b[i * 2 + 1] *= g * mk;
        }
    } else if (t == "a_limit") {
        float c = dbToGain(V("ceil", -1)), in = dbToGain(V("input"));
        float rl = std::exp(-1.0 / (.08 * kRate));
        for (int i = 0; i < n; ++i) {
            float l = b[i * 2] * in, r = b[i * 2 + 1] * in, pk = qMax(std::fabs(l), std::fabs(r));
            float need = pk > c ? pk / c : 1.f;
            s.env = qMax(need, 1 + (s.env < 1 ? 0.f : s.env - 1) * rl);
            b[i * 2] = l / s.env; b[i * 2 + 1] = r / s.env;
        }
    } else if (t == "a_delay") {
        int d = qBound(1, int(V("time", 300) * .001 * kRate), int(kRate * 2.5));
        if (s.delay.isEmpty()) s.delay.fill(0, int(kRate * 2.5) * 2);
        float fb = V("fb", 30) / 100, mix = V("mix", 30) / 100;
        int N = s.delay.size() / 2;
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                int rd = (s.dpos - d + N) % N;
                float dl = s.delay[rd * 2 + c], x = b[i * 2 + c];
                s.delay[s.dpos * 2 + c] = x + dl * fb;
                b[i * 2 + c] = x * (1 - mix) + dl * mix;
                if (c == 1) s.dpos = (s.dpos + 1) % N;
            }
    } else if (t == "a_reverb") {
        static const int lens[4] = {1557, 1617, 1491, 1422};
        float size = V("size", 50) / 100, damp = V("damp", 50) / 100, wet = V("mix", 25) / 100;
        float fb = .7f + size * .28f;
        for (int k = 0; k < 4; ++k)
            for (int c = 0; c < 2; ++c)
                if (s.comb[k][c].isEmpty()) s.comb[k][c].fill(0, lens[k] * 2 + c * 23);
        for (int i = 0; i < n; ++i) {
            float in[2] = {b[i * 2], b[i * 2 + 1]}, out[2] = {0, 0};
            for (int k = 0; k < 4; ++k)
                for (int c = 0; c < 2; ++c) {
                    auto &cb = s.comb[k][c];
                    int p = s.cpos[k] % cb.size();
                    float y = cb[p];
                    s.lp[k][c] = y * (1 - damp) + s.lp[k][c] * damp;
                    cb[p] = in[c] * .25f + s.lp[k][c] * fb;
                    out[c] += y;
                }
            for (int k = 0; k < 4; ++k) s.cpos[k]++;
            b[i * 2] = in[0] * (1 - wet) + out[0] * wet;
            b[i * 2 + 1] = in[1] * (1 - wet) + out[1] * wet;
        }
    } else if (t == "a_enhance") {
        // Dialogue clean-up chain: rumble filter, noise reduction, mud cut, presence and air,
        // de-essing, gentle compression, then a slow leveller towards -20 dB RMS and a ceiling.
        const double amt = qBound(0.0, V("amount", 70), 100.0) / 100;
        setPass(s.b[0], 80, true);
        setPeaking(s.b[1], 300, -4 * amt, 1);
        setPeaking(s.b[2], 3200, 4 * amt, .8);
        setShelf(s.b[3], 10000, 3 * amt, true);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) b[i * 2 + c] = s.b[0].run(c, b[i * 2 + c]);
        if (amt > 0) denoise(s, b, n, float(6 + 18 * amt), float(1.5 + amt));
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                float x = b[i * 2 + c];
                x = s.b[1].run(c, x); x = s.b[2].run(c, x); x = s.b[3].run(c, x);
                b[i * 2 + c] = x;
            }
        deEss(s, s.b[4], b, n, 6000, -28, 8 * amt);
        compress(s, b, n, -20, 1 + 1.5 * amt, 5, 120, 0);
        if (V("level", 1) > .5) {
            const float slow = coef(1500);
            for (int i = 0; i < n; ++i) {
                float lv = (b[i * 2] * b[i * 2] + b[i * 2 + 1] * b[i * 2 + 1]) * .5f;
                if (lv > 1e-6f) s.level = slow * s.level + (1 - slow) * lv; // learn only from sound, not pauses
                double rms = 10 * std::log10(qMax(1e-9f, s.level));
                float want = s.level > 0 ? dbToGain(qBound(-6.0, -20 - rms, 12.0)) : 1.f;
                s.gain = .9995f * s.gain + .0005f * want;
                b[i * 2] *= s.gain; b[i * 2 + 1] *= s.gain;
            }
        }
        for (int i = 0; i < n * 2; ++i) b[i] = qBound(-.891f, b[i], .891f); // -1 dBFS ceiling
    } else if (t == "a_denoise") {
        denoise(s, b, n, float(V("reduce", 18)), float(qBound(1.0, V("sens", 2), 4.0)));
    } else if (t == "a_deess") {
        deEss(s, s.b[0], b, n, V("freq", 6000), V("thresh", -30), V("cut", 12));
    } else if (t == "a_dehum") {
        const double f0 = V("freq") > .5 ? 60 : 50, Q = qMax(1.0, V("q", 30));
        const int harm = qBound(1, int(V("harm", 4)), 8);
        for (int h = 0; h < harm; ++h) setNotch(s.b[h], f0 * (h + 1), Q);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                float x = b[i * 2 + c];
                for (int h = 0; h < harm; ++h) x = s.b[h].run(c, x);
                b[i * 2 + c] = x;
            }
    } else if (t == "a_gate") {
        const double th = V("thresh", -45), range = qMin(0.0, V("range", -60));
        const double up = -range / qMax(1.0, V("attack", 2) * .001 * kRate), down = -range / qMax(1.0, V("release", 150) * .001 * kRate);
        const int holdN = int(V("hold", 50) * .001 * kRate);
        const float det = coef(1);
        for (int i = 0; i < n; ++i) {
            float lv = qMax(std::fabs(b[i * 2]), std::fabs(b[i * 2 + 1]));
            s.env = lv > s.env ? lv : det * s.env;
            if (20 * std::log10(qMax(1e-6f, s.env)) > th) s.hold = holdN;
            else if (s.hold > 0) --s.hold;
            s.level = s.hold > 0 ? float(qMin(0.0, s.level + up)) : float(qMax(range, s.level - down)); // gain in dB
            const float g = dbToGain(s.level);
            b[i * 2] *= g; b[i * 2 + 1] *= g;
        }
    } else if (t == "a_chorus") {
        const bool flanger = V("mode") > .5;
        const double base = flanger ? 2.5 : 18, depthMs = (flanger ? 2.0 : 8.0) * V("depth", 50) / 100;
        const float mix = V("mix", 50) / 100, fb = (flanger ? .7f : .2f) * V("fb", 30) / 100;
        const double rate = qMax(.01, V("rate", flanger ? .25 : 1.2));
        const int N = int(kRate * .05);
        if (s.delay.size() != N * 2) { s.delay.fill(0, N * 2); s.dpos = 0; }
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 2; ++c) {
                double lfo = std::sin(s.phase + c * M_PI_2);
                double dl = (base + depthMs * (lfo + 1) / 2) * .001 * kRate;
                double rp = s.dpos - dl;
                while (rp < 0) rp += N;
                int i0 = int(rp) % N, i1 = (i0 + 1) % N;
                float fr = float(rp - std::floor(rp));
                float wet = s.delay[i0 * 2 + c] * (1 - fr) + s.delay[i1 * 2 + c] * fr;
                float x = b[i * 2 + c];
                s.delay[s.dpos * 2 + c] = x + wet * fb;
                b[i * 2 + c] = x * (1 - mix * .5f) + wet * mix;
            }
            s.dpos = (s.dpos + 1) % N;
            s.phase += 2 * M_PI * rate / kRate;
            if (s.phase > 2 * M_PI) s.phase -= 2 * M_PI;
        }
    } else if (t == "a_invert") {
        for (int i = 0; i < n * 2; ++i) b[i] = -b[i];
    } else if (t == "a_swap") {
        for (int i = 0; i < n; ++i) std::swap(b[i * 2], b[i * 2 + 1]);
    } else if (t == "a_mono") {
        for (int i = 0; i < n; ++i) b[i * 2 + 1] = b[i * 2];
    }
}

void AudioMixer::mixClip(const ProjectState &st, const Sequence &q, const Track &t, const Clip &c, float *out, int frames, qint64 pos, int depth)
{
    const double fps = q.fps();
    const double spf = kRate / fps;
    qint64 cs = qint64(c.start * spf), ce = qint64(c.end() * spf);
    qint64 fadeInA = -1, fadeInB = -1, fadeOutA = -1, fadeOutB = -1;
    QString inType, outType;
    for (auto &x : t.transitions) {
        qint64 a = qint64(x.start() * spf), b = qint64(x.end() * spf);
        if (x.cut == c.start) { fadeInA = a; fadeInB = b; cs = qMin(cs, a); inType = x.type; }
        if (x.cut == c.end()) { fadeOutA = a; fadeOutB = b; ce = qMax(ce, b); outType = x.type; }
    }
    qint64 a = qMax(pos, cs), b = qMin(pos + frames, ce);
    if (b <= a || !c.enabled) return;
    const MediaItem *m = st.mediaById(c.mediaId);
    if (!m) return;
    int n = int(b - a);
    QVector<float> &buf = scratchClip;
    QVector<float> local(n * 2, 0.f);
    (void)buf;
    float *d = local.data();
    const qint64 clipStart = qint64(c.start * spf);

    if (m->kind == MediaKind::Nested) {
        const Sequence *inner = st.seqById(m->seqId);
        if (inner && inner->id != q.id && depth < 6)
            mixSeq(st, *inner, d, n, qint64(c.srcIn * kRate) + (a - clipStart), depth + 1, false, c.cam);
    } else {
        PcmData pcm = CACHE->pcm(m->path);
        if (!pcm.data) return;
        const double step = c.speed;
        const double base = c.srcIn * kRate;
        const double clipSamples = double(ce - clipStart);
        if (qFuzzyCompare(step, 1.0) && !c.reverse) {
            qint64 si = qint64(base + 0.5) + (a - clipStart);
            for (int i = 0; i < n; ++i, ++si) {
                if (si >= 0 && si < pcm.frames) {
                    d[i * 2] = pcm.data[si * 2] / 32768.f;
                    d[i * 2 + 1] = pcm.data[si * 2 + 1] / 32768.f;
                }
            }
        } else if (!c.reverse) {
            StretchCache &sc = stretch[c.id];
            if (sc.speed != step || sc.base != base) { sc = StretchCache(); sc.speed = step; sc.base = base; }
            stretchInto(pcm, sc, a - clipStart, n, d);
        } else {
            for (int i = 0; i < n; ++i) {
                double local2 = double(a - clipStart + i);
                if (c.reverse) local2 = clipSamples - 1 - local2;
                double sp = base + local2 * step;
                qint64 si = qint64(std::floor(sp));
                float fr = float(sp - si);
                if (si >= 0 && si + 1 < pcm.frames) {
                    d[i * 2] = (pcm.data[si * 2] * (1 - fr) + pcm.data[si * 2 + 2] * fr) / 32768.f;
                    d[i * 2 + 1] = (pcm.data[si * 2 + 1] * (1 - fr) + pcm.data[si * 2 + 3] * fr) / 32768.f;
                }
            }
        }
    }

    Frame kt = c.kt(Frame(a / spf));
    for (int i = 0; i < c.fx.size(); ++i) {
        const Effect &e = c.fx[i];
        if (!e.enabled || e.type == "volume" || e.type == "panner") continue;
        runFx(e, (quint64(uint(c.id)) << 8) | uint(i), d, n, kt);
    }

    const Effect *vol = c.effect("volume"), *pan = c.effect("panner");
    bool bypass = !vol || !vol->enabled || vol->val("bypass", 0) > .5;
    const int blk = 128;
    for (int i = 0; i < n; i += blk) {
        int e = qMin(n, i + blk);
        Frame k0 = c.kt(Frame((a + i) / spf)), k1 = c.kt(Frame((a + e) / spf));
        float g0 = bypass ? 1.f : dbToGain(vol->val("level", k0));
        float g1 = bypass ? 1.f : dbToGain(vol->val("level", k1));
        double bal = pan && pan->enabled ? pan->val("balance", k0) / 100.0 : 0.0;
        float gl = bal > 0 ? float(1 - bal) : 1.f, gr = bal < 0 ? float(1 + bal) : 1.f;
        for (int j = i; j < e; ++j) {
            float u = float(j - i) / (e - i), g = g0 + (g1 - g0) * u;
            qint64 s = a + j;
            if (fadeInA >= 0 && s < fadeInB) {
                float w = qBound(0.f, float(s - fadeInA) / qMax<qint64>(1, fadeInB - fadeInA), 1.f);
                g *= inType == "a_cgain" ? w : inType == "a_expo" ? w * w : std::sin(w * float(M_PI_2));
            }
            if (fadeOutA >= 0 && s >= fadeOutA) {
                float w = 1 - qBound(0.f, float(s - fadeOutA) / qMax<qint64>(1, fadeOutB - fadeOutA), 1.f);
                g *= outType == "a_cgain" ? w : outType == "a_expo" ? w * w : std::sin(w * float(M_PI_2));
            }
            out[(s - pos) * 2] += d[j * 2] * g * gl;
            out[(s - pos) * 2 + 1] += d[j * 2 + 1] * g * gr;
        }
    }
}

void AudioMixer::mixSeq(const ProjectState &st, const Sequence &q, float *out, int frames, qint64 pos, int depth, bool meters, int solo)
{
    bool anySolo = false;
    for (auto &t : q.audio) anySolo |= t.solo;
    QVector<float> tb(frames * 2);
    const double spf = kRate / q.fps();
    int ti = 0;
    for (const Track &t : q.audio) {
        int idx = ti++;
        if (meters && idx < kMaxTracks) trackIds[idx] = t.id;
        if (t.muted || (anySolo && !t.solo) || (solo >= 0 && idx != solo)) {
            if (meters && idx < kMaxTracks) { trackPeak[idx][0] = 0; trackPeak[idx][1] = 0; }
            continue;
        }
        std::fill(tb.begin(), tb.end(), 0.f);
        Frame lo = Frame(pos / spf), hi = Frame((pos + frames) / spf) + 1;
        for (const Clip &c : t.clips) {
            Frame margin = 0;
            for (auto &x : t.transitions) if (x.cut == c.start || x.cut == c.end()) margin = qMax(margin, x.dur);
            if (c.end() + margin < lo || c.start - margin > hi) continue;
            mixClip(st, q, t, c, tb.data(), frames, pos, depth);
        }
        for (int i = 0; i < t.fx.size(); ++i)
            if (t.fx[i].enabled) runFx(t.fx[i], (quint64(1) << 62) | (quint64(uint(t.id)) << 8) | uint(i), tb.data(), frames, Frame(pos / spf));
        float pkL = 0, pkR = 0;
        const bool autoV = t.volAuto.animated && !t.volAuto.keys.isEmpty(), autoP = t.panAuto.animated && !t.panAuto.keys.isEmpty();
        for (int b = 0; b < frames; b += 128) {
            int e = qMin(frames, b + 128);
            Frame f0 = Frame((pos + b) / spf), f1 = Frame((pos + e) / spf);
            float g0 = dbToGain(autoV ? t.volAuto.at(f0) : t.volumeDb), g1 = dbToGain(autoV ? t.volAuto.at(f1) : t.volumeDb);
            double pan = autoP ? t.panAuto.at(f0) : t.pan;
            float pl = pan > 0 ? float(1 - pan) : 1.f, pr = pan < 0 ? float(1 + pan) : 1.f;
            for (int i = b; i < e; ++i) {
                float g = g0 + (g1 - g0) * float(i - b) / (e - b);
                float l = tb[i * 2] * g * pl, r = tb[i * 2 + 1] * g * pr;
                pkL = qMax(pkL, std::fabs(l)); pkR = qMax(pkR, std::fabs(r));
                out[i * 2] += l; out[i * 2 + 1] += r;
            }
        }
        if (meters && idx < kMaxTracks) { trackPeak[idx][0] = pkL; trackPeak[idx][1] = pkR; }
    }
}

void AudioMixer::mix(float *out, int frames, qint64 pos)
{
    std::memset(out, 0, sizeof(float) * frames * 2);
    QMutexLocker l(&mx);
    if (!state) return;
    const Sequence *q = state->seqById(seqId);
    if (!q) return;
    mixSeq(*state, *q, out, frames, pos, 0, tapMeters);
    float g = dbToGain(q->masterDb), pkL = 0, pkR = 0;
    for (int i = 0; i < frames; ++i) {
        out[i * 2] *= g; out[i * 2 + 1] *= g;
        pkL = qMax(pkL, std::fabs(out[i * 2])); pkR = qMax(pkR, std::fabs(out[i * 2 + 1]));
    }
    masterPeak[0] = pkL; masterPeak[1] = pkR;
}
