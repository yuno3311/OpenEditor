// Content analysis behind the assisted-editing tools: silence, scene cuts, loudness, audio sync,
// subtitle file formats and Whisper speech transcription.
#pragma once
#include "../core/model.h"
#include <QObject>
#include <QStringList>
#include <QThread>
#include <atomic>

using FrameRange = QPair<Frame, Frame>;

namespace Analysis {
QVector<FrameRange> findSilence(const ProjectState &st, int seqId, double thresholdDb, double minSeconds, double padSeconds = .1);
QVector<double> detectScenes(const QString &path, double threshold = .35); // cut times in seconds
double measureRmsDb(const ProjectState &st, int seqId);
double syncOffsetSeconds(const QString &reference, const QString &other, double maxSeconds = 60);
QVector<Caption> parseSubtitles(const QString &text, double fps);          // SRT and WebVTT
QString writeSrt(const QVector<Caption> &caps, double fps);
QString writeVtt(const QVector<Caption> &caps, double fps);
QVector<Word> parseWordSrt(const QString &text, double fps);
QVector<Word> placeWords(const QVector<Word> &words, const QVector<FrameRange> &speech, double fps, const float *pcm, qint64 frames);
QVector<FrameRange> speechRegions(const float *pcm, qint64 frames, double fps);
QVector<Caption> groupIntoCaptions(const QVector<Word> &words, double fps);
QString joinWords(const QVector<Word> &words);
QVector<Word> retimeWords(const QVector<Word> &old, const QString &text, Frame start, Frame end);
QString whisperModelLabel(const QString &name);
QString whisperModelDir();
QString whisperModelPath(const QString &name);
QStringList whisperModels();
qint64 whisperModelSizeMB(const QString &name);
}

class Transcriber : public QThread {
    Q_OBJECT
public:
    Transcriber(const ProjectState &st, int seqId, const QString &modelPath, const QString &language, QObject *p = nullptr);
    void cancel() { stop = true; }
signals:
    void progress(double fraction, const QString &status);
    void finishedOk(const QVector<Caption> &captions);
    void failed(const QString &why);
protected:
    void run() override;
private:
    ProjectState st;
    int seqId;
    QString model, lang;
    QStringList audioFiles;
    std::atomic<bool> stop{false};
};

// Downloads a Whisper model once, with explicit user consent at the call site.
class ModelDownloader : public QObject {
    Q_OBJECT
public:
    explicit ModelDownloader(QObject *p = nullptr);
    void start(const QString &name);
signals:
    void progress(qint64 got, qint64 total);
    void finished(const QString &path);
    void failed(const QString &why);
private:
    class QNetworkAccessManager *net;
};
