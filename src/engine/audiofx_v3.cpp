// Audio effects added in 3.0. Buffers are interleaved stereo float at 48 kHz; all state lives in
// FxState so playback, seeking and export stay consistent.
#include "audiomix.h"
#include "../core/mediacache.h"
#include <QtMath>
#include <cmath>

namespace {
constexpr double kRate = MediaCache::kRate;
inline float dbToGain(double db) { return db <= -96 ? 0.f : float(std::pow(10.0, db / 20.0)); }
inline float coef(double ms) { return float(std::exp(-1.0 / (qMax(.01, ms) * .001 * kRate))); }
inline double clampF(double f) { return qBound(10.0, f, kRate * .47); }

void peaking(Biquad &q, double f, double gainDb, double Q)
{
    const double A = std::pow(10, gainDb / 40), w = 2 * M_PI * clampF(f) / kRate, al = std::sin(w) / (2 * Q), c = std::cos(w), a0 = 1 + al / A;
    q.b0 = (1 + al * A) / a0; q.b1 = -2 * c / a0; q.b2 = (1 - al * A) / a0; q.a1 = -2 * c / a0; q.a2 = (1 - al / A) / a0;
}
void shelf(Biquad &q, double f, double gainDb, bool high)
{
    const double A = std::pow(10, gainDb / 40), w = 2 * M_PI * clampF(f) / kRate, c = std::cos(w), al = std::sin(w) / 2 * std::sqrt(2), sq = 2 * std::sqrt(A) * al;
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
void pass(Biquad &q, double f, bool high, double Q = 0.7071)
{
    const double w = 2 * M_PI * clampF(f) / kRate, c = std::cos(w), al = std::sin(w) / (2 * Q), a0 = 1 + al;
    if (high) { q.b0 = (1 + c) / 2 / a0; q.b1 = -(1 + c) / a0; q.b2 = q.b0; }
    else { q.b0 = (1 - c) / 2 / a0; q.b1 = (1 - c) / a0; q.b2 = q.b0; }
    q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
}
void notch(Biquad &q, double f, double Q)
{
    const double w = 2 * M_PI * clampF(f) / kRate, c = std::cos(w), al = std::sin(w) / (2 * Q), a0 = 1 + al;
    q.b0 = 1 / a0; q.b1 = -2 * c / a0; q.b2 = 1 / a0; q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
}
void bandpass(Biquad &q, double f, double Q) // unity gain at the centre
{
    const double w = 2 * M_PI * clampF(f) / kRate, c = std::cos(w), al = std::sin(w) / (2 * Q), a0 = 1 + al;
    q.b0 = al / a0; q.b1 = 0; q.b2 = -al / a0; q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
}
inline void need(FxState &s, int n) { if (s.bq.size() != n) s.bq = QVector<Biquad>(n); }
inline void run(Biquad &q, float *b, int n)
{
    for (int i = 0; i < n; ++i) { b[i * 2] = q.run(0, b[i * 2]); b[i * 2 + 1] = q.run(1, b[i * 2 + 1]); }
}
}

bool runAudioFxV3(const Effect &e, FxState &s, float *b, int n, Frame kt)
{
    const QString &t = e.type;
    if (!t.startsWith(QLatin1String("a_"))) return false;
    auto V = [&](const char *id, double d = 0) { return e.val(id, kt, d); };

    if (t == "a_geq") {
        static const double freqs[10] = {31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
        static const char *ids[10] = {"g0", "g1", "g2", "g3", "g4", "g5", "g6", "g7", "g8", "g9"};
        need(s, 10);
        for (int k = 0; k < 10; ++k) {
            const double g = V(ids[k]);
            if (std::fabs(g) < .01) continue; // a flat band costs nothing and adds no phase shift
            peaking(s.bq[k], freqs[k], g, 1.41);
            run(s.bq[k], b, n);
        }
    } else if (t == "a_bass" || t == "a_treble") {
        need(s, 1);
        shelf(s.bq[0], V("freq", t == "a_bass" ? 120 : 8000), V("gain", 6), t == "a_treble");
        run(s.bq[0], b, n);
    } else if (t == "a_notch") {
        need(s, 1);
        notch(s.bq[0], V("freq", 1000), qMax(.1, V("q", 10)));
        run(s.bq[0], b, n);
    } else if (t == "a_bandpass") {
        need(s, 1);
        bandpass(s.bq[0], V("freq", 1000), qMax(.1, V("q", 1)));
        run(s.bq[0], b, n);
    } else if (t == "a_mbcomp") {
        // Linkwitz-Riley crossovers: two Butterworth sections per slope, so the bands add back flat.
        need(s, 8);
        const double f1 = V("lowF", 200), f2 = qMax(f1 * 1.5, V("highF", 3000));
        pass(s.bq[0], f1, false); pass(s.bq[1], f1, false);
        pass(s.bq[2], f1, true); pass(s.bq[3], f1, true);
        pass(s.bq[4], f2, false); pass(s.bq[5], f2, false);
        pass(s.bq[6], f2, true); pass(s.bq[7], f2, true);
        const double th[3] = {V("tLow", -24), V("tMid", -24), V("tHigh", -24)}, ratio = qMax(1.0, V("ratio", 3));
        const float at = coef(V("attack", 10)), rl = coef(V("release", 150)), mk = dbToGain(V("makeup"));
        for (int i = 0; i < n; ++i) {
            float band[3][2];
            for (int c = 0; c < 2; ++c) {
                const float x = b[i * 2 + c];
                band[0][c] = s.bq[1].run(c, s.bq[0].run(c, x));
                const float rest = s.bq[3].run(c, s.bq[2].run(c, x));
                band[1][c] = s.bq[5].run(c, s.bq[4].run(c, rest));
                band[2][c] = s.bq[7].run(c, s.bq[6].run(c, rest));
            }
            float out[2] = {0, 0};
            for (int k = 0; k < 3; ++k) {
                const float lv = qMax(std::fabs(band[k][0]), std::fabs(band[k][1]));
                float &env = s.v[k];
                env = lv > env ? at * env + (1 - at) * lv : rl * env + (1 - rl) * lv;
                const double db = 20 * std::log10(qMax(1e-9f, env));
                const float g = db > th[k] ? dbToGain((th[k] - db) * (1 - 1 / ratio)) : 1.f;
                out[0] += band[k][0] * g; out[1] += band[k][1] * g;
            }
            b[i * 2] = out[0] * mk; b[i * 2 + 1] = out[1] * mk;
        }
    } else if (t == "a_width") {
        const float wd = float(qBound(0.0, V("width", 150) / 100, 4.0));
        for (int i = 0; i < n; ++i) {
            const float m = (b[i * 2] + b[i * 2 + 1]) * .5f, sd = (b[i * 2] - b[i * 2 + 1]) * .5f * wd;
            b[i * 2] = m + sd; b[i * 2 + 1] = m - sd;
        }
    } else if (t == "a_center") {
        const bool keep = V("mode") > .5;
        const float k = float(qBound(0.0, V("amount", 100) / 100, 1.0));
        for (int i = 0; i < n; ++i) {
            const float l = b[i * 2], r = b[i * 2 + 1], m = (l + r) * .5f;
            if (keep) { b[i * 2] = l + (m - l) * k; b[i * 2 + 1] = r + (m - r) * k; }
            else { b[i * 2] = l - m * k; b[i * 2 + 1] = r - m * k; }
        }
    } else if (t == "a_chanvol") {
        const float gl = dbToGain(V("left")), gr = dbToGain(V("right"));
        for (int i = 0; i < n; ++i) { b[i * 2] *= gl; b[i * 2 + 1] *= gr; }
    } else if (t == "a_tremolo" || t == "a_autopan") {
        const bool pan = t == "a_autopan";
        const float depth = float(qBound(0.0, V("depth", pan ? 100 : 60) / 100, 1.0));
        const double inc = 2 * M_PI * qMax(.001, V("rate", pan ? .5 : 5)) / kRate;
        for (int i = 0; i < n; ++i) {
            const float lfo = float(.5 - .5 * std::cos(s.ph2)); // 0..1, starts at full level
            if (pan) { b[i * 2] *= 1 - depth * lfo; b[i * 2 + 1] *= 1 - depth * (1 - lfo); }
            else { const float g = 1 - depth * lfo; b[i * 2] *= g; b[i * 2 + 1] *= g; }
            s.ph2 += inc;
            if (s.ph2 > 2 * M_PI) s.ph2 -= 2 * M_PI;
        }
    } else if (t == "a_phaser") {
        // Four first-order all-pass stages swept by an LFO; v[] holds each stage's state per channel.
        const float depth = float(qBound(0.0, V("depth", 70) / 100, 1.0)), fb = float(qBound(0.0, V("fb", 40) / 100, .95)), mix = float(qBound(0.0, V("mix", 50) / 100, 1.0));
        const double inc = 2 * M_PI * qMax(.001, V("rate", .4)) / kRate;
        for (int i = 0; i < n; ++i) {
            const double lfo = .5 - .5 * std::cos(s.ph2);
            const double fc = 300 * std::pow(2.0, 4.0 * depth * lfo); // 300 Hz up to 4.8 kHz
            const double tn = std::tan(M_PI * fc / kRate);
            const float a = float((tn - 1) / (tn + 1));
            for (int c = 0; c < 2; ++c) {
                const float in = b[i * 2 + c];
                float x = in + s.v[8 + c] * fb;
                for (int st = 0; st < 4; ++st) {
                    float &z = s.v[st * 2 + c];
                    const float y = a * x + z;
                    z = x - a * y;
                    x = y;
                }
                s.v[8 + c] = x;
                b[i * 2 + c] = in + (x - in) * mix * .5f; // an equal blend makes the deepest notches
            }
            s.ph2 += inc;
            if (s.ph2 > 2 * M_PI) s.ph2 -= 2 * M_PI;
        }
    } else if (t == "a_distort") {
        need(s, 1);
        pass(s.bq[0], V("tone", 6000), false);
        const float drive = dbToGain(V("drive", 18)), mix = float(qBound(0.0, V("mix", 100) / 100, 1.0)), out = dbToGain(V("out", -6));
        for (int i = 0; i < n * 2; ++i) {
            // The tone filter can ring a little past the clipper's ceiling, so the ceiling is enforced after it.
            const float wet = qBound(-1.f, s.bq[0].run(i & 1, std::tanh(b[i] * drive)), 1.f);
            b[i] = (b[i] * (1 - mix) + wet * mix) * (mix > 0 ? 1 + (out - 1) * mix : 1);
        }
    } else if (t == "a_bitcrush") {
        const int bits = qBound(1, int(std::lround(V("bits", 8))), 24), down = qBound(1, int(std::lround(V("down", 4))), 200);
        const float steps = float(std::pow(2.0, bits - 1)), mix = float(qBound(0.0, V("mix", 100) / 100, 1.0));
        for (int i = 0; i < n; ++i) {
            if (s.rpos % down == 0) {
                s.v[0] = std::round(b[i * 2] * steps) / steps;
                s.v[1] = std::round(b[i * 2 + 1] * steps) / steps;
            }
            s.rpos = (s.rpos + 1) % down;
            b[i * 2] += (s.v[0] - b[i * 2]) * mix;
            b[i * 2 + 1] += (s.v[1] - b[i * 2 + 1]) * mix;
        }
    } else if (t == "a_voicefx") {
        // Band-limit like the device, then add the grit it is known for.
        static const struct { double lo, hi, drive; } presets[4] = {{300, 3400, 2}, {180, 4500, 4}, {500, 3000, 12}, {400, 2600, 8}};
        const auto &p = presets[qBound(0, int(V("preset")), 3)];
        const float k = float(qBound(0.0, V("amount", 100) / 100, 1.0));
        need(s, 4);
        pass(s.bq[0], p.lo, true); pass(s.bq[1], p.lo, true);
        pass(s.bq[2], p.hi, false); pass(s.bq[3], p.hi, false);
        // Quiet speech comes up by sqrt(drive) and loud speech is squashed, as a small speaker does.
        const float drive = float(p.drive), norm = 1 / std::sqrt(drive);
        for (int i = 0; i < n * 2; ++i) {
            const int c = i & 1;
            float x = s.bq[1].run(c, s.bq[0].run(c, b[i]));
            x = s.bq[3].run(c, s.bq[2].run(c, x));
            x = std::tanh(x * drive) * norm;
            b[i] += (x - b[i]) * k;
        }
    } else if (t == "a_pitch") {
        // Two read heads sweep a short delay line at the pitch ratio and cross-fade as each wraps.
        const double ratio = std::pow(2.0, qBound(-24.0, V("semi", 3), 24.0) / 12);
        const float mix = float(qBound(0.0, V("mix", 100) / 100, 1.0));
        const int W = 2400, N = 8192; // 50 ms window
        if (s.ring.size() != N * 2) { s.ring.fill(0, N * 2); s.rpos = 0; s.ph2 = 0; }
        for (int i = 0; i < n; ++i) {
            s.ring[s.rpos * 2] = b[i * 2];
            s.ring[s.rpos * 2 + 1] = b[i * 2 + 1];
            float wet[2] = {0, 0};
            for (int tap = 0; tap < 2; ++tap) {
                double ph = s.ph2 + tap * .5;
                if (ph >= 1) ph -= 1;
                const double delay = ph * W + 2;
                const float g = float(.5 - .5 * std::cos(2 * M_PI * ph));
                double rp = s.rpos - delay;
                if (rp < 0) rp += N;
                const int i0 = int(rp) % N, i1 = (i0 + 1) % N;
                const float fr = float(rp - std::floor(rp));
                for (int c = 0; c < 2; ++c) wet[c] += g * (s.ring[i0 * 2 + c] * (1 - fr) + s.ring[i1 * 2 + c] * fr);
            }
            // The delay shrinks when pitching up (reading faster than writing) and grows when down.
            s.ph2 += (1 - ratio) / W;
            while (s.ph2 >= 1) s.ph2 -= 1;
            while (s.ph2 < 0) s.ph2 += 1;
            s.rpos = (s.rpos + 1) % N;
            b[i * 2] += (wet[0] - b[i * 2]) * mix;
            b[i * 2 + 1] += (wet[1] - b[i * 2 + 1]) * mix;
        }
    } else {
        return false;
    }
    return true;
}
