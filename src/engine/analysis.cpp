#include "analysis.h"
#include "../core/mediacache.h"
#include "audiomix.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>

namespace Analysis {

static Frame seqLen(const ProjectState &st, int id)
{
    auto *q = st.seqById(id);
    return q ? q->duration() : 0;
}

QVector<FrameRange> findSilence(const ProjectState &st, int seqId, double thresholdDb, double minSeconds, double pad)
{
    QVector<FrameRange> out;
    auto *q = st.seqById(seqId);
    if (!q) return out;
    AudioMixer mix;
    mix.tapMeters = false;
    mix.setState(std::make_shared<const ProjectState>(st), seqId);
    const double spf = MediaCache::kRate / q->fps();
    const qint64 total = qint64(q->duration() * spf);
    const int win = MediaCache::kRate / 50; // 20 ms windows
    QVector<float> buf(win * 2);
    const float thresh = float(std::pow(10.0, thresholdDb / 20));
    qint64 silentFrom = -1;
    auto close = [&](qint64 from, qint64 to) {
        double a = from / double(MediaCache::kRate) + pad, b = to / double(MediaCache::kRate) - pad;
        if (from == 0) a = 0;
        if (to >= total) b = to / double(MediaCache::kRate);
        if (to - from < minSeconds * MediaCache::kRate || b <= a) return;
        Frame fa = Frame(std::ceil(a * q->fps())), fb = Frame(b * q->fps());
        if (fb > fa) out << qMakePair(fa, fb);
    };
    for (qint64 p = 0; p < total; p += win) {
        mix.mix(buf.data(), win, p);
        double sum = 0;
        for (float v : buf) sum += double(v) * v;
        float rms = float(std::sqrt(sum / buf.size()));
        if (rms < thresh) { if (silentFrom < 0) silentFrom = p; }
        else if (silentFrom >= 0) { close(silentFrom, p); silentFrom = -1; }
    }
    if (silentFrom >= 0) close(silentFrom, total);
    return out;
}

QVector<double> detectScenes(const QString &path, double threshold)
{
    QVector<double> cuts;
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(CACHE->ffmpeg(), {"-hide_banner", "-nostdin", "-i", path, "-an", "-sn", "-vf",
                              QString("scale=320:-2,select='gt(scene,%1)',showinfo").arg(threshold), "-f", "null", "-"});
    p.waitForFinished(-1);
    static const QRegularExpression re("pts_time:\\s*([0-9.]+)");
    auto it = re.globalMatch(QString::fromUtf8(p.readAll()));
    while (it.hasNext()) cuts << it.next().captured(1).toDouble();
    return cuts;
}

double measureRmsDb(const ProjectState &st, int seqId)
{
    auto *q = st.seqById(seqId);
    if (!q) return -96;
    AudioMixer mix;
    mix.tapMeters = false;
    mix.setState(std::make_shared<const ProjectState>(st), seqId);
    const qint64 total = qint64(seqLen(st, seqId) * (MediaCache::kRate / q->fps()));
    const int blk = 4800;
    QVector<float> buf(blk * 2);
    double sum = 0;
    qint64 n = 0;
    for (qint64 p = 0; p < total; p += blk) {
        mix.mix(buf.data(), blk, p);
        double s = 0;
        for (float v : buf) s += double(v) * v;
        // Ignore pauses so one quiet stretch does not skew the program level.
        if (s / buf.size() > 1e-5) { sum += s; n += buf.size(); }
    }
    return n ? 10 * std::log10(sum / n) : -96;
}

// Cross-correlates the loudness envelopes of two conformed files at 50 Hz.
double syncOffsetSeconds(const QString &ref, const QString &other, double maxSeconds)
{
    auto a = CACHE->peaks(ref), b = CACHE->peaks(other);
    if (!a || !b) return 0;
    const double peaksPerSec = double(MediaCache::kRate) / PeakData::kSamplesPerPeak;
    const int step = qMax(1, int(peaksPerSec / 50));
    auto env = [&](const PeakData &p) {
        QVector<float> e;
        const auto *d = reinterpret_cast<const qint8 *>(p.bytes.constData());
        for (qint64 i = 0; i + step <= p.count(); i += step) {
            int m = 0;
            for (int k = 0; k < step; ++k) m = qMax(m, qMax(std::abs(int(d[(i + k) * 4 + 1])), std::abs(int(d[(i + k) * 4]))));
            e << float(m);
        }
        float mean = 0;
        for (float v : e) mean += v;
        mean /= qMax<qsizetype>(1, e.size());
        for (auto &v : e) v -= mean;
        return e;
    };
    QVector<float> x = env(*a), y = env(*b);
    const double rate = peaksPerSec / step;
    int maxLag = qMin(int(maxSeconds * rate), int(qMax(x.size(), y.size())));
    double best = -1e30;
    int bestLag = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double s = 0;
        int count = 0;
        for (int i = qMax(0, -lag); i < y.size() && i + lag < x.size(); ++i) { s += double(x[i + lag]) * y[i]; ++count; }
        if (count < rate) continue;
        s /= std::sqrt(double(count));
        if (s > best) { best = s; bestLag = lag; }
    }
    return bestLag / rate; // positive: other starts this many seconds into the reference
}

static double parseStamp(QString s)
{
    s = s.trimmed().replace(',', '.');
    auto p = s.split(':');
    double t = 0;
    for (auto &x : p) t = t * 60 + x.toDouble();
    return t;
}

QVector<Caption> parseSubtitles(const QString &text, double fps)
{
    QVector<Caption> out;
    static const QRegularExpression timing("^\\s*([0-9:.,]+)\\s*-->\\s*([0-9:.,]+)");
    const QStringList lines = QString(text).remove('\r').split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        auto m = timing.match(lines[i]);
        if (!m.hasMatch()) continue;
        Caption c;
        c.start = qRound64(parseStamp(m.captured(1)) * fps);
        c.end = qRound64(parseStamp(m.captured(2)) * fps);
        QStringList body;
        for (++i; i < lines.size() && !lines[i].trimmed().isEmpty(); ++i) body << lines[i].trimmed();
        c.text = body.join('\n');
        if (c.end > c.start && !c.text.isEmpty()) out << c;
    }
    std::sort(out.begin(), out.end(), [](const Caption &a, const Caption &b) { return a.start < b.start; });
    return out;
}

static QString stamp(Frame f, double fps, QChar sep)
{
    qint64 ms = qRound64(f / fps * 1000);
    return QString("%1:%2:%3%4%5").arg(ms / 3600000, 2, 10, QChar('0')).arg((ms / 60000) % 60, 2, 10, QChar('0'))
        .arg((ms / 1000) % 60, 2, 10, QChar('0')).arg(sep).arg(ms % 1000, 3, 10, QChar('0'));
}
QString writeSrt(const QVector<Caption> &caps, double fps)
{
    QString s;
    int n = 1;
    for (auto &c : caps) s += QString("%1\n%2 --> %3\n%4\n\n").arg(n++).arg(stamp(c.start, fps, ','), stamp(c.end, fps, ','), c.text);
    return s;
}
QString writeVtt(const QVector<Caption> &caps, double fps)
{
    QString s = "WEBVTT\n\n";
    for (auto &c : caps) s += QString("%1 --> %2\n%3\n\n").arg(stamp(c.start, fps, '.'), stamp(c.end, fps, '.'), c.text);
    return s;
}

// Word-level SRT from Whisper: with max_len=1 each entry is one word. Bracketed entries are sound tags.
QVector<Word> parseWordSrt(const QString &text, double fps)
{
    QVector<Word> out;
    static const QRegularExpression timing("^\\s*([0-9:.,]+)\\s*-->\\s*([0-9:.,]+)");
    const QStringList lines = QString(text).remove('\r').split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        auto m = timing.match(lines[i]);
        if (!m.hasMatch()) continue;
        QString tok;
        for (++i; i < lines.size() && !lines[i].trimmed().isEmpty(); ++i) tok += lines[i];
        const Frame a = qRound64(parseStamp(m.captured(1)) * fps), b = qRound64(parseStamp(m.captured(2)) * fps);
        if (tok.trimmed().isEmpty() || tok.trimmed().startsWith('[') || tok.trimmed().startsWith('(')) continue;
        out << Word{a, qMax(a + 1, b), tok.trimmed()};
    }
    return out;
}

QString joinWords(const QVector<Word> &w)
{
    QString s;
    for (auto &x : w) {
        const QChar first = x.text.isEmpty() ? QChar(' ') : x.text.front();
        const bool cjk = (first.unicode() >= 0x3040 && first.unicode() <= 0x30FF) || (first.unicode() >= 0x4E00 && first.unicode() <= 0x9FFF);
        if (!s.isEmpty() && !cjk) s += ' ';
        s += x.text;
    }
    return s;
}


// Retyping a caption keeps the words that match and spreads changed text over the same time.
QVector<Word> retimeWords(const QVector<Word> &old, const QString &text, Frame start, Frame end)
{
    if (old.isEmpty()) return {};
    const QStringList parts = text.simplified().split(' ', Qt::SkipEmptyParts);
    QVector<Word> out;
    if (parts.size() == old.size()) {
        out = old;
        for (int i = 0; i < parts.size(); ++i) out[i].text = parts[i];
        return out;
    }
    int chars = 0;
    for (auto &p : parts) chars += int(p.size());
    if (!chars) return {};
    double t = start, perChar = double(end - start) / chars;
    for (auto &p : parts) {
        double e = t + perChar * p.size();
        out << Word{Frame(t), qMax(Frame(t) + 1, Frame(e)), p};
        t = e;
    }
    return out;
}

// Readable captions: split at sentence ends and pauses, then divide long sentences evenly so no
// caption is a stranded word.
QVector<Caption> groupIntoCaptions(const QVector<Word> &words, double fps)
{
    const Frame pause = Frame(fps * .7);
    const int maxChars = 62;
    QVector<QVector<Word>> sentences;
    QVector<Word> cur;
    auto endSentence = [&] { if (!cur.isEmpty()) { sentences << cur; cur.clear(); } };
    for (auto &w : words) {
        if (!cur.isEmpty() && w.start - cur.last().end >= pause) endSentence();
        cur << w;
        const QChar last = w.text.isEmpty() ? QChar(' ') : w.text.back();
        if ((last == '.' || last == '?' || last == '!' || last.unicode() == 0x3002 || last.unicode() == 0xFF1F || last.unicode() == 0xFF01) && joinWords(cur).size() >= 10) endSentence();
    }
    endSentence();
    QVector<Caption> out;
    auto emitCaption = [&](const QVector<Word> &ws) {
        Caption c;
        c.words = ws; c.start = ws.first().start; c.end = ws.last().end; c.text = joinWords(ws);
        out << c;
    };
    for (auto &s : std::as_const(sentences)) {
        const int total = int(joinWords(s).size());
        const int parts = (total + maxChars - 1) / maxChars;
        if (parts <= 1) { emitCaption(s); continue; }
        const int target = total / parts + 1;
        QVector<Word> piece;
        for (int i = 0; i < s.size(); ++i) {
            piece << s[i];
            const bool comma = s[i].text.endsWith(',') && joinWords(piece).size() > target * 2 / 3;
            if ((joinWords(piece).size() >= target || comma) && i + 1 < s.size()) { emitCaption(piece); piece.clear(); }
        }
        if (!piece.isEmpty()) emitCaption(piece);
    }
    // Hold each caption until the next begins when the pause is short, so text does not flicker.
    for (int i = 0; i + 1 < out.size(); ++i)
        if (out[i + 1].start - out[i].end < Frame(fps * .4)) out[i].end = out[i + 1].start;
    return out;
}

// Where speech is, from the audio itself. The threshold follows the recording's noise floor, so
// room tone or a music bed does not read as continuous speech.
QVector<FrameRange> speechRegions(const float *pcm, qint64 frames, double fps)
{
    QVector<FrameRange> out;
    const int win = MediaCache::kRate / 50, bridge = 12;
    const qint64 n = frames / win;
    if (n < 2) return out;
    QVector<float> db(n);
    for (qint64 i = 0; i < n; ++i) {
        double s = 0;
        for (int k = 0; k < win * 2; ++k) { const float v = pcm[i * win * 2 + k]; s += double(v) * v; }
        db[i] = float(10 * std::log10(s / (win * 2) + 1e-14));
    }
    QVector<float> sorted = db;
    std::sort(sorted.begin(), sorted.end());
    const float floorDb = sorted[n / 10], peakDb = sorted[n * 95 / 100];
    const float thresh = qMax(-46.f, floorDb + (peakDb - floorDb) * .3f);
    const double perWin = fps / 50.0;
    qint64 from = -1, last = -1;
    for (qint64 i = 0; i < n; ++i) {
        if (db[i] > thresh) { if (from < 0) from = i; last = i; }
        else if (from >= 0 && i - last > bridge) { out << qMakePair(Frame(from * perWin), Frame((last + 1) * perWin) + 1); from = -1; }
    }
    if (from >= 0) out << qMakePair(Frame(from * perWin), Frame((last + 1) * perWin) + 1);
    return out;
}


// Places Whisper's words on the audio. Its token times are only a guide (typically 300 ms off, up to
// a second), so words are assigned to measured speech regions by dynamic programming, using speaking
// rate, those guide times and punctuation. Inside a region, boundaries settle on real energy dips.
QVector<Word> placeWords(const QVector<Word> &in, const QVector<FrameRange> &speech, double fps, const float *pcm, qint64 frames)
{
    if (in.isEmpty() || speech.isEmpty()) return {};
    // Words with no speech anywhere near them were invented over silence.
    const Frame nearby = Frame(fps * 3);
    QVector<Word> w;
    auto gapTo = [&](Frame f, const FrameRange &r) { return f < r.first ? r.first - f : f >= r.second ? f - r.second + 1 : Frame(0); };
    for (auto &x : in) {
        const Frame mid = (x.start + x.end) / 2;
        Frame best = Frame(1) << 50;
        for (auto &r : speech) best = qMin(best, gapTo(mid, r));
        if (best <= nearby) w << x;
    }
    if (w.isEmpty()) return {};
    const int n = w.size(), m = speech.size();
    QVector<double> pre(n + 1, 0.0), rawMid(n);
    auto chars = [&](const QString &t) { int c = 0; for (QChar ch : t) c += ch.isLetterOrNumber(); return double(qMax(1, c)) + .6; };
    for (int i = 0; i < n; ++i) { pre[i + 1] = pre[i] + chars(w[i].text); rawMid[i] = (w[i].start + w[i].end) / 2.0; }
    double speechLen = 0;
    for (auto &r : speech) speechLen += double(r.second - r.first);
    const double rate = speechLen / pre[n]; // frames per character
    auto ender = [&](int i) {
        const QChar c = w[i].text.isEmpty() ? QChar(' ') : w[i].text.back();
        if (c == '.' || c == '?' || c == '!' || c.unicode() == 0x3002 || c.unicode() == 0xFF1F || c.unicode() == 0xFF01) return 2;
        return (c == ',' || c == ';' || c == ':' || c.unicode() == 0x3001) ? 1 : 0;
    };
    const double inf = 1e18;
    const int reachWords = 120;
    QVector<QVector<double>> dp(m + 1, QVector<double>(n + 1, inf));
    QVector<QVector<int>> from(m + 1, QVector<int>(n + 1, -1));
    dp[0][0] = 0;
    for (int r = 0; r < m; ++r) {
        const double dur = double(speech[r].second - speech[r].first);
        for (int k = 0; k <= n; ++k) {
            if (dp[r][k] >= inf) continue;
            // A region can be non-speech: leaving it empty costs in proportion to its length.
            const double skip = dp[r][k] + dur / fps * 1.2;
            if (skip < dp[r + 1][k]) { dp[r + 1][k] = skip; from[r + 1][k] = k; }
            double prior = 0;
            for (int j = k + 1; j <= qMin(n, k + reachWords); ++j) {
                prior += double(gapTo(Frame(rawMid[j - 1]), speech[r])) / fps;
                const double want = (pre[j] - pre[k]) * rate;
                double c = std::fabs(want - dur) / fps + prior / (j - k) * 1.5;
                const int e = ender(j - 1);
                if (r + 1 < m) c += e == 2 ? -.5 : e == 1 ? -.2 : .3;
                const double total = dp[r][k] + c;
                if (total < dp[r + 1][j]) { dp[r + 1][j] = total; from[r + 1][j] = k; }
                if (want > dur * 2.5 + fps * 2) break;
            }
        }
    }
    QVector<int> groupEnd(m + 1, n);
    for (int r = m, j = n; r > 0; --r) { groupEnd[r] = j; j = from[r][j] >= 0 ? from[r][j] : j; groupEnd[r - 1] = j; }

    // Energy at 10 ms resolution, for settling boundaries into the dips between words.
    const int hop = MediaCache::kRate / 100;
    const qint64 hops = frames / hop;
    QVector<float> env(hops);
    for (qint64 i = 0; i < hops; ++i) {
        double s = 0;
        for (int k = 0; k < hop * 2; ++k) { const float v = pcm[i * hop * 2 + k]; s += double(v) * v; }
        env[i] = float(s);
    }
    auto settle = [&](double frame, double lo, double hi) {
        const double perHop = fps / 100;
        const qint64 c = qint64(frame / perHop), span = qint64(.07 * 100);
        qint64 best = c;
        float bestE = 1e30f;
        for (qint64 i = qMax<qint64>(0, c - span); i <= qMin(hops - 1, c + span); ++i) {
            const double f = i * perHop;
            if (f <= lo || f >= hi) continue;
            const float e = env[i] * float(1 + std::fabs(double(i - c)) / span);
            if (e < bestE) { bestE = e; best = i; }
        }
        return best * perHop;
    };
    QVector<Word> out = w;
    for (int r = 0; r < m; ++r) {
        const int a = r ? groupEnd[r] : 0, b = groupEnd[r + 1];
        if (b <= a) continue;
        const double s = double(speech[r].first), e = double(speech[r].second);
        double rawA = double(w[a].start), rawSpan = qMax(1.0, double(w[b - 1].end - w[a].start));
        double prev = s;
        for (int i = a; i < b; ++i) {
            const double byChars = s + (pre[i] - pre[a]) / qMax(1.0, pre[b] - pre[a]) * (e - s);
            const double byRaw = s + qBound(0.0, (double(w[i].start) - rawA) / rawSpan, 1.0) * (e - s);
            double t = i == a ? s : settle(byChars * .6 + byRaw * .4, prev + 1, e - 1);
            out[i].start = Frame(std::round(qMax(prev, t)));
            prev = double(out[i].start);
            if (i > a) out[i - 1].end = qMax(out[i - 1].start + 1, out[i].start);
        }
        out[b - 1].end = qMax(out[b - 1].start + 1, Frame(e));
    }
    Frame last = 0;
    for (auto &x : out) { x.start = qMax(x.start, last); x.end = qMax(x.end, x.start + 1); last = x.start; }
    return out;
}

QString whisperModelDir()
{
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/WhisperModels";
    QDir().mkpath(d);
    return d;
}
QString whisperModelPath(const QString &n) { return whisperModelDir() + "/ggml-" + n + ".bin"; }
QStringList whisperModels() { return {"large-v3-turbo-q5_0", "large-v3-turbo", "large-v3-q5_0", "medium", "small", "base", "tiny"}; }
QString whisperModelLabel(const QString &n)
{
    if (n == "large-v3-turbo-q5_0") return QStringLiteral("Best: Large v3 Turbo");
    if (n == "large-v3-turbo") return QStringLiteral("Large v3 Turbo, full precision");
    if (n == "large-v3-q5_0") return QStringLiteral("Large v3, slower");
    return n.left(1).toUpper() + n.mid(1);
}
qint64 whisperModelSizeMB(const QString &n)
{
    if (n == "large-v3-turbo-q5_0") return 574;
    if (n == "large-v3-turbo") return 1624;
    if (n == "large-v3-q5_0") return 1081;
    if (n == "tiny") return 75;
    if (n == "base") return 142;
    if (n == "small") return 466;
    return 1500;
}
}

Transcriber::Transcriber(const ProjectState &s, int id, const QString &m, const QString &l, QObject *p) : QThread(p), st(s), seqId(id), model(m), lang(l)
{
    // Every audio file the mix can touch, including inside nested sequences.
    for (auto &md : st.media)
        if (md.hasAudio && !md.path.isEmpty() && !md.offline) { audioFiles << md.path; CACHE->ensureAudio(md.path); }
}


void Transcriber::run()
{
    const Sequence *q = st.seqById(seqId);
    if (!q || q->duration() <= 0) { emit failed(tr("The sequence is empty.")); return; }
    if (!QFileInfo::exists(model)) { emit failed(tr("The speech model is not installed.")); return; }

    // Audio is mixed from conformed files. Transcribing before they exist would feed Whisper silence.
    emit progress(0, tr("Preparing audio..."));
    QElapsedTimer wait;
    wait.start();
    for (;;) {
        bool ready = true;
        for (const QString &p : std::as_const(audioFiles)) if (!CACHE->pcm(p).data && !CACHE->audioFailed(p)) ready = false;
        if (ready) break;
        if (stop) { emit failed(tr("Transcription cancelled.")); return; }
        if (wait.elapsed() > 1800000) { emit failed(tr("Timed out preparing audio.")); return; }
        msleep(100);
    }

    QTemporaryDir tmp;
    QString raw = tmp.filePath("mix.f32");
    emit progress(.02, tr("Mixing audio..."));
    const qint64 total = qint64(q->duration() * (MediaCache::kRate / q->fps()));
    {
        AudioMixer mix;
        mix.tapMeters = false;
        mix.setState(std::make_shared<const ProjectState>(st), seqId);
        QFile f(raw);
        if (!f.open(QIODevice::WriteOnly)) { emit failed(tr("Cannot write temporary audio.")); return; }
        const int blk = 4096;
        QVector<float> buf(blk * 2);
        for (qint64 p = 0; p < total && !stop; p += blk) {
            int n = int(qMin<qint64>(blk, total - p));
            mix.mix(buf.data(), n, p);
            f.write(reinterpret_cast<const char *>(buf.constData()), n * 8);
        }
    }
    if (stop) { emit failed(tr("Transcription cancelled.")); return; }
    QFile rawFile(raw);
    if (!rawFile.open(QIODevice::ReadOnly) || rawFile.size() == 0) { emit failed(tr("The sequence has no audio.")); return; }
    const float *pcm = reinterpret_cast<const float *>(rawFile.map(0, rawFile.size()));
    const qint64 pcmFrames = rawFile.size() / 8;
    auto rmsDb = [&](qint64 a, qint64 b) {
        a = qBound<qint64>(0, a, pcmFrames); b = qBound<qint64>(a, b, pcmFrames);
        if (b <= a) return -120.0;
        double s = 0;
        for (qint64 i = a * 2; i < b * 2; ++i) s += double(pcm[i]) * pcm[i];
        return 10 * std::log10(s / double((b - a) * 2) + 1e-14);
    };
    if (rmsDb(0, pcmFrames) < -70) { emit finishedOk({}); return; }

    // The whole mix goes through in one pass: Whisper keeps context across the file and picks one
    // language. Slicing it (short queue or VAD) re-detects language per slice and loses accuracy.
    const double seconds = q->duration() / q->fps();
    QString escaped = QDir::fromNativeSeparators(model).replace(":", "\\:").replace("'", "\\'");
    QString filter = QString("whisper=model='%1':language=%2:queue=%3:max_len=1:destination=out.srt:format=srt")
                         .arg(escaped, lang.isEmpty() ? "auto" : lang).arg(int(seconds) + 10);
    QProcess p;
    p.setWorkingDirectory(tmp.path());
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(CACHE->ffmpeg(), {"-hide_banner", "-nostdin", "-loglevel", "error", "-f", "f32le", "-ar", QString::number(MediaCache::kRate),
                              "-ac", "2", "-i", raw, "-af", filter, "-f", "null", "-"});
    if (!p.waitForStarted(10000)) { emit failed(tr("Could not start ffmpeg.")); return; }
    QElapsedTimer tm;
    tm.start();
    QByteArray log;
    while (!p.waitForFinished(250)) {
        if (stop) { p.kill(); p.waitForFinished(2000); emit failed(tr("Transcription cancelled.")); return; }
        if (p.state() == QProcess::NotRunning) break;
        log += p.readAll();
        if (log.size() > 65536) log = log.right(32768);
        // Whisper reports nothing until it finishes, so this is an estimate from typical speed.
        double est = qMax(12.0, seconds / 2.5);
        emit progress(qMin(.95, .05 + .9 * tm.elapsed() / 1000.0 / est), tr("Transcribing speech... %1 s").arg(tm.elapsed() / 1000));
    }
    log += p.readAll();
    QFile srt(tmp.filePath("out.srt"));
    if (p.exitCode() != 0 || !srt.open(QIODevice::ReadOnly)) {
        emit failed(tr("Transcription failed: %1").arg(QString::fromUtf8(log.right(400)).trimmed()));
        return;
    }
    QVector<Word> words = Analysis::parseWordSrt(QString::fromUtf8(srt.readAll()), q->fps());
    const auto speech = Analysis::speechRegions(pcm, pcmFrames, q->fps());
    emit finishedOk(Analysis::groupIntoCaptions(Analysis::placeWords(words, speech, q->fps(), pcm, pcmFrames), q->fps()));
}

ModelDownloader::ModelDownloader(QObject *p) : QObject(p), net(new QNetworkAccessManager(this))
{
    net->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}
void ModelDownloader::start(const QString &name)
{
    QString dest = Analysis::whisperModelPath(name);
    auto *out = new QSaveFile(dest, this);
    if (!out->open(QIODevice::WriteOnly)) { emit failed(tr("Cannot write %1").arg(dest)); return; }
    QNetworkRequest rq(QUrl("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-" + name + ".bin"));
    rq.setTransferTimeout(60000);
    auto *r = net->get(rq);
    connect(r, &QNetworkReply::readyRead, out, [r, out] { out->write(r->readAll()); });
    connect(r, &QNetworkReply::downloadProgress, this, &ModelDownloader::progress);
    connect(r, &QNetworkReply::finished, this, [this, r, out, dest] {
        out->write(r->readAll());
        bool ok = r->error() == QNetworkReply::NoError;
        QString err = r->errorString();
        r->deleteLater();
        if (ok && out->commit()) emit finished(dest);
        else { out->cancelWriting(); emit failed(ok ? tr("Could not save the model.") : err); }
        out->deleteLater();
    });
}
