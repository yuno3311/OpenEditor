// Frame-accurate video reader. A worker thread pulls raw BGRA frames from an ffmpeg pipe into the
// shared frame cache; stepping forward just reads on, jumps restart ffmpeg at the wanted frame.
#pragma once
#include "pipeproc.h"
#include <QImage>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

using Frame = qint64;

class VideoReader {
public:
    VideoReader(const QString &path, int w, int h, double fps, bool alpha, const QString &prefilter = {});
    static QString hdrToneMapFilter();
    static std::atomic<bool> hwDecode;
    ~VideoReader();
    QImage get(Frame n, int timeoutMs);
    void prefetch(Frame n);
    static void setCacheBudget(qint64 bytes);
    static void clearCache();
private:
    void run();
    quint64 key(Frame n) const;
    QString path;
    int w, h;
    double fps;
    bool alpha;
    QString prefilter;
    quint64 pathHash;
    std::thread th;
    std::mutex m;
    std::condition_variable cv, done;
    Frame want = 0, cur = -1, seekTo = -1, lastAsked = -1;
    Frame eofAt = -1;
    bool stop = false;
    QImage last;
};
