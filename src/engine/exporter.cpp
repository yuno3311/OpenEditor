#include "exporter.h"
#include "../core/mediacache.h"
#include "audiodsp.h"
#include "audiomix.h"
#include "analysis.h"
#include "compositor.h"
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
        {"WAV (Audio Only)", "wav", false, true, {}, {"-c:a", "pcm_s16le"}},
        {"MP3 (Audio Only)", "mp3", false, true, {}, {"-c:a", "libmp3lame"}},
        {"AAC (Audio Only)", "m4a", false, true, {}, {"-c:a", "aac"}}};
    for (auto &hw : std::initializer_list<std::pair<const char *, const char *>>{{"H.264 (NVIDIA NVENC)", "h264_nvenc"}, {"HEVC (NVIDIA NVENC)", "hevc_nvenc"}, {"H.264 (Intel Quick Sync)", "h264_qsv"}, {"H.264 (AMD AMF)", "h264_amf"}})
        if (encoderWorks(hw.second)) l.insert(2, ExportFormat{hw.first, "mp4", true, true, {"-c:v", hw.second, "-pix_fmt", "yuv420p"}, {"-c:a", "aac"}});
    return l;
    }();
    return v;
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
    QFile::remove(cfg.path);
    args << cfg.path;

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
        QFile::remove(cfg.path);
        emit failed(tr("Export stopped."));
        return;
    }
    int code = enc.wait();
    if (code != 0 || !QFileInfo::exists(cfg.path)) { emit failed(tr("ffmpeg exited with code %1. The chosen codec may be unavailable in this ffmpeg build.").arg(code)); return; }
    if (cfg.sidecarSrt && !q->captions.isEmpty()) {
        QFile srt(QFileInfo(cfg.path).dir().filePath(QFileInfo(cfg.path).completeBaseName() + ".srt"));
        if (srt.open(QIODevice::WriteOnly)) srt.write(Analysis::writeSrt(q->captions, q->fps()).toUtf8());
    }
    emit finishedOk(cfg.path);
}
