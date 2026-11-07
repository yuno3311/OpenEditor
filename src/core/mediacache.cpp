#include "mediacache.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QtConcurrent>

static double parseRate(const QString &s)
{
    auto p = s.split('/');
    if (p.size() == 2 && p[1].toDouble() != 0) return p[0].toDouble() / p[1].toDouble();
    return s.toDouble();
}

MediaCache *MediaCache::instance() { static MediaCache c; return &c; }

MediaCache::MediaCache(QObject *p) : QObject(p), thumbs(2000)
{
    audioPool.setMaxThreadCount(2);
    thumbPool.setMaxThreadCount(3);
    QDir().mkpath(cacheDir());
}
QString MediaCache::cacheDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/MediaCache";
}
static QString locate(const QString &name)
{
    QString side = QCoreApplication::applicationDirPath() + "/" + name + ".exe";
    if (QFileInfo::exists(side)) return side;
    return QStandardPaths::findExecutable(name);
}
QString MediaCache::ffmpeg() const
{
    if (ffmpegPath.isEmpty()) ffmpegPath = locate("ffmpeg");
    return ffmpegPath;
}
QString MediaCache::ffprobe() const
{
    if (ffprobePath.isEmpty()) ffprobePath = locate("ffprobe");
    return ffprobePath;
}
QString MediaCache::keyFor(const QString &path) const
{
    QFileInfo fi(path);
    QByteArray k = (fi.absoluteFilePath() + QString::number(fi.size()) + QString::number(fi.lastModified().toMSecsSinceEpoch())).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(k, QCryptographicHash::Sha1).toHex());
}

bool MediaCache::probe(const QString &path, MediaItem &m)
{
    QFileInfo fi(path);
    if (!fi.exists()) return false;
    m.path = fi.absoluteFilePath();
    m.name = fi.fileName();
    m.size = fi.size();

    static const QSet<QString> stills{"png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp", "gif"};
    if (stills.contains(fi.suffix().toLower())) {
        QImageReader r(path);
        QSize sz = r.size();
        if (sz.isValid()) {
            m.kind = MediaKind::Image;
            m.w = sz.width(); m.h = sz.height(); m.hasVideo = true; m.fps = 0; m.duration = 5;
            m.hasAlpha = fi.suffix().toLower() == "png" || fi.suffix().toLower() == "webp" || fi.suffix().toLower() == "tif";
            m.label = defaultLabel(MediaKind::Image);
            return true;
        }
    }
    QProcess p;
    p.start(ffprobe(), {"-v", "error", "-print_format", "json", "-show_format", "-show_streams", path});
    if (!p.waitForFinished(20000) || p.exitCode() != 0) return false;
    auto doc = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    auto streams = doc["streams"].toArray();
    auto fmt = doc["format"].toObject();
    m.duration = fmt["duration"].toString().toDouble();
    m.bitrate = fmt["bit_rate"].toString().toLongLong();
    for (auto sv : streams) {
        auto s = sv.toObject();
        QString type = s["codec_type"].toString();
        if (type == "video" && !m.hasVideo) {
            if (s["disposition"].toObject()["attached_pic"].toInt() == 1) continue;
            m.hasVideo = true;
            m.w = s["width"].toInt(); m.h = s["height"].toInt();
            m.vcodec = s["codec_name"].toString();
            m.fps = parseRate(s["avg_frame_rate"].toString());
            if (m.fps <= 0) m.fps = parseRate(s["r_frame_rate"].toString());
            if (m.fps <= 0 || m.fps > 480) m.fps = 30;
            QString pf = s["pix_fmt"].toString();
            m.hasAlpha = pf.contains("yuva") || pf.contains("rgba") || pf.contains("argb") || pf.contains("gbrap") || pf.contains("bgra");
            m.colorTransfer = s["color_transfer"].toString();
            m.hdr = m.colorTransfer == "smpte2084" || m.colorTransfer == "arib-std-b67";
            double rot = 0;
            for (auto sd : s["side_data_list"].toArray()) {
                auto o = sd.toObject();
                if (o.contains("rotation")) rot = o["rotation"].toDouble();
            }
            if (s["tags"].toObject().contains("rotate")) rot = s["tags"].toObject()["rotate"].toString().toDouble();
            if (qAbs(std::fmod(rot, 180.0)) > 45) std::swap(m.w, m.h);
            double sd = s["duration"].toString().toDouble();
            if (m.duration <= 0) m.duration = sd;
        } else if (type == "audio" && !m.hasAudio) {
            m.hasAudio = true;
            m.channels = s["channels"].toInt();
            m.sampleRate = s["sample_rate"].toString().toInt();
            m.acodec = s["codec_name"].toString();
            if (m.duration <= 0) m.duration = s["duration"].toString().toDouble();
        }
    }
    if (!m.hasVideo && !m.hasAudio) return false;
    m.kind = m.hasVideo ? MediaKind::Video : MediaKind::Audio;
    m.label = defaultLabel(m.kind);
    return true;
}

void MediaCache::bumpJobs(int d)
{
    pending += d;
    emit jobsChanged(pending);
}

void MediaCache::ensureAudio(const QString &path)
{
    {
        QMutexLocker l(&mx);
        if (audio.contains(path) || running.contains(path)) return;
        running << path;
    }
    QString base = cacheDir() + "/" + keyFor(path);
    QString pcmFile = base + ".pcm", pekFile = base + ".pek";
    if (QFileInfo::exists(pcmFile) && QFileInfo::exists(pekFile)) { conformDone(path); return; }
    bumpJobs(1);
    QThreadPool *pool = &audioPool;
    (void)QtConcurrent::run(pool, [=, this] {
        QString tmp = pcmFile + ".tmp";
        QProcess p;
        p.start(ffmpeg(), {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", path, "-vn", "-map", "0:a:0", "-ac", "2",
                           "-ar", QString::number(kRate), "-f", "s16le", tmp});
        p.waitForFinished(-1);
        bool ok = p.exitCode() == 0 && QFileInfo(tmp).size() > 0;
        if (ok) {
            QFile in(tmp), pk(pekFile);
            if (in.open(QIODevice::ReadOnly) && pk.open(QIODevice::WriteOnly)) {
                const int n = PeakData::kSamplesPerPeak;
                QByteArray buf(n * 4, 0);
                for (;;) {
                    qint64 got = in.read(buf.data(), buf.size());
                    if (got <= 0) break;
                    auto *s = reinterpret_cast<const qint16 *>(buf.constData());
                    int fr = int(got / 4);
                    int mnL = 0, mxL = 0, mnR = 0, mxR = 0;
                    for (int i = 0; i < fr; ++i) {
                        mnL = qMin<int>(mnL, s[i * 2]); mxL = qMax<int>(mxL, s[i * 2]);
                        mnR = qMin<int>(mnR, s[i * 2 + 1]); mxR = qMax<int>(mxR, s[i * 2 + 1]);
                    }
                    char out[4] = {char(mnL >> 8), char(mxL >> 8), char(mnR >> 8), char(mxR >> 8)};
                    pk.write(out, 4);
                }
                in.close(); pk.close();
                QFile::remove(pcmFile);
                QFile::rename(tmp, pcmFile);
            }
        }
        QMetaObject::invokeMethod(this, [=, this] { bumpJobs(-1); conformDone(path); }, Qt::QueuedConnection);
    });
}

void MediaCache::conformDone(const QString &path)
{
    QString base = cacheDir() + "/" + keyFor(path);
    auto f = std::make_shared<QFile>(base + ".pcm");
    Mapped m;
    if (f->open(QIODevice::ReadOnly) && f->size() > 0) {
        m.d.data = reinterpret_cast<const qint16 *>(f->map(0, f->size()));
        m.d.frames = f->size() / 4;
        m.f = f;
        QFile pk(base + ".pek");
        m.pk = std::make_shared<PeakData>();
        if (pk.open(QIODevice::ReadOnly)) m.pk->bytes = pk.readAll();
    }
    {
        QMutexLocker l(&mx);
        running.remove(path);
        if (!m.d.data) audioBad << path;
        if (m.d.data) audio.insert(path, m);
    }
    if (m.d.data) emit audioReady(path);
}

bool MediaCache::audioFailed(const QString &path)
{
    QMutexLocker l(&mx);
    return audioBad.contains(path);
}

PcmData MediaCache::pcm(const QString &path)
{
    QMutexLocker l(&mx);
    auto it = audio.constFind(path);
    return it == audio.constEnd() ? PcmData{} : it->d;
}
std::shared_ptr<PeakData> MediaCache::peaks(const QString &path)
{
    QMutexLocker l(&mx);
    auto it = audio.constFind(path);
    return it == audio.constEnd() ? nullptr : it->pk;
}

QImage MediaCache::thumbnail(const QString &path, double sec, int width, bool isImage)
{
    int bucket = isImage ? 0 : int(sec * 2);
    QString key = QString("%1|%2|%3").arg(path).arg(bucket).arg(width);
    {
        QMutexLocker l(&mx);
        if (auto *img = thumbs.object(key)) return *img;
        if (thumbInflight.contains(key)) return {};
        if (thumbInflight.size() > 48) return {};
        thumbInflight << key;
    }
    double t = bucket / 2.0;
    (void)QtConcurrent::run(&thumbPool, [=, this] {
        QImage img;
        if (isImage) {
            QImageReader r(path);
            r.setAutoTransform(true);
            QSize sz = r.size();
            if (sz.isValid()) r.setScaledSize(sz.scaled(width, width * 4, Qt::KeepAspectRatio));
            img = r.read();
        } else {
            QProcess p;
            p.start(ffmpeg(), {"-hide_banner", "-loglevel", "error", "-nostdin", "-ss", QString::number(t, 'f', 3), "-i", path,
                               "-frames:v", "1", "-an", "-vf", QString("scale=%1:-2").arg(width), "-c:v", "bmp", "-f", "image2pipe", "pipe:1"});
            if (p.waitForFinished(15000)) img = QImage::fromData(p.readAllStandardOutput());
        }
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QMetaObject::invokeMethod(this, [=, this] {
            { QMutexLocker l(&mx);
              thumbInflight.remove(key);
              thumbs.insert(key, new QImage(img.isNull() ? QImage(1, 1, QImage::Format_ARGB32_Premultiplied) : img)); }
            emit thumbReady(path);
        }, Qt::QueuedConnection);
    });
    return {};
}

void MediaCache::clearDiskCache()
{
    QDir d(cacheDir());
    QMutexLocker l(&mx);
    for (auto &n : d.entryList(QDir::Files)) {
        bool used = false;
        for (auto it = audio.constBegin(); it != audio.constEnd(); ++it)
            if (keyFor(it.key()) == QFileInfo(n).completeBaseName()) used = true;
        if (!used) d.remove(n);
    }
}

QString MediaCache::proxyFile(const QString &path) const { return cacheDir() + "/" + keyFor(path) + ".proxy.mp4"; }

QString MediaCache::proxyFor(const QString &path)
{
    QMutexLocker l(&mx);
    auto it = proxies.constFind(path);
    if (it != proxies.constEnd()) return *it;
    l.unlock();
    QString f = proxyFile(path);
    if (!QFileInfo::exists(f)) return {};
    l.relock();
    proxies.insert(path, f);
    return f;
}

void MediaCache::makeProxy(const QString &path, bool hdr)
{
    {
        QMutexLocker l(&mx);
        if (proxyRunning.contains(path)) return;
        proxyRunning << path;
    }
    QString out = proxyFile(path);
    if (QFileInfo::exists(out)) { QMutexLocker l(&mx); proxyRunning.remove(path); return; }
    bumpJobs(1);
    (void)QtConcurrent::run(&audioPool, [=, this] {
        QString tmp = out + ".part.mp4";
        QString vf = (hdr ? QStringLiteral("zscale=t=linear:npl=203,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,format=yuv420p,") : QString())
                     + QStringLiteral("scale='min(960,iw)':-2");
        QProcess p;
        p.start(ffmpeg(), {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", path, "-an", "-sn", "-vf", vf, "-c:v", "libx264",
                           "-preset", "veryfast", "-crf", "24", "-g", "12", "-pix_fmt", "yuv420p", "-movflags", "+faststart", tmp});
        p.waitForFinished(-1);
        bool ok = p.exitCode() == 0 && QFileInfo(tmp).size() > 0;
        if (ok) { QFile::remove(out); ok = QFile::rename(tmp, out); } else QFile::remove(tmp);
        QMetaObject::invokeMethod(this, [=, this] {
            { QMutexLocker l(&mx); proxyRunning.remove(path); }
            bumpJobs(-1);
            if (ok) emit proxyReady(path);
        }, Qt::QueuedConnection);
    });
}
