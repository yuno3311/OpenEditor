// Measures transcript word timing against ground truth.
// oe_align_eval <audio> <truth.tsv> [fps]: truth lines are "seconds<TAB>word" for each word start.
#include "core/mediacache.h"
#include "engine/analysis.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdio>

struct T { double t; QString w; };
static QString norm(QString s) { return s.toLower().remove(QRegularExpression("[^\\p{L}\\p{N}]")); }

static void report(const char *name, const QVector<T> &truth, const QVector<Word> &words, double fps)
{
    QVector<double> errs;
    int j = 0, matched = 0;
    for (auto &tr : truth) {
        for (int k = j; k < qMin<int>(j + 4, words.size()); ++k)
            if (norm(words[k].text) == norm(tr.w)) { errs << (words[k].start / fps - tr.t); j = k + 1; ++matched; break; }
    }
    if (errs.isEmpty()) { printf("%-22s no matches\n", name); return; }
    QVector<double> ab;
    double bias = 0;
    for (double e : errs) { ab << std::fabs(e); bias += e; }
    std::sort(ab.begin(), ab.end());
    double mean = 0;
    for (double e : ab) mean += e;
    printf("%-22s matched %d/%d  mean %4.0f ms  median %4.0f ms  p95 %4.0f ms  max %4.0f ms  bias %+4.0f ms\n", name, matched, int(truth.size()), mean / ab.size() * 1000,
           ab[ab.size() / 2] * 1000, ab[int(ab.size() * .95)] * 1000, ab.last() * 1000, bias / errs.size() * 1000);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Open Editor");
    QCoreApplication::setApplicationName("Open Editor");
    if (argc < 3) return 2;
    const QString audio = argv[1], truthPath = argv[2];
    const double fps = argc > 3 ? atof(argv[3]) : 30;
    QVector<T> truth;
    QFile tf(truthPath);
    tf.open(QIODevice::ReadOnly);
    for (auto l : QString::fromUtf8(tf.readAll()).remove(QChar(0xFEFF)).remove('\r').split('\n')) {
        auto p = l.split('\t');
        if (p.size() == 2) truth << T{p[0].toDouble(), p[1].trimmed()};
    }
    std::sort(truth.begin(), truth.end(), [](const T &a, const T &b) { return a.t < b.t; });
    QTemporaryDir tmp;
    QString raw = tmp.filePath("mix.f32");
    QProcess::execute(CACHE->ffmpeg(), {"-hide_banner", "-loglevel", "error", "-y", "-i", audio, "-ac", "2", "-ar", "48000", "-f", "f32le", raw});
    QFile rf(raw);
    rf.open(QIODevice::ReadOnly);
    const float *pcm = reinterpret_cast<const float *>(rf.map(0, rf.size()));
    const qint64 frames = rf.size() / 8;
    const QString model = Analysis::whisperModelPath("large-v3-turbo-q5_0");
    QString esc = QDir::fromNativeSeparators(model).replace(":", "\\:");
    QProcess p;
    p.setWorkingDirectory(tmp.path());
    p.setProcessChannelMode(QProcess::ForwardedChannels);
    p.start(CACHE->ffmpeg(), {"-hide_banner", "-loglevel", "error", "-nostdin", "-f", "f32le", "-ar", "48000", "-ac", "2", "-i", raw, "-af",
                              QString("whisper=model='%1':language=auto:queue=%2:max_len=1:destination=out.srt:format=srt").arg(esc).arg(int(frames / 48000) + 10), "-f", "null", "-"});
    p.waitForFinished(-1);
    QFile srt(tmp.filePath("out.srt"));
    srt.open(QIODevice::ReadOnly);
    const auto words = Analysis::parseWordSrt(QString::fromUtf8(srt.readAll()), fps);
    printf("%lld words from Whisper, %lld truth words\n", (long long)words.size(), (long long)truth.size());
    const auto speech = Analysis::speechRegions(pcm, frames, fps);
    printf("speech regions: %lld\n", (long long)speech.size());
    report("raw whisper", truth, words, fps);
    report("final placement", truth, Analysis::placeWords(words, speech, fps, pcm, frames), fps);
    return 0;
}
