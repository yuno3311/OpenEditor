#include "videoreader.h"
#include "../core/mediacache.h"
#include <QCache>
#include <QMutex>
#include <chrono>

namespace {
QMutex cacheMx;
QCache<quint64, QImage> &cache()
{
    static QCache<quint64, QImage> c(1536); // cost unit: MiB-ish, see cost()
    return c;
}
qsizetype costOf(const QImage &i) { return qMax<qsizetype>(1, i.sizeInBytes() / (1 << 16)); }
QImage cacheGet(quint64 k)
{
    QMutexLocker l(&cacheMx);
    auto *p = cache().object(k);
    return p ? *p : QImage();
}
void cachePut(quint64 k, const QImage &img)
{
    QMutexLocker l(&cacheMx);
    cache().insert(k, new QImage(img), costOf(img));
}
}

void VideoReader::setCacheBudget(qint64 bytes)
{
    QMutexLocker l(&cacheMx);
    cache().setMaxCost(bytes / (1 << 16));
}
void VideoReader::clearCache()
{
    QMutexLocker l(&cacheMx);
    cache().clear();
}

std::atomic<bool> VideoReader::hwDecode{true};

// PQ and HLG sources: linearise, convert primaries, tone map, return to Rec.709 for the 8-bit pipeline.
QString VideoReader::hdrToneMapFilter()
{
    return QStringLiteral("zscale=t=linear:npl=203,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,format=yuv420p,");
}

VideoReader::VideoReader(const QString &p, int w, int h, double f, bool a, const QString &pre) : path(p), w(w), h(h), fps(f), alpha(a), prefilter(pre)
{
    pathHash = qHash(path) ^ (quint64(w) << 12) ^ (quint64(h) << 28);
    pathHash <<= 24;
    {
        QMutexLocker l(&cacheMx);
        if (cache().maxCost() == 1536) cache().setMaxCost((qint64(3) << 30) / (1 << 16));
    }
    th = std::thread([this] { run(); });
}
VideoReader::~VideoReader()
{
    { std::lock_guard<std::mutex> l(m); stop = true; }
    cv.notify_all();
    th.join();
}
quint64 VideoReader::key(Frame n) const { return pathHash ^ quint64(n); }

void VideoReader::run()
{
    PipeProc proc;
    const qint64 bytes = qint64(w) * h * 4;
    const int ahead = qMax(10, int(fps / 2));
    std::unique_lock<std::mutex> l(m);
    for (;;) {
        cv.wait(l, [&] { return stop || seekTo >= 0 || (cur >= 0 && eofAt < 0 && cur <= want + ahead); });
        if (stop) break;
        if (seekTo >= 0) {
            Frame n = seekTo;
            seekTo = -1;
            eofAt = -1;
            cur = n;
            l.unlock();
            QStringList args{"-hide_banner", "-loglevel", "error", "-nostdin", "-threads", "4"};
            if (hwDecode && !alpha) args << "-hwaccel" << "auto";
            if (n > 0) args << "-ss" << QString::number((n - 0.25) / fps, 'f', 6);
            args << "-i" << path << "-map" << "0:v:0" << "-an" << "-sn"
                 << "-vf" << prefilter + QString("fps=%1,scale=%2:%3:flags=bilinear,format=bgra").arg(fps, 0, 'f', 6).arg(w).arg(h)
                 << "-f" << "rawvideo" << "pipe:1";
            proc.start(CACHE->ffmpeg(), args);
            l.lock();
            if (seekTo >= 0) continue;
        }
        Frame idx = cur;
        l.unlock();
        QImage img(w, h, alpha ? QImage::Format_ARGB32 : QImage::Format_ARGB32_Premultiplied);
        bool ok = proc.read(img.bits(), bytes);
        if (ok) {
            if (alpha) img.convertTo(QImage::Format_ARGB32_Premultiplied);
            cachePut(key(idx), img);
        }
        l.lock();
        if (!ok) { eofAt = idx; }
        else {
            last = img;
            if (seekTo < 0 && cur == idx) cur = idx + 1;
        }
        done.notify_all();
    }
    l.unlock();
    proc.kill();
}

QImage VideoReader::get(Frame n, int timeoutMs)
{
    n = qMax<Frame>(0, n);
    const quint64 k = key(n);
    QImage hit = cacheGet(k);
    std::unique_lock<std::mutex> l(m);
    bool reversing = lastAsked >= 0 && n < lastAsked;
    lastAsked = n;
    want = n;
    if (!hit.isNull()) { cv.notify_all(); return hit; }
    if (eofAt >= 0 && n >= eofAt) return last;
    Frame pos = seekTo >= 0 ? seekTo : cur;
    Frame jump = Frame(fps * 2) + 4;
    if (pos < 0 || n < pos || n > pos + jump) {
        Frame lead = reversing ? qMin<Frame>(n, Frame(fps / 2) + 1) : 0;
        seekTo = n - lead;
    }
    cv.notify_all();
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        done.wait_until(l, until);
        l.unlock();
        hit = cacheGet(k);
        l.lock();
        if (!hit.isNull()) return hit;
        if (eofAt >= 0 && seekTo < 0 && n >= eofAt) return last;
        if (want != n) return last;
        if (std::chrono::steady_clock::now() >= until) return last;
    }
}

void VideoReader::prefetch(Frame n)
{
    std::lock_guard<std::mutex> l(m);
    if (cur >= 0 || seekTo >= 0) return;
    want = n;
    seekTo = qMax<Frame>(0, n);
    cv.notify_all();
}
