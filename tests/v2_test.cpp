// Checks for the 2.0 features: each effect is measured against what it should do, not just run.
#include "core/effectdefs.h"
#include "core/mediacache.h"
#include "core/ops.h"
#include "engine/analysis.h"
#include "engine/audiodsp.h"
#include "engine/audiomix.h"
#include "engine/exporter.h"
#include "engine/imagefx.h"
#include "engine/stabilize.h"
#include "text_ref_styles.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <cstdio>
#include <random>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } else printf("ok   %s\n", #c); fflush(stdout); } while (0)
#define NEAR(a, b, tol) do { double _a = (a), _b = (b); bool _ok = std::fabs(_a - _b) <= (tol); printf("%s %s = %.2f (want %.2f +/- %.2f)\n", _ok ? "ok  " : "FAIL", #a, _a, _b, double(tol)); fflush(stdout); fails += !_ok; } while (0)

static const int R = 48000;

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
// Stereo buffer built sample by sample from f(t).
template <class F> static QVector<float> make(double sec, F f)
{
    QVector<float> v(int(sec * R) * 2);
    for (int i = 0; i < v.size() / 2; ++i) v[i * 2] = v[i * 2 + 1] = float(f(double(i) / R, i));
    return v;
}
static double rmsDb(const QVector<float> &v, double a, double b)
{
    double s = 0;
    int n = 0;
    for (int i = int(a * R); i < int(b * R) && i < v.size() / 2; ++i, ++n) s += double(v[i * 2]) * v[i * 2];
    return 10 * std::log10(qMax(1e-20, s / qMax(1, n)));
}
// Level of one frequency over a stretch (Goertzel), in dB relative to a full-scale sine.
static double toneDb(const QVector<float> &v, double f, double a, double b)
{
    const int i0 = int(a * R), n = int((b - a) * R);
    const double w = 2 * M_PI * f / R, cw = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) { double s = v[(i0 + i) * 2] + cw * s1 - s2; s2 = s1; s1 = s; }
    double p = s1 * s1 + s2 * s2 - cw * s1 * s2;
    return 10 * std::log10(qMax(1e-20, p)) - 20 * std::log10(n / 2.0);
}
// Tone level as heard: power averaged over 50 ms windows, so small phase steps do not hide it.
static double toneDbShort(const QVector<float> &v, double f, double a, double b)
{
    double p = 0;
    int n = 0;
    for (double t = a; t + .05 <= b; t += .05, ++n) p += std::pow(10, toneDb(v, f, t, t + .05) / 10);
    return n ? 10 * std::log10(p / n) : -200;
}
static QVector<float> runFx(const QString &id, QVector<float> v, const QVector<QPair<const char *, double>> &params = {})
{
    Effect e = EffectRegistry::make(id);
    for (auto &p : params) { CHECK(e.find(p.first)); if (auto *q = e.find(p.first)) q->value = p.second; }
    AudioMixer m;
    for (int i = 0; i < v.size() / 2; i += 512) m.process(e, v.data() + i * 2, qMin(512, v.size() / 2 - i));
    return v;
}
static bool finite(const QVector<float> &v)
{
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}
static double lufsOf(const QVector<float> &v)
{
    Dsp::LoudnessMeter m;
    m.add(v.constData(), v.size() / 2);
    return m.integrated();
}
static double ffmpegLufs(const QString &file)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(CACHE->ffmpeg(), {"-hide_banner", "-nostdin", "-i", file, "-af", "ebur128", "-f", "null", "-"});
    p.waitForFinished(60000);
    auto m = QRegularExpression("I:\\s+(-?[0-9.]+) LUFS").globalMatch(QString::fromUtf8(p.readAll()));
    double last = 0;
    while (m.hasNext()) last = m.next().captured(1).toDouble();
    return last;
}
static QImage ramp(int w, int h)
{
    QImage i(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) i.setPixel(x, y, qRgb(x * 255 / qMax(1, w - 1), y * 255 / qMax(1, h - 1), 90));
    return i;
}
// Ramp with an 8 px checker on top, so warps of any kind show.
static QImage checker(int w, int h)
{
    QImage i = ramp(w, h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (((x / 8) + (y / 8)) & 1) i.setPixel(x, y, qRgb(qRed(i.pixel(x, y)) / 3, 40, 200));
    return i;
}
static QImage solid(QRgb c) { QImage i(64, 36, QImage::Format_ARGB32_Premultiplied); i.fill(c); return i; }
static int maxDiff(const QImage &a, const QImage &b)
{
    int d = 0;
    for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) {
        QRgb p = a.pixel(x, y), q = b.pixel(x, y);
        d = qMax(d, qMax(qMax(std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q))), qMax(std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q)))));
    }
    return d;
}
static QImage fx(const QString &id, QImage img, const QVector<QPair<const char *, double>> &params = {}, double seconds = 0)
{
    Effect e = EffectRegistry::make(id);
    for (auto &p : params) if (auto *q = e.find(p.first)) q->value = p.second;
    ImageFx::Ctx c;
    c.seconds = seconds;
    ImageFx::apply(e, img, c);
    return img;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("Open Editor Test");
    QCoreApplication::setOrganizationName("Open Editor");
    // --text-ref <dir>: write the reference title renders (run once, from the 2.0 renderer).
    if (argc > 2 && QString(argv[1]) == "--text-ref") {
        QDir().mkpath(argv[2]);
        for (auto &s : textRefStyles())
            for (double sc : {.5, 1.0}) ImageFx::renderText(s.second, int(1280 * sc), int(720 * sc), sc).save(QString("%1/%2_%3.png").arg(argv[2], s.first).arg(int(sc * 100)));
        return 0;
    }
    QTemporaryDir dir;
    std::mt19937 rng(7);
    std::normal_distribution<double> gauss(0, 1);

    printf("--- loudness (BS.1770 reference: a -23 dBFS 1 kHz stereo sine reads -23 LUFS)\n");
    NEAR(lufsOf(make(10, [](double t, int) { return std::pow(10, -23 / 20.0) * std::sin(2 * M_PI * 997 * t); })), -23, .2);
    NEAR(lufsOf(make(10, [](double t, int) { return std::pow(10, -18 / 20.0) * std::sin(2 * M_PI * 997 * t); })), -18, .2);
    CHECK(lufsOf(make(3, [](double, int) { return 0.0; })) < -90);

    // Speech-like test signal: noise throughout, 1 kHz bursts of 0.3 s every 0.5 s after the first second.
    const double noiseAmp = std::pow(10, -40 / 20.0);
    auto speechy = make(6, [&](double t, int) {
        bool on = t >= 1 && std::fmod(t - 1, .5) < .3;
        return noiseAmp * gauss(rng) + (on ? .25 * std::sin(2 * M_PI * 1000 * t) : 0);
    });
    const double lat = SpectralDenoiser::kLatency / double(R);
    const double gapA = 4.33 + lat, gapB = 4.47 + lat, burstA = 4.55 + lat, burstB = 4.75 + lat;

    printf("--- DeNoise\n");
    {
        auto out = runFx("a_denoise", speechy, {{"reduce", 24}});
        CHECK(finite(out));
        double cut = rmsDb(speechy, 4.33, 4.47) - rmsDb(out, gapA, gapB);
        printf("     noise between words reduced by %.1f dB\n", cut);
        CHECK(cut >= 10);
        NEAR(toneDb(out, 1000, burstA, burstB), toneDb(speechy, 1000, 4.55, 4.75), 3);
    }
    printf("--- Enhance Speech\n");
    {
        auto out = runFx("a_enhance", speechy);
        CHECK(finite(out));
        double snrIn = toneDb(speechy, 1000, 4.55, 4.75) - rmsDb(speechy, 4.33, 4.47);
        double snrOut = toneDb(out, 1000, burstA, burstB) - rmsDb(out, gapA, gapB);
        printf("     speech-to-noise %.1f dB -> %.1f dB\n", snrIn, snrOut);
        CHECK(snrOut - snrIn >= 8);
        float pk = 0;
        for (float x : out) pk = qMax(pk, std::fabs(x));
        CHECK(pk <= .892f);
        auto quiet = runFx("a_enhance", make(8, [](double t, int) { return .01 * std::sin(2 * M_PI * 300 * t); }), {{"amount", 0}});
        printf("     leveller lifts a -43 dB voice to %.1f dB RMS\n", rmsDb(quiet, 6, 8));
        CHECK(rmsDb(quiet, 6, 8) > -32);
    }
    printf("--- DeHum\n");
    {
        auto in = make(3, [](double t, int) { return .25 * std::sin(2 * M_PI * 60 * t) + .25 * std::sin(2 * M_PI * 1000 * t) + .1 * std::sin(2 * M_PI * 180 * t); });
        auto out = runFx("a_dehum", in);
        printf("     60 Hz %.1f -> %.1f dB, 180 Hz %.1f -> %.1f dB\n", toneDb(in, 60, 2, 3), toneDb(out, 60, 2, 3), toneDb(in, 180, 2, 3), toneDb(out, 180, 2, 3));
        CHECK(toneDb(in, 60, 2, 3) - toneDb(out, 60, 2, 3) >= 20);
        CHECK(toneDb(in, 180, 2, 3) - toneDb(out, 180, 2, 3) >= 20);
        NEAR(toneDb(out, 1000, 2, 3), toneDb(in, 1000, 2, 3), 1);
        auto out50 = runFx("a_dehum", make(3, [](double t, int) { return .25 * std::sin(2 * M_PI * 50 * t); }), {{"freq", 0}});
        CHECK(toneDb(out50, 50, 2, 3) < -30);
    }
    printf("--- Noise Gate\n");
    {
        auto in = make(3, [&](double t, int) { return t >= 1 && t < 2 ? .3 * std::sin(2 * M_PI * 440 * t) : .001 * gauss(rng); });
        auto out = runFx("a_gate", in);
        printf("     hiss %.1f -> %.1f dB\n", rmsDb(in, 2.5, 3), rmsDb(out, 2.5, 3));
        CHECK(rmsDb(in, 2.5, 3) - rmsDb(out, 2.5, 3) >= 40);
        NEAR(rmsDb(out, 1.2, 1.9), rmsDb(in, 1.2, 1.9), .5);
    }
    printf("--- DeEsser\n");
    {
        auto ess = make(2, [](double t, int) { return .5 * std::sin(2 * M_PI * 7000 * t); });
        auto low = make(2, [](double t, int) { return .5 * std::sin(2 * M_PI * 300 * t); });
        double cut = toneDb(ess, 7000, 1, 2) - toneDb(runFx("a_deess", ess), 7000, 1, 2);
        printf("     7 kHz sibilance reduced by %.1f dB\n", cut);
        CHECK(cut >= 6);
        NEAR(toneDb(runFx("a_deess", low), 300, 1, 2), toneDb(low, 300, 1, 2), .5);
    }
    printf("--- Chorus/Flanger\n");
    for (int mode : {0, 1}) {
        auto in = make(2, [](double t, int) { return .3 * std::sin(2 * M_PI * 440 * t); });
        auto out = runFx("a_chorus", in, {{"mode", mode}});
        CHECK(finite(out));
        double diff = 0;
        for (int i = 0; i < in.size(); ++i) diff = qMax(diff, double(std::fabs(in[i] - out[i])));
        CHECK(diff > .05);
    }

    printf("--- video effects\n");
    {
        const QStringList ids = {"leavecolor", "colorkey", "findedges", "emboss", "twirl", "spherize", "replicate", "linearwipe", "ramp", "strobe", "stabilize"};
        for (const QString &id : ids) {
            CHECK(EffectRegistry::find(id));
            for (QSize s : {QSize(1, 1), QSize(2, 2), QSize(3, 7), QSize(160, 90)}) fx(id, ramp(s.width(), s.height()));
        }
        printf("ok   every new video effect runs on 1x1 to 160x90 images\n");
        const QImage r = checker(160, 90);
        for (const char *id : {"findedges", "emboss", "twirl", "spherize", "replicate", "ramp", "strobe"}) {
            int d = maxDiff(fx(id, r), r);
            printf("%s %s changes the picture (max diff %d)\n", d > 10 ? "ok  " : "FAIL", id, d);
            fails += d <= 10;
        }
        CHECK(maxDiff(fx("twirl", r, {{"angle", 0}}), r) <= 1);
        CHECK(maxDiff(fx("spherize", r, {{"amount", 0}}), r) <= 1);
        CHECK(maxDiff(fx("linearwipe", r, {{"done", 0}}), r) == 0);
        QImage gone = fx("linearwipe", r, {{"done", 100}});
        CHECK(qAlpha(gone.pixel(0, 0)) == 0 && qAlpha(gone.pixel(159, 89)) == 0);
        QImage half = fx("linearwipe", r, {{"done", 50}});
        CHECK(qAlpha(half.pixel(10, 45)) == 0 && qAlpha(half.pixel(150, 45)) == 255);
        CHECK(qAlpha(fx("colorkey", solid(qRgb(0, 0, 255))).pixel(5, 5)) == 0);
        CHECK(maxDiff(fx("colorkey", solid(qRgb(200, 30, 30))), solid(qRgb(200, 30, 30))) == 0);
        CHECK(maxDiff(fx("leavecolor", solid(qRgb(208, 48, 48))), solid(qRgb(208, 48, 48))) <= 1);
        QRgb grey = fx("leavecolor", solid(qRgb(30, 60, 220))).pixel(5, 5);
        CHECK(std::abs(qRed(grey) - qBlue(grey)) <= 2 && std::abs(qGreen(grey) - qBlue(grey)) <= 2);
        CHECK(maxDiff(fx("strobe", r, {}, .3), r) == 0);
        QImage rep = fx("replicate", r, {{"count", 2}});
        CHECK(maxDiff(rep.copy(0, 0, 80, 45), rep.copy(80, 45, 80, 45)) <= 1);
        CHECK(maxDiff(fx("stabilize", r), r) == 0); // not analysed yet: leaves the picture alone
    }

    printf("--- Warp Stabilizer\n");
    {
        QVector<unsigned char> a(192 * 108), b(192 * 108);
        std::uniform_int_distribution<int> px(0, 255);
        for (auto &v : a) v = (unsigned char)px(rng);
        for (int y = 0; y < 108; ++y) for (int x = 0; x < 192; ++x) b[y * 192 + x] = a[qBound(0, y + 2, 107) * 192 + qBound(0, x - 3, 191)];
        double dx, dy;
        Stabilize::estimateShift(a.constData(), b.constData(), 192, 108, dx, dy);
        NEAR(dx, 3, .25);
        NEAR(dy, -2, .25);

        // A shaky camera: a 560x315 window wobbling over a still 640x360 scene.
        const QString scene = dir.filePath("scene.png"), shaky = dir.filePath("shaky.mp4"), raw = dir.filePath("shaky.gray");
        {
            QImage sc(640, 360, QImage::Format_RGB32);
            sc.fill(qRgb(70, 90, 110));
            QPainter p(&sc);
            p.setRenderHint(QPainter::Antialiasing);
            std::uniform_int_distribution<int> X(0, 640), Y(0, 360), S(8, 70), C(0, 255);
            for (int i = 0; i < 260; ++i) {
                p.setBrush(QColor(C(rng), C(rng), C(rng)));
                p.setPen(QPen(QColor(C(rng), C(rng), C(rng)), 2));
                if (i % 2) p.drawEllipse(X(rng), Y(rng), S(rng), S(rng));
                else p.drawRect(X(rng), Y(rng), S(rng), S(rng));
            }
            p.end();
            CHECK(sc.save(scene));
        }
        CHECK(gen(shaky, {"-loop", "1", "-framerate", "30", "-t", "4", "-i", scene, "-vf", "crop=560:316:40+30*sin(n/3):22+20*cos(n/4),setsar=1", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "12"}));
        auto res = Stabilize::analyze(CACHE->ffmpeg(), shaky, 0, 4, 560, 316, 30);
        CHECK(res.error.isEmpty());
        const QVariantList tx = res.extra.value("tx").toList(), ty = res.extra.value("ty").toList();
        printf("     tracked %lld frames\n", (long long)tx.size());
        CHECK(tx.size() >= 115 && tx.size() <= 121);
        double worst = 0;
        for (int n = 0; n < tx.size(); ++n) {
            // The window moving right by d moves the picture left by d.
            double ex = -(30 * std::sin(n / 3.0) - 0) / 560, ey = -(20 * std::cos(n / 4.0) - 20) / 560;
            worst = qMax(worst, qMax(std::fabs(tx[n].toDouble() - ex), std::fabs(ty[n].toDouble() - ey)) * 560);
            if (n < 8 || n % 20 == 0) printf("     frame %3d  x %7.2f want %7.2f   y %7.2f want %7.2f\n", n, tx[n].toDouble() * 560, ex * 560, ty[n].toDouble() * 560, ey * 560);
        }
        // The path is a running sum of per-frame shifts, so small errors add up over the clip. Under
        // two analysis pixels after four seconds is drift that the path smoothing absorbs.
        printf("     worst tracking error %.2f source pixels (%.2f analysis pixels)\n", worst, worst * 192 / 560);
        CHECK(worst * 192 / 560 < 2);

        // Render each frame through the effect and measure what shake is left.
        CHECK(gen(raw, {"-i", shaky, "-vf", "scale=192:108,format=gray", "-f", "rawvideo"}));
        QFile f(raw);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray all = f.readAll();
        const int fb = 192 * 108, frames = int(all.size() / fb);
        Effect e = EffectRegistry::make("stabilize");
        e.extra = res.extra;
        e.find("smooth")->value = 100;
        QVector<QByteArray> steady;
        for (int n = 0; n < frames; ++n) {
            QImage img(192, 108, QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < 108; ++y) for (int x = 0; x < 192; ++x) { int v = (unsigned char)all[n * fb + y * 192 + x]; img.setPixel(x, y, qRgb(v, v, v)); }
            ImageFx::Ctx c;
            c.srcSec = n / 30.0;
            ImageFx::apply(e, img, c);
            QByteArray g(fb, 0);
            for (int y = 0; y < 108; ++y) for (int x = 0; x < 192; ++x) g[y * 192 + x] = char(qGray(img.pixel(x, y)));
            steady << g;
        }
        double before = 0, after = 0;
        for (int n = 1; n < frames; ++n) {
            double x0, y0, x1, y1;
            Stabilize::estimateShift(reinterpret_cast<const unsigned char *>(all.constData() + (n - 1) * fb), reinterpret_cast<const unsigned char *>(all.constData() + n * fb), 192, 108, x0, y0);
            Stabilize::estimateShift(reinterpret_cast<const unsigned char *>(steady[n - 1].constData()), reinterpret_cast<const unsigned char *>(steady[n].constData()), 192, 108, x1, y1);
            before += std::hypot(x0, y0);
            after += std::hypot(x1, y1);
        }
        printf("     frame-to-frame shake %.2f px before, %.2f px after\n", before / (frames - 1), after / (frames - 1));
        CHECK(after < before * .35);
    }

    printf("--- Auto Ducking, filler words, loudness on export\n");
    {
        const QString dlg = dir.filePath("dialogue.wav"), mus = dir.filePath("music.wav");
        CHECK(gen(dlg, {"-f", "lavfi", "-i", "aevalsrc='if(between(t,1,2)+between(t,4,5),0.5*sin(2*PI*1000*t),0)':s=48000:d=6", "-ac", "2"}));
        CHECK(gen(mus, {"-f", "lavfi", "-i", "sine=f=440:r=48000:d=6", "-ac", "2"}));
        ProjectState st;
        st.nextId = 100;
        MediaItem md, mm;
        CHECK(CACHE->probe(dlg, md) && CACHE->probe(mus, mm));
        md.id = 1; mm.id = 2;
        st.media << md << mm;
        CACHE->ensureAudio(dlg); CACHE->ensureAudio(mus);
        for (int i = 0; i < 400 && (!CACHE->pcm(dlg).data || !CACHE->pcm(mus).data); ++i) pump(25);
        Sequence q; q.id = 10; q.w = 320; q.h = 180; q.fpsNum = 30;
        Ops::addTrack(st, q, TrackType::Audio); Ops::addTrack(st, q, TrackType::Audio);
        Ops::overwriteMedia(st, q, md, 0, md.duration, 0, Ops::Patch{-1, 0});
        Ops::overwriteMedia(st, q, mm, 0, mm.duration, 0, Ops::Patch{-1, 1});
        st.seqs << q;
        st.activeSeq = q.id;
        const int musicTrack = q.audio[1].id;
        auto regions = Analysis::dialogueRegions(st, q.id, {musicTrack});
        CHECK(regions.size() == 2);
        if (regions.size() == 2) {
            printf("     dialogue at frames %lld-%lld and %lld-%lld\n", (long long)regions[0].first, (long long)regions[0].second, (long long)regions[1].first, (long long)regions[1].second);
            CHECK(std::abs(regions[0].first - 30) <= 2 && std::abs(regions[0].second - 60) <= 2);
            CHECK(std::abs(regions[1].first - 120) <= 2 && std::abs(regions[1].second - 150) <= 2);
        }
        Sequence &sq = *st.seqById(q.id);
        CHECK(Analysis::applyDucking(sq, {musicTrack}, regions, -15, 15) == 2);
        const Clip &mc = sq.audio[1].clips[0];
        const Param *lv = mc.effect("volume")->find("level");
        NEAR(lv->at(mc.kt(45)), -15, .01);
        NEAR(lv->at(mc.kt(90)), 0, .01);
        NEAR(lv->at(mc.kt(22)), -7.5, .6); // halfway down the fade-in
        CHECK(sq.audio[0].clips[0].effect("volume")->find("level")->keys.isEmpty()); // dialogue untouched
        CHECK(Analysis::dialogueRegions(st, q.id, {musicTrack}).size() == 2);

        // Loudness: our meter against FFmpeg's own ebur128, then an export normalised to -16 LUFS.
        const double ours = Analysis::measureLufs(st, q.id);
        const QString wav = dir.filePath("mix.wav"), norm = dir.filePath("norm.wav");
        ExportSettings es;
        es.path = wav; es.format = "WAV (Audio Only)"; es.video = false;
        auto exportWith = [&](const ExportSettings &s) {
            Exporter ex(st, q.id, s);
            bool ok = false;
            QObject::connect(&ex, &Exporter::finishedOk, [&](const QString &) { ok = true; });
            ex.start();
            ex.wait();
            return ok;
        };
        CHECK(exportWith(es));
        NEAR(ours, ffmpegLufs(wav), .3);
        es.path = norm; es.loudnessLufs = -16;
        CHECK(exportWith(es));
        NEAR(ffmpegLufs(norm), -16, .5);

        Sequence cq;
        Caption cap;
        auto w = [](Frame a, Frame b, const char *t) { Word x; x.start = a; x.end = b; x.text = QString::fromUtf8(t); return x; };
        cap.words = {w(0, 10, "So"), w(10, 20, "um,"), w(20, 30, "this"), w(30, 36, "uh"), w(36, 40, "Uhm."), w(40, 50, "works"), w(50, 60, "\xec\x9d\x8c"), w(60, 70, "\xec\x9d\x8c\xec\x8b\x9d")};
        cq.captions << cap;
        QStringList found;
        auto fr = Analysis::fillerWordRanges(cq, &found);
        printf("     fillers: %s\n", qPrintable(found.join(" ")));
        CHECK(fr.size() == 3);
        if (fr.size() == 3) CHECK(fr[0] == qMakePair(Frame(10), Frame(20)) && fr[1] == qMakePair(Frame(30), Frame(40)) && fr[2] == qMakePair(Frame(50), Frame(60)));
        CHECK(!Analysis::isFillerWord("umbrella") && !Analysis::isFillerWord(QString::fromUtf8("\xec\x9d\x8c\xec\x8b\x9d")) && Analysis::isFillerWord("HMM!"));
    }

    printf("--- speed changes keep the voice's pitch\n");
    {
        // A 440 Hz tone with a 60 ms click-free burst of 2 kHz at each whole second of source.
        const QString src = dir.filePath("pitch.wav");
        // Two channels generated directly: a mono source upmixed by FFmpeg would sit 3 dB lower.
        CHECK(gen(src, {"-f", "lavfi", "-i", "aevalsrc='0.3*sin(2*PI*440*t)+if(lt(mod(t,1),0.06),0.3*sin(2*PI*2000*t)*sin(PI*mod(t,1)/0.06),0)|0.3*sin(2*PI*440*t)+if(lt(mod(t,1),0.06),0.3*sin(2*PI*2000*t)*sin(PI*mod(t,1)/0.06),0)':s=48000:d=8"}));
        ProjectState st;
        st.nextId = 100;
        MediaItem m;
        CHECK(CACHE->probe(src, m));
        m.id = 1;
        st.media << m;
        CACHE->ensureAudio(src);
        for (int i = 0; i < 400 && !CACHE->pcm(src).data; ++i) pump(25);
        for (double speed : {1.0001, 2.0, 0.5, 1.5}) {
            ProjectState s = st;
            Sequence q; q.id = 10; q.w = 320; q.h = 180; q.fpsNum = 30;
            Ops::addTrack(s, q, TrackType::Audio);
            Ops::overwriteMedia(s, q, m, 0, m.duration, 0, Ops::Patch{-1, 0});
            Clip &c = q.audio[0].clips[0];
            c.speed = speed;
            c.dur = Frame(m.duration / speed * 30);
            s.seqs << q;
            const int len = int(qMin(3.0, m.duration / speed) * R);
            AudioMixer whole;
            whole.setState(std::make_shared<const ProjectState>(s), q.id);
            QVector<float> a(len * 2), b(len * 2);
            whole.mix(a.data(), len, 0);
            // Playback renders in small blocks and seeks; it must produce the same samples as export.
            AudioMixer blocks;
            blocks.setState(std::make_shared<const ProjectState>(s), q.id);
            QVector<float> tmp(777 * 2);
            for (qint64 p0 = len - 777 * ((len / 777) / 2); p0 >= 0; p0 -= 777 * 3) { // jump around first
                const int nn = int(qMin<qint64>(777, len - p0));
                blocks.mix(tmp.data(), nn, p0);
            }
            for (qint64 p0 = 0; p0 < len; p0 += 777) {
                const int nn = int(qMin<qint64>(777, len - p0));
                blocks.mix(tmp.data(), nn, p0);
                std::copy(tmp.begin(), tmp.begin() + nn * 2, b.begin() + p0 * 2);
            }
            float diff = 0, jump = 0;
            for (int i = 0; i < a.size(); ++i) diff = qMax(diff, std::fabs(a[i] - b[i]));
            const double t0 = .3, t1 = qMin(2.9, len / double(R) - .1);
            // Between bursts the signal is the pure tone, so any step much bigger than its slope is a click.
            for (int i = 1; i < len; ++i) {
                double t = i / double(R), srcT = t * speed, ph = srcT - std::floor(srcT);
                if (ph > .08 && ph < .95) jump = qMax(jump, std::fabs(a[i * 2] - a[(i - 1) * 2]));
            }
            const double maxSlope = .3 * 2 * M_PI * 440 / R;
            // Where the second burst lands tells whether the audio stays in sync with the picture.
            double best = 0, at = 0;
            for (double t = .9 / speed; t < 1.1 / speed && t * R + 2400 < len; t += .002) {
                double e = toneDb(a, 2000, t, t + .03);
                if (e > best || at == 0) { best = e; at = t; }
            }
            const double want = 1.0 / speed + .015;
            // Bursts are skipped: only the stretches of pure tone between them are measured.
            auto between = [&](double f) {
                double p = 0; int n = 0;
                for (double t = t0; t + .05 <= t1; t += .05) { double s = t * speed, ph = s - std::floor(s); if (ph > .1 && ph + .05 * speed < .97) { p += std::pow(10, toneDbShort(a, f, t, t + .05) / 10); ++n; } }
                return n ? 10 * std::log10(p / n) : -200;
            };
            const double f440 = between(440), fShift = between(440 * speed), lvl = f440 - (20 * std::log10(.3));
            printf("     speed %.1fx: 440 Hz %.1f dB, %.0f Hz %.1f dB; level change %.2f dB; burst at %.3f s (want %.3f); worst step %.4f (tone slope %.4f); block render difference %g\n",
                   speed, f440, 440 * speed, fShift, lvl, at, want, jump, maxSlope, double(diff));
            if (std::fabs(speed - 1) > .1) CHECK(f440 > fShift + 20); // at 1.0001x both frequencies are the same
            CHECK(std::fabs(lvl) < 1.5);
            CHECK(std::fabs(at - want) < .03);
            CHECK(jump < maxSlope * 1.6);
            CHECK(diff < 1e-6f);
        }
    }

    printf(fails ? "\n%d FAILURES\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
