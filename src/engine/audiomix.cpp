#include "audiomix.h"
#include "../core/mediacache.h"
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
}

void AudioMixer::runFx(const Effect &e, quint64 key, float *b, int n, Frame kt)
{
    const QString &t = e.type;
    auto V = [&](const char *id, double d = 0) { return e.val(id, kt, d); };
    FxState &s = fx[key];
    if (t.startsWith(QLatin1String("plugin."))) { PluginHost::audio(e, b, n, kt, s.plugin); return; }
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
