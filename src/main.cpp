#include "core/fonts.h"
#include "core/mediacache.h"
#include "core/ops.h"
#include "core/project.h"
#include "engine/videoreader.h"
#include "engine/gpufx.h"
#include "engine/pluginhost.h"
#include "ui/aitools.h"
#include "ui/mcpserver.h"
#include "ui/mainwindow.h"
#include "ui/setupwizard.h"
#include "ui/theme.h"
#include "ui/pages.h"
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QIcon>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <cstdio>

// --ui-smoke <outdir> <video> [more media]: drives the real window through a scripted edit, saves a
// screenshot of each workspace and reports playback results. Used to verify the UI end to end.
static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(4); }
}
int textSelfTest(MainWindow &w, const QStringList &args);
int aiSelfTest(MainWindow &w, const QStringList &args);
int v2SelfTest(MainWindow &w, const QStringList &args);

static int failures = 0;
static void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    failures += !ok;
}
static void mouseEv(QWidget *w, QEvent::Type t, QPoint p, Qt::MouseButton b, Qt::MouseButtons bs, Qt::KeyboardModifiers m)
{
    QMouseEvent e(t, QPointF(p), QPointF(w->mapToGlobal(p)), b, bs, m);
    QApplication::sendEvent(w, &e);
}
static void drag(QWidget *w, QPoint a, QPoint b, Qt::KeyboardModifiers m = {})
{
    mouseEv(w, QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton, m);
    mouseEv(w, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton, m);
    for (int i = 1; i <= 5; ++i) mouseEv(w, QEvent::MouseMove, a + (b - a) * i / 5, Qt::NoButton, Qt::LeftButton, m);
    mouseEv(w, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton, m);
    QApplication::processEvents();
}
static void click(QWidget *w, QPoint p, Qt::KeyboardModifiers m = {}) { drag(w, p, p, m); }

// Real mouse events through every timeline tool, checked against the model, then undone.
static void interactionTests(MainWindow &w)
{
    auto *v = w.timeline->view;
    v->zoomFit();
    pump(50);
    auto q = [] { return PRJ->active(); };
    auto v1 = [&] { return q()->video[0].clips; };
    auto a1 = [&] { return q()->audio[0].clips; };
    const int yV = v->trackRect(true, 0).center().y(), yA = v->trackRect(false, 0).center().y();
    // Ids are never reused after undo and the playhead is not an edit, so both are excluded.
    auto editState = [] { auto j = stateToJson(PRJ->s); j.remove("nextId"); auto a = j["seqs"].toArray(); for (int i = 0; i < a.size(); ++i) { auto o = a[i].toObject(); o.remove("ph"); a[i] = o; } j["seqs"] = a; return j; };
    const QJsonObject start = editState();
    int mark = PRJ->undo.index();
    auto back = [&] { while (PRJ->undo.index() > mark) PRJ->undo.undo(); EDIT->setTool(Tool::Select); PRJ->setSelection({}); };
    auto X = [&](Frame f) { return v->xFromFrame(f); };
    const Frame len0 = v1()[0].dur;
    const int px = 60;
    const Frame df = Frame(px / v->ppf);

    Frame bStart = v1()[1].start;
    drag(v, {X(bStart + 60), yV}, {X(bStart + 60) + px, yV});
    expect(v1().size() == 2 && v1()[1].start > bStart && v1()[1].start == a1()[1].start, "selection: move drags clip with its linked audio");
    back();

    drag(v, {X(len0) - 2, yV}, {X(len0) - 2 - px, yV});
    expect(v1()[0].dur < len0 && v1()[0].dur == a1()[0].dur && v1()[1].start == bStart, "selection: trim out shortens linked pair, next clip stays");
    back();

    EDIT->setTool(Tool::Razor);
    click(v, {X(len0 / 2), yV});
    expect(v1().size() == 3 && a1().size() == 3 && q()->audio[1].clips.size() == 1, "razor: cuts clip and linked audio only");
    expect(v1()[0].end() == v1()[1].start && qAbs(v1()[1].srcIn - v1()[0].dur / q()->fps()) < 1e-6, "razor: cut is continuous in source time");
    back();

    EDIT->setTool(Tool::Ripple);
    drag(v, {X(len0) - 2, yV}, {X(len0) - 2 - px, yV});
    expect(v1()[0].end() == v1()[1].start && v1()[1].start < bStart, "ripple: following clip closes up");
    back();

    // Two adjacent pieces with handles for roll/slip/slide
    EDIT->setTool(Tool::Razor);
    click(v, {X(60), yV});
    click(v, {X(120), yV});
    click(v, {X(180), yV});
    expect(v1().size() == 5, "razor: three more cuts");
    int m2 = PRJ->undo.index();
    auto back2 = [&] { while (PRJ->undo.index() > m2) PRJ->undo.undo(); };

    Frame cut = v1()[1].start, tot = q()->duration();
    EDIT->setTool(Tool::Roll);
    drag(v, {X(cut) + 1, yV}, {X(cut) + 1 + px / 2, yV});
    expect(v1()[1].start != cut && v1()[0].end() == v1()[1].start && q()->duration() == tot, "roll: moves the cut, duration unchanged");
    back2();

    double in1 = v1()[1].srcIn;
    Frame s1 = v1()[1].start;
    EDIT->setTool(Tool::Slip);
    drag(v, {X(s1 + 30), yV}, {X(s1 + 30) + 20, yV});
    expect(v1()[1].srcIn < in1 && v1()[1].start == s1 && a1()[1].srcIn == v1()[1].srcIn, "slip: changes source in only, audio follows");
    back2();

    EDIT->setTool(Tool::Slide);
    drag(v, {X(s1 + 30), yV}, {X(s1 + 30) + 20, yV});
    expect(v1()[1].start > s1 && v1()[0].end() == v1()[1].start && v1()[1].end() == v1()[2].start && q()->duration() == tot, "slide: moves clip between neighbours");
    back2();

    Frame e2 = v1()[1].end();
    EDIT->setTool(Tool::Rate);
    drag(v, {X(e2) - 2, yV}, {X(e2) - 2 - 20, yV});
    expect(v1()[1].speed > 1 && v1()[1].end() < e2, "rate stretch: shorter clip plays faster");
    back();
    expect(editState() == start, "undo: every tool edit reverted exactly");

    drag(v, {X(bStart + 60), yV}, {X(120) + (X(bStart + 60) - X(bStart)), yV}, Qt::ControlModifier);
    expect(v1().size() == 3 && v1()[1].start < bStart && v1()[2].start >= v1()[1].end(), "ctrl-drag: insert splits the clip underneath");
    back();

    EDIT->setTool(Tool::Pen);
    QRect tr = v->trackRect(true, 0);
    drag(v, {X(30), tr.top() + 16}, {X(30), tr.top() + 28});
    auto op = [&] { return v1()[0].effect("opacity")->find("opacity"); };
    expect(op()->value < 100 && !op()->animated, "pen: dragging the band lowers opacity");
    click(v, {X(50), v->trackRect(true, 0).top() + 22}, Qt::ControlModifier);
    expect(op()->animated && op()->keys.size() == 1, "pen: ctrl-click adds a keyframe");
    back();

    drag(v, {X(bStart + 60), tr.bottom() + 8 > yA ? yA : tr.top() - 6}, {X(bStart + 90), yV});
    expect(!PRJ->selClips.isEmpty(), "marquee: selects clips");
    PRJ->setSelection({});

    click(v, {X(100), 20});
    expect(qAbs(q()->playhead - 100) <= 2, "ruler: click moves the playhead");

    PRJ->setSelection({v1()[0].id});
    pump(60);
    QRectF ir = w.program->viewer->imageRect();
    double posX = v1()[0].effect("motion")->find("posX")->value;
    drag(w.program->viewer, ir.center().toPoint(), ir.center().toPoint() + QPoint(40, 0));
    expect(v1()[0].effect("motion")->find("posX")->value > posX, "program monitor: dragging moves the selected clip");
    w.timeline->deleteSelection(true);
    expect(v1().size() == 1 && v1()[0].start == 0, "ripple delete: closes the gap on all sync tracks");
    back();

    expect(editState() == start, "undo: back to the starting state");

    QString tmp = QDir::temp().filePath("oe_roundtrip.oep");
    PRJ->save(tmp);
    QJsonObject before = stateToJson(PRJ->s);
    PRJ->load(tmp);
    expect(stateToJson(PRJ->s) == before, "project: save and reload is lossless");
    QFile::remove(tmp);

    w.raise();
    w.activateWindow();
    pump(200);
    QString lay = w.panels->saveLayout();
    auto *pg = w.panels->panel("program");
    w.panels->beginDrag(w.panels->panel("effects"));
    w.panels->dragEnd(pg->mapToGlobal(QPoint(pg->width() - 12, pg->height() / 2)));
    pump(50);
    expect(w.panels->saveLayout() != lay && w.panels->isVisible("effects"), "docking: dragging a panel splits the target group");
    w.panels->applyLayout(lay);
    pump(50);
}

static int uiSmoke(MainWindow &w, const QStringList &args)
{
    QString out = args[0];
    QDir().mkpath(out);
    auto shot = [&](const QString &n) { pump(250); w.grab().save(out + "/" + n + ".png"); };
    PRJ->reset();
    PRJ->loaded = true;
    w.showPage(MainWindow::PageEdit);
    w.setWorkspace("Editing");
    w.project->importFiles(args.mid(1));
    printf("imported %lld items\n", (long long)PRJ->s.media.size());
    const MediaItem m0 = PRJ->s.media.first();
    PRJ->createSequence("Sequence 01", m0.w > 0 ? m0.w : 1920, m0.h > 0 ? m0.h : 1080, 30, 1);
    int n = 0;
    PRJ->edit("Build", [&](ProjectState &st) {
        Sequence &q = *st.seqById(st.activeSeq);
        Frame at = 0;
        for (int i = 0; i < st.media.size(); ++i) {
            const MediaItem m = st.media[i];
            if (m.kind == MediaKind::Nested) continue;
            bool vid = m.kind != MediaKind::Audio;
            Ops::Patch p{vid ? 0 : -1, m.hasAudio ? (vid ? 0 : 1) : -1};
            Ops::overwriteMedia(st, q, m, 0, m.duration, vid ? at : 0, p);
            if (vid) at += qRound64(m.duration * q.fps());
            ++n;
        }
    });
    for (int i = 0; i < 400 && CACHE->pendingJobs(); ++i) pump(25);
    w.timeline->view->zoomFit();
    w.source->setMedia(m0.id);
    PRJ->setPlayhead(PRJ->active()->duration() / 3);
    interactionTests(w);
    shot("editing");

    auto *q = PRJ->active();
    w.timeline->razorAtPlayhead(true);
    printf("razor: V1 clips=%lld\n", (long long)q->video[0].clips.size());
    PRJ->setSelection(PRJ->withLinked({PRJ->active()->video[0].clips[0].id}));
    w.timeline->addTransitionAtPlayhead(true, true);
    w.timeline->addTextClip();
    PRJ->active()->video[1].clips.isEmpty();
    w.program->togglePlay();
    QElapsedTimer t; t.start();
    Frame f0 = PRJ->active()->playhead;
    pump(3000);
    Frame f1 = PRJ->active()->playhead;
    double fps = (f1 - f0) / (t.elapsed() / 1000.0);
    printf("playback: %.1f timeline fps, dropped %.1f%%\n", fps, w.program->engine->droppedRatio() * 100);
    printf("audio device started after %.0f ms\n", w.program->engine->audioClockLeadMs());
    w.program->togglePlay();
    shot("after_edit");
    for (auto &name : {"Assembly", "Color", "Effects", "Audio", "Graphics"}) { w.setWorkspace(name); shot(QString(name).toLower()); }
    w.setWorkspace("Editing");
    w.showPage(MainWindow::PageExport); shot("export");
    w.showPage(MainWindow::PageImport); shot("import");
    w.showPage(MainWindow::PageHome); shot("home");
    PRJ->undo.setClean();
    printf(failures ? "\n%d UI FAILURES\n" : "\nALL UI TESTS PASSED\n", failures);
    fflush(stdout);
    return failures ? 1 : 0;
}

// Saves the app logo as a Windows .ico (PNG entries) for the executable and the installer.
static bool writeIcon(const QString &path)
{
    const QIcon icon = appIcon();
    QList<QByteArray> pngs;
    const QList<int> sizes{16, 24, 32, 48, 64, 128, 256};
    for (int s : sizes) {
        QByteArray b;
        QBuffer buf(&b);
        buf.open(QIODevice::WriteOnly);
        if (!icon.pixmap(s, s).toImage().scaled(s, s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(&buf, "PNG")) return false;
        pngs << b;
    }
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << quint16(0) << quint16(1) << quint16(sizes.size());
    quint32 offset = 6 + 16 * sizes.size();
    for (int i = 0; i < sizes.size(); ++i) {
        const quint8 d = sizes[i] >= 256 ? 0 : quint8(sizes[i]);
        ds << d << d << quint8(0) << quint8(0) << quint16(1) << quint16(32) << quint32(pngs[i].size()) << offset;
        offset += pngs[i].size();
    }
    for (auto &p : pngs) out += p;
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}

static bool isTest(const QStringList &a) { return a.contains("--ui-smoke") || a.contains("--text-test") || a.contains("--ai-test") || a.contains("--setup-test") || a.contains("--v2-test"); }

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // The smoke test keeps its layouts and caches away from the user's own settings.
    QApplication::setApplicationName(isTest(QApplication::arguments()) ? "Open Editor Test" : "Open Editor");
    QApplication::setOrganizationName("Open Editor");
    QApplication::setApplicationVersion(OE_VERSION);
    Theme::apply(app);
    GpuFx::prepareOnMainThread();
    GpuFx::enabled = QSettings().value("gpu", true).toBool();
    VideoReader::hwDecode = QSettings().value("hwDecode", true).toBool();
    Fonts::loadImported();
    PluginHost::loadFrom(QApplication::applicationDirPath() + "/oe-plugins");
    PluginHost::loadFrom(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/plugins");
    VideoReader::setCacheBudget(qint64(QSettings().value("cacheGB", 3).toInt()) << 30);

    auto args = QApplication::arguments();
    if (int t = args.indexOf("--write-icon"); t >= 0 && t + 1 < args.size()) return writeIcon(args[t + 1]) ? 0 : 1;
    if (int t = args.indexOf("--setup-test"); t >= 0) return Setup::selfTest(args.mid(t + 1));
    if (!isTest(args) && Setup::needed() && !Setup::run(true)) return 1;

    MainWindow w;
    w.show();
    if (int t = args.indexOf("--text-test"); t >= 0) { pump(300); return textSelfTest(w, args.mid(t + 1)); }
    if (int t = args.indexOf("--ai-test"); t >= 0) { pump(300); return aiSelfTest(w, args.mid(t + 1)); }
    if (int t = args.indexOf("--v2-test"); t >= 0) { pump(300); return v2SelfTest(w, args.mid(t + 1)); }
    int smoke = args.indexOf("--ui-smoke");
    if (smoke >= 0 && args.size() > smoke + 2) {
        pump(300);
        return uiSmoke(w, args.mid(smoke + 1));
    }
    if (args.size() > 1 && args[1].endsWith(".oep") && PRJ->load(args[1])) w.showPage(MainWindow::PageEdit);
    return QApplication::exec();
}
