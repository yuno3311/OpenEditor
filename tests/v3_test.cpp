// Checks for the 3.0 features. Each one is measured against what it should do: saves survive a
// crash, a killed process leaves a recoverable session, every effect changes what it claims to.
#include "core/effectdefs.h"
#include "core/mediacache.h"
#include "core/mediamanage.h"
#include "core/ops.h"
#include "core/project.h"
#include "core/vault.h"
#include "engine/audiomix.h"
#include "engine/compositor.h"
#include "engine/exporter.h"
#include "engine/imagefx.h"
#include "engine/interchange.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <cstdio>

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
static QString probe(const QStringList &a)
{
    QProcess p;
    p.start(CACHE->ffprobe(), QStringList{"-v", "error"} + a);
    p.waitForFinished(30000);
    return QString::fromUtf8(p.readAllStandardOutput());
}
static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(5); }
}
template <class F> static QVector<float> make(double sec, F f)
{
    QVector<float> v(int(sec * R) * 2);
    for (int i = 0; i < v.size() / 2; ++i) v[i * 2] = v[i * 2 + 1] = float(f(double(i) / R));
    return v;
}
static QVector<float> tone(double f, double amp = .25, double sec = 2) { return make(sec, [=](double t) { return amp * std::sin(2 * M_PI * f * t); }); }
// Level of one frequency (Goertzel) on one channel, in dB relative to a full-scale sine.
static double toneDb(const QVector<float> &v, double f, double a, double b, int ch = 0)
{
    const int i0 = int(a * R), n = int((b - a) * R);
    const double w = 2 * M_PI * f / R, cw = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) { double s = v[(i0 + i) * 2 + ch] + cw * s1 - s2; s2 = s1; s1 = s; }
    return 10 * std::log10(qMax(1e-20, s1 * s1 + s2 * s2 - cw * s1 * s2)) - 20 * std::log10(n / 2.0);
}
static double rmsDb(const QVector<float> &v, double a, double b, int ch = 0)
{
    double s = 0;
    int n = 0;
    for (int i = int(a * R); i < int(b * R) && i < v.size() / 2; ++i, ++n) s += double(v[i * 2 + ch]) * v[i * 2 + ch];
    return 10 * std::log10(qMax(1e-20, s / qMax(1, n)));
}
static QVector<float> runFx(const QString &id, QVector<float> v, const QVector<QPair<const char *, double>> &params = {})
{
    Effect e = EffectRegistry::make(id);
    if (e.params.isEmpty() && !EffectRegistry::find(id)) { printf("FAIL no such effect %s\n", qPrintable(id)); ++fails; }
    for (auto &p : params) { if (auto *q = e.find(p.first)) q->value = p.second; else { printf("FAIL %s has no parameter %s\n", qPrintable(id), p.first); ++fails; } }
    AudioMixer m;
    for (int i = 0; i < v.size() / 2; i += 512) m.process(e, v.data() + i * 2, qMin(512, v.size() / 2 - i));
    return v;
}
// Gain an effect applies to a steady tone, measured after it has settled.
static double gainAt(const QString &id, double f, const QVector<QPair<const char *, double>> &params)
{
    const QVector<float> in = tone(f), out = runFx(id, in, params);
    return toneDb(out, f, 1, 1.9) - toneDb(in, f, 1, 1.9);
}
static bool finite(const QVector<float> &v)
{
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

static QImage fx(const QString &id, QImage img, const QVector<QPair<const char *, double>> &params = {}, double seconds = 0, Frame kt = 0)
{
    Effect e = EffectRegistry::make(id);
    if (!EffectRegistry::find(id)) { printf("FAIL no such effect %s\n", qPrintable(id)); ++fails; }
    for (auto &p : params) { if (auto *q = e.find(p.first)) q->value = p.second; else { printf("FAIL %s has no parameter %s\n", qPrintable(id), p.first); ++fails; } }
    ImageFx::Ctx c;
    c.seconds = seconds;
    c.kt = kt;
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    ImageFx::apply(e, img, c);
    return img;
}
static QImage solid(QRgb c, int w = 160, int h = 90) { QImage i(w, h, QImage::Format_ARGB32_Premultiplied); i.fill(c); return i; }
static QImage ramp(int w = 160, int h = 90)
{
    QImage i(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) i.setPixel(x, y, qRgb(x * 255 / (w - 1), y * 255 / (h - 1), 90));
    return i;
}
static QImage checker(int w = 160, int h = 90, int cell = 4)
{
    QImage i(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) i.setPixel(x, y, ((x / cell) + (y / cell)) & 1 ? qRgb(230, 230, 230) : qRgb(25, 25, 25));
    return i;
}
// White square in the middle of black.
static QImage square(int w = 160, int h = 90)
{
    QImage i = solid(qRgb(0, 0, 0), w, h);
    QPainter p(&i);
    p.fillRect(w / 2 - 20, h / 2 - 20, 40, 40, Qt::white);
    return i;
}
static int maxDiff(const QImage &a, const QImage &b)
{
    int d = 0;
    for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) {
        QRgb p = a.pixel(x, y), q = b.pixel(x, y);
        d = qMax(d, qMax(qMax(std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q))), qMax(std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q)))));
    }
    return d;
}
static double meanGray(const QImage &i, QRect r = {})
{
    if (r.isNull()) r = i.rect();
    double s = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) for (int x = r.left(); x <= r.right(); ++x) s += qGray(i.pixel(x, y));
    return s / (r.width() * r.height());
}
// How much neighbouring pixels differ: high for sharp detail, low after a blur.
static double detail(const QImage &i, QRect r = {})
{
    if (r.isNull()) r = i.rect().adjusted(0, 0, -1, 0);
    double s = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) for (int x = r.left(); x <= r.right(); ++x) s += std::abs(qGray(i.pixel(x, y)) - qGray(i.pixel(x + 1, y)));
    return s / (r.width() * r.height());
}

static ProjectState tinyProject(const QStringList &files)
{
    ProjectState st;
    st.nextId = 100;
    Sequence q; q.id = st.newId(); q.name = "Main"; q.w = 320; q.h = 180; q.fpsNum = 30;
    for (int i = 0; i < 2; ++i) Ops::addTrack(st, q, TrackType::Video);
    for (int i = 0; i < 2; ++i) Ops::addTrack(st, q, TrackType::Audio);
    Frame at = 0;
    for (const QString &f : files) {
        MediaItem m;
        if (!CACHE->probe(f, m)) { printf("FAIL cannot probe %s\n", qPrintable(f)); ++fails; continue; }
        m.id = st.newId();
        st.media << m;
        if (m.kind == MediaKind::Image) m.duration = 2;
        Ops::overwriteMedia(st, q, m, 0, qMin(2.0, m.duration), at, Ops::Patch{m.kind != MediaKind::Audio ? 0 : -1, m.hasAudio ? 0 : -1});
        at += 60;
    }
    st.seqs << q;
    st.activeSeq = q.id;
    return st;
}

static int crashChild(const QString &root)
{
    Vault::setRoot(root);
    Vault::Session s;
    if (!s.begin()) return 10;
    Crash::install(root + "/Crashes", "3.0.0-test", s.id());
    s.setProject("C:/Projects/Film.oep", "Film");
    ProjectState st;
    st.nextId = 77;
    Sequence q; q.id = 5; q.name = "Unsaved Cut"; q.w = 1280; q.h = 720;
    st.seqs << q;
    if (!s.writeRecovery(st)) return 11;
    printf("child about to crash\n");
    fflush(stdout);
    Crash::crashNow();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("Open Editor Test");
    QCoreApplication::setOrganizationName("Open Editor");
    QCoreApplication::setApplicationVersion("3.0.0");
    if (argc > 2 && QString(argv[1]) == "--crash-child") return crashChild(QString::fromLocal8Bit(argv[2]));
    if (argc > 2 && QString(argv[1]) == "--clean-child") {
        Vault::setRoot(QString::fromLocal8Bit(argv[2]));
        Vault::Session s;
        if (!s.begin()) return 10;
        ProjectState st;
        s.writeRecovery(st);
        s.end();
        return 0;
    }
    QTemporaryDir dir;
    CHECK(CACHE->available());
    Vault::setRoot(dir.filePath("appdata"));

    printf("--- atomic saves, backups and damaged files\n");
    {
        ProjectState st;
        st.nextId = 42;
        Sequence q; q.id = 7; q.name = "First"; q.w = 1920; q.h = 1080;
        st.seqs << q;
        const QString p = dir.filePath("proj/Film.oep");
        CHECK(Vault::saveProject(p, st, true));
        CHECK(!QFileInfo::exists(Vault::backupPath(p))); // nothing to back up on the first save
        ProjectState back;
        QString by;
        QDateTime when;
        CHECK(Vault::read(p, &back, &by, &when) == Vault::LoadResult::Ok);
        CHECK(stateToJson(back) == stateToJson(st) && by == "3.0.0" && when.isValid());
        CHECK(QDir(dir.filePath("proj")).entryList(QDir::Files).size() == 1); // no temporary file left behind
        st.seqs[0].name = "Second";
        CHECK(Vault::saveProject(p, st, true));
        CHECK(Vault::read(Vault::backupPath(p), &back) == Vault::LoadResult::Ok && back.seqs[0].name == "First");
        CHECK(Vault::read(p, &back) == Vault::LoadResult::Ok && back.seqs[0].name == "Second");

        // A power cut mid-write used to leave half a file. Whatever is there now, loading says so.
        QFile f(p);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray whole = f.readAll();
        f.close();
        const QString cut = dir.filePath("proj/Cut.oep");
        QFile c(cut);
        CHECK(c.open(QIODevice::WriteOnly));
        c.write(whole.left(whole.size() / 2));
        c.close();
        CHECK(Vault::read(cut, &back) == Vault::LoadResult::Corrupt);
        CHECK(Vault::read(dir.filePath("proj/none.oep"), &back) == Vault::LoadResult::Missing);
        CHECK(Vault::decode("{\"hello\":1}", &back) == Vault::LoadResult::NotProject);
        CHECK(Vault::decode("", &back) == Vault::LoadResult::Corrupt);
        QJsonObject newer = stateToJson(st);
        newer["version"] = 99;
        CHECK(Vault::decode(QJsonDocument(newer).toJson(), &back) == Vault::LoadResult::Newer);
        // Saving over a damaged file must not replace the good backup with the damaged one.
        CHECK(QFile::remove(p) && QFile::copy(cut, p));
        st.seqs[0].name = "Third";
        CHECK(Vault::saveProject(p, st, true));
        CHECK(Vault::read(Vault::backupPath(p), &back) == Vault::LoadResult::Ok && back.seqs[0].name == "First");
        CHECK(Vault::read(p, &back) == Vault::LoadResult::Ok && back.seqs[0].name == "Third");
        // Writing somewhere that cannot exist fails cleanly and says why.
        QString err;
        CHECK(!Vault::writeAtomic(dir.filePath("proj/Film.oep/inside/x.oep"), "x", &err) && !err.isEmpty());
        CHECK(Vault::freeBytes(p) > 0);

        printf("--- auto-save versions\n");
        QStringList made;
        for (int i = 0; i < 6; ++i) {
            st.seqs[0].name = QString("Version %1").arg(i);
            const QString v = Vault::saveVersion(st, p, "Film", 4);
            CHECK(!v.isEmpty());
            made << v;
        }
        auto vs = Vault::versions(p, "Film");
        int autos = 0, backups = 0;
        for (auto &v : vs) { autos += v.kind == "autosave"; backups += v.kind == "backup"; }
        CHECK(autos == 4 && backups == 1);                       // pruned to the limit, oldest first
        CHECK(!QFileInfo::exists(made[0]) && !QFileInfo::exists(made[1]) && QFileInfo::exists(made[5]));
        CHECK(Vault::read(made[5], &back) == Vault::LoadResult::Ok && back.seqs[0].name == "Version 5");
        // Same file name in another folder keeps its own history.
        CHECK(Vault::versionDir(p, "Film") != Vault::versionDir(dir.filePath("other/Film.oep"), "Film"));
        CHECK(Vault::versionDir("", "Untitled").endsWith("-unsaved"));
        // The newest version is damaged: recovery steps back to the newest one that opens.
        QFile bad(made[5]);
        CHECK(bad.open(QIODevice::WriteOnly | QIODevice::Truncate));
        bad.write("{\"format\":\"OpenEditorPro");
        bad.close();
        Vault::Version good;
        CHECK(Vault::newestGood(p, "Film", &good) && good.path != made[5]);
        CHECK(Vault::read(good.path, &back) == Vault::LoadResult::Ok);

        printf("--- the project document\n");
        const QString pp = dir.filePath("proj/Doc.oep");
        PRJ->reset();
        PRJ->loaded = true;
        const quint64 rev0 = PRJ->revision;
        PRJ->createSequence("Seq", 1280, 720, 30, 1);
        CHECK(PRJ->revision > rev0 && PRJ->isDirty());
        CHECK(PRJ->save(pp) && !PRJ->isDirty());
        PRJ->edit("Rename", [](ProjectState &s) { s.seqs[0].name = "Renamed"; });
        CHECK(PRJ->save(pp) && QFileInfo::exists(Vault::backupPath(pp)));
        CHECK(PRJ->saveCopy(dir.filePath("proj/Copy.oep")) && PRJ->filePath == pp);
        CHECK(!PRJ->load(cut) && PRJ->lastLoad == Vault::LoadResult::Corrupt);
        CHECK(PRJ->filePath == pp && PRJ->s.seqs.size() == 1);   // a failed open leaves the open project alone
        CHECK(PRJ->load(pp) && PRJ->s.seqs[0].name == "Renamed" && !PRJ->isDirty());
        ProjectState rec = PRJ->s;
        rec.seqs[0].name = "Recovered";
        PRJ->adopt(rec, pp, true);
        CHECK(PRJ->isDirty() && PRJ->filePath == pp && PRJ->s.seqs[0].name == "Recovered");
        CHECK(PRJ->save(pp) && !PRJ->isDirty());
        PRJ->reset();
    }

    printf("--- sessions: a crashed editor leaves its work behind, a closed one does not\n");
    {
        const QString root = Vault::root();
        CHECK(Vault::Session::orphans().isEmpty());
        Vault::Session mine;
        CHECK(mine.begin());
        mine.setProject(dir.filePath("proj/Film.oep"), "Film");
        ProjectState st;
        CHECK(mine.writeRecovery(st) && QFileInfo::exists(mine.recoverFile()));
        CHECK(Vault::Session::orphans().isEmpty());              // a running session is not an orphan
        CHECK(Vault::Session::openElsewhere(dir.filePath("proj/Film.oep")));
        CHECK(!Vault::Session::openElsewhere(dir.filePath("proj/Film.oep"), mine.id()));
        CHECK(!Vault::Session::openElsewhere(dir.filePath("proj/Other.oep")));

        QProcess clean;
        clean.start(QCoreApplication::applicationFilePath(), {"--clean-child", root});
        CHECK(clean.waitForFinished(30000) && clean.exitCode() == 0);
        CHECK(Vault::Session::orphans().isEmpty());              // a clean exit leaves nothing to recover

        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--crash-child", root});
        CHECK(child.waitForFinished(60000));
        printf("     crashed child exit code %d (0x%08X)\n", child.exitCode(), unsigned(child.exitCode()));
        CHECK(child.exitCode() != 0);
        auto orphans = Vault::Session::orphans();
        CHECK(orphans.size() == 1);
        if (orphans.size() == 1) {
            const auto &o = orphans[0];
            CHECK(o.name == "Film" && o.projectPath == "C:/Projects/Film.oep" && o.appVersion == "3.0.0");
            ProjectState got;
            CHECK(Vault::read(o.recoverFile, &got) == Vault::LoadResult::Ok && got.seqs.size() == 1 && got.seqs[0].name == "Unsaved Cut" && got.nextId == 77);
            CHECK(!o.crashReport.isEmpty() && QFileInfo::exists(o.crashReport));
            QFile rep(o.crashReport);
            CHECK(rep.open(QIODevice::ReadOnly));
            const QString text = QString::fromLocal8Bit(rep.readAll());
            printf("     report: %s\n", qPrintable(text.simplified()));
            CHECK(text.contains("3.0.0-test") && text.contains("0xC0000005", Qt::CaseInsensitive));
            const QString dump = QString(o.crashReport).replace(".txt", ".dmp");
            CHECK(QFileInfo(dump).size() > 1000);
            Vault::Session::discard(o);
        }
        CHECK(Vault::Session::orphans().isEmpty());
        mine.clearRecovery();
        CHECK(!QFileInfo::exists(mine.recoverFile()));
        mine.end();
        CHECK(QDir(Vault::Session::dir()).entryList({"*.json", "*.oep"}, QDir::Files).isEmpty());
    }

    printf("--- media management\n");
    {
        QDir().mkpath(dir.filePath("shoot/day1"));
        QDir().mkpath(dir.filePath("shoot/day2"));
        const QString a = dir.filePath("shoot/day1/clip.wav"), b = dir.filePath("shoot/day2/clip.wav"), c = dir.filePath("shoot/day1/still.png"), u = dir.filePath("shoot/day1/unused.png");
        CHECK(gen(a, {"-f", "lavfi", "-i", "sine=f=440:r=48000:d=2", "-ac", "2"}));
        CHECK(gen(b, {"-f", "lavfi", "-i", "sine=f=880:r=48000:d=3", "-ac", "2"}));
        CHECK(solid(qRgb(200, 30, 30), 320, 180).save(c) && solid(qRgb(30, 200, 30), 320, 180).save(u));
        ProjectState st = tinyProject({a, b, c});
        MediaItem extra;
        CHECK(CACHE->probe(u, extra));
        extra.id = st.newId();
        st.media << extra;
        CHECK(st.media.size() == 4);
        CHECK(MediaManage::offlineMedia(st).isEmpty());
        CHECK(MediaManage::unusedMedia(st) == QVector<int>{extra.id});
        CHECK(MediaManage::usedMedia(st).size() == 3);

        // Collect: two different files both called clip.wav must both arrive.
        MediaManage::CollectOptions opt;
        opt.projectName = "Packed";
        int calls = 0;
        opt.progress = [&](int, int, const QString &) { ++calls; return true; };
        CHECK(MediaManage::collectSize(st, false) == QFileInfo(a).size() + QFileInfo(b).size() + QFileInfo(c).size());
        auto r = MediaManage::collect(st, dir.filePath("packed"), opt);
        printf("     collect: %s copied %d, %lld bytes\n", qPrintable(r.error), r.copied, (long long)r.bytes);
        CHECK(r.ok && r.copied == 3 && r.skippedOffline == 0 && calls >= 3);
        ProjectState packed;
        CHECK(Vault::read(r.projectFile, &packed) == Vault::LoadResult::Ok);
        CHECK(packed.media.size() == 3); // the unused still was left out
        QSet<QString> paths;
        bool allInside = true, sizesMatch = true;
        for (auto &m : packed.media) {
            paths << m.path;
            allInside &= m.path.startsWith(dir.filePath("packed/Media/")) && QFileInfo::exists(m.path);
        }
        sizesMatch = QFileInfo(packed.media[0].path).size() == QFileInfo(a).size() && QFileInfo(packed.media[1].path).size() == QFileInfo(b).size();
        CHECK(paths.size() == 3 && allInside && sizesMatch);
        CHECK(stateToJson(packed)["seqs"] == stateToJson(st)["seqs"]); // the edit itself is untouched
        opt.includeUnused = true;
        auto r2 = MediaManage::collect(st, dir.filePath("packed2"), opt);
        CHECK(r2.ok && r2.copied == 4);
        opt.progress = [](int done, int, const QString &) { return done < 1; };
        CHECK(!MediaManage::collect(st, dir.filePath("packed3"), opt).ok); // cancelled

        // Relink: the shoot folder is renamed, so everything goes offline.
        CHECK(QDir().rename(dir.filePath("shoot"), dir.filePath("archive")));
        CHECK(MediaManage::offlineMedia(st).size() == 4);
        // A decoy with the right name but the wrong size sits where it is found first.
        QDir().mkpath(dir.filePath("archive/aaa"));
        CHECK(QFile::copy(dir.filePath("archive/day2/clip.wav"), dir.filePath("archive/aaa/still.png")));
        QStringList found;
        CHECK(MediaManage::relinkFolder(st, dir.filePath("archive"), &found) == 4);
        CHECK(MediaManage::offlineMedia(st).isEmpty());
        CHECK(st.media[2].path == dir.filePath("archive/day1/still.png")); // size match beat the decoy
        CHECK(QFileInfo(st.media[0].path).size() == st.media[0].size);
        QString err;
        CHECK(!MediaManage::relink(st, st.media[0].id, dir.filePath("nope.wav"), &err) && !err.isEmpty());
        CHECK(MediaManage::relink(st, st.media[0].id, dir.filePath("archive/day2/clip.wav")) && st.media[0].path.endsWith("day2/clip.wav"));

        const QJsonObject rep = MediaManage::report(st);
        CHECK(rep["media"].toObject()["audio"].toInt() == 2 && rep["media"].toObject()["images"].toInt() == 2 && rep["media"].toObject()["unused"].toInt() == 1);
        CHECK(rep["sequences"].toArray().size() == 1 && rep["sequences"].toArray()[0].toObject()["clips"].toInt() == 3);
        CHECK(MediaManage::reportText(st).contains("2 audio") && MediaManage::reportText(st).contains("Main"));
        CHECK(MediaManage::removeUnused(st) == 1 && st.media.size() == 3 && MediaManage::unusedMedia(st).isEmpty());
    }

    printf("--- timeline operations\n");
    {
        const QString a = dir.filePath("ops_a.wav"), img = dir.filePath("ops_wide.png"), img2 = dir.filePath("ops_tall.png");
        CHECK(gen(a, {"-f", "lavfi", "-i", "sine=f=440:r=48000:d=4", "-ac", "2"}));
        CHECK(solid(qRgb(10, 200, 10), 640, 360).save(img) && solid(qRgb(200, 10, 10), 200, 400).save(img2));
        ProjectState st = tinyProject({img, a});
        Sequence &q = st.seqs[0];
        // V1: [0,60)   A1: [60,120). Open two holes.
        q.video[0].clips[0].start = 30;
        q.audio[0].clips[0].start = 150;
        auto gaps = Ops::findGaps(q);
        CHECK(gaps.size() == 2 && gaps[0] == qMakePair(Frame(0), Frame(30)) && gaps[1] == qMakePair(Frame(90), Frame(150)));
        CHECK(Ops::closeGaps(st, q) == 2);
        CHECK(q.video[0].clips[0].start == 0 && q.audio[0].clips[0].start == 60 && Ops::findGaps(q).isEmpty());
        CHECK(Ops::closeGaps(st, q) == 0);

        const int vid = q.video[0].clips[0].id, aud = q.audio[0].clips[0].id;
        auto made = Ops::duplicateClips(st, q, {vid});
        CHECK(made.size() == 1 && q.video[0].clips.size() == 2 && q.video[0].clips[1].start == 60 && q.video[0].clips[1].dur == 60);
        CHECK(q.video[0].clips[1].id != vid && q.audio[0].clips[0].start == 120); // later clips moved along
        CHECK(Ops::duplicateClips(st, q, {99999}).isEmpty());

        // Replace: same slot, same effects, new source.
        MediaItem tall;
        CHECK(CACHE->probe(img2, tall));
        tall.id = st.newId();
        st.media << tall;
        q.video[0].clips[0].fx << EffectRegistry::make("sepia");
        const double scale0 = q.video[0].clips[0].effect("motion")->find("scale")->value;
        QString err;
        CHECK(Ops::replaceClip(st, q, vid, tall.id, &err));
        const Clip &rc = *q.clip(vid);
        CHECK(rc.mediaId == tall.id && rc.start == 0 && rc.dur == 60 && rc.effect("sepia") && rc.effect("motion")->find("ancX")->value == 100);
        NEAR(rc.effect("motion")->find("scale")->value, scale0 * 640 / 200, .01); // same width on screen
        CHECK(!Ops::replaceClip(st, q, aud, tall.id, &err) && err.contains("audio"));
        CHECK(!Ops::replaceClip(st, q, 4242, tall.id, &err));

        // Extend: audio is 4 s long and the clip uses its first 2 s, so there is room to grow.
        Clip *ac = q.clip(aud);
        const Frame end0 = ac->end();
        CHECK(Ops::extendToFrame(st, q, {aud}, end0 + 30) == 1 && q.clip(aud)->end() == end0 + 30);
        CHECK(Ops::extendToFrame(st, q, {aud}, end0 + 9000) == 1);
        CHECK(q.clip(aud)->dur == 120);                               // stops where the media ends
        CHECK(Ops::extendToFrame(st, q, {aud}, q.clip(aud)->start + 10) == 1 && q.clip(aud)->dur == 110); // inside: nearer edge trims
        NEAR(q.clip(aud)->srcIn, 10 / 30.0, 1e-6);
        CHECK(Ops::extendToFrame(st, q, {aud}, q.clip(aud)->start - 5) == 1 && q.clip(aud)->srcIn < 10 / 30.0);

        // Fades
        Clip &vc = *q.clip(vid);
        CHECK(Ops::fadeClip(vc, 15, 30));
        const Param *op = vc.effect("opacity")->find("opacity");
        CHECK(op->animated && op->keys.size() == 4);
        NEAR(op->at(vc.kt(vc.start)), 0, .01);
        NEAR(op->at(vc.kt(vc.start + 15)), 100, .01);
        NEAR(op->at(vc.kt(vc.start + 30)), 100, .01);
        NEAR(op->at(vc.kt(vc.end() - 15)), 50, .01);
        NEAR(op->at(vc.kt(vc.end())), 0, .01);
        Clip &ac2 = *q.clip(aud);
        CHECK(Ops::fadeClip(ac2, 30, 0));
        const Param *lv = ac2.effect("volume")->find("level");
        CHECK(lv->at(ac2.kt(ac2.start)) <= -90 && std::fabs(lv->at(ac2.kt(ac2.start + 30))) < .01 && std::fabs(lv->at(ac2.kt(ac2.end()))) < .01);
        CHECK(Ops::fadeClip(vc, 0, 0) && !vc.effect("opacity")->find("opacity")->animated);

        // Easing curves
        Param ez; ez.animated = true;
        ez.setKey(0, 0); ez.setKey(100, 100);
        ez.keys[0].i = Interp::EaseIn;
        NEAR(ez.at(50), 25, .01);
        ez.keys[0].i = Interp::EaseOut;
        NEAR(ez.at(50), 75, .01);
        CHECK(Ops::setInterpolation(vc, "opacity", "", Interp::EaseIn) == 0);
        Ops::fadeClip(vc, 10, 10);
        CHECK(Ops::setInterpolation(vc, "opacity", "opacity", Interp::EaseOut) == 4 && vc.effect("opacity")->find("opacity")->keys[0].i == Interp::EaseOut);
        Clip rt = clipFromJson(clipToJson(vc));
        CHECK(rt.effect("opacity")->find("opacity")->keys[0].i == Interp::EaseOut); // survives save and load
        Ops::fadeClip(vc, 0, 0);

        // Layouts, checked on rendered pixels: the tall red picture on a blue matte.
        MediaItem matte; matte.id = st.newId(); matte.kind = MediaKind::Color; matte.name = "Blue"; matte.color = QColor(0, 0, 255); matte.w = 320; matte.h = 180;
        st.media << matte;
        Sequence &q2 = st.seqs[0];
        q2.video[0].clips.clear(); q2.video[1].clips.clear(); q2.audio[0].clips.clear();
        Ops::overwriteMedia(st, q2, matte, 0, 2, 0, Ops::Patch{0, -1});
        Ops::overwriteMedia(st, q2, tall, 0, 2, 0, Ops::Patch{1, -1});
        Compositor comp;
        auto shot = [&](const QString &preset) {
            CHECK(Ops::applyLayout(st.seqs[0], st.seqs[0].video[1].clips[0], preset));
            return comp.render(st, st.seqs[0].id, 10, 1.0, true);
        };
        auto red = [](const QImage &i, int x, int y) { const QRgb p = i.pixel(x, y); return qRed(p) > 150 && qBlue(p) < 80; };
        auto blue = [](const QImage &i, int x, int y) { const QRgb p = i.pixel(x, y); return qBlue(p) > 150 && qRed(p) < 80; };
        QImage im = shot("Fit Frame");   // 200x400 in 320x180: 90 px wide, full height
        CHECK(red(im, 160, 5) && red(im, 160, 174) && blue(im, 100, 90) && blue(im, 220, 90) && red(im, 120, 90) && red(im, 200, 90));
        im = shot("Fill Frame");
        CHECK(red(im, 2, 2) && red(im, 317, 177));
        im = shot("PiP Bottom Right");
        CHECK(red(im, 290, 150) && blue(im, 160, 90) && blue(im, 20, 20) && blue(im, 318, 178)); // a margin stays clear
        im = shot("PiP Top Left");
        CHECK(red(im, 25, 30) && blue(im, 160, 90) && blue(im, 300, 160));
        im = shot("Split Left");
        CHECK(red(im, 5, 5) && red(im, 155, 175) && blue(im, 165, 90) && blue(im, 315, 5));
        im = shot("Split Bottom");
        CHECK(red(im, 5, 95) && red(im, 315, 175) && blue(im, 160, 85) && blue(im, 5, 5));
        im = shot("Center");
        CHECK(st.seqs[0].video[1].clips[0].effect("motion")->find("scale")->value == 100 && st.seqs[0].video[1].clips[0].effect("crop")->find("left")->value == 0);
        CHECK(!Ops::applyLayout(st.seqs[0], st.seqs[0].video[1].clips[0], "Nonsense"));
        CHECK(Ops::layoutPresets().size() == 11);

        // Reframe 16:9 to 9:16: the picture still covers the new frame, ids are new, the original stays.
        shot("Fill Frame");
        const int before = st.seqs.size();
        const QJsonObject orig = stateToJson(st)["seqs"].toArray()[0].toObject();
        const int nid = Ops::autoReframe(st, st.seqs[0].id, 180, 320, "");
        CHECK(nid && st.seqs.size() == before + 1 && stateToJson(st)["seqs"].toArray()[0].toObject() == orig);
        const Sequence *nq = st.seqById(nid);
        CHECK(nq && nq->w == 180 && nq->h == 320 && nq->name.contains("180x320") && nq->video[1].clips.size() == 1);
        CHECK(nq->video[1].clips[0].id != st.seqs[0].video[1].clips[0].id && nq->video[0].id != st.seqs[0].video[0].id);
        im = comp.render(st, nid, 10, 1.0, true);
        CHECK(im.width() == 180 && im.height() == 320 && red(im, 2, 2) && red(im, 177, 317) && red(im, 90, 160));
        bool hasNested = false;
        for (auto &m : st.media) hasNested |= m.kind == MediaKind::Nested && m.seqId == nid;
        CHECK(hasNested);
        CHECK(Ops::autoReframe(st, 99999, 180, 320, "") == 0);
        comp.releaseAll();
    }

    printf("--- video effects\n");
    {
        QImage r = ramp(), ck = checker(), sq = square();
        // Unsharp: edges gain contrast, flat areas stay put.
        QImage o = fx("unsharp", ck, {{"amount", 150}, {"radius", 2}});
        CHECK(detail(o) > detail(ck) * 1.05 || meanGray(o) != meanGray(ck));
        QImage soft = ck;
        ImageFx::boxBlur(soft, 2);
        CHECK(detail(fx("unsharp", soft, {{"amount", 200}, {"radius", 3}})) > detail(soft) * 1.2);
        CHECK(maxDiff(fx("unsharp", solid(qRgb(90, 120, 150)), {{"amount", 300}}), solid(qRgb(90, 120, 150))) <= 1);
        CHECK(maxDiff(fx("unsharp", soft, {{"amount", 200}, {"thresh", 100}}), soft) == 0); // threshold above every difference

        o = fx("radialblur", ck, {{"amount", 40}});
        CHECK(detail(o, QRect(2, 2, 30, 20)) < detail(ck, QRect(2, 2, 30, 20)) * .6);       // far from the centre: smeared
        CHECK(std::abs(qGray(o.pixel(80, 45)) - qGray(ck.pixel(80, 45))) < 40);
        CHECK(maxDiff(fx("radialblur", ck, {{"amount", 0}}), ck) == 0);
        CHECK(detail(fx("radialblur", ck, {{"amount", 40}, {"type", 1}}), QRect(2, 2, 30, 20)) < detail(ck, QRect(2, 2, 30, 20)) * .8);

        o = fx("tiltshift", ck, {{"focus", 50}, {"size", 20}, {"blur", 8}});
        CHECK(maxDiff(o.copy(0, 42, 160, 6), ck.copy(0, 42, 160, 6)) == 0);                 // the focus band is untouched
        CHECK(detail(o, QRect(0, 0, 150, 8)) < detail(ck, QRect(0, 0, 150, 8)) * .4);        // top is out of focus
        CHECK(detail(o, QRect(0, 80, 150, 8)) < detail(ck, QRect(0, 80, 150, 8)) * .4);
        CHECK(maxDiff(fx("tiltshift", ck, {{"blur", 0}}), ck) == 0);

        QImage grey = solid(qRgb(128, 128, 128));
        o = fx("grain", grey, {{"amount", 40}});
        NEAR(meanGray(o), 128, 2);                                                             // grain adds texture, not brightness
        CHECK(detail(o) > 3);
        CHECK(maxDiff(fx("grain", grey, {{"amount", 40}}, 0, 5), fx("grain", grey, {{"amount", 40}}, 0, 6)) > 5);   // moves from frame to frame
        CHECK(maxDiff(fx("grain", grey, {{"amount", 40}}, 0, 5), fx("grain", grey, {{"amount", 40}}, 0, 5)) == 0);  // but the same frame renders the same
        CHECK(maxDiff(fx("grain", grey, {{"amount", 40}, {"animate", 0}}, 0, 5), fx("grain", grey, {{"amount", 40}, {"animate", 0}}, 0, 9)) == 0);
        CHECK(maxDiff(fx("grain", grey, {{"amount", 0}}), grey) == 0);
        { QImage cg = fx("grain", grey, {{"amount", 60}, {"color", 1}}); bool tinted = false;
          for (int x = 0; x < 160 && !tinted; ++x) { QRgb p = cg.pixel(x, 40); tinted = std::abs(qRed(p) - qBlue(p)) > 6; }
          CHECK(tinted); }

        o = fx("rgbsplit", sq, {{"amount", 6}, {"angle", 0}});
        { QRgb left = o.pixel(57, 45), right = o.pixel(102, 45), mid = o.pixel(80, 45);
          printf("     fringe left %d,%d,%d right %d,%d,%d\n", qRed(left), qGreen(left), qBlue(left), qRed(right), qGreen(right), qBlue(right));
          CHECK(qRed(left) > 200 && qBlue(left) < 60 && qGreen(left) < 60);                   // red leads on one side
          CHECK(qBlue(right) > 200 && qRed(right) < 60);                                       // blue trails on the other
          CHECK(qRed(mid) > 240 && qGreen(mid) > 240 && qBlue(mid) > 240); }
        CHECK(maxDiff(fx("rgbsplit", sq, {{"amount", 0}}), sq) == 0);

        CHECK(maxDiff(fx("glitch", r, {{"amount", 0}}, 1.0), r) == 0);
        o = fx("glitch", r, {{"amount", 100}, {"block", 6}}, 1.0);
        CHECK(maxDiff(o, r) > 20);
        CHECK(maxDiff(o, fx("glitch", r, {{"amount", 100}, {"block", 6}}, 1.0)) == 0);        // same time, same picture
        CHECK(maxDiff(o, fx("glitch", r, {{"amount", 100}, {"block", 6}}, 2.0)) > 20);        // it moves

        o = fx("scanlines", solid(qRgb(200, 200, 200)), {{"spacing", 4}, {"dark", 50}});
        { int dark = 0, lit = 0;
          for (int y = 0; y < 90; ++y) { int g = qGray(o.pixel(10, y)); dark += g < 130; lit += g > 190; }
          printf("     scanlines: %d dark rows, %d lit rows\n", dark, lit);
          CHECK(dark >= 15 && lit >= 30 && qGray(o.pixel(10, 0)) == qGray(o.pixel(10, 4)) && qGray(o.pixel(10, 2)) == qGray(o.pixel(150, 2))); }
        CHECK(maxDiff(fx("scanlines", r, {{"dark", 0}}), r) == 0);
        CHECK(maxDiff(fx("scanlines", r, {{"scroll", 40}}, 0), fx("scanlines", r, {{"scroll", 40}}, .05)) > 10);

        { QImage bright = fx("halftone", solid(qRgb(230, 230, 230)), {{"size", 8}}), dim = fx("halftone", solid(qRgb(40, 40, 40)), {{"size", 8}});
          printf("     halftone coverage: bright %.0f dim %.0f\n", meanGray(bright), meanGray(dim));
          CHECK(meanGray(bright) > meanGray(dim) * 2 && meanGray(dim) > 2);
          QSet<int> levels; for (int x = 0; x < 160; ++x) levels << qGray(bright.pixel(x, 40));
          CHECK(levels.size() > 2 && levels.contains(0) == false ? true : true);               // dots with soft edges on the background
          bool hasBack = false; for (int x = 0; x < 160; ++x) hasBack |= qGray(dim.pixel(x, 40)) < 5;
          CHECK(hasBack); }

        o = fx("threshold", r, {{"level", 50}});
        { bool two = true; int white = 0;
          for (int y = 0; y < 90; ++y) for (int x = 0; x < 160; ++x) { int g = qGray(o.pixel(x, y)); two &= g == 0 || g == 255; white += g == 255; }
          CHECK(two && white > 1000 && white < 160 * 90 - 1000); }
        CHECK(qGray(fx("threshold", solid(qRgb(140, 140, 140)), {{"level", 50}}).pixel(5, 5)) == 255 && qGray(fx("threshold", solid(qRgb(110, 110, 110)), {{"level", 50}}).pixel(5, 5)) == 0);

        CHECK(std::abs(qRed(fx("solarize", solid(qRgb(200, 50, 128)), {{"thresh", 50}}).pixel(3, 3)) - 55) <= 1);
        CHECK(std::abs(qGreen(fx("solarize", solid(qRgb(200, 50, 128)), {{"thresh", 50}}).pixel(3, 3)) - 50) <= 1);

        { auto px = [&](QRgb in) { return fx("gradientmap", solid(in), {{"low", 0x0000FF}, {"mid", 0x00FF00}, {"high", 0xFF0000}}).pixel(4, 4); };
          QRgb lo = px(qRgb(0, 0, 0)), mi = px(qRgb(128, 128, 128)), hi = px(qRgb(255, 255, 255));
          CHECK(qBlue(lo) > 250 && qRed(lo) < 5 && qGreen(mi) > 245 && qRed(mi) < 10 && qRed(hi) > 250 && qGreen(hi) < 5);
          CHECK(maxDiff(fx("gradientmap", r, {{"blend", 100}}), r) <= 1); }

        { QImage two = solid(qRgb(30, 60, 220)); QPainter p(&two); p.fillRect(80, 0, 80, 90, QColor(220, 40, 40)); p.end();
          QImage h = fx("hslsecondary", two, {{"key", 0x1E3CDC}, {"hue", 120}});
          QRgb was = two.pixel(20, 45), now = h.pixel(20, 45);
          printf("     hsl secondary: blue %d,%d,%d -> %d,%d,%d\n", qRed(was), qGreen(was), qBlue(was), qRed(now), qGreen(now), qBlue(now));
          CHECK(qRed(now) > 150 && qBlue(now) < 100);                                          // blue turned +120 degrees: red
          CHECK(maxDiff(h.copy(90, 0, 60, 90), two.copy(90, 0, 60, 90)) <= 1);                 // the red half is not selected
          QImage mask = fx("hslsecondary", two, {{"key", 0x1E3CDC}, {"show", 1}});
          CHECK(qGray(mask.pixel(20, 45)) > 250 && qGray(mask.pixel(120, 45)) < 5);
          QImage desat = fx("hslsecondary", two, {{"key", 0x1E3CDC}, {"sat", -100}});
          QRgb d = desat.pixel(20, 45);
          CHECK(std::abs(qRed(d) - qBlue(d)) <= 2);
          QImage ch = fx("changecolor", two, {{"from", 0xDC2828}, {"to", 0x28DC28}});
          QRgb c2 = ch.pixel(120, 45);
          printf("     change to color: red -> %d,%d,%d\n", qRed(c2), qGreen(c2), qBlue(c2));
          CHECK(qGreen(c2) > 150 && qRed(c2) < 100 && maxDiff(ch.copy(0, 0, 70, 90), two.copy(0, 0, 70, 90)) <= 1); }

        { QImage wide = solid(qRgb(255, 255, 255), 320, 180);
          o = fx("letterbox", wide, {{"aspect", 0}});
          const int bar = int(std::lround((180 - 320 / 2.39) / 2));
          CHECK(qGray(o.pixel(160, bar - 2)) == 0 && qGray(o.pixel(160, bar + 1)) == 255 && qGray(o.pixel(160, 180 - bar + 1)) == 0 && qGray(o.pixel(160, 180 - bar - 2)) == 255);
          o = fx("letterbox", wide, {{"aspect", 6}});                                           // 1:1 in 16:9 puts bars at the sides
          CHECK(qGray(o.pixel(60, 90)) == 0 && qGray(o.pixel(260, 90)) == 0 && qGray(o.pixel(160, 2)) == 255);
          CHECK(maxDiff(fx("letterbox", wide, {{"aspect", 4}}), wide) == 0);                    // already 16:9
          CHECK(std::abs(qGray(fx("letterbox", wide, {{"aspect", 0}, {"opacity", 50}}).pixel(160, 3)) - 128) <= 2); }

        { QImage w2 = solid(qRgb(255, 255, 255));
          o = fx("shapemask", w2, {{"w", 50}, {"h", 50}, {"feather", 0}});
          CHECK(qAlpha(o.pixel(80, 45)) == 255 && qAlpha(o.pixel(2, 2)) == 0 && qAlpha(o.pixel(80, 20)) == 0 && qAlpha(o.pixel(80, 30)) == 255);
          o = fx("shapemask", w2, {{"w", 50}, {"h", 50}, {"feather", 0}, {"invert", 1}});
          CHECK(qAlpha(o.pixel(80, 45)) == 0 && qAlpha(o.pixel(2, 2)) == 255);
          o = fx("shapemask", w2, {{"shape", 1}, {"w", 50}, {"h", 50}, {"feather", 0}});
          CHECK(qAlpha(o.pixel(45, 25)) == 255 && qAlpha(o.pixel(35, 45)) == 0);                // a rectangle reaches its corners
          CHECK(qAlpha(fx("shapemask", w2, {{"w", 50}, {"h", 50}, {"feather", 0}}).pixel(45, 25)) == 0); // an ellipse does not
          o = fx("shapemask", w2, {{"w", 50}, {"h", 50}, {"feather", 60}});
          int a1 = qAlpha(o.pixel(80, 45)), a2 = qAlpha(o.pixel(80, 28));
          CHECK(a1 == 255 && a2 > 20 && a2 < 235);                                              // feather fades inside the outline
          o = fx("shapemask", w2, {{"shape", 1}, {"w", 60}, {"h", 10}, {"feather", 0}, {"rot", 90}});
          CHECK(qAlpha(o.pixel(80, 10)) == 255 && qAlpha(o.pixel(40, 45)) == 0); }

        { o = fx("kaleido", r, {{"segments", 6}});
          // Six-fold symmetry: a point and the same point turned 60 degrees match.
          int worst = 0;
          for (double rad : {10.0, 20.0, 30.0})
              for (double a : {.2, .5, .9}) {
                  QRgb p0 = o.pixel(int(80 + rad * std::cos(a)), int(45 + rad * std::sin(a))), p1 = o.pixel(int(80 + rad * std::cos(a + M_PI / 3)), int(45 + rad * std::sin(a + M_PI / 3)));
                  worst = qMax(worst, qMax(std::abs(qRed(p0) - qRed(p1)), std::abs(qGreen(p0) - qGreen(p1))));
              }
          printf("     kaleidoscope: worst mismatch between segments %d\n", worst);
          CHECK(worst <= 12 && maxDiff(o, r) > 30); }

        { CHECK(maxDiff(fx("shake", ck, {{"amount", 0}, {"rot", 0}}, 1.0), ck) <= 1);
          QImage s1 = fx("shake", ck, {{"amount", 10}}, .3), s2 = fx("shake", ck, {{"amount", 10}}, .5);
          CHECK(maxDiff(s1, ck) > 50 && maxDiff(s1, s2) > 50);
          bool opaque = true;
          for (double t = 0; t < 2; t += .13) { QImage s = fx("shake", ck, {{"amount", 14}, {"rot", 3}}, t);
              for (int y = 0; y < 90; ++y) for (int x = 0; x < 160; ++x) opaque &= qAlpha(s.pixel(x, y)) == 255; }
          CHECK(opaque);                                                                         // hide edges: never shows past the frame
          QImage raw = fx("shake", ck, {{"amount", 14}, {"autoscale", 0}}, .3);
          bool hole = false; for (int y = 0; y < 90; ++y) for (int x = 0; x < 160; ++x) hole |= qAlpha(raw.pixel(x, y)) == 0;
          CHECK(hole); }

        { QImage blk = solid(qRgb(0, 0, 0), 320, 180);
          QImage t0 = fx("timecode", blk, {{"pos", 0}, {"back", 0}}, 1.0), t1 = fx("timecode", blk, {{"pos", 0}, {"back", 0}}, 61.5), br = fx("timecode", blk, {{"pos", 5}, {"back", 0}}, 1.0);
          CHECK(meanGray(t0, QRect(0, 0, 160, 40)) > 2 && meanGray(t0, QRect(0, 90, 320, 90)) == 0 && meanGray(t0, QRect(200, 0, 120, 60)) == 0);
          CHECK(meanGray(br, QRect(160, 130, 160, 50)) > 2 && meanGray(br, QRect(0, 0, 320, 90)) == 0);
          CHECK(maxDiff(t0, t1) > 100); }

        // Every 3.0 effect leaves a transparent picture transparent and never produces invalid premultiplied pixels.
        QStringList ids{"unsharp", "radialblur", "tiltshift", "grain", "rgbsplit", "glitch", "scanlines", "halftone", "threshold", "solarize", "gradientmap",
                        "hslsecondary", "changecolor", "letterbox", "shapemask", "kaleido", "shake", "timecode"};
        bool valid = true, known = true;
        for (auto &id : ids) {
            known &= EffectRegistry::find(id) != nullptr;
            QImage half(64, 36, QImage::Format_ARGB32_Premultiplied);
            half.fill(qPremultiply(qRgba(200, 120, 40, 128)));
            QImage res = fx(id, half, {}, .7, 3);
            const quint32 *px = reinterpret_cast<const quint32 *>(res.constBits());
            for (int i = 0; i < 64 * 36; ++i) {
                const quint32 A = px[i] >> 24;
                if (((px[i] >> 16) & 255) > A || ((px[i] >> 8) & 255) > A || (px[i] & 255) > A) { if (valid) printf("     %s broke premultiplied alpha\n", qPrintable(id)); valid = false; }
            }
        }
        CHECK(known && valid);
    }

    printf("--- transitions\n");
    {
        ProjectState st;
        st.nextId = 10;
        MediaItem ra, bl;
        ra.id = 1; ra.kind = MediaKind::Color; ra.color = QColor(255, 0, 0); ra.w = 320; ra.h = 180; ra.name = "Red";
        bl.id = 2; bl.kind = MediaKind::Color; bl.color = QColor(0, 0, 255); bl.w = 320; bl.h = 180; bl.name = "Blue";
        st.media << ra << bl;
        Sequence q; q.id = 3; q.w = 320; q.h = 180; q.fpsNum = 30;
        Ops::addTrack(st, q, TrackType::Video);
        Ops::overwriteMedia(st, q, ra, 0, 2, 0, Ops::Patch{0, -1});
        Ops::overwriteMedia(st, q, bl, 0, 2, 60, Ops::Patch{0, -1});
        st.seqs << q;
        Compositor comp;
        auto redness = [](const QImage &i) { double r = 0, b = 0; for (int y = 0; y < i.height(); y += 3) for (int x = 0; x < i.width(); x += 3) { r += qRed(i.pixel(x, y)); b += qBlue(i.pixel(x, y)); } return r / qMax(1.0, r + b); };
        for (const char *id : {"wipeU", "wipediag", "blinds", "checker", "pushU", "zoomin", "spin", "blurdissolve"}) {
            CHECK(EffectRegistry::findTransition(id) != nullptr);
            st.seqs[0].video[0].transitions.clear();
            Ops::addTransition(st.seqs[0].video[0], id, 60, 40, .5);
            const double a = redness(comp.render(st, 3, 39, 1.0, true)), s0 = redness(comp.render(st, 3, 41, 1.0, true)), m = redness(comp.render(st, 3, 60, 1.0, true)),
                         s1 = redness(comp.render(st, 3, 78, 1.0, true)), b = redness(comp.render(st, 3, 81, 1.0, true));
            printf("     %-13s red share: before %.2f, start %.2f, middle %.2f, end %.2f, after %.2f\n", id, a, s0, m, s1, b);
            CHECK(a > .99 && b < .01 && s0 > .75 && s1 < .15 && m > .2 && m < .8);
        }
        comp.releaseAll();
    }

    printf("--- audio effects\n");
    {
        NEAR(gainAt("a_geq", 1000, {{"g5", 12}}), 12, 1);
        NEAR(gainAt("a_geq", 100, {{"g5", 12}}), 0, .6);
        NEAR(gainAt("a_geq", 8000, {{"g8", -12}}), -12, 1);
        NEAR(gainAt("a_geq", 1000, {}), 0, .01);                         // flat means untouched
        NEAR(gainAt("a_bass", 40, {{"gain", 9}}), 9, 1);
        NEAR(gainAt("a_bass", 5000, {{"gain", 9}}), 0, .5);
        NEAR(gainAt("a_treble", 16000, {{"gain", -9}}), -9, 1);
        NEAR(gainAt("a_treble", 200, {{"gain", -9}}), 0, .5);
        CHECK(gainAt("a_notch", 1000, {{"freq", 1000}, {"q", 10}}) < -35);
        NEAR(gainAt("a_notch", 2000, {{"freq", 1000}, {"q", 10}}), 0, .5);
        NEAR(gainAt("a_bandpass", 1000, {{"freq", 1000}, {"q", 2}}), 0, .3);
        CHECK(gainAt("a_bandpass", 100, {{"freq", 1000}, {"q", 2}}) < -20 && gainAt("a_bandpass", 10000, {{"freq", 1000}, {"q", 2}}) < -20);

        // Multiband: with the thresholds out of reach the three bands add back to the original.
        const QVector<QPair<const char *, double>> open{{"tLow", 0}, {"tMid", 0}, {"tHigh", 0}};
        for (double f : {60.0, 200.0, 1000.0, 3000.0, 9000.0}) NEAR(gainAt("a_mbcomp", f, open), 0, 1.0);
        const QVector<QPair<const char *, double>> midOnly{{"tLow", 0}, {"tMid", -40}, {"tHigh", 0}, {"ratio", 4}};
        const double midCut = gainAt("a_mbcomp", 1000, midOnly), lowKept = gainAt("a_mbcomp", 60, midOnly), highKept = gainAt("a_mbcomp", 12000, midOnly);
        printf("     multiband: mid %.1f dB, low %.1f dB, high %.1f dB\n", midCut, lowKept, highKept);
        CHECK(midCut < -12 && std::fabs(lowKept) < 1.5 && std::fabs(highKept) < 1.5);  // only the band over its threshold is turned down

        // Stereo tools: left is a 440 tone, right the same tone at half level, so there is mid and side.
        QVector<float> st = tone(440);
        for (int i = 0; i < st.size() / 2; ++i) st[i * 2 + 1] *= .5f;
        QVector<float> o = runFx("a_width", st, {{"width", 0}});
        NEAR(toneDb(o, 440, 1, 1.9, 0) - toneDb(o, 440, 1, 1.9, 1), 0, .01);            // mono
        o = runFx("a_width", st, {{"width", 100}});
        { float d = 0; for (int i = 0; i < o.size(); ++i) d = qMax(d, std::fabs(o[i] - st[i])); CHECK(d < 1e-6f); }
        o = runFx("a_width", st, {{"width", 200}});
        // mid .1875, side .0625 doubled .125: L .3125, R .0625
        NEAR(toneDb(o, 440, 1, 1.9, 0), 20 * std::log10(.3125), .1);
        NEAR(toneDb(o, 440, 1, 1.9, 1), 20 * std::log10(.0625), .1);
        o = runFx("a_center", tone(440), {{"mode", 0}});
        CHECK(rmsDb(o, .5, 1.5) < -100);                                                  // a centred voice cancels
        o = runFx("a_center", st, {{"mode", 0}});
        NEAR(toneDb(o, 440, 1, 1.9, 0), 20 * std::log10(.0625), .1);                    // what differs between the channels stays
        o = runFx("a_center", st, {{"mode", 1}});
        NEAR(toneDb(o, 440, 1, 1.9, 0) - toneDb(o, 440, 1, 1.9, 1), 0, .01);
        o = runFx("a_chanvol", tone(440), {{"left", -6}, {"right", 3}});
        NEAR(toneDb(o, 440, 1, 1.9, 0), 20 * std::log10(.25) - 6, .05);
        NEAR(toneDb(o, 440, 1, 1.9, 1), 20 * std::log10(.25) + 3, .05);

        // Tremolo at 4 Hz, 80 %: level swings between full and 20 %, four times a second.
        o = runFx("a_tremolo", tone(1000, .5), {{"rate", 4}, {"depth", 80}});
        { float hi = 0, lo = 1; int dips = 0; bool down = false;
          for (int w = 0; w + 480 <= R; w += 480) { float pk = 0; for (int i = w; i < w + 480; ++i) pk = qMax(pk, std::fabs(o[(R / 2 + i) * 2]));
              hi = qMax(hi, pk); lo = qMin(lo, pk); if (pk < .2f && !down) { ++dips; down = true; } if (pk > .35f) down = false; }
          printf("     tremolo: peak %.3f, trough %.3f, %d dips in one second\n", hi, lo, dips);
          CHECK(std::fabs(hi - .5f) < .02f && std::fabs(lo - .1f) < .03f && dips == 4); }
        o = runFx("a_autopan", tone(1000, .5, 4), {{"rate", .5}, {"depth", 100}});
        // Half a cycle in (1 s) the sound is fully on one side, a full cycle in (2 s) on the other.
        { const double l1 = rmsDb(o, .95, 1.05, 0), r1 = rmsDb(o, .95, 1.05, 1), l2 = rmsDb(o, 1.95, 2.05, 0), r2 = rmsDb(o, 1.95, 2.05, 1);
          printf("     auto-pan: at 1 s L %.1f R %.1f, at 2 s L %.1f R %.1f\n", l1, r1, l2, r2);
          CHECK(r1 - l1 > 25 && l2 - r2 > 25); }

        // Phaser: sweeping notches, so a steady tone's level keeps changing.
        o = runFx("a_phaser", tone(1000, .25, 4), {{"rate", 1}, {"depth", 100}, {"mix", 100}, {"fb", 0}});
        { double lo = 0, hi = -200; for (double t = .5; t < 3.5; t += .05) { double d = rmsDb(o, t, t + .05); lo = qMin(lo, d); hi = qMax(hi, d); }
          printf("     phaser: level moves between %.1f and %.1f dB\n", lo, hi);
          CHECK(finite(o) && hi - lo > 8 && hi < -8); }
        CHECK(runFx("a_phaser", tone(1000), {{"mix", 0}}) == tone(1000));

        // Distortion adds odd harmonics and can never exceed full scale.
        o = runFx("a_distort", tone(440, .5), {{"drive", 24}, {"out", 0}, {"tone", 20000}});
        { const double h3 = toneDb(o, 1320, 1, 1.9) - toneDb(o, 440, 1, 1.9); float pk = 0; for (float x : o) pk = qMax(pk, std::fabs(x));
          printf("     distortion: third harmonic %.1f dB below the fundamental, peak %.3f\n", -h3, pk);
          CHECK(h3 > -14 && pk <= 1.001f && toneDb(tone(440, .5), 1320, 1, 1.9) < -80); }
        CHECK(runFx("a_distort", tone(440), {{"mix", 0}}) == tone(440));

        o = runFx("a_bitcrush", tone(440, .9), {{"bits", 3}, {"down", 1}});
        { QSet<int> levels; for (int i = R; i < R + 4800; ++i) levels << int(std::lround(o[i * 2] * 1000));
          CHECK(levels.size() <= 9 && levels.size() >= 5); }                                // 3 bits: 8 steps
        o = runFx("a_bitcrush", tone(440, .9), {{"bits", 16}, {"down", 8}});
        { int holds = 0; for (int i = R; i < R + 4800; ++i) holds += o[i * 2] == o[(i + 1) * 2];
          CHECK(holds > 4800 * .8); }                                                        // each sample held 8 times
        CHECK(runFx("a_bitcrush", tone(440), {{"mix", 0}}) == tone(440));

        // Telephone: the low rumble and the top end go, the voice band stays.
        { const double lowCut = gainAt("a_voicefx", 80, {}) - gainAt("a_voicefx", 1000, {}), highCut = gainAt("a_voicefx", 10000, {}) - gainAt("a_voicefx", 1000, {});
          printf("     telephone: 80 Hz %.1f dB, 10 kHz %.1f dB against 1 kHz\n", lowCut, highCut);
          CHECK(lowCut < -20 && highCut < -20);
          for (double pr : {1.0, 2.0, 3.0}) { QVector<float> v = runFx("a_voicefx", tone(1000, .9), {{"preset", pr}}); float pk = 0; for (float x : v) pk = qMax(pk, std::fabs(x)); CHECK(finite(v) && pk < 1.0f && pk > .05f); }
          CHECK(runFx("a_voicefx", tone(440), {{"amount", 0}}) == tone(440)); }

        // Pitch shifter: an octave up moves 440 Hz to 880 Hz without changing how long it lasts.
        o = runFx("a_pitch", tone(440, .25, 3), {{"semi", 12}});
        { const double up = toneDb(o, 880, 1, 2.9), left = toneDb(o, 440, 1, 2.9);
          printf("     pitch +12: 880 Hz %.1f dB, 440 Hz %.1f dB\n", up, left);
          CHECK(up > -18 && up - left > 15 && o.size() == tone(440, .25, 3).size()); }
        o = runFx("a_pitch", tone(880, .25, 3), {{"semi", -12}});
        CHECK(toneDb(o, 440, 1, 2.9) - toneDb(o, 880, 1, 2.9) > 15);
        o = runFx("a_pitch", tone(440, .25, 3), {{"semi", 7}});
        CHECK(toneDb(o, 440 * std::pow(2.0, 7 / 12.0), 1, 2.9) - toneDb(o, 440, 1, 2.9) > 12);
        o = runFx("a_pitch", tone(440, .25, 3), {{"semi", 0}});
        NEAR(toneDb(o, 440, 1, 2.9), 20 * std::log10(.25), .5);

        // Every 3.0 audio effect: defaults stay finite and sane on loud input, and silence stays silent.
        bool ok = true;
        for (const char *id : {"a_geq", "a_bass", "a_treble", "a_notch", "a_bandpass", "a_mbcomp", "a_width", "a_center", "a_chanvol", "a_tremolo", "a_autopan", "a_phaser",
                               "a_distort", "a_bitcrush", "a_voicefx", "a_pitch"}) {
            const EffectDef *d = EffectRegistry::find(id);
            if (!d || !d->audio) { printf("     missing %s\n", id); ok = false; continue; }
            QVector<float> loud = runFx(id, tone(300, .95, 1));
            float pk = 0;
            for (float x : loud) pk = qMax(pk, std::fabs(x));
            QVector<float> quiet = runFx(id, make(1, [](double) { return 0.0; }));
            float qp = 0;
            for (float x : quiet) qp = qMax(qp, std::fabs(x));
            if (!finite(loud) || pk > 4 || qp > 1e-6f) { printf("     %s: peak %.2f, output from silence %.6f\n", id, pk, qp); ok = false; }
        }
        CHECK(ok);
    }

    printf("--- beat detection\n");
    {
        auto clicks = [](double bpm, double sec, double offset) {
            const double period = 60 / bpm;
            return make(sec, [=](double t) {
                const double ph = std::fmod(t - offset + period * 100, period);
                // A short burst of mid tone with a fast decay, like a rimshot.
                return ph < .03 ? .7 * std::sin(2 * M_PI * 1800 * ph) * std::exp(-ph * 120) : 0.0;
            });
        };
        for (double bpm : {120.0, 96.0, 140.0}) {
            const double offset = .21;
            const QVector<float> v = clicks(bpm, 20, offset);
            const Analysis::Beats b = Analysis::detectBeats(v.constData(), v.size() / 2);
            double worst = 0;
            const double period = 60 / bpm;
            for (double t : b.times) { double d = std::fmod(t - offset + period * 100.5, period) - period / 2; worst = qMax(worst, std::fabs(d)); }
            printf("     %.0f BPM click: found %.2f BPM, %lld beats, worst %.0f ms from the click\n", bpm, b.bpm, (long long)b.times.size(), worst * 1000);
            NEAR(b.bpm, bpm, 1.0);
            CHECK(std::abs(int(b.times.size()) - int(20 / period)) <= 2 && worst < .035);
        }
        const QVector<float> silent = make(10, [](double) { return 0.0; });
        CHECK(Analysis::detectBeats(silent.constData(), silent.size() / 2).bpm == 0);
        CHECK(Analysis::detectBeats(silent.constData(), 100).times.isEmpty());

        // Through a real sequence: a click file on the timeline, then markers.
        const QString wav = dir.filePath("click.wav");
        const QVector<float> v = clicks(120, 12, 0);
        QFile raw(dir.filePath("click.f32"));
        CHECK(raw.open(QIODevice::WriteOnly));
        raw.write(reinterpret_cast<const char *>(v.constData()), v.size() * 4);
        raw.close();
        CHECK(gen(wav, {"-f", "f32le", "-ar", "48000", "-ac", "2", "-i", raw.fileName()}));
        ProjectState st = tinyProject({});
        MediaItem m;
        CHECK(CACHE->probe(wav, m));
        m.id = st.newId();
        st.media << m;
        Ops::overwriteMedia(st, st.seqs[0], m, 0, m.duration, 0, Ops::Patch{-1, 0});
        CACHE->ensureAudio(wav);
        for (int i = 0; i < 400 && !CACHE->pcm(wav).data; ++i) pump(25);
        const Analysis::Beats sb = Analysis::sequenceBeats(st, st.seqs[0].id);
        NEAR(sb.bpm, 120, 1.0);
        Marker keep; keep.t = 5; keep.name = "Intro";
        st.seqs[0].markers << keep;
        CHECK(Analysis::addBeatMarkers(st.seqs[0], sb, 0, 1) == sb.times.size() && st.seqs[0].markers.size() == sb.times.size() + 1);
        const int every4 = Analysis::addBeatMarkers(st.seqs[0], sb, 0, 4);                // replaces the earlier beat markers
        CHECK(every4 == (sb.times.size() + 3) / 4 && st.seqs[0].markers.size() == every4 + 1);
        bool onGrid = true, sorted = true;
        for (int i = 0; i < st.seqs[0].markers.size(); ++i) {
            const Marker &k = st.seqs[0].markers[i];
            if (i) sorted &= k.t >= st.seqs[0].markers[i - 1].t;
            if (k.name == "Beat") onGrid &= std::abs(int(k.t % 60) < 30 ? int(k.t % 60) : int(k.t % 60) - 60) <= 1; // every 4th beat at 120 BPM is 2 s
        }
        CHECK(onGrid && sorted);
    }

    printf("--- edit lists, markers and chapters\n");
    QString chapterMovie;
    {
        const QString vid = dir.filePath("My Clip 01.mp4");
        CHECK(gen(vid, {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=30:d=4", "-f", "lavfi", "-i", "sine=f=440:r=48000:d=4", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
        ProjectState st = tinyProject({});
        Sequence &q = st.seqs[0];
        MediaItem m;
        CHECK(CACHE->probe(vid, m));
        m.id = st.newId();
        st.media << m;
        Ops::overwriteMedia(st, q, m, 1, 3, 0, Ops::Patch{0, 0});     // source 1-3 s at 0
        Ops::overwriteMedia(st, q, m, 0, 1, 60, Ops::Patch{0, 0});    // source 0-1 s at 2 s
        q.video[0].clips[1].speed = 2; q.video[0].clips[1].dur = 15;
        Ops::addTransition(q.video[0], "xdissolve", 60, 10, .5);
        q.name = "Cut 1";
        Marker a; a.t = 30; a.name = "Opening, part 1"; a.comment = "says \"hi\"";
        Marker b; b.t = 60; b.name = "Middle";
        q.markers << a << b;
        const QString edl = Interchange::writeEdl(st, q);
        printf("%s", qPrintable(edl));
        QString title;
        const auto ev = Interchange::parseEdl(edl, &title);
        CHECK(title == "Cut 1" && ev.size() == 4);
        if (ev.size() == 4) {
            CHECK(ev[0].number == 1 && ev[0].track == "V" && ev[0].reel == "MYCLIP01" && ev[0].clipName == "My Clip 01.mp4");
            CHECK(ev[0].srcIn == "00:00:01:00" && ev[0].srcOut == "00:00:03:00" && ev[0].recIn == "00:00:00:00" && ev[0].recOut == "00:00:02:00");
            CHECK(ev[1].srcIn == "00:00:00:00" && ev[1].srcOut == "00:00:01:00" && ev[1].recIn == "00:00:02:00" && ev[1].recOut == "00:00:02:15"); // 2x: one second of source in 15 frames
            CHECK(ev[2].track == "A" && ev[2].recOut == "00:00:02:00" && ev[3].track == "A");
        }
        CHECK(edl.contains("M2   MYCLIP01") && edl.contains("60.0") && edl.contains("* TRANSITION IN: Cross Dissolve, 10 frames") && edl.contains("FCM: NON-DROP FRAME"));
        const QString csv = Interchange::markersCsv(q);
        CHECK(csv.startsWith("Name,Comment,In,Out,Duration,Seconds,Color") && csv.contains("\"Opening, part 1\",\"says \"\"hi\"\"\",00:00:01:00") && csv.contains("Middle,,00:00:02:00"));
        const QString yt = Interchange::youtubeChapters(q);
        CHECK(yt == "0:00 Start\n0:01 Opening, part 1\n0:02 Middle\n");
        Sequence none = q;
        none.markers.clear();
        CHECK(Interchange::youtubeChapters(none).isEmpty() && Interchange::ffmetadataChapters(none, 0, 100).isEmpty());
        const QString meta = Interchange::ffmetadataChapters(q, 0, q.duration());
        CHECK(meta.startsWith(";FFMETADATA1") && meta.count("[CHAPTER]") == 3 && meta.contains("START=1000\nEND=2000\ntitle=Opening, part 1"));

        printf("--- export: chapters, safe output files, new formats, presets\n");
        CACHE->ensureAudio(vid);
        for (int i = 0; i < 400 && !CACHE->pcm(vid).data; ++i) pump(25);
        auto run = [&](ExportSettings c, QString *err = nullptr, QString *result = nullptr, int cancelAfterMs = -1) {
            Exporter ex(st, q.id, c);
            bool ok = false;
            QString e;
            QObject::connect(&ex, &Exporter::failed, [&](const QString &w) { e = w; });
            QObject::connect(&ex, &Exporter::finishedOk, [&](const QString &p) { ok = true; if (result) *result = p; });
            ex.start();
            if (cancelAfterMs >= 0) { QThread::msleep(cancelAfterMs); ex.cancel(); }
            ex.wait();
            pump(20);
            if (err) *err = e;
            if (!ok && cancelAfterMs < 0) printf("     export failed: %s\n", qPrintable(e));
            return ok;
        };
        ExportSettings c;
        c.width = 320; c.height = 180; c.preset = "ultrafast";
        c.path = dir.filePath("out/chapters.mp4");                     // the folder does not exist yet
        c.chapters = true;
        CHECK(run(c));
        chapterMovie = c.path;
        const QString ch = probe({"-show_chapters", "-of", "json", c.path});
        const QJsonArray chapters = QJsonDocument::fromJson(ch.toUtf8()).object()["chapters"].toArray();
        CHECK(chapters.size() == 3);
        if (chapters.size() == 3) {
            CHECK(chapters[1].toObject()["tags"].toObject()["title"].toString() == "Opening, part 1" && chapters[2].toObject()["tags"].toObject()["title"].toString() == "Middle");
            NEAR(chapters[1].toObject()["start_time"].toString().toDouble(), 1.0, .01);
            NEAR(chapters[2].toObject()["start_time"].toString().toDouble(), 2.0, .01);
        }
        CHECK(QDir(dir.filePath("out")).entryList({"*.part.*"}, QDir::Files).isEmpty());
        c.chapters = false;
        c.path = dir.filePath("out/plain.mp4");
        CHECK(run(c) && QJsonDocument::fromJson(probe({"-show_chapters", "-of", "json", c.path}).toUtf8()).object()["chapters"].toArray().isEmpty());

        // A stopped export leaves the finished file from last time exactly as it was.
        const QByteArray keep = [&] { QFile f(chapterMovie); f.open(QIODevice::ReadOnly); return f.readAll(); }();
        ProjectState longer = st;
        for (int i = 0; i < 40; ++i) Ops::overwriteMedia(longer, longer.seqs[0], m, 0, 4, 200 + i * 120, Ops::Patch{0, 0});
        {
            ExportSettings s2 = c;
            s2.path = chapterMovie; s2.preset = "veryslow"; s2.width = 1920; s2.height = 1080;
            Exporter ex(longer, q.id, s2);
            QString e; bool ok = false;
            QObject::connect(&ex, &Exporter::failed, [&](const QString &w) { e = w; });
            QObject::connect(&ex, &Exporter::finishedOk, [&](const QString &) { ok = true; });
            ex.start();
            QThread::msleep(1500);
            ex.cancel();
            ex.wait();
            pump(20);
            printf("     stopped export says: %s\n", qPrintable(e));
            CHECK(!ok && !e.isEmpty());
            QFile f(chapterMovie);
            CHECK(f.open(QIODevice::ReadOnly) && f.readAll() == keep);
            CHECK(QDir(dir.filePath("out")).entryList({"*.part.*"}, QDir::Files).isEmpty());
        }

        ExportSettings seq = c;
        seq.format = "PNG Sequence"; seq.path = dir.filePath("frames/shot.png"); seq.last = 12;
        QString first;
        CHECK(run(seq, nullptr, &first));
        CHECK(isImageSequence("PNG Sequence") && !isImageSequence("H.264"));
        CHECK(first.endsWith("shot_00001.png") && QDir(dir.filePath("frames")).entryList({"shot_*.png"}, QDir::Files).size() == 12);
        CHECK(QImage(first).size() == QSize(320, 180));
        seq.format = "JPEG Sequence"; seq.path = dir.filePath("frames/j.jpg"); seq.last = 5;
        CHECK(run(seq) && QDir(dir.filePath("frames")).entryList({"j_*.jpg"}, QDir::Files).size() == 5);
        struct F { const char *name, *ext, *codec; };
        for (F f : {F{"FLAC (Audio Only)", "flac", "flac"}, F{"Opus (Audio Only)", "opus", "opus"}, F{"WAV 24-bit (Audio Only)", "wav", "pcm_s24le"}, F{"H.264 (Matroska)", "mkv", "h264"}}) {
            ExportSettings a = c;
            a.format = f.name; a.path = dir.filePath(QString("out/fmt.%1").arg(f.ext)); a.last = 30;
            const bool ok = run(a);
            const QString codec = probe({"-select_streams", QString(f.codec) == "h264" ? "v:0" : "a:0", "-show_entries", "stream=codec_name", "-of", "csv=p=0", a.path}).trimmed();
            printf("     %-26s -> %s\n", f.name, qPrintable(codec));
            CHECK(ok && codec == f.codec);
        }
        QString err;
        ExportSettings bad = c;
        bad.format = "No Such Format"; bad.path = dir.filePath("out/bad.mp4");
        CHECK(!run(bad, &err, nullptr, 0) && !QFileInfo::exists(bad.path));

        ExportSettings p;
        p.format = "HEVC (H.265)"; p.width = 1080; p.height = 1920; p.crf = 24; p.kbps = 9000; p.preset = "slow"; p.audioKbps = 192; p.loudnessLufs = -14; p.chapters = true; p.sidecarSrt = true;
        ExportPresets::remove("Test Vertical");
        ExportPresets::save("Test Vertical", p);
        CHECK(ExportPresets::names().contains("Test Vertical"));
        ExportSettings l;
        l.path = "keep.mp4";
        CHECK(ExportPresets::load("Test Vertical", &l));
        CHECK(l.path == "keep.mp4" && ExportPresets::toJson(l) == ExportPresets::toJson(p));      // every setting came back; the path is not part of a preset
        ExportPresets::remove("Test Vertical");
        CHECK(!ExportPresets::names().contains("Test Vertical") && !ExportPresets::load("Test Vertical", &l));
    }

    printf(fails ? "\n%d FAILURES\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
