// --v3-test <outdir>: the 3.0 features in the real window with real media. It saves, crashes a
// second copy of the editor on purpose and recovers its work, relinks moved files, and drives
// every new tool through the same table the menus and MCP use.
#include "aitools.h"
#include "exportqueue.h"
#include "mainwindow.h"
#include "safety.h"
#include "setupwizard.h"
#include "../core/effectdefs.h"
#include "../core/mediacache.h"
#include "../core/mediamanage.h"
#include "../core/ops.h"
#include "../core/project.h"
#include "../engine/interchange.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <cstdio>

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
    printf("     %s -> %s\n", qPrintable(n), qPrintable(text(r).left(160)));
    fflush(stdout);
    return r;
}
QJsonObject obj(const QJsonObject &r) { return QJsonDocument::fromJson(text(r).toUtf8()).object(); }
QJsonArray arr(const QJsonObject &r) { return QJsonDocument::fromJson(text(r).toUtf8()).array(); }
bool ok(const QJsonObject &r) { return !r["isError"].toBool(); }
bool ffmpeg(const QStringList &a)
{
    QStringList args{"-hide_banner", "-loglevel", "error", "-y"};
    return QProcess::execute(CACHE->ffmpeg(), args + a) == 0;
}
QString ffprobe(const QStringList &a)
{
    QProcess p;
    p.start(CACHE->ffprobe(), QStringList{"-v", "error"} + a);
    p.waitForFinished(30000);
    return QString::fromUtf8(p.readAllStandardOutput());
}
// Presses a button of the dialog that is about to open, after saving a picture of it.
void pressWhenShown(const QString &buttonText, const QString &shot = {}, int after = 700)
{
    QTimer::singleShot(after, [=] {
        QWidget *m = QApplication::activeModalWidget();
        if (!m) return;
        if (!shot.isEmpty()) m->grab().save(shot);
        for (auto *b : m->findChildren<QAbstractButton *>())
            if (b->text().remove('&').contains(buttonText, Qt::CaseInsensitive)) { b->click(); return; }
        m->close();
    });
}
QJsonObject edited() // the project without ids that move and without playheads
{
    auto j = stateToJson(PRJ->s);
    j.remove("nextId");
    return j;
}
void cleanAppData()
{
    for (const char *sub : {"/AutoSave", "/Crashes"}) QDir(Vault::root() + sub).removeRecursively();
    // Leave our own session alone; drop whatever earlier test runs left behind.
    for (auto &o : Vault::Session::orphans()) Vault::Session::discard(o);
}
int mediaIdFor(const QString &path)
{
    for (auto &m : PRJ->s.media)
        if (QFileInfo(m.path).canonicalFilePath().compare(QFileInfo(path).canonicalFilePath(), Qt::CaseInsensitive) == 0) return m.id;
    return 0;
}
}

// The child of the crash test: real edits, a save, one more unsaved edit, then an access violation.
int v3CrashStage(MainWindow &w, const QStringList &args)
{
    if (args.size() < 2) return 2;
    const QString out = QDir::fromNativeSeparators(args[0]), vid = QDir::fromNativeSeparators(args[1]);
    SAFETY->recoveryDelayMs = 150;
    PRJ->reset();
    PRJ->loaded = true;
    w.showPage(MainWindow::PageEdit);
    AITOOLS->call("import_media", {{"paths", QJsonArray{vid}}});
    for (int i = 0; i < 400 && CACHE->pendingJobs(); ++i) pump(25);
    if (PRJ->s.media.isEmpty()) return 3;
    AITOOLS->call("create_sequence", {{"name", "Crash Cut"}, {"width", 640}, {"height", 360}});
    AITOOLS->call("add_clip", {{"media_id", PRJ->s.media.first().id}, {"frame", 0}});
    AITOOLS->call("add_marker", {{"frame", 10}, {"name", "SAVED MARK"}});
    if (!ok(AITOOLS->call("save_project", {{"path", out + "/Crash.oep"}}))) return 4;
    SAFETY->projectSwitched();
    AITOOLS->call("add_marker", {{"frame", 40}, {"name", "UNSAVED MARK"}});
    AITOOLS->call("split_at", {{"frame", 45}});
    for (int i = 0; i < 200 && !QFileInfo::exists(SAFETY->session.recoverFile()); ++i) pump(25);
    pump(400);
    printf("stage: recovery file %s\n", QFileInfo::exists(SAFETY->session.recoverFile()) ? "written" : "MISSING");
    fflush(stdout);
    Crash::crashNow();
}

int v3SelfTest(MainWindow &w, const QStringList &args)
{
    if (args.isEmpty()) return 2;
    const QString out = QDir::fromNativeSeparators(QDir(args[0]).absolutePath());
    QDir(out).removeRecursively();
    QDir().mkpath(out + "/media");
    cleanAppData();
    QSettings().remove("autosave");
    QSettings().remove("export/presets");
    SAFETY->recoveryDelayMs = 150;

    const QString vid = out + "/media/clip.mp4", vid2 = out + "/media/second.mp4", click = out + "/media/click.wav", still = out + "/media/still.png", roam = out + "/media/roaming.mp4";
    expect(ffmpeg({"-f", "lavfi", "-i", "testsrc2=s=640x360:r=30:d=6", "-f", "lavfi", "-i", "sine=f=440:r=48000:d=6", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", vid}), "generated video");
    expect(ffmpeg({"-f", "lavfi", "-i", "smptebars=s=320x240:r=30:d=4", "-f", "lavfi", "-i", "sine=f=660:r=48000:d=4", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", vid2}), "generated second video");
    expect(ffmpeg({"-f", "lavfi", "-i", "aevalsrc='if(lt(mod(t,0.5),0.03),0.7*sin(2*PI*1800*mod(t,0.5))*exp(-120*mod(t,0.5)),0)':s=48000:d=12", "-ac", "2", click}), "generated a 120 BPM click track");
    { QImage i(800, 400, QImage::Format_RGB32); i.fill(qRgb(40, 160, 220)); expect(i.save(still), "generated a still"); }
    expect(ffmpeg({"-f", "lavfi", "-i", "rgbtestsrc=s=320x240:r=30:d=3", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", roam}), "generated a video that will be moved");

    // ================================================================= safety
    printf("--- saving, versions and recovery\n");
    expect(SAFETY->session.active(), "the editor holds a session for crash recovery");
    expect(Setup::check().storage, "setup check: the auto-save folder can be written");
    PRJ->reset();
    PRJ->loaded = true;
    w.showPage(MainWindow::PageEdit);
    expect(ok(call("import_media", {{"paths", QJsonArray{vid, vid2, click, still, roam}}})), "imported five files");
    for (int i = 0; i < 600 && CACHE->pendingJobs(); ++i) pump(25);
    const int mVid = mediaIdFor(vid), mVid2 = mediaIdFor(vid2), mClick = mediaIdFor(click), mStill = mediaIdFor(still), mRoam = mediaIdFor(roam);
    expect(mVid && mVid2 && mClick && mStill && mRoam, "found the imported media");
    if (!mVid || !mVid2 || !mClick || !mStill || !mRoam) return 1;
    call("create_sequence", {{"name", "Main"}, {"width", 640}, {"height", 360}});
    call("add_clip", {{"media_id", mVid}, {"frame", 0}, {"out_seconds", 3}});
    auto q = [] { return PRJ->active(); };
    const QString proj = out + "/Film.oep";
    expect(ok(call("save_project", {{"path", proj}})) && !PRJ->isDirty(), "saved the project");
    SAFETY->projectSwitched();
    expect(!QFileInfo::exists(SAFETY->session.recoverFile()), "a saved project has no recovery file");

    call("add_marker", {{"frame", 20}, {"name", "First"}});
    for (int i = 0; i < 120 && !QFileInfo::exists(SAFETY->session.recoverFile()); ++i) pump(25);
    pump(300);
    ProjectState rec;
    expect(Vault::read(SAFETY->session.recoverFile(), &rec) == Vault::LoadResult::Ok && stateToJson(rec) == stateToJson(PRJ->s), "an unsaved edit is in the recovery file moments later");
    {
        ProjectState onDisk;
        Vault::read(proj, &onDisk);
        expect(onDisk.seqs[0].markers.isEmpty(), "and the project file itself was not touched");
    }

    QString v1;
    expect(SAFETY->autosaveNow(false, &v1) && QFileInfo::exists(v1), "auto-save wrote a version of the changed project");
    expect(!SAFETY->autosaveNow(false), "auto-save skips a project that has not changed since");
    const QJsonObject atV1 = edited();
    call("add_marker", {{"frame", 50}, {"name", "Second"}});
    SAFETY->setAutosaveIntervalMs(250);
    const QString before = SAFETY->lastAutosave;
    for (int i = 0; i < 80 && SAFETY->lastAutosave == before; ++i) pump(25);
    expect(SAFETY->lastAutosave != before && QFileInfo::exists(SAFETY->lastAutosave), "the auto-save timer writes a new version by itself");
    SAFETY->applyOptions(); // back to the real interval
    const QJsonArray versions = arr(call("list_versions"));
    expect(versions.size() == 2 && versions[0].toObject()["kind"].toString() == "autosave", "list_versions shows both versions, newest first");

    expect(ok(call("save_project")) && !PRJ->isDirty(), "saved again");
    SAFETY->projectSwitched();
    pump(200);
    expect(!QFileInfo::exists(SAFETY->session.recoverFile()), "saving clears the recovery file");
    {
        ProjectState bak;
        expect(Vault::read(Vault::backupPath(proj), &bak) == Vault::LoadResult::Ok && bak.seqs[0].markers.isEmpty(), "the previous save is kept as Film.oep.bak");
    }
    expect(arr(call("list_versions")).size() == 3, "the backup is listed with the auto-saves");

    // Restore the first auto-save (one marker) over the current state (two markers).
    int idx = -1;
    for (auto v : arr(AITOOLS->call("list_versions", {}))) if (v.toObject()["path"].toString() == v1) idx = v.toObject()["index"].toInt();
    expect(idx >= 0 && ok(call("restore_version", {{"index", idx}})), "restore_version ran");
    expect(edited() == atV1 && q()->markers.size() == 1 && PRJ->isDirty() && PRJ->filePath == proj, "the project is back at that version, unsaved, still pointing at Film.oep");
    expect(call("restore_version", {{"index", 99}})["isError"].toBool(), "restore_version refuses a version that does not exist");
    expect(ok(call("autosave_now")), "autosave_now writes a version on request");
    expect(ok(call("save_project")), "saved the restored state");
    SAFETY->projectSwitched();

    // Restore Version dialog
    pressWhenShown("Cancel", out + "/dialog_versions.png");
    SAFETY->restoreVersionDialog(&w);
    expect(QFileInfo::exists(out + "/dialog_versions.png"), "the Restore Version dialog opened");

    // Save Incremental
    expect(MainWindow::nextIncrement(out + "/Film.oep").endsWith("/Film_v002.oep") && MainWindow::nextIncrement(out + "/Film_v002.oep").endsWith("/Film_v003.oep")
           && MainWindow::nextIncrement(out + "/Cut_v41.oep").endsWith("/Cut_v042.oep"), "incremental names count up: Film, Film_v002, Film_v003");
    auto action = [&](const QString &name) -> QAction * {
        for (auto *a : w.findChildren<QAction *>()) if (a->text().remove('&') == name) return a;
        return nullptr;
    };
    for (const char *n : {"Save a Copy...", "Save Incremental", "Revert to Saved", "Restore Version...", "Link Media...", "Project Manager...", "Remove Unused Media", "Project Report...",
                          "Edit Decision List (EDL)...", "Markers as CSV...", "Markers as YouTube Chapters...", "Select Clips at Playhead", "Duplicate", "Replace With Clip From Source Monitor",
                          "Fade In / Out...", "Match Frame", "Close All Gaps", "Extend Selected Edit to Playhead", "Auto Reframe Sequence...", "Add Beat Markers...", "Clear Beat Markers"})
        expect(action(n) != nullptr, QString("menu has \"%1\"").arg(n));
    if (auto *a = action("Save Incremental")) a->trigger();
    expect(PRJ->filePath.endsWith("/Film_v002.oep") && QFileInfo::exists(out + "/Film_v002.oep") && QFileInfo::exists(proj) && !PRJ->isDirty(), "Save Incremental saved Film_v002.oep and moved to it");

    // A damaged project file: the editor offers the newest version that still opens.
    const QString broken = out + "/Broken.oep";
    expect(PRJ->save(broken), "saved Broken.oep");
    call("add_marker", {{"frame", 70}, {"name", "Kept in backup"}});
    expect(PRJ->save(broken), "saved it again, so a backup exists");
    const QJsonObject good = edited();
    { QFile f(broken); f.open(QIODevice::ReadWrite); f.resize(f.size() / 3); }
    expect(!PRJ->load(broken) && PRJ->lastLoad == Vault::LoadResult::Corrupt, "a truncated project file is reported as damaged, not opened as garbage");
    expect(PRJ->filePath == broken && edited() == good, "and the open project is left alone");
    pressWhenShown("Earlier", out + "/dialog_damaged.png");
    w.openFile(broken);
    expect(PRJ->filePath == broken && PRJ->isDirty() && q() && q()->markers.size() == 1, "opening it offers the earlier version, which opens as unsaved changes");
    expect(PRJ->save(broken) && PRJ->load(broken), "saving repairs the file");
    SAFETY->projectSwitched();

    // ----------------------------------------------------------------- crash and recover
    printf("--- crash recovery across two processes\n");
    QDir().mkpath(out + "/crash");
    expect(Vault::Session::orphans().isEmpty(), "nothing to recover before the crash");
    QProcess child;
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QApplication::applicationFilePath(), {"--crash-stage", out + "/crash", vid});
    bool done = child.waitForFinished(120000);
    printf("     child output: %s\n", qPrintable(QString::fromLocal8Bit(child.readAll()).simplified()));
    printf("     child exit code 0x%08X\n", unsigned(child.exitCode()));
    expect(done && child.exitCode() != 0, "the second editor crashed");
    const auto orphans = Vault::Session::orphans();
    expect(orphans.size() == 1, "its session is found as recoverable");
    if (orphans.size() == 1) {
        const auto o = orphans[0];
        expect(o.name == "Crash" && QDir::cleanPath(o.projectPath) == QDir::cleanPath(out + "/crash/Crash.oep"), "it names the project it was working on");
        expect(!o.crashReport.isEmpty() && QFileInfo::exists(o.crashReport) && QFileInfo(QString(o.crashReport).replace(".txt", ".dmp")).size() > 1000, "a crash report and a minidump were written");
        expect(Setup::check().recoverable == 1, "the setup check reports work waiting to be recovered");
        pressWhenShown("Recover", out + "/dialog_recover.png");
        const bool recovered = w.startupRecovery();
        expect(recovered && QFileInfo::exists(out + "/dialog_recover.png"), "the recovery dialog opened and Recover was pressed");
        QStringList names;
        if (q()) for (auto &m : q()->markers) names << m.name;
        expect(q() && names == QStringList{"SAVED MARK", "UNSAVED MARK"} && q()->video[0].clips.size() == 2, "the unsaved marker and the unsaved cut are back");
        expect(PRJ->isDirty() && QDir::cleanPath(PRJ->filePath) == QDir::cleanPath(out + "/crash/Crash.oep"), "recovered work is unsaved and Save goes to the original file");
        ProjectState disk;
        Vault::read(out + "/crash/Crash.oep", &disk);
        expect(disk.seqs.size() == 1 && disk.seqs[0].markers.size() == 1, "the file on disk still holds only what was saved");
        expect(Vault::Session::orphans().isEmpty(), "the crashed session is cleaned up after recovery");
        expect(QFileInfo::exists(SAFETY->session.recoverFile()), "and the recovered work is already protected by this session");
        expect(ok(call("save_project")), "saved the recovered project");
        Vault::read(out + "/crash/Crash.oep", &disk);
        expect(disk.seqs[0].markers.size() == 2, "now the file has the recovered work");
    }

    // ================================================================= media management
    printf("--- media management\n");
    expect(PRJ->load(proj), "reopened Film.oep");
    w.afterProjectOpened();
    call("add_clip", {{"media_id", mClick}, {"frame", 0}, {"audio_track", 2}});
    expect(arr(call("list_offline_media")).isEmpty(), "everything is online");
    QDir().mkpath(out + "/moved/deep");
    pump(1500); // thumbnail readers let go of the file
    expect(QFile::rename(roam, out + "/moved/deep/roaming.mp4"), "moved roaming.mp4 to another folder");
    const QJsonArray off = arr(call("list_offline_media"));
    expect(off.size() == 1 && off[0].toObject()["id"].toInt() == mRoam, "list_offline_media reports the moved file");
    expect(call("relink_media", {{"folder", out + "/nowhere"}})["isError"].toBool(), "relink_media refuses a folder that does not exist");
    pressWhenShown("Cancel", out + "/dialog_link_media.png");
    if (auto *a = action("Link Media...")) a->trigger();
    expect(QFileInfo::exists(out + "/dialog_link_media.png"), "the Link Media dialog opened");
    const QJsonObject rl = obj(call("relink_media", {{"folder", out + "/moved"}}));
    expect(rl["relinked"].toInt() == 1 && rl["still_offline"].toInt() == 0, "relink_media found it in a sub-folder");
    expect(PRJ->s.mediaById(mRoam)->path.endsWith("/moved/deep/roaming.mp4") && !PRJ->s.mediaById(mRoam)->offline, "the media points at the new place");
    call("add_clip", {{"media_id", mRoam}, {"frame", 300}});
    {
        const QByteArray png = QByteArray::fromBase64(AITOOLS->call("get_frame", {{"frame", 320}})["content"].toArray().first().toObject()["data"].toString().toLatin1());
        const QImage f = QImage::fromData(png, "PNG");
        const QRgb top = f.isNull() ? 0 : f.pixel(f.width() / 2, f.height() * 3 / 10);
        printf("     relinked clip renders a %dx%d frame, top band %d,%d,%d\n", f.width(), f.height(), qRed(top), qGreen(top), qBlue(top));
        expect(!f.isNull() && qRed(top) > 100 && qGreen(top) < 40 && qBlue(top) < 40, "and its frames render from there (the red ramp of the RGB test picture)");
    }
    expect(call("relink_media", {{"media_id", mRoam}, {"path", out + "/moved/missing.mp4"}})["isError"].toBool(), "relinking to a missing file is refused");
    PRJ->undo.undo();
    expect(!PRJ->s.mediaById(mRoam)->offline, "the link survives undoing the clip that used it");

    const QJsonObject rep = obj(call("project_report"));
    expect(rep["media"].toObject()["video"].toInt() == 3 && rep["media"].toObject()["unused"].toInt() == 3 && rep["media"].toObject()["offline"].toInt() == 0 && rep["sequences"].toArray().size() == 1, "project_report counts media, unused files and sequences");
    pressWhenShown("Close", out + "/dialog_report.png");
    if (auto *a = action("Project Report...")) a->trigger();
    expect(QFileInfo::exists(out + "/dialog_report.png"), "the Project Report dialog opened");

    const QJsonObject col = obj(call("collect_project", {{"folder", out + "/collected"}, {"name", "Packed"}}));
    expect(col["files_copied"].toInt() == 2 && QFileInfo::exists(out + "/collected/Packed.oep"), "collect_project copied the two files in use");
    {
        ProjectState packed;
        bool inside = Vault::read(out + "/collected/Packed.oep", &packed) == Vault::LoadResult::Ok;
        for (auto &m : packed.media) if (!m.path.isEmpty()) inside &= m.path.startsWith(out + "/collected/Media/") && QFileInfo::exists(m.path);
        expect(inside && !packed.media.isEmpty(), "the collected project opens and all its media is inside its own folder");
    }
    expect(text(call("remove_unused_media")).contains("3"), "remove_unused_media removed the three unused items");
    expect(!PRJ->s.mediaById(mVid2) && !PRJ->s.mediaById(mStill) && !PRJ->s.mediaById(mRoam) && PRJ->s.mediaById(mVid) && PRJ->s.mediaById(mClick), "only unused media went");
    PRJ->undo.undo();
    expect(PRJ->s.mediaById(mVid2) && PRJ->s.mediaById(mStill), "undo brings them back");

    // ================================================================= timeline tools
    printf("--- timeline tools\n");
    PRJ->edit("Rebuild", [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        for (auto *t : s.allTracks()) { t->clips.clear(); t->transitions.clear(); }
        s.markers.clear();
        Ops::overwriteMedia(st, s, *st.mediaById(mVid), 0, 2, 30, Ops::Patch{0, 0});    // 30..90
        Ops::overwriteMedia(st, s, *st.mediaById(mVid), 2, 4, 150, Ops::Patch{0, 0});   // 150..210
    });
    auto v1c = [&] { return q()->video[0].clips; };
    const int mark = PRJ->undo.index();
    expect(text(call("close_gaps")).contains("2") && v1c()[0].start == 0 && v1c()[1].start == 60 && q()->audio[0].clips[1].start == 60, "close_gaps closed both gaps on video and audio together");
    expect(text(call("close_gaps")).contains("0"), "and finds nothing more to close");

    const int a = v1c()[0].id, b = v1c()[1].id;
    QJsonObject dup = obj(call("duplicate_clips", {{"clip_ids", QJsonArray{a}}}));
    expect(dup["new_clip_ids"].toArray().size() == 2 && v1c().size() == 3 && v1c()[1].start == 60 && v1c()[2].id == b && v1c()[2].start == 120, "duplicate_clips copied the clip with its linked audio and moved the rest along");
    expect(v1c()[1].link != v1c()[0].link && v1c()[1].link == q()->audio[0].clips[1].link, "the copy has its own link");
    PRJ->undo.undo();
    expect(v1c().size() == 2, "undo removes the duplicate");

    expect(ok(call("replace_clip", {{"clip_id", b}, {"media_id", mVid2}})) && q()->clip(b)->mediaId == mVid2 && q()->clip(b)->start == 60 && q()->clip(b)->dur == 60, "replace_clip swapped the source in place");
    expect(call("replace_clip", {{"clip_id", b}, {"media_id", mClick}})["isError"].toBool(), "replace_clip refuses audio for a video clip");
    expect(!call("get_frame", {{"frame", 80}})["isError"].toBool(), "the replaced clip renders");

    const Frame end0 = q()->clip(b)->end();
    expect(ok(call("extend_to_frame", {{"clip_ids", QJsonArray{b}}, {"frame", double(end0 + 20)}})) && q()->clip(b)->end() == end0 + 20, "extend_to_frame grew the clip to the frame");
    int linkedAudio = 0;
    for (auto &c : q()->audio[0].clips) if (c.link == q()->clip(b)->link) linkedAudio = c.id;
    expect(linkedAudio && q()->clip(linkedAudio)->end() == end0 + 20, "and its linked audio with it");
    expect(call("extend_to_frame", {{"clip_ids", QJsonArray{b}}, {"frame", double(end0 + 20)}})["isError"].toBool(), "extend_to_frame says so when nothing can move");

    expect(ok(call("fade_clip", {{"clip_id", a}, {"fade_in_frames", 15}, {"fade_out_frames", 15}})), "fade_clip ran");
    {
        const Clip &c = *q()->clip(a);
        const Param *op = c.effect("opacity")->find("opacity");
        expect(op->animated && op->at(c.kt(c.start)) == 0 && op->at(c.kt(c.start + 15)) == 100 && op->at(c.kt(c.end())) == 0, "the clip fades from 0 to 100 and back to 0");
    }
    expect(ok(call("set_keyframe_interpolation", {{"clip_id", a}, {"type", "ease_out"}})) && q()->clip(a)->effect("opacity")->find("opacity")->keys[0].i == Interp::EaseOut, "set_keyframe_interpolation changed the keyframes to ease out");
    expect(call("set_keyframe_interpolation", {{"clip_id", a}, {"type", "wobbly"}})["isError"].toBool(), "and refuses an unknown type");
    expect(call("set_keyframe_interpolation", {{"clip_id", b}, {"type", "ease"}})["isError"].toBool(), "and says so when a clip has no keyframes");

    expect(ok(call("apply_layout", {{"clip_id", b}, {"preset", "PiP Bottom Right"}})), "apply_layout ran");
    {
        const Effect *mo = q()->clip(b)->effect("motion");
        expect(mo->find("posX")->value > 480 && mo->find("posY")->value > 270 && mo->find("scale")->value < 60, "the clip sits small in the bottom right");
    }
    expect(call("apply_layout", {{"clip_id", b}, {"preset", "Sideways"}})["isError"].toBool(), "apply_layout refuses an unknown preset");
    expect(call("apply_layout", {{"clip_id", linkedAudio}, {"preset", "Fit Frame"}})["isError"].toBool(), "and an audio clip");

    const int seqBefore = PRJ->s.seqs.size(), mainSeq = q()->id;
    const QJsonObject rf = obj(call("auto_reframe", {{"width", 360}, {"height", 640}}));
    expect(PRJ->s.seqs.size() == seqBefore + 1 && q()->id == rf["sequence_id"].toInt() && q()->w == 360 && q()->h == 640 && q()->video[0].clips.size() == 2, "auto_reframe made a vertical copy and opened it");
    {
        QJsonObject fr = call("get_frame", {{"frame", 30}});
        expect(!fr["isError"].toBool(), "the reframed sequence renders");
    }
    expect(call("auto_reframe", {{"width", 4}, {"height", 640}})["isError"].toBool(), "auto_reframe refuses a silly size");
    call("set_active_sequence", {{"id", mainSeq}});

    // Menu commands that act directly
    PRJ->setPlayhead(10);
    if (auto *ac = action("Select Clips at Playhead")) ac->trigger();
    expect(PRJ->selClips.contains(a) && PRJ->selClips.size() == 2, "Select Clips at Playhead selects the clips under it");
    w.source->setMedia(0);
    PRJ->setPlayhead(75);
    PRJ->setSelection({});
    if (auto *ac = action("Match Frame")) ac->trigger();
    expect(w.source->mediaId == mVid2, "Match Frame opens the clip under the playhead in the Source Monitor");
    while (PRJ->undo.index() > mark) PRJ->undo.undo();
    expect(v1c().size() == 2 && v1c()[0].start == 30, "every timeline edit undoes back to where it started");

    // ================================================================= effects through the app
    printf("--- new effects and transitions in the app\n");
    QString names, trans;
    {
        const QJsonObject fxList = obj(AITOOLS->call("list_effects", {}));
        for (auto v : fxList["effects"].toArray()) names += v.toObject()["id"].toString() + " ";
        for (auto v : fxList["transitions"].toArray()) trans += v.toObject()["id"].toString() + " ";
    }
    const QStringList vfx{"unsharp", "radialblur", "tiltshift", "grain", "rgbsplit", "glitch", "scanlines", "halftone", "threshold", "solarize", "gradientmap", "hslsecondary", "changecolor",
                          "letterbox", "shapemask", "kaleido", "shake", "timecode"};
    const QStringList afx{"a_geq", "a_bass", "a_treble", "a_notch", "a_bandpass", "a_mbcomp", "a_width", "a_center", "a_chanvol", "a_tremolo", "a_autopan", "a_phaser", "a_distort", "a_bitcrush",
                          "a_voicefx", "a_pitch"};
    bool listed = true;
    for (auto &id : vfx + afx) if (!names.contains(id + " ")) { listed = false; printf("     not listed: %s\n", qPrintable(id)); }
    expect(listed, "list_effects offers all 34 new effects");
    bool tl = true;
    for (const char *id : {"wipeU", "wipediag", "blinds", "checker", "pushU", "zoomin", "spin", "blurdissolve"}) tl &= trans.contains(QString(id) + " ");
    expect(tl, "and the 8 new transitions");
    const int vc = v1c()[0].id, ac0 = q()->audio[0].clips[0].id;
    auto frameData = [&](Frame f) { return AITOOLS->call("get_frame", {{"frame", double(f)}})["content"].toArray().first().toObject()["data"].toString(); };
    const QString clean = frameData(50);
    bool allChange = true, allAdd = true;
    for (auto &id : vfx) {
        allAdd &= ok(AITOOLS->call("add_effect", {{"clip_id", vc}, {"effect", id}}));
        const QString got = frameData(50);
        // HSL Secondary starts neutral, like any colour corrector: it selects but changes nothing yet.
        if (id == "hslsecondary") {
            const bool neutral = got == clean;
            AITOOLS->call("set_effect_param", {{"clip_id", vc}, {"effect", id}, {"param", "hue"}, {"value", 120}});
            AITOOLS->call("set_effect_param", {{"clip_id", vc}, {"effect", id}, {"param", "range"}, {"value", 180}});
            if (!neutral || frameData(50) == clean) { allChange = false; printf("     hslsecondary: neutral %d\n", neutral); }
        } else if (got == clean) { allChange = false; printf("     %s did not change the picture\n", qPrintable(id)); }
        AITOOLS->call("remove_effect", {{"clip_id", vc}, {"effect", id}});
    }
    expect(allAdd && allChange, "each new video effect can be added in the app and changes the rendered frame");
    expect(frameData(50) == clean, "removing them gives back the original frame exactly");
    bool audioOk = true;
    for (auto &id : afx) audioOk &= ok(AITOOLS->call("add_effect", {{"clip_id", ac0}, {"effect", id}}));
    expect(audioOk && q()->clip(ac0)->fx.size() == 2 + afx.size(), "all 16 new audio effects are on one clip");
    w.panels->focusPanel("effectcontrols");
    PRJ->setSelection({ac0});
    pump(500);
    w.grab().save(out + "/effect_controls.png");
    {
        // The whole chain renders through the exporter without failing.
        ExportSettings c;
        c.format = "WAV (Audio Only)"; c.path = out + "/chain.wav"; c.last = 60;
        Exporter ex(PRJ->s, q()->id, c);
        bool fine = false;
        QObject::connect(&ex, &Exporter::finishedOk, [&](const QString &) { fine = true; });
        ex.start();
        while (ex.isRunning()) pump(50);
        pump(50);
        expect(fine && QFileInfo(c.path).size() > 10000, "a clip with every new audio effect exports");
    }
    for (auto &id : afx) AITOOLS->call("remove_effect", {{"clip_id", ac0}, {"effect", id}});
    // A new transition between two touching clips, rendered at its midpoint.
    PRJ->edit("Touch", [&](ProjectState &st) { Sequence &s = *st.seqById(st.activeSeq); Ops::closeGaps(st, s); Ops::replaceClip(st, s, s.video[0].clips[1].id, mVid2); });
    const Frame cut = v1c()[1].start;
    const QString hard = frameData(cut - 3);
    expect(ok(call("add_transition", {{"track_id", q()->video[0].id}, {"frame", double(cut)}, {"transition", "checker"}, {"duration_frames", 20}})) && q()->video[0].transitions.size() == 1, "added the Checker Wipe transition");
    expect(frameData(cut - 3) != hard, "and it changes the frames around the cut");

    // ================================================================= beats
    printf("--- beat markers\n");
    PRJ->edit("Music", [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        for (auto *t : s.allTracks()) { t->clips.clear(); t->transitions.clear(); }
        s.markers.clear();
        Ops::overwriteMedia(st, s, *st.mediaById(mClick), 0, 12, 0, Ops::Patch{-1, 0});
        Ops::overwriteMedia(st, s, *st.mediaById(mVid), 0, 6, 0, Ops::Patch{0, -1});
    });
    for (int i = 0; i < 400 && !CACHE->pcm(click).data; ++i) pump(25);
    const QJsonObject beats = obj(call("detect_beats", {{"add_markers", true}, {"every", 4}}));
    expect(std::fabs(beats["bpm"].toDouble() - 120) < 1.5, QString("detect_beats found %1 BPM in a 120 BPM track").arg(beats["bpm"].toDouble()));
    expect(beats["markers_added"].toInt() >= 5 && q()->markers.size() == beats["markers_added"].toInt(), "and put a marker on every 4th beat");
    bool grid = !q()->markers.isEmpty();
    for (auto &m : q()->markers) { const int r = int(m.t % 60); grid &= r <= 1 || r >= 59; }
    expect(grid, "the markers sit on the bars, two seconds apart");
    if (auto *ac = action("Clear Beat Markers")) ac->trigger();
    expect(q()->markers.isEmpty(), "Clear Beat Markers removes them");
    PRJ->edit("Silence", [&](ProjectState &st) { st.seqById(st.activeSeq)->audio[0].clips.clear(); });
    expect(call("detect_beats")["isError"].toBool(), "detect_beats says so when there is no beat");
    PRJ->undo.undo();

    // ================================================================= interchange and export
    printf("--- EDL, markers, presets and the render queue\n");
    call("add_marker", {{"frame", 0}, {"name", "Intro"}});
    call("add_marker", {{"frame", 60}, {"name", "Verse"}});
    call("add_marker", {{"frame", 120}, {"name", "Chorus"}});
    const QJsonObject edl = obj(call("export_edl", {{"path", out + "/cut.edl"}}));
    {
        QFile f(out + "/cut.edl");
        const QString t = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        expect(edl["events"].toInt() == 2 && t.startsWith("TITLE: Main") && t.contains("CLIP     V") && Interchange::parseEdl(t).size() == 2, "export_edl wrote a CMX3600 list of the sequence");
    }
    expect(ok(call("export_markers", {{"path", out + "/markers.csv"}})) && ok(call("export_markers", {{"path", out + "/chapters.txt"}, {"format", "youtube"}})), "export_markers wrote both formats");
    {
        QFile f(out + "/chapters.txt");
        expect(f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()) == "0:00 Intro\n0:02 Verse\n0:04 Chorus\n", "the YouTube chapter list is ready to paste");
    }
    expect(call("export_markers", {{"path", out + "/x.csv"}, {"format", "pdf"}})["isError"].toBool(), "export_markers refuses an unknown format");

    expect(ok(call("save_export_preset", {{"name", "Small Web"}, {"format", "H.264"}, {"width", 320}, {"height", 180}, {"crf", 30}, {"chapters", true}})), "save_export_preset saved a preset");
    expect(call("save_export_preset", {{"name", "Bad"}, {"format", "Betamax"}})["isError"].toBool(), "and refuses an unknown format");
    const QJsonArray presets = arr(call("list_export_presets"));
    expect(presets.size() == 1 && presets[0].toObject()["name"].toString() == "Small Web" && presets[0].toObject()["width"].toInt() == 320, "list_export_presets shows it");
    w.showPage(MainWindow::PageExport);
    pump(300);
    // The page was built before the preset existed; reopening it must pick the preset up.
    expect(ok(call("queue_export", {{"path", out + "/queue_a.mp4"}, {"preset", "Small Web"}, {"last_frame", 150}})), "queued an export from the preset");
    expect(ok(call("queue_export", {{"path", out + "/queue_b.flac"}, {"format", "FLAC (Audio Only)"}, {"last_frame", 90}})), "queued an audio export");
    expect(call("queue_export", {{"path", out + "/queue_a.mp4"}, {"preset", "Small Web"}})["isError"].toBool(), "the same output cannot be queued twice");
    expect(call("queue_export", {{"path", out + "/queue_c.mp4"}, {"preset", "No Such"}})["isError"].toBool(), "an unknown preset is refused");
    // Editing after queueing must not change what the queue renders.
    PRJ->edit("After Queue", [&](ProjectState &st) { st.seqById(st.activeSeq)->markers.clear(); });
    pump(200);
    w.grab().save(out + "/export_queue.png");
    expect(ok(call("start_export_queue")), "started the queue");
    QElapsedTimer t;
    t.start();
    while ((EXPORTQ->running() || EXPORTQ->pending()) && t.elapsed() < 180000) pump(100);
    pump(200);
    const QJsonArray items = obj(call("export_queue_status"))["items"].toArray();
    bool allDone = items.size() == 2;
    for (auto v : items) allDone &= v.toObject()["state"].toString() == "done";
    expect(allDone && QFileInfo::exists(out + "/queue_a.mp4") && QFileInfo::exists(out + "/queue_b.flac"), "both queued exports finished, one after the other");
    {
        const QJsonArray ch = QJsonDocument::fromJson(ffprobe({"-show_chapters", "-of", "json", out + "/queue_a.mp4"}).toUtf8()).object()["chapters"].toArray();
        const QString size = ffprobe({"-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0", out + "/queue_a.mp4"}).trimmed();
        expect(size == "320,180", QString("the preset's frame size was used (%1)").arg(size));
        expect(ch.size() == 3 && ch[1].toObject()["tags"].toObject()["title"].toString() == "Verse", "the movie has the three chapters the project had when it was queued");
        expect(ffprobe({"-select_streams", "a:0", "-show_entries", "stream=codec_name", "-of", "csv=p=0", out + "/queue_b.flac"}).trimmed() == "flac", "the audio export is FLAC");
    }
    w.grab().save(out + "/export_queue_done.png");
    call("clear_export_queue");
    expect(EXPORTQ->items().isEmpty() && call("start_export_queue")["isError"].toBool(), "clear_export_queue emptied the queue");
    PRJ->undo.undo();

    // export_sequence with chapters and a preset, directly
    const int job = obj(call("export_sequence", {{"path", out + "/direct.mkv"}, {"format", "H.264 (Matroska)"}, {"preset", "Small Web"}, {"last_frame", 150}}))["job_id"].toInt();
    for (int i = 0; i < 1200; ++i) {
        if (obj(AITOOLS->call("job_status", {{"job_id", job}}))["state"].toString() != "running") break;
        pump(100);
    }
    expect(QFileInfo::exists(out + "/direct.mkv") && !QDir(out).entryList({"*.part.*"}, QDir::Files).size(), "export_sequence wrote the MKV and left no temporary file");
    expect(QJsonDocument::fromJson(ffprobe({"-show_chapters", "-of", "json", out + "/direct.mkv"}).toUtf8()).object()["chapters"].toArray().size() == 3, "with chapters, from the preset");

    // The Export page offers the saved preset and applies it.
    {
        auto *page = w.exportPageWidget();
        ExportPresets::save("Tall", ExportPresets::fromJson({{"format", "HEVC (H.265)"}, {"width", 720}, {"height", 1280}, {"crf", 27}, {"chapters", true}, {"lufs", -14.0}}));
        w.showPage(MainWindow::PageHome);
        w.showPage(MainWindow::PageExport);
        pump(300);
        expect(page->presetNames().contains("Small Web") && page->presetNames().contains("Tall") && page->presetNames().contains("YouTube 1080p Full HD"), "the Export page lists saved presets after the built-in ones");
        page->selectPreset("Tall");
        pump(100);
        const ExportSettings s = page->currentSettings();
        expect(s.format == "HEVC (H.265)" && s.width == 720 && s.height == 1280 && s.crf == 27 && s.chapters && s.loudnessLufs == -14, "choosing a saved preset fills in every setting");
        page->selectPreset("YouTube 1080p Full HD");
        pump(100);
        expect(page->currentSettings().width == 1920 && page->currentSettings().kbps == 16000, "and the built-in presets still work");
        w.grab().save(out + "/export_page.png");
    }
    QSettings().remove("export/presets");

    // Preferences
    pressWhenShown("Cancel", out + "/dialog_preferences.png");
    if (auto *ac = action("Preferences...")) ac->trigger();
    expect(QFileInfo::exists(out + "/dialog_preferences.png"), "the Preferences dialog opened");
    Vault::Options o = Vault::options();
    expect(o.autosave && o.minutes == 3 && o.maxVersions == 20 && o.backup && o.recovery, "auto-save defaults: on, every 3 minutes, 20 versions, backup and recovery on");

    w.showPage(MainWindow::PageEdit);
    w.setWorkspace("Editing");
    pump(400);
    w.grab().save(out + "/editing.png");
    PRJ->undo.setClean();
    printf(fails ? "\n%d V3 FAILURES\n" : "\nALL V3 TESTS PASSED\n", fails);
    fflush(stdout);
    return fails ? 1 : 0;
}
