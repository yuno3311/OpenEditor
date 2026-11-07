// --v2-test <outdir>: drives every 2.0 tool through the same AI tool table the menus and MCP use,
// in the real window, with real media, and checks the project and the exported file afterwards.
#include "aitools.h"
#include "mainwindow.h"
#include "projectpanel.h"
#include "panels.h"
#include "../core/fonts.h"
#include "../core/mediacache.h"
#include "../core/ops.h"
#include "../core/project.h"
#include "../engine/analysis.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <random>

namespace {
int fails = 0;
void expect(bool ok, const QString &what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    fflush(stdout);
    fails += !ok;
}
void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(4); }
}
QString text(const QJsonObject &r) { return r["content"].toArray().first().toObject()["text"].toString(); }
QJsonObject call(const QString &n, const QJsonObject &a = {})
{
    QJsonObject r = AITOOLS->call(n, a);
    printf("     %s -> %s\n", qPrintable(n), qPrintable(text(r).left(150)));
    return r;
}
bool ffmpeg(const QStringList &a)
{
    QStringList args{"-hide_banner", "-loglevel", "error", "-y"};
    return QProcess::execute(CACHE->ffmpeg(), args + a) == 0;
}
QString waitJob(int id, int ms)
{
    QElapsedTimer t;
    t.start();
    QString st;
    while (t.elapsed() < ms) {
        st = QJsonDocument::fromJson(text(AITOOLS->call("job_status", {{"job_id", id}})).toUtf8()).object()["state"].toString();
        if (st != "running") break;
        pump(100);
    }
    return st;
}
double lufs(const QString &file)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(CACHE->ffmpeg(), {"-hide_banner", "-nostdin", "-i", file, "-af", "ebur128", "-f", "null", "-"});
    p.waitForFinished(120000);
    auto m = QRegularExpression("I:\\s+(-?[0-9.]+) LUFS").globalMatch(QString::fromUtf8(p.readAll()));
    double v = 0;
    while (m.hasNext()) v = m.next().captured(1).toDouble();
    return v;
}
} // namespace

int v2SelfTest(MainWindow &w, const QStringList &args)
{
    if (args.isEmpty()) return 2;
    // Forward slashes, so paths compare equal to the ones the project stores after import.
    const QString out = QDir::fromNativeSeparators(QDir(args[0]).absolutePath());
    QDir().mkpath(out);
    QDir(out).removeRecursively();
    QDir().mkpath(out);

    // Media: a shaky camera over a still scene, a dialogue track with two lines, and music.
    QImage sc(640, 360, QImage::Format_RGB32);
    sc.fill(qRgb(70, 90, 110));
    {
        QPainter p(&sc);
        std::mt19937 rng(3);
        std::uniform_int_distribution<int> X(0, 640), Y(0, 360), S(8, 70), C(0, 255);
        for (int i = 0; i < 260; ++i) { p.setBrush(QColor(C(rng), C(rng), C(rng))); p.drawEllipse(X(rng), Y(rng), S(rng), S(rng)); }
    }
    sc.save(out + "/scene.png");
    const QString vid = out + "/shaky.mp4", dlg = out + "/dialogue.wav", mus = out + "/music.wav";
    expect(ffmpeg({"-loop", "1", "-framerate", "30", "-t", "6", "-i", out + "/scene.png", "-vf", "crop=560:316:40+30*sin(n/3):22+20*cos(n/4),scale=640:360,setsar=1",
                   "-c:v", "libx264", "-pix_fmt", "yuv420p", vid}), "generated shaky video");
    expect(ffmpeg({"-f", "lavfi", "-i", "aevalsrc='if(between(t,1,2)+between(t,4,5),0.4*sin(2*PI*700*t)+0.004*random(0),0.004*random(0))':s=48000:d=6", "-ac", "2", dlg}), "generated dialogue");
    expect(ffmpeg({"-f", "lavfi", "-i", "sine=f=330:r=48000:d=6", "-af", "volume=0.3", "-ac", "2", mus}), "generated music");

    PRJ->reset();
    PRJ->loaded = true;
    w.showPage(MainWindow::PageEdit);
    w.project->importFiles({vid, dlg, mus});
    for (int i = 0; i < 400 && CACHE->pendingJobs(); ++i) pump(25);
    expect(PRJ->s.media.size() == 3, "imported three files");
    if (PRJ->s.media.size() != 3) return 1;
    MediaItem mv, md, mm;
    auto same = [](const QString &a, const QString &b) { return QFileInfo(a).canonicalFilePath().compare(QFileInfo(b).canonicalFilePath(), Qt::CaseInsensitive) == 0; };
    for (auto &m : PRJ->s.media) { if (same(m.path, vid)) mv = m; if (same(m.path, dlg)) md = m; if (same(m.path, mus)) mm = m; }
    expect(mv.id && md.id && mm.id, "found the imported files in the project");
    if (!mv.id || !md.id || !mm.id) return 1;
    PRJ->createSequence("V2", 640, 360, 30, 1);
    PRJ->edit("Build", [&](ProjectState &st) {
        Sequence &q = *st.seqById(st.activeSeq);
        Ops::overwriteMedia(st, q, mv, 0, mv.duration, 0, Ops::Patch{0, -1});
        Ops::overwriteMedia(st, q, md, 0, md.duration, 0, Ops::Patch{-1, 0});
        Ops::overwriteMedia(st, q, mm, 0, mm.duration, 0, Ops::Patch{-1, 1});
    });
    for (int i = 0; i < 400 && CACHE->pendingJobs(); ++i) pump(25);
    auto q = [] { return PRJ->active(); };
    const int vClip = q()->video[0].clips[0].id, dClip = q()->audio[0].clips[0].id, mClip = q()->audio[1].clips[0].id, musicTrack = q()->audio[1].id;

    QString names;
    for (auto v : QJsonDocument::fromJson(text(AITOOLS->call("list_effects", {})).toUtf8()).object()["effects"].toArray()) names += v.toObject()["id"].toString() + " ";
    for (const char *id : {"a_enhance", "a_denoise", "a_deess", "a_dehum", "a_gate", "a_chorus", "stabilize", "leavecolor", "colorkey", "findedges", "emboss",
                           "twirl", "spherize", "replicate", "linearwipe", "ramp", "strobe"})
        expect(names.contains(QString(id) + " "), QString("list_effects offers %1").arg(id));

    expect(!call("enhance_speech", {{"clip_ids", QJsonArray{dClip}}})["isError"].toBool() && q()->clip(dClip)->effect("a_enhance"), "Enhance Speech added to the dialogue clip");
    expect(call("enhance_speech", {{"clip_ids", QJsonArray{vClip}}})["isError"].toBool(), "Enhance Speech refuses a clip without audio");
    PRJ->setSelection({dClip});
    pump(300);
    w.grab().save(out + "/enhance_speech.png");

    QJsonObject r = call("auto_duck", {{"track_ids", QJsonArray{musicTrack}}});
    const Param *lv = q()->clip(mClip)->effect("volume")->find("level");
    const Clip *mc = q()->clip(mClip);
    expect(!r["isError"].toBool() && lv->animated, "Auto Ducking keyframed the music");
    expect(std::abs(lv->at(mc->kt(45)) + 15) < .1 && std::abs(lv->at(mc->kt(95))) < .1, "music is down 15 dB under the first line and back up between lines");
    expect(call("auto_duck", {{"track_ids", QJsonArray{q()->audio[0].id, musicTrack}}})["isError"].toBool(), "Auto Ducking refuses to duck every track");
    PRJ->undo.undo();
    expect(!q()->clip(mClip)->effect("volume")->find("level")->animated, "undo removes the ducking");
    call("auto_duck", {{"track_ids", QJsonArray{musicTrack}}});

    r = call("match_loudness", {{"target_lufs", -16}});
    expect(!r["isError"].toBool(), "Match Loudness ran");
    double now = Analysis::measureLufs(PRJ->s, q()->id);
    printf("     sequence now %.2f LUFS\n", now);
    expect(std::abs(now + 16) < .3, "the sequence measures -16 LUFS afterwards");
    expect(call("match_loudness", {{"target_lufs", 5}})["isError"].toBool(), "Match Loudness refuses a positive target");

    expect(text(call("remove_filler_words")).startsWith("No filler words"), "Remove Filler Words explains when there is no transcript");
    PRJ->edit("Words", [&](ProjectState &st) {
        Caption c;
        c.start = 30; c.end = 60;
        auto word = [](Frame a, Frame b, const char *t) { Word x; x.start = a; x.end = b; x.text = t; return x; };
        c.words = {word(30, 40, "Hello"), word(40, 46, "um"), word(46, 60, "there")};
        c.text = "Hello um there";
        st.seqById(st.activeSeq)->captions = {c};
    });
    const Frame before = q()->duration();
    r = call("remove_filler_words");
    expect(!r["isError"].toBool() && q()->duration() == before - 6, "Remove Filler Words cut the 6-frame 'um' from the footage");
    expect(q()->captions.size() == 1 && !q()->captions[0].text.contains("um"), "and from the transcript");

    const int job = QJsonDocument::fromJson(text(call("stabilize_clip", {{"clip_id", vClip}})).toUtf8()).object()["job_id"].toInt();
    expect(job > 0, "Warp Stabilizer started a job");
    expect(waitJob(job, 120000) == "done", "Warp Stabilizer finished");
    const Effect *se = q()->clip(vClip) ? q()->clip(vClip)->effect("stabilize") : nullptr;
    const int frames = se ? se->extra.value("tx").toList().size() : 0, want = int(q()->clip(vClip)->dur);
    printf("     camera path: %d frames for a %d-frame clip\n", frames, want);
    expect(se && std::abs(frames - want) <= 2, "the clip carries a camera path covering exactly its own source range");
    PRJ->setSelection({vClip});
    PRJ->setPlayhead(40);
    pump(500);
    w.grab().save(out + "/stabilizer.png");
    expect(!call("get_frame", {{"frame", 40}})["isError"].toBool(), "a stabilised frame renders");

    const QString mp4 = out + "/export.mp4";
    r = call("export_sequence", {{"path", mp4}, {"loudness_lufs", -14}});
    const int ej = QJsonDocument::fromJson(text(r).toUtf8()).object()["job_id"].toInt();
    expect(waitJob(ej, 300000) == "done", "export with a loudness target finished");
    const double got = lufs(mp4);
    printf("     exported file measures %.2f LUFS\n", got);
    expect(std::abs(got + 14) < .6, "the exported file is at -14 LUFS (FFmpeg ebur128)");
    expect(call("export_sequence", {{"path", out + "/bad.mp4"}, {"loudness_lufs", 3}})["isError"].toBool(), "export refuses a positive loudness target");

    // ------------------------------------------------------------ titles and fonts
    QDir(Fonts::folder()).removeRecursively();
    const QString qtFont = "C:/Qt/Examples/Qt-6.11.1/canvaspainter/gallery/fonts/Pacifico.ttf";
    const QString fontFile = args.size() > 1 ? args[1] : qtFont;
    if (!QFileInfo::exists(fontFile)) {
        printf("SKIP font import in the app: %s not found\n", qPrintable(fontFile));
    } else {
        QFile::copy(fontFile, out + "/Pacifico.ttf");
        r = call("import_font", {{"path", out + "/Pacifico.ttf"}});
        expect(!r["isError"].toBool() && text(r).contains("Pacifico"), "import_font added Pacifico");
        expect(text(call("list_fonts", {{"contains", "Pacific"}})).contains("\"imported\":true"), "list_fonts marks it as imported");
        expect(call("import_font", {{"path", out + "/scene.png"}})["isError"].toBool(), "import_font refuses an image");
        // The project panel's import takes font files too, and must not add them as media.
        const int mediaBefore = PRJ->s.media.size();
        QTimer::singleShot(800, [] { if (auto *m = QApplication::activeModalWidget()) m->close(); });
        w.project->importFiles({out + "/Pacifico.ttf"});
        expect(PRJ->s.media.size() == mediaBefore, "importing a font file from the project panel adds no media");
    }
    const int presets = QJsonDocument::fromJson(text(call("list_text_presets", {})).toUtf8()).array().size();
    expect(presets >= 10, "list_text_presets lists the built-in looks");
    r = call("add_title", {{"text", "Font test"}, {"frame", 10}, {"preset", "Lower Third Slide"}, {"style", QJsonObject{{"font", "Pacifico"}, {"glow", true}, {"shadowA", 60}}}});
    const int title = QJsonDocument::fromJson(text(r).toUtf8()).object()["clip_id"].toInt();
    const Clip *tc = q()->clip(title);
    expect(tc && tc->text.font == "Pacifico" && tc->text.glow && tc->text.bgPerLine && tc->text.animIn == TextAnimSlideUp && tc->text.shadowAngle == 60 && tc->text.text == "Font test",
           "add_title applied the preset, then the style on top");
    expect(call("set_text_style", {{"clip_id", title}, {"style", QJsonObject{{"colour", "#ff0000"}}}})["isError"].toBool(), "set_text_style names unknown keys instead of ignoring them");
    expect(call("set_text_style", {{"clip_id", vClip}, {"style", QJsonObject{{"size", 50}}}})["isError"].toBool(), "set_text_style refuses a video clip");
    call("set_text_style", {{"clip_id", title}, {"style", QJsonObject{{"weight", 900}, {"caps", 1}, {"fillType", 1}, {"fill2", "#ff3366ff"}, {"skew", 10}}}});
    expect(q()->clip(title)->text.weight == 900 && q()->clip(title)->text.caps == 1 && q()->clip(title)->text.fillType == 1, "set_text_style changed weight, caps and fill");
    PRJ->undo.undo();
    expect(q()->clip(title)->text.weight == 600 && q()->clip(title)->text.caps == 0, "undo puts the style back");
    expect(!call("apply_text_preset", {{"clip_id", title}, {"name", "Neon Glow"}})["isError"].toBool() && q()->clip(title)->text.glowStrength == 2, "apply_text_preset applied Neon Glow");
    expect(call("apply_text_preset", {{"clip_id", title}, {"name", "Nope"}})["isError"].toBool(), "apply_text_preset refuses an unknown preset");
    expect(!call("get_frame", {{"frame", 30}})["isError"].toBool(), "a frame with the animated title renders");

    const QString proj = out + "/titles.oep";
    const QJsonObject savedStyle = textStyleToJson(q()->clip(title)->text);
    expect(PRJ->save(proj), "project saved");
    expect(PRJ->load(proj), "project reloaded");
    const Clip *rc = PRJ->active() ? PRJ->active()->clip(title) : nullptr;
    expect(rc && textStyleToJson(rc->text) == savedStyle, "every title setting survived save and reload");

    PRJ->setSelection({title});
    PRJ->setPlayhead(40);
    w.panels->focusPanel("graphics");
    pump(600);
    w.grab().save(out + "/graphics_panel.png");
    QDir(Fonts::folder()).removeRecursively();

    PRJ->undo.setClean();
    printf(fails ? "\n%d V2 FAILURES\n" : "\nALL V2 TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
