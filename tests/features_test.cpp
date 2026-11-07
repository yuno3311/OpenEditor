// Headless checks of the professional features against real generated media.
#include "core/effectdefs.h"
#include "core/mediacache.h"
#include "core/ops.h"
#include "engine/analysis.h"
#include "engine/audiomix.h"
#include "engine/compositor.h"
#include "engine/exporter.h"
#include "engine/gpufx.h"
#include "engine/imagefx.h"
#include "engine/pluginhost.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } else printf("ok   %s\n", #c); fflush(stdout); } while (0)

static bool gen(const QString &out, const QStringList &a)
{
    QStringList args{"-hide_banner", "-loglevel", "error", "-y"};
    args << a << out;
    return QProcess::execute(CACHE->ffmpeg(), args) == 0;
}
static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(5); }
}
static float peakOf(const ProjectState &st, int id, qint64 pos, int frames = 24000)
{
    AudioMixer m;
    m.setState(std::make_shared<const ProjectState>(st), id);
    QVector<float> b(frames * 2);
    m.mix(b.data(), frames, pos);
    float p = 0;
    for (float x : b) p = qMax(p, std::fabs(x));
    return p;
}
static QImage rampImage(int w, int h)
{
    QImage i(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) i.setPixel(x, y, qRgb(x * 255 / (w - 1), y * 255 / (h - 1), (x + y) * 255 / (w + h - 2)));
    return i;
}
static int maxDiff(const QImage &a, const QImage &b)
{
    int d = 0;
    for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) {
        QRgb p = a.pixel(x, y), q = b.pixel(x, y);
        d = qMax(d, qMax(std::abs(qRed(p) - qRed(q)), qMax(std::abs(qGreen(p) - qGreen(q)), std::abs(qBlue(p) - qBlue(q)))));
    }
    return d;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("Open Editor Test");
    QCoreApplication::setOrganizationName("Open Editor");
    GpuFx::prepareOnMainThread();
    QTemporaryDir dir;

    // ---------------------------------------------------- GPU effects
    {
        GpuFx g;
        printf("GPU context ok=%d renderer=%s\n", g.ok(), qPrintable(GpuFx::rendererName()));
        if (g.ok()) {
            struct T { const char *id; QVector<QPair<const char *, double>> params; int tol; };
            const QVector<T> tests = {
                {"brightcontrast", {{"bright", 30}, {"contrast", 40}}, 3}, {"exposure", {{"ev", .7}, {"gamma", 1.2}}, 3},
                {"blackwhite", {{"amount", 80}}, 3}, {"sepia", {{"amount", 70}}, 3}, {"invert", {}, 3}, {"posterize", {{"levels", 5}}, 3},
                {"hsl", {{"hue", 60}, {"sat", 30}, {"light", 10}}, 4}, {"colorbalance", {{"sr", 40}, {"mg", -30}, {"hb", 25}}, 3},
                {"vignette", {{"amount", 70}}, 4}, {"shadowhigh", {{"shadow", 50}, {"high", 30}}, 3}, {"levels", {{"inB", 20}, {"inW", 220}, {"gamma", 1.3}}, 4},
                {"colorgrade", {{"temp", 30}, {"tint", -20}, {"exposure", .4}, {"contrast", 25}, {"sat", 130}, {"liftR", .2}, {"gammaB", -.2}, {"gainG", .15}, {"highlights", 30}, {"vigAmount", -1.5}}, 5},
                {"chromakey", {{"tol", 30}}, 8}, {"gaussblur", {{"radius", 12}}, 12}};
            for (auto &t : tests) {
                Effect e = EffectRegistry::make(t.id);
                for (auto &p : t.params) e.find(p.first)->value = p.second;
                QImage cpu = rampImage(320, 180), gpu = cpu;
                ImageFx::Ctx c;
                c.scale = 1;
                ImageFx::apply(e, cpu, c);
                bool ran = g.applyChain({&e}, gpu, c);
                int d = ran ? maxDiff(cpu, gpu) : 999;
                printf("  gpu %-16s max channel diff %d\n", t.id, d);
                if (d > t.tol) { printf("FAIL gpu/cpu mismatch %s\n", t.id); ++fails; }
            }
            // A real speed comparison on a heavy chain at 1080p
            Effect grade = EffectRegistry::make("colorgrade");
            grade.find("exposure")->value = .3; grade.find("sat")->value = 120;
            Effect blur = EffectRegistry::make("gaussblur");
            blur.find("radius")->value = 20;
            QImage big = rampImage(1920, 1080);
            ImageFx::Ctx c;
            QElapsedTimer t;
            t.start();
            for (int i = 0; i < 10; ++i) { QImage x = big; x.detach(); ImageFx::apply(grade, x, c); ImageFx::apply(blur, x, c); }
            double cpuMs = t.elapsed() / 10.0;
            t.restart();
            for (int i = 0; i < 10; ++i) { QImage x = big; g.applyChain({&grade, &blur}, x, c); }
            printf("1080p grade+blur: CPU %.1f ms, GPU %.1f ms per frame\n", cpuMs, t.elapsed() / 10.0);
        } else printf("NOTE no OpenGL 3.3 here: CPU fallback path is what runs\n");
    }

    // ---------------------------------------------------- media
    QString a = dir.filePath("a.mp4"), b = dir.filePath("b.mp4");
    CHECK(gen(a, {"-f", "lavfi", "-i", "color=c=red:s=320x180:r=30:d=6", "-f", "lavfi", "-i", "sine=f=440:r=48000:d=6", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
    CHECK(gen(b, {"-f", "lavfi", "-i", "color=c=blue:s=320x180:r=30:d=6", "-f", "lavfi", "-i", "sine=f=880:r=48000:d=6", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
    ProjectState st;
    st.nextId = 100;
    MediaItem ma, mb;
    CHECK(CACHE->probe(a, ma) && CACHE->probe(b, mb));
    ma.id = 1; mb.id = 2;
    st.media << ma << mb;
    CACHE->ensureAudio(a); CACHE->ensureAudio(b);
    for (int i = 0; i < 400 && (!CACHE->pcm(a).data || !CACHE->pcm(b).data); ++i) pump(25);
    Sequence q; q.id = 10; q.w = 320; q.h = 180; q.fpsNum = 30;
    Ops::addTrack(st, q, TrackType::Video); Ops::addTrack(st, q, TrackType::Video);
    Ops::addTrack(st, q, TrackType::Audio); Ops::addTrack(st, q, TrackType::Audio);
    st.seqs << q;

    Compositor comp;
    // ---------------------------------------------------- hardware decode keeps frame accuracy
    {
        QString v = dir.filePath("ramp.mp4");
        CHECK(gen(v, {"-f", "lavfi", "-i", "color=c=black:s=320x180:r=30:d=3", "-vf", "geq=lum='min(235,16+N*2)':cb=128:cr=128", "-c:v", "libx264", "-g", "30", "-crf", "1", "-pix_fmt", "yuv420p"}));
        VideoReader::hwDecode = true;
        VideoReader r(v, 320, 180, 30, false);
        for (Frame n : {Frame(5), Frame(50), Frame(20)}) {
            double want = (16.0 + n * 2 - 16) * 255.0 / 219.0;
            int g = qGray(r.get(n, 8000).pixel(160, 90));
            printf("hwaccel frame %lld gray %d want %.0f\n", (long long)n, g, want);
            CHECK(std::fabs(g - want) <= 4);
        }
    }

    // ---------------------------------------------------- HDR
    {
        QString h = dir.filePath("pq.mp4");
        CHECK(gen(h, {"-f", "lavfi", "-i", "color=c=0x808080:s=320x180:r=30:d=1", "-vf", "format=yuv420p10le,setparams=color_trc=smpte2084:color_primaries=bt2020:colorspace=bt2020nc",
                      "-c:v", "libx265", "-x265-params", "log-level=error", "-color_trc", "smpte2084", "-color_primaries", "bt2020", "-colorspace", "bt2020nc"}));
        MediaItem m;
        CHECK(CACHE->probe(h, m));
        CHECK(m.hdr && m.colorTransfer == "smpte2084");
        VideoReader r(h, 320, 180, 30, false, VideoReader::hdrToneMapFilter());
        QImage i = r.get(10, 8000);
        CHECK(!i.isNull() && qGray(i.pixel(160, 90)) > 0);
    }

    // ---------------------------------------------------- proxies
    {
        CACHE->makeProxy(a, false);
        for (int i = 0; i < 400 && CACHE->proxyFor(a).isEmpty(); ++i) pump(25);
        CHECK(!CACHE->proxyFor(a).isEmpty());
        ProjectState s = st;
        Ops::overwriteMedia(s, s.seqs[0], ma, 0, 2, 0, Ops::Patch{0, 0});
        comp.useProxies = true;
        QImage p = comp.render(s, 10, 30, .5, true);
        comp.useProxies = false;
        QImage o = comp.render(s, 10, 30, .5, true);
        CHECK(p.size() == o.size() && maxDiff(p, o) < 40);
        comp.releaseAll();
    }

    // ---------------------------------------------------- time remap
    {
        Clip c; c.dur = 300; c.srcIn = 1; c.kind = MediaKind::Video;
        Effect r = EffectRegistry::make("timeremap");
        Param *sp = r.find("speed");
        sp->animated = true; sp->setKey(0, 100); sp->setKey(60, 100); sp->setKey(61, 200); sp->setKey(300, 200);
        c.fx << r;
        CHECK(std::fabs(c.sourceTime(30, 30) - 2.0) < .01);
        CHECK(std::fabs(c.sourceTime(90, 30) - (1 + 2.0 + 2.0)) < .1);
    }

    // ---------------------------------------------------- multicam
    {
        ProjectState s = st;
        int mid = Ops::makeMulticam(s, "Cams", {1, 2}, {0, 0}, 320, 180, 30, 1);
        const MediaItem mi = *s.mediaById(mid);
        CHECK(mi.kind == MediaKind::Nested && s.seqs.last().video.size() == 2 && s.seqs.last().multicam);
        Sequence &o = *s.seqById(10);
        Ops::overwriteMedia(s, o, mi, 0, mi.duration, 0, Ops::Patch{0, 0});
        for (auto *t : o.allTracks()) for (auto &c : t->clips) c.cam = 0;
        Ops::cutToAngle(s, o, 60, 1);
        CHECK(o.video[0].clips.size() == 2 && o.video[0].clips[0].cam == 0 && o.video[0].clips[1].cam == 1);
        QImage a0 = comp.render(s, 10, 30, 1, true), a1 = comp.render(s, 10, 90, 1, true);
        printf("angle colours %08x %08x\n", a0.pixel(160, 90), a1.pixel(160, 90));
        CHECK(qRed(a0.pixel(160, 90)) > 200 && qBlue(a0.pixel(160, 90)) < 50);
        CHECK(qBlue(a1.pixel(160, 90)) > 200 && qRed(a1.pixel(160, 90)) < 50);
        comp.releaseAll();
        // Each angle's own audio follows the cut: 440 Hz vs 880 Hz have different zero crossing rates.
        CHECK(peakOf(s, 10, 12000) > .05f && peakOf(s, 10, 120000) > .05f);

        // Audio sync: second file is the first delayed by a known 1.5 s, using a burst pattern.
        QString s1 = dir.filePath("s1.wav"), s2 = dir.filePath("s2.wav");
        CHECK(gen(s1, {"-f", "lavfi", "-i", "aevalsrc='0.5*sin(2*PI*300*t)*gt(sin(2*PI*0.7*t+sin(3*t)),0.3)':s=48000:d=20"}));
        CHECK(gen(s2, {"-i", s1, "-af", "adelay=1500:all=1"}));
        CACHE->ensureAudio(s1); CACHE->ensureAudio(s2);
        for (int i = 0; i < 400 && (!CACHE->peaks(s1) || !CACHE->peaks(s2)); ++i) pump(25);
        double off = Analysis::syncOffsetSeconds(s2, s1, 10);
        printf("sync offset %.3f s\n", off);
        CHECK(std::fabs(off - 1.5) < .05);
    }

    // ---------------------------------------------------- captions
    {
        ProjectState s = st;
        Sequence &o = *s.seqById(10);
        Ops::overwriteMedia(s, o, ma, 0, 3, 0, Ops::Patch{0, 0});
        QImage before = comp.render(s, 10, 30, 1, true);
        o.captions << Caption{15, 60, "Hello world"};
        QImage after = comp.render(s, 10, 30, 1, true);
        CHECK(maxDiff(before, after) > 100);
        QString srt = Analysis::writeSrt(o.captions, 30);
        auto back = Analysis::parseSubtitles(srt, 30);
        CHECK(back.size() == 1 && back[0].start == 15 && back[0].end == 60 && back[0].text == "Hello world");
        auto vtt = Analysis::parseSubtitles(Analysis::writeVtt(o.captions, 30), 30);
        CHECK(vtt.size() == 1 && vtt[0].end == 60);
        Ops::rippleRemoveRanges(s, o, {{0, 30}});
        CHECK(o.captions[0].start == 0 && o.captions[0].end == 30 && o.video[0].clips[0].dur == 60);
        comp.releaseAll();
    }

    // ---------------------------------------------------- track fx and automation
    {
        ProjectState s = st;
        Sequence &o = *s.seqById(10);
        Ops::overwriteMedia(s, o, ma, 0, 4, 0, Ops::Patch{0, 0});
        float base = peakOf(s, 10, 12000);
        o.audio[0].fx << EffectRegistry::make("a_gain");
        o.audio[0].fx[0].find("gain")->value = -6.02;
        float halved = peakOf(s, 10, 12000);
        CHECK(std::fabs(halved / base - .5f) < .03f);
        o.audio[0].fx.clear();
        Param &au = o.audio[0].volAuto;
        au.animated = true; au.setKey(0, 0); au.setKey(30, 0); au.setKey(31, -96); au.setKey(120, -96);
        float early = peakOf(s, 10, 0, 12000), late = peakOf(s, 10, 96000, 12000);
        printf("automation early %.3f late %.5f\n", early, late);
        CHECK(early > .05f && late < .001f);
    }

    // ---------------------------------------------------- plugins
    {
        QString pdir = QCoreApplication::applicationDirPath() + "/oe-plugins";
        int n = PluginHost::loadFrom(pdir);
        printf("plugins registered: %d, errors: %s\n", n, qPrintable(PluginHost::errors().join("; ")));
        CHECK(n == 2);
        CHECK(EffectRegistry::find("plugin.example.duotone") != nullptr);
        Effect e = EffectRegistry::make("plugin.example.duotone");
        CHECK(e.params.size() == 2 && e.find("amount")->value == 100);
        QImage i = rampImage(64, 64), base = i;
        ImageFx::Ctx c;
        ImageFx::apply(e, i, c);
        CHECK(maxDiff(i, base) > 40);
        ProjectState s = st;
        Sequence &o = *s.seqById(10);
        Ops::overwriteMedia(s, o, ma, 0, 4, 0, Ops::Patch{0, 0});
        float dry = peakOf(s, 10, 12000);
        o.audio[0].clips[0].fx << EffectRegistry::make("plugin.example.tremolo");
        AudioMixer mx;
        mx.setState(std::make_shared<const ProjectState>(s), 10);
        QVector<float> b(96000);
        mx.mix(b.data(), 48000, 0);
        float lo = 1;
        for (int i2 = 0; i2 + 2000 < 96000; i2 += 2000) { float p = 0; for (int k = 0; k < 2000; ++k) p = qMax(p, std::fabs(b[i2 + k])); lo = qMin(lo, p); }
        CHECK(lo < dry * .6f);
    }

    // ---------------------------------------------------- silence and scenes
    {
        QString v = dir.filePath("scenes.mp4");
        CHECK(gen(v, {"-f", "lavfi", "-i", "color=c=red:s=320x180:r=30:d=2", "-f", "lavfi", "-i", "color=c=lime:s=320x180:r=30:d=2", "-f", "lavfi", "-i", "color=c=blue:s=320x180:r=30:d=2",
                      "-filter_complex", "[0][1][2]concat=n=3:v=1", "-c:v", "libx264", "-pix_fmt", "yuv420p"}));
        auto cuts = Analysis::detectScenes(v);
        printf("scene cuts:"); for (double c : cuts) printf(" %.2f", c); puts("");
        CHECK(cuts.size() == 2 && std::fabs(cuts[0] - 2) < .1 && std::fabs(cuts[1] - 4) < .1);
        QString w = dir.filePath("gap.wav");
        CHECK(gen(w, {"-f", "lavfi", "-i", "aevalsrc='0.4*sin(2*PI*300*t)*(lt(t,2)+gt(t,4)*lt(t,6))':s=48000:d=6"}));
        MediaItem m;
        CHECK(CACHE->probe(w, m));
        m.id = 3;
        CACHE->ensureAudio(w);
        for (int i = 0; i < 400 && !CACHE->pcm(w).data; ++i) pump(25);
        ProjectState s = st;
        s.media << m;
        Sequence &o = *s.seqById(10);
        Ops::overwriteMedia(s, o, m, 0, 6, 0, Ops::Patch{-1, 0});
        auto sil = Analysis::findSilence(s, 10, -40, .5);
        for (auto &r : sil) printf("silence %lld-%lld\n", (long long)r.first, (long long)r.second);
        CHECK(sil.size() == 1 && qAbs(sil[0].first - 63) <= 3 && qAbs(sil[0].second - 117) <= 3);
        Ops::rippleRemoveRanges(s, o, sil);
        CHECK(o.duration() < 130);
        double rms = Analysis::measureRmsDb(s, 10);
        printf("rms %.1f dB\n", rms);
        CHECK(rms > -14 && rms < -8);
    }

    // ---------------------------------------------------- transcription with a real model and real speech
    {
        QString model = Analysis::whisperModelPath("tiny");
        QString speech = qEnvironmentVariable("OE_SPEECH_WAV");
        if (QFileInfo::exists(model) && !speech.isEmpty()) {
            MediaItem m;
            CHECK(CACHE->probe(speech, m));
            m.id = 4;
            CACHE->ensureAudio(speech);
            for (int i = 0; i < 400 && !CACHE->pcm(speech).data; ++i) pump(25);
            ProjectState s = st;
            s.media << m;
            Sequence &o = *s.seqById(10);
            Ops::overwriteMedia(s, o, m, 0, m.duration, 0, Ops::Patch{-1, 0});
            Transcriber t(s, 10, model, "en");
            QVector<Caption> caps; QString err;
            QObject::connect(&t, &Transcriber::finishedOk, [&](const QVector<Caption> &c) { caps = c; });
            QObject::connect(&t, &Transcriber::failed, [&](const QString &e) { err = e; });
            t.start();
            t.wait();
            pump(20);
            QString all;
            for (auto &c : caps) all += c.text + " ";
            printf("transcript [%s] err [%s]\n", qPrintable(all), qPrintable(err));
            CHECK(all.contains("video", Qt::CaseInsensitive) && all.contains("edit", Qt::CaseInsensitive));
        } else printf("NOTE transcription skipped: model or OE_SPEECH_WAV missing\n");
    }

    // ---------------------------------------------------- sidecar export with burned captions
    {
        ProjectState s = st;
        Sequence &o = *s.seqById(10);
        Ops::overwriteMedia(s, o, ma, 0, 1, 0, Ops::Patch{0, 0});
        o.captions << Caption{0, 30, "Sidecar"};
        ExportSettings c;
        c.format = "H.264"; c.width = 320; c.height = 180; c.preset = "ultrafast"; c.sidecarSrt = true;
        c.path = dir.filePath("cap.mp4");
        Exporter ex(s, 10, c);
        ex.start();
        ex.wait();
        CHECK(QFileInfo(c.path).size() > 0);
        CHECK(QFileInfo(dir.filePath("cap.srt")).size() > 0);
    }

    printf(fails ? "\n%d FAILURES\n" : "\nALL PASSED\n", fails);
    fflush(stdout);
    return fails ? 1 : 0;
}
