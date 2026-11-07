// Media services: ffprobe, conformed audio (48 kHz stereo s16), peak files and thumbnails.
#pragma once
#include "model.h"
#include <QCache>
#include <QFile>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <memory>

struct PcmData {
    const qint16 *data = nullptr; // interleaved stereo
    qint64 frames = 0;
};
struct PeakData {
    static constexpr int kSamplesPerPeak = 256;
    QByteArray bytes; // minL maxL minR maxR as qint8, per peak
    qint64 count() const { return bytes.size() / 4; }
};

class MediaCache : public QObject {
    Q_OBJECT
public:
    static MediaCache *instance();
    static constexpr int kRate = 48000;
    QString ffmpeg() const;
    QString ffprobe() const;
    bool available() const { return !ffmpeg().isEmpty(); }
    // Looked up beside the app, then the folder chosen in setup, then the downloaded copy, then PATH.
    static QString downloadedToolsDir();
    void reloadTools();
    QString cacheDir() const;
    bool probe(const QString &path, MediaItem &out);
    void ensureAudio(const QString &path);
    PcmData pcm(const QString &path);
    std::shared_ptr<PeakData> peaks(const QString &path);
    QImage thumbnail(const QString &path, double sec, int width, bool isImage = false);
    void makeProxy(const QString &path, bool hdr);
    QString proxyFor(const QString &path);     // empty until the proxy is finished
    QString proxyFile(const QString &path) const;
    int pendingJobs() const { return pending; }
    void clearDiskCache();
    bool audioFailed(const QString &path);
signals:
    void audioReady(const QString &path);
    void thumbReady(const QString &path);
    void proxyReady(const QString &path);
    void jobsChanged(int pending);
private:
    explicit MediaCache(QObject *p = nullptr);
    QString keyFor(const QString &path) const;
    void conformDone(const QString &path);
    void bumpJobs(int d);
    QMutex mx;
    struct Mapped { std::shared_ptr<QFile> f; PcmData d; std::shared_ptr<PeakData> pk; };
    QHash<QString, Mapped> audio;
    QSet<QString> running;
    QSet<QString> audioBad;
    QHash<QString, QString> proxies;
    QSet<QString> proxyRunning;
    QCache<QString, QImage> thumbs;
    QSet<QString> thumbInflight;
    QThreadPool audioPool, thumbPool;
    mutable QString ffmpegPath, ffprobePath;
    int pending = 0;
};
#define CACHE (MediaCache::instance())
