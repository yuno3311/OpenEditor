#include "interchange.h"
#include "../core/effectdefs.h"
#include "../core/mediacache.h"
#include "audiodsp.h"
#include "audiomix.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <complex>

namespace Analysis {

Beats detectBeats(const float *stereo, qint64 frames, double minBpm, double maxBpm)
{
    Beats out;
    const int N = 1024, hop = 480;             // 100 onset values a second
    const double envRate = double(MediaCache::kRate) / hop;
    if (frames < N * 8) return out;
    const int count = int((frames - N) / hop);
    QVector<float> env(count, 0.f), prev(N / 2, 0.f), win(N);
    for (int i = 0; i < N; ++i) win[i] = float(.5 - .5 * std::cos(2 * M_PI * i / N));
    QVector<std::complex<float>> buf(N);
    for (int k = 0; k < count; ++k) {
        const float *s = stereo + qint64(k) * hop * 2;
        for (int i = 0; i < N; ++i) buf[i] = (s[i * 2] + s[i * 2 + 1]) * .5f * win[i];
        Dsp::fft(buf.data(), N, false);
        // Spectral flux: how much louder each band got since the last frame. Compressing the
        // magnitudes first lets quiet hi-hats count next to a loud kick.
        float flux = 0;
        for (int i = 1; i < N / 2; ++i) {
            const float m = std::log1p(60 * std::abs(buf[i]) / (N / 4));
            if (m > prev[i]) flux += m - prev[i];
            prev[i] = m;
        }
        env[k] = flux;
    }
    // Remove the slow-moving average so only peaks remain.
    {
        QVector<float> e2(count);
        const int r = int(envRate * .4);
        double acc = 0;
        int a = 0, b = 0;
        for (int i = 0; i < count; ++i) {
            while (b < qMin(count, i + r + 1)) acc += env[b++];
            while (a < i - r) acc -= env[a++];
            e2[i] = qMax(0.f, env[i] - float(acc / (b - a)));
        }
        env = e2;
    }
    double total = 0;
    for (float v : env) total += v;
    if (total <= 1e-6) return out;

    // Tempo: the lag at which the onset curve best matches itself, with a mild preference for
    // tempos near 120 so half and double time do not win by accident.
    const int lagMin = qMax(2, int(envRate * 60 / maxBpm)), lagMax = qMin(count / 2, int(envRate * 60 / minBpm) + 1);
    if (lagMax <= lagMin) return out;
    QVector<double> ac(lagMax + 2, 0.0);
    for (int lag = lagMin - 1; lag <= lagMax + 1; ++lag) {
        double s = 0;
        for (int i = 0; i + lag < count; ++i) s += double(env[i]) * env[i + lag];
        ac[lag] = s / (count - lag);
    }
    double zero = 0;
    for (int i = 0; i < count; ++i) zero += double(env[i]) * env[i];
    zero /= count;
    int bestLag = 0;
    double best = 0;
    for (int lag = lagMin; lag <= lagMax; ++lag) {
        const double bpm = 60 * envRate / lag, oct = std::log2(bpm / 120);
        // Also count the match two and three beats apart, which a real pulse has and noise lacks.
        double v = ac[lag];
        if (lag * 2 < ac.size()) v += .5 * ac[lag * 2];
        const double score = v * std::exp(-.5 * oct * oct / (.9 * .9));
        if (score > best) { best = score; bestLag = lag; }
    }
    if (!bestLag || ac[bestLag] < zero * .08) return out;
    // Refine the period between whole lags with a parabola through the neighbours.
    double period = bestLag;
    {
        const double y0 = ac[bestLag - 1], y1 = ac[bestLag], y2 = ac[bestLag + 1], d = y0 - 2 * y1 + y2;
        if (std::fabs(d) > 1e-12) period += qBound(-.5, .5 * (y0 - y2) / d, .5);
    }
    // A long take drifts if the period is slightly off, so fit it again over many beats.
    {
        double bestP = period, bestS = -1;
        for (double p = period - .6; p <= period + .6; p += .02) {
            double top = 0;
            for (int ph = 0; ph < int(p); ++ph) {
                double s = 0;
                for (double x = ph; x < count; x += p) s += env[int(x)];
                top = qMax(top, s);
            }
            if (top > bestS) { bestS = top; bestP = p; }
        }
        period = bestP;
    }
    out.bpm = 60 * envRate / period;

    // Phase: the offset whose comb collects the most onset energy.
    double bestPhase = 0, bestSum = -1;
    for (double ph = 0; ph < period; ph += .25) {
        double s = 0;
        for (double x = ph; x < count; x += period) s += env[int(x)];
        if (s > bestSum) { bestSum = s; bestPhase = ph; }
    }
    // Each beat settles on the strongest onset close to where the grid expects it.
    const int reach = qMax(1, int(period * .12));
    for (double x = bestPhase; x < count; x += period) {
        int at = int(std::lround(x)), pick = at;
        float top = -1;
        for (int j = qMax(0, at - reach); j <= qMin(count - 1, at + reach); ++j)
            if (env[j] > top) { top = env[j]; pick = j; }
        const double sec = (top > 0 ? pick : at) * hop / double(MediaCache::kRate) + (N / 2 - hop) / double(MediaCache::kRate);
        out.times << qMax(0.0, sec);
    }
    return out;
}

Beats sequenceBeats(const ProjectState &st, int seqId, Frame first, Frame last)
{
    const Sequence *q = st.seqById(seqId);
    if (!q) return {};
    if (last < 0) last = q->duration();
    first = qMax<Frame>(0, first);
    const double spf = MediaCache::kRate / q->fps();
    // Ten minutes is plenty to find a tempo and keeps memory bounded.
    const qint64 a = qint64(first * spf), n = qMin<qint64>(qint64((last - first) * spf), qint64(MediaCache::kRate) * 600);
    if (n <= 0) return {};
    AudioMixer mix;
    mix.tapMeters = false;
    mix.setState(std::make_shared<const ProjectState>(st), seqId);
    QVector<float> buf(n * 2);
    for (qint64 p = 0; p < n; p += 4800) mix.mix(buf.data() + p * 2, int(qMin<qint64>(4800, n - p)), a + p);
    return detectBeats(buf.constData(), n);
}

int addBeatMarkers(Sequence &q, const Beats &b, Frame offset, int every)
{
    q.markers.erase(std::remove_if(q.markers.begin(), q.markers.end(), [](const Marker &m) { return m.name == QLatin1String("Beat"); }), q.markers.end());
    int n = 0;
    every = qMax(1, every);
    for (int i = 0; i < b.times.size(); i += every) {
        Marker m;
        m.t = offset + Frame(std::llround(b.times[i] * q.fps()));
        m.name = QStringLiteral("Beat");
        m.color = 4;
        q.markers << m;
        ++n;
    }
    std::sort(q.markers.begin(), q.markers.end(), [](const Marker &a, const Marker &c) { return a.t < c.t; });
    return n;
}
}

namespace Interchange {
static QString reelName(const MediaItem *m)
{
    if (!m || m->path.isEmpty()) return QStringLiteral("AX");
    QString r = QFileInfo(m->path).completeBaseName().toUpper();
    r.remove(QRegularExpression("[^A-Z0-9_]"));
    return r.isEmpty() ? QStringLiteral("AX") : r.left(8);
}

QString writeEdl(const ProjectState &st, const Sequence &q)
{
    const double fps = q.fps();
    auto tc = [&](Frame f) { return framesToTimecode(f, fps, q.dropFrame); };
    QStringList l;
    l << "TITLE: " + q.name << (q.dropFrame ? "FCM: DROP FRAME" : "FCM: NON-DROP FRAME") << QString();
    int n = 0;
    auto emitTrack = [&](const Track &t, const QString &chan) {
        for (const Clip &c : t.clips) {
            if (!c.enabled) continue;
            const MediaItem *m = st.mediaById(c.mediaId);
            const bool file = m && !m->path.isEmpty();
            const Frame srcIn = Frame(std::llround(c.srcIn * fps)), srcOut = srcIn + Frame(std::llround(c.dur * c.speed));
            l << QString("%1  %2 %3 C        %4 %5 %6 %7").arg(++n, 3, 10, QChar('0')).arg(file ? reelName(m) : QStringLiteral("AX"), -8).arg(chan, -5)
                     .arg(tc(srcIn), tc(srcOut), tc(c.start), tc(c.end()));
            if (std::fabs(c.speed - 1) > 1e-6 || c.reverse)
                l << QString("M2   %1       %2                %3").arg(file ? reelName(m) : QStringLiteral("AX"), -8).arg((c.reverse ? -1 : 1) * fps * c.speed, 5, 'f', 1).arg(tc(srcIn));
            l << "* FROM CLIP NAME: " + (file ? QFileInfo(m->path).fileName() : c.name);
            if (c.kind == MediaKind::Text) l << "* TITLE: " + QString(c.text.text).replace('\n', ' ');
            for (const Transition &x : t.transitions)
                if (x.cut == c.start) {
                    const TransitionDef *d = EffectRegistry::findTransition(x.type);
                    l << QString("* TRANSITION IN: %1, %2 frames").arg(d ? d->name : x.type).arg(x.dur);
                }
        }
    };
    if (!q.video.isEmpty()) emitTrack(q.video[0], "V");
    for (int i = 0; i < qMin<int>(4, q.audio.size()); ++i) emitTrack(q.audio[i], i ? QString("A%1").arg(i + 1) : QStringLiteral("A"));
    for (const Marker &m : q.markers) l << QString("* MARKER %1 %2").arg(tc(m.t), m.name);
    return l.join("\r\n") + "\r\n";
}

QVector<EdlEvent> parseEdl(const QString &text, QString *title)
{
    QVector<EdlEvent> out;
    static const QRegularExpression ev("^(\\d{3,})\\s+(\\S+)\\s+(\\S+)\\s+C\\s+(\\S+)\\s+(\\S+)\\s+(\\S+)\\s+(\\S+)\\s*$");
    for (QString line : text.split('\n')) {
        line = line.trimmed();
        if (line.startsWith("TITLE:")) { if (title) *title = line.mid(6).trimmed(); continue; }
        if (line.startsWith("* FROM CLIP NAME:") && !out.isEmpty()) { out.last().clipName = line.mid(17).trimmed(); continue; }
        const auto m = ev.match(line);
        if (!m.hasMatch()) continue;
        out << EdlEvent{m.captured(1).toInt(), m.captured(2), m.captured(3), m.captured(4), m.captured(5), m.captured(6), m.captured(7), {}};
    }
    return out;
}

static QString csvField(QString s)
{
    if (s.contains(',') || s.contains('"') || s.contains('\n')) { s.replace('"', "\"\""); return '"' + s + '"'; }
    return s;
}
QString markersCsv(const Sequence &q)
{
    QStringList l{"Name,Comment,In,Out,Duration,Seconds,Color"};
    for (const Marker &m : q.markers)
        l << QStringList{csvField(m.name), csvField(m.comment), framesToTimecode(m.t, q.fps(), q.dropFrame), framesToTimecode(m.t + m.dur, q.fps(), q.dropFrame),
                         framesToTimecode(m.dur, q.fps(), q.dropFrame), QString::number(m.t / q.fps(), 'f', 3), labelName(m.color)}.join(',');
    return l.join("\r\n") + "\r\n";
}

QString youtubeChapters(const Sequence &q)
{
    if (q.markers.isEmpty()) return {};
    QVector<Marker> mk = q.markers;
    std::sort(mk.begin(), mk.end(), [](const Marker &a, const Marker &b) { return a.t < b.t; });
    const bool hours = q.duration() / q.fps() >= 3600;
    auto stamp = [&](Frame f) {
        const int s = int(f / q.fps());
        return hours ? QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                     : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
    };
    QStringList l;
    // YouTube only reads a chapter list whose first entry is at 0:00.
    if (mk.first().t > 0) l << stamp(0) + " Start";
    int n = 0;
    for (const Marker &m : mk) l << stamp(m.t) + " " + (m.name.isEmpty() ? QString("Chapter %1").arg(++n) : m.name);
    return l.join('\n') + '\n';
}

QString ffmetadataChapters(const Sequence &q, Frame first, Frame last)
{
    QVector<Marker> mk;
    for (const Marker &m : q.markers) if (m.t >= first && m.t < last) mk << m;
    if (mk.isEmpty()) return {};
    std::sort(mk.begin(), mk.end(), [](const Marker &a, const Marker &b) { return a.t < b.t; });
    auto esc = [](QString s) { for (const char *c : {"\\", "=", ";", "#"}) s.replace(c, QString("\\") + c); return s.replace('\n', ' '); };
    auto ms = [&](Frame f) { return qint64(std::llround((f - first) * 1000.0 / q.fps())); };
    QString o = ";FFMETADATA1\n";
    int n = 0;
    if (mk.first().t > first) o += QString("[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=%1\ntitle=Start\n").arg(ms(mk.first().t));
    for (int i = 0; i < mk.size(); ++i) {
        const Frame end = i + 1 < mk.size() ? mk[i + 1].t : last;
        o += QString("[CHAPTER]\nTIMEBASE=1/1000\nSTART=%1\nEND=%2\ntitle=%3\n").arg(ms(mk[i].t)).arg(ms(end)).arg(esc(mk[i].name.isEmpty() ? QString("Chapter %1").arg(++n) : mk[i].name));
    }
    return o;
}
}
