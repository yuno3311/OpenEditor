#include "exporter.h"
#include "../core/mediacache.h"
#include "audiodsp.h"
#include "audiomix.h"
#include "analysis.h"
#include "compositor.h"
#include "interchange.h"
#include "../core/vault.h"
#include <QJsonDocument>
#include <QSettings>
#include "pipeproc.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <cmath>

// A hardware encoder is listed only if it really initialises on this machine.
static bool encoderWorks(const char *name)
{
    QProcess p;
    p.start(CACHE->ffmpeg(), {"-hide_banner", "-loglevel", "error", "-nostdin", "-f", "lavfi", "-i", "color=black:s=320x240:r=30:d=0.1", "-c:v", name, "-f", "null", "-"});
    return p.waitForFinished(8000) && p.exitCode() == 0;
}

const QVector<ExportFormat> &exportFormats()
{
    static const QVector<ExportFormat> v = [] {
    QVector<ExportFormat> l = {
        {"H.264", "mp4", true, true, {"-c:v", "libx264", "-pix_fmt", "yuv420p"}, {"-c:a", "aac"}},
        {"HEVC (H.265)", "mp4", true, true, {"-c:v", "libx265", "-pix_fmt", "yuv420p", "-tag:v", "hvc1"}, {"-c:a", "aac"}},
        {"QuickTime ProRes 422", "mov", true, true, {"-c:v", "prores_ks", "-profile:v", "2", "-pix_fmt", "yuv422p10le"}, {"-c:a", "pcm_s16le"}},
        {"QuickTime ProRes 4444", "mov", true, true, {"-c:v", "prores_ks", "-profile:v", "4", "-pix_fmt", "yuva444p10le"}, {"-c:a", "pcm_s16le"}},
        {"DNxHR HQ", "mxf", true, true, {"-c:v", "dnxhd", "-profile:v", "dnxhr_hq", "-pix_fmt", "yuv422p"}, {"-c:a", "pcm_s16le"}},
        {"VP9 (WebM)", "webm", true, true, {"-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p", "-row-mt", "1"}, {"-c:a", "libopus"}},
        {"AV1", "mp4", true, true, {"-c:v", "libsvtav1", "-pix_fmt", "yuv420p"}, {"-c:a", "aac"}},
        {"Animated GIF", "gif", true, false, {"-vf", "fps=15,split[a][b];[a]palettegen[p];[b][p]paletteuse"}, {}},
        {"H.264 (Matroska)", "mkv", true, true, {"-c:v", "libx264", "-pix_fmt", "yuv420p"}, {"-c:a", "aac"}},
        {"PNG Sequence", "png", true, false, {"-c:v", "png", "-pix_fmt", "rgba"}, {}},
        {"JPEG Sequence", "jpg", true, false, {"-c:v", "mjpeg", "-pix_fmt", "yuvj420p", "-q:v", "2"}, {}},
        {"WAV (Audio Only)", "wav", false, true, {}, {"-c:a", "pcm_s16le"}},
        {"WAV 24-bit (Audio Only)", "wav", false, true, {}, {"-c:a", "pcm_s24le"}},
        {"FLAC (Audio Only)", "flac", false, true, {}, {"-c:a", "flac"}},
        {"Opus (Audio Only)", "opus", false, true, {}, {"-c:a", "libopus"}},
        {"MP3 (Audio Only)", "mp3", false, true, {}, {"-c:a", "libmp3lame"}},
        {"AAC (Audio Only)", "m4a", false, true, {}, {"-c:a", "aac"}}};
    for (auto &hw : std::initializer_list<std::pair<const char *, const char *>>{{"H.264 (NVIDIA NVENC)", "h264_nvenc"}, {"HEVC (NVIDIA NVENC)", "hevc_nvenc"}, {"H.264 (Intel Quick Sync)", "h264_qsv"}, {"H.264 (AMD AMF)", "h264_amf"}})
        if (encoderWorks(hw.second)) l.insert(2, ExportFormat{hw.first, "mp4", true, true, {"-c:v", hw.second, "-pix_fmt", "yuv420p"}, {"-c:a", "aac"}});
    return l;
    }();
    return v;
}

bool isImageSequence(const QString &n) { return n == "PNG Sequence" || n == "JPEG Sequence"; }
QString imageSequencePattern(const QString &path)
{
    const QFileInfo fi(path);
    return fi.dir().filePath(fi.completeBaseName() + "_%05d." + fi.suffix());
}

namespace ExportPresets {
QJsonObject toJson(const ExportSettings &s)
{
    return {{"format", s.format}, {"width", s.width}, {"height", s.height}, {"crf", s.crf}, {"kbps", s.kbps}, {"speed", s.preset}, {"audioKbps", s.audioKbps},
            {"video", s.video}, {"audio", s.audio}, {"srt", s.sidecarSrt}, {"lufs", s.loudnessLufs}, {"chapters", s.chapters}};
}
ExportSettings fromJson(const QJsonObject &o, ExportSettings s)
{
    s.format = o["format"].toString(s.format);
    s.width = o["width"].toInt(s.width); s.height = o["height"].toInt(s.height);
    s.crf = o["crf"].toInt(s.crf); s.kbps = o["kbps"].toInt(s.kbps);
    s.preset = o["speed"].toString(s.preset); s.audioKbps = o["audioKbps"].toInt(s.audioKbps);
    s.video = o["video"].toBool(s.video); s.audio = o["audio"].toBool(s.audio);
    s.sidecarSrt = o["srt"].toBool(s.sidecarSrt); s.loudnessLufs = o["lufs"].toDouble(s.loudnessLufs);
    s.chapters = o["chapters"].toBool(s.chapters);
    return s;
}
static QJsonObject all() { return QJsonDocument::fromJson(QSettings().value("export/presets").toByteArray()).object(); }
QStringList names() { return all().keys(); }
bool load(const QString &name, ExportSettings *out)
{
    const QJsonObject a = all();
    if (!a.contains(name)) return false;
    if (out) *out = fromJson(a[name].toObject(), *out);
    return true;
}
void save(const QString &name, const ExportSettings &s)
{
    QJsonObject a = all();
    a[name] = toJson(s);
    QSettings().setValue("export/presets", QJsonDocument(a).toJson(QJsonDocument::Compact));
}
void remove(const QString &name)
{
    QJsonObject a = all();
    a.remove(name);
    QSettings().setValue("export/presets", QJsonDocument(a).toJson(QJsonDocument::Compact));
}
}

Exporter::Exporter(const ProjectState &s, int id, const ExportSettings &c, QObject *p) : QThread(p), st(s), seqId(id), cfg(c) {}

// Measures the rendered mix and scales it to the target. The gain stops short of pushing samples
// past -1 dBFS, so a very dynamic mix may land a little under the target rather than clip.
bool Exporter::normalizeLoudness(const QString &path)
{
    emit progress(.05, tr("Measuring loudness..."));
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) return false;
    Dsp::LoudnessMeter meter;
    float peak = 0;
    for (QByteArray chunk = f.read(1 << 20); !chunk.isEmpty(); chunk = f.read(1 << 20)) {
        auto *s = reinterpret_cast<const float *>(chunk.constData());
        const qint64 n = chunk.size() / 8;
        meter.add(s, n);
        for (qint64 i = 0; i < n * 2; ++i) peak = qMax(peak, std::fabs(s[i]));
    }
    measuredLufs = meter.integrated();
    if (measuredLufs <= -90 || peak <= 0) return true;
    appliedGainDb = qMin(cfg.loudnessLufs - measuredLufs, 20 * std::log10(.891 / peak));
    const float g = float(std::pow(10.0, appliedGainDb / 20));
    f.seek(0);
    for (qint64 pos = 0;; pos += 1 << 20) {
        f.seek(pos);
        QByteArray chunk = f.read(1 << 20);
        if (chunk.isEmpty()) break;
        auto *s = reinterpret_cast<float *>(chunk.data());
        for (qint64 i = 0; i < chunk.size() / 4; ++i) s[i] *= g;
        f.seek(pos);
        if (f.write(chunk) != chunk.size()) return false;
    }
    return true;
}

void Exporter::run()
{
    const Sequence *q = st.seqById(seqId);
    if (!q) { emit failed("Sequence not found."); return; }
    const ExportFormat *fmt = nullptr;
    for (auto &f : exportFormats()) if (f.name == cfg.format) fmt = &f;
    if (!fmt) { emit failed("Unknown format."); return; }

    Frame first = qMax<Frame>(0, cfg.first), last = cfg.last > 0 ? cfg.last : q->duration();
    Frame total = last - first;
    if (total <= 0) { emit failed("Nothing to export: the range is empty."); return; }
    const bool withV = fmt->video && cfg.video, withA = fmt->audio && cfg.audio;
    if (!withV && !withA) { emit failed("Enable video or audio output."); return; }

    // Fail before rendering anything if the output cannot be written.
    const QFileInfo outInfo(cfg.path);
    if (!outInfo.dir().exists() && !QDir().mkpath(outInfo.absolutePath())) { emit failed(tr("The output folder does not exist and could not be created.")); return; }
    if (const qint64 space = Vault::freeBytes(cfg.path); space >= 0 && space < (64 << 20)) {
        emit failed(tr("The disk is almost full: only %1 MB left where the export would go.").arg(space >> 20));
        return;
    }
    const bool imageSeq = isImageSequence(fmt->name);
    // Rendered under a temporary name and renamed at the end, so a stopped or failed export never
    // leaves a broken file where a finished one is expected, and never destroys the old one.
    const QString outPath = imageSeq ? imageSequencePattern(cfg.path)
                                     : outInfo.dir().filePath(outInfo.completeBaseName() + ".part." + outInfo.suffix());

    QTemporaryDir tmp;
    QString wav = tmp.filePath("mix.f32");
    if (withA) {
        emit progress(0, tr("Rendering audio..."));
        AudioMixer mixer;
        mixer.tapMeters = false;
        mixer.setState(std::make_shared<const ProjectState>(st), seqId);
        QFile f(wav);
        if (!f.open(QIODevice::WriteOnly)) { emit failed("Cannot write temporary audio."); return; }
        const double spf = MediaCache::kRate / q->fps();
        qint64 a = qint64(first * spf), b = qint64(last * spf);
        const int blk = 4096;
        QVector<float> buf(blk * 2);
        for (qint64 p = a; p < b && !stop; p += blk) {
            int n = int(qMin<qint64>(blk, b - p));
            mixer.mix(buf.data(), n, p);
            f.write(reinterpret_cast<const char *>(buf.constData()), n * 8);
            if (((p - a) / blk) % 256 == 0) emit progress(0.05 * double(p - a) / (b - a), tr("Rendering audio..."));
        }
        f.close();
        if (cfg.loudnessLufs < 0 && !stop && !normalizeLoudness(wav)) { emit failed("Cannot read the temporary audio."); return; }
    }
    if (stop) { emit failed(tr("Export cancelled.")); return; }

    QStringList args{"-hide_banner", "-loglevel", "error", "-nostdin", "-y"};
    int w = cfg.width & ~1, h = cfg.height & ~1;
    if (withV)
        args << "-f" << "rawvideo" << "-pix_fmt" << "bgra" << "-s" << QString("%1x%2").arg(q->w & ~1).arg(q->h & ~1)
             << "-r" << QString("%1/%2").arg(q->fpsNum).arg(q->fpsDen) << "-i" << "pipe:0";
    if (withA) args << "-f" << "f32le" << "-ar" << QString::number(MediaCache::kRate) << "-ac" << "2" << "-i" << wav;
    static const QStringList chapterExts{"mp4", "mov", "mkv", "webm", "m4a"};
    if (cfg.chapters && chapterExts.contains(fmt->ext)) {
        const QString meta = Interchange::ffmetadataChapters(*q, first, last);
        QFile mf(tmp.filePath("chapters.txt"));
        if (!meta.isEmpty() && mf.open(QIODevice::WriteOnly)) {
            mf.write(meta.toUtf8());
            mf.close();
            const int idx = int(withV) + int(withA);
            args << "-f" << "ffmetadata" << "-i" << mf.fileName() << "-map_chapters" << QString::number(idx);
        }
    }
    if (withV) {
        args << fmt->videoArgs;
        const bool scaled = (w != (q->w & ~1) || h != (q->h & ~1));
        if (scaled && cfg.format != "Animated GIF") args << "-vf" << QString("scale=%1:%2:flags=lanczos").arg(w).arg(h);
        const QString name = fmt->name;
        if (name.contains("NVENC") || name.contains("Quick Sync") || name.contains("AMF")) args << "-b:v" << QString("%1k").arg(cfg.kbps > 0 ? cfg.kbps : 12000);
        if (name == "H.264" || name == "HEVC (H.265)") {
            args << "-preset" << cfg.preset;
            if (cfg.kbps > 0) args << "-b:v" << QString("%1k").arg(cfg.kbps);
            else args << "-crf" << QString::number(cfg.crf);
            if (name == "H.264") args << "-movflags" << "+faststart";
        } else if (name.startsWith("VP9") || name == "AV1") {
            if (cfg.kbps > 0) args << "-b:v" << QString("%1k").arg(cfg.kbps);
            else args << "-crf" << QString::number(qMin(63, cfg.crf + 12)) << "-b:v" << "0";
        }
    }
    if (withA) {
        args << fmt->audioArgs;
        if (fmt->audioArgs.contains("aac") || fmt->audioArgs.contains("libmp3lame") || fmt->audioArgs.contains("libopus"))
            args << "-b:a" << QString("%1k").arg(cfg.audioKbps);
    }
    if (imageSeq) args << "-start_number" << "1";
    else QFile::remove(outPath);
    args << outPath;

    PipeProc enc;
    if (!enc.start(CACHE->ffmpeg(), args, withV)) { emit failed("Could not start ffmpeg."); return; }

    if (withV) {
        Compositor comp;
        QElapsedTimer tm;
        tm.start();
        for (Frame f = first; f < last; ++f) {
            if (stop) break;
            QImage img = comp.render(st, seqId, f, 1.0, true);
            if (img.isNull() || !enc.write(img.constBits(), img.sizeInBytes())) { stop = true; break; }
            if ((f - first) % 4 == 0) {
                double fr = double(f - first + 1) / total;
                double remain = tm.elapsed() / 1000.0 * (1 / fr - 1);
                emit progress(.05 + fr * .95, tr("Frame %1 of %2 - about %3 s left").arg(f - first + 1).arg(total).arg(int(remain)));
            }
        }
        comp.releaseAll();
    }
    enc.closeStdin();
    if (stop) {
        enc.kill();
        enc.wait();
        // Windows keeps the file locked for a moment after the encoder dies.
        for (int i = 0; !imageSeq && QFileInfo::exists(outPath) && !QFile::remove(outPath) && i < 40; ++i) QThread::msleep(50);
        emit failed(tr("Export stopped."));
        return;
    }
    int code = enc.wait();
    QString result = cfg.path;
    if (imageSeq) {
        result = QString::asprintf(qPrintable(outPath), 1);
        if (code != 0 || !QFileInfo::exists(result)) { emit failed(tr("ffmpeg exited with code %1 while writing the image sequence.").arg(code)); return; }
    } else {
        if (code != 0 || !QFileInfo::exists(outPath) || QFileInfo(outPath).size() == 0) {
            QFile::remove(outPath);
            emit failed(tr("ffmpeg exited with code %1. The chosen codec may be unavailable in this ffmpeg build.").arg(code));
            return;
        }
        QFile::remove(cfg.path);
        if (!QFile::rename(outPath, cfg.path)) { emit failed(tr("The export finished but %1 could not be replaced. The new file is %2.").arg(outInfo.fileName(), QFileInfo(outPath).fileName())); return; }
    }
    if (cfg.sidecarSrt && !q->captions.isEmpty()) {
        QFile srt(QFileInfo(cfg.path).dir().filePath(QFileInfo(cfg.path).completeBaseName() + ".srt"));
        if (srt.open(QIODevice::WriteOnly)) srt.write(Analysis::writeSrt(q->captions, q->fps()).toUtf8());
    }
    emit finishedOk(result);
}
