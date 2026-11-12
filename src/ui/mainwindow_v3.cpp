// Main window commands added in 3.0: versions and recovery, media management, interchange
// files, the new clip and sequence commands, and the tabbed Preferences dialog.
#include "mainwindow.h"
#include "safety.h"
#include "../core/mediacache.h"
#include "../core/mediamanage.h"
#include "../core/ops.h"
#include "../core/project.h"
#include "../engine/gpufx.h"
#include "../engine/interchange.h"
#include "../engine/videoreader.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

static QJsonArray toArray(const QList<int> &ids)
{
    QJsonArray a;
    for (int id : ids) a.append(id);
    return a;
}

// Everything that has to follow a project being opened, recovered or restored.
void MainWindow::afterProjectOpened()
{
    SAFETY->projectSwitched();
    for (auto &m : PRJ->s.media) if (m.hasAudio && !m.path.isEmpty() && !m.offline) CACHE->ensureAudio(m.path);
    showPage(PageEdit);
    updateTitle();
}

bool MainWindow::startupRecovery()
{
    if (SAFETY->offerRecovery(this)) {
        afterProjectOpened();
        statusBar()->showMessage(tr("Recovered unsaved changes. Save the project to keep them."), 10000);
        return true;
    }
    if (Vault::options().reopenLast) {
        const QStringList recent = QSettings().value("recent").toStringList();
        if (!recent.isEmpty() && QFileInfo::exists(recent.first())) { openProject(recent.first()); return true; }
    }
    return false;
}

void MainWindow::openFile(const QString &path) { openProject(path); }

void MainWindow::rebuildRecentMenu()
{
    recentMenu->clear();
    const QStringList recent = QSettings().value("recent").toStringList();
    for (const QString &p : recent) {
        auto *a = recentMenu->addAction(QDir::toNativeSeparators(p), this, [this, p] { openProject(p); });
        a->setEnabled(QFileInfo::exists(p));
    }
    if (recent.isEmpty()) recentMenu->addAction(tr("No recent projects"))->setEnabled(false);
    else {
        recentMenu->addSeparator();
        recentMenu->addAction(tr("Clear List"), this, [] { QSettings().remove("recent"); });
    }
}

void MainWindow::saveCopy()
{
    if (!PRJ->loaded) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Save a Copy"), PRJ->displayName() + " copy.oep", tr("Open Editor Project (*.oep)"));
    if (path.isEmpty()) return;
    if (!PRJ->saveCopy(path)) { QMessageBox::warning(this, tr("Save a Copy"), tr("Could not write %1.\n%2").arg(QDir::toNativeSeparators(path), PRJ->lastError)); return; }
    statusBar()->showMessage(tr("Copy saved to %1. You are still working in %2.").arg(QDir::toNativeSeparators(path), PRJ->displayName()), 8000);
}

// Film.oep -> Film_v002.oep -> Film_v003.oep: the next free number becomes the project file.
QString MainWindow::nextIncrement(const QString &path)
{
    const QFileInfo fi(path);
    QString base = fi.completeBaseName();
    int n = 1;
    static const QRegularExpression re("^(.*)_v(\\d{2,})$");
    if (const auto m = re.match(base); m.hasMatch()) { base = m.captured(1); n = m.captured(2).toInt(); }
    QString next;
    do next = fi.dir().filePath(QString("%1_v%2.oep").arg(base).arg(++n, 3, 10, QChar('0')));
    while (QFileInfo::exists(next));
    return next;
}

void MainWindow::saveIncremental()
{
    if (!PRJ->loaded) return;
    if (PRJ->filePath.isEmpty()) { saveProject(true); return; }
    const QString next = nextIncrement(PRJ->filePath);
    if (!PRJ->save(next)) { QMessageBox::warning(this, tr("Save Incremental"), tr("Could not write %1.\n%2").arg(QDir::toNativeSeparators(next), PRJ->lastError)); return; }
    addRecent(next);
    SAFETY->projectSwitched();
    updateTitle();
    statusBar()->showMessage(tr("Saved as %1.").arg(QFileInfo(next).fileName()), 6000);
}

void MainWindow::revertToSaved()
{
    if (PRJ->filePath.isEmpty() || !PRJ->isDirty()) { statusBar()->showMessage(tr("There are no unsaved changes to revert."), 5000); return; }
    if (QMessageBox::question(this, tr("Revert"), tr("Go back to the last saved version of \"%1\"? Your unsaved changes are kept as an auto-save version.").arg(PRJ->displayName())) != QMessageBox::Yes) return;
    SAFETY->autosaveNow(true);
    const QString path = PRJ->filePath;
    if (!PRJ->load(path)) { SAFETY->offerFallback(this, path, PRJ->lastLoad); return; }
    afterProjectOpened();
}

void MainWindow::linkMediaDialog()
{
    ProjectState st = PRJ->s;
    const QVector<int> missing = MediaManage::offlineMedia(st);
    if (missing.isEmpty()) { QMessageBox::information(this, tr("Link Media"), tr("All media files are online.")); return; }
    QDialog d(this);
    d.setWindowTitle(tr("Link Media"));
    d.resize(760, 420);
    auto *v = new QVBoxLayout(&d);
    auto *head = new QLabel(&d);
    head->setWordWrap(true);
    v->addWidget(head);
    auto *tb = new QTableWidget(missing.size(), 3, &d);
    tb->setHorizontalHeaderLabels({tr("Clip"), tr("Status"), tr("File")});
    tb->verticalHeader()->hide();
    tb->setSelectionBehavior(QAbstractItemView::SelectRows);
    tb->setSelectionMode(QAbstractItemView::SingleSelection);
    tb->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tb->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    tb->setColumnWidth(0, 200);
    tb->setColumnWidth(1, 90);
    v->addWidget(tb, 1);
    auto refresh = [&] {
        int left = 0;
        for (int i = 0; i < missing.size(); ++i) {
            const MediaItem *m = st.mediaById(missing[i]);
            left += m->offline;
            tb->setItem(i, 0, new QTableWidgetItem(m->name));
            auto *s = new QTableWidgetItem(m->offline ? tr("Offline") : tr("Linked"));
            s->setForeground(QColor(m->offline ? "#e06c5a" : "#6cc070"));
            tb->setItem(i, 1, s);
            tb->setItem(i, 2, new QTableWidgetItem(QDir::toNativeSeparators(m->path)));
        }
        head->setText(left ? tr("<b>%n media file(s) cannot be found.</b> Locate one and the others in the same folder are found with it, or search a whole folder.", "", left)
                           : tr("<b>Every file is linked.</b> Press Done to apply."));
    };
    refresh();
    tb->selectRow(0);
    auto *bb = new QDialogButtonBox(&d);
    auto *locate = bb->addButton(tr("Locate..."), QDialogButtonBox::ActionRole);
    auto *search = bb->addButton(tr("Search Folder..."), QDialogButtonBox::ActionRole);
    auto *done = bb->addButton(tr("Done"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    done->setProperty("primary", true);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(done, &QPushButton::clicked, &d, &QDialog::accept);
    connect(locate, &QPushButton::clicked, &d, [&] {
        const int row = tb->currentRow();
        if (row < 0) return;
        const MediaItem *m = st.mediaById(missing[row]);
        const QString name = QFileInfo(m->path).fileName();
        const QString file = QFileDialog::getOpenFileName(&d, tr("Locate %1").arg(name), QSettings().value("linkDir").toString(), tr("%1;;All Files (*)").arg(name));
        if (file.isEmpty()) return;
        QSettings().setValue("linkDir", QFileInfo(file).absolutePath());
        QString err;
        if (!MediaManage::relink(st, m->id, file, &err)) { QMessageBox::warning(&d, tr("Link Media"), err); return; }
        MediaManage::relinkFolder(st, QFileInfo(file).absolutePath());
        refresh();
    });
    connect(search, &QPushButton::clicked, &d, [&] {
        const QString dir = QFileDialog::getExistingDirectory(&d, tr("Search Folder"), QSettings().value("linkDir").toString());
        if (dir.isEmpty()) return;
        QSettings().setValue("linkDir", dir);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const int n = MediaManage::relinkFolder(st, dir);
        QApplication::restoreOverrideCursor();
        refresh();
        if (!n) QMessageBox::information(&d, tr("Link Media"), tr("None of the missing files are in that folder."));
    });
    if (d.exec() != QDialog::Accepted) return;
    int linked = 0;
    for (int id : missing) linked += !st.mediaById(id)->offline;
    if (!linked) return;
    PRJ->edit(tr("Link Media"), [&](ProjectState &s) { s.media = st.media; });
    for (int id : missing) if (const MediaItem *m = PRJ->s.mediaById(id); m && !m->offline && m->hasAudio) CACHE->ensureAudio(m->path);
    statusBar()->showMessage(tr("Linked %n file(s).", "", linked), 6000);
}

void MainWindow::projectManagerDialog()
{
    if (!PRJ->loaded) return;
    QDialog d(this);
    d.setWindowTitle(tr("Project Manager"));
    d.setMinimumWidth(560);
    auto *f = new QFormLayout(&d);
    auto *intro = new QLabel(tr("Copies the project and its media into one folder, ready to archive or move to another computer. Your current project is not changed."), &d);
    intro->setWordWrap(true);
    f->addRow(intro);
    auto *name = new QLineEdit(PRJ->displayName(), &d);
    auto *folder = new QLineEdit(QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + PRJ->displayName() + " Collected"), &d);
    auto *pick = new QPushButton(tr("Browse..."), &d);
    auto *row = new QHBoxLayout;
    row->addWidget(folder, 1);
    row->addWidget(pick);
    auto *unused = new QCheckBox(tr("Include media that is not used on any timeline"), &d);
    auto *size = new QLabel(&d);
    f->addRow(tr("Project name:"), name);
    f->addRow(tr("Destination:"), row);
    f->addRow(unused);
    f->addRow(tr("To copy:"), size);
    auto update = [&] {
        const qint64 need = MediaManage::collectSize(PRJ->s, unused->isChecked()), free_ = Vault::freeBytes(QFileInfo(QDir::fromNativeSeparators(folder->text())).absolutePath());
        size->setText(tr("%1 MB (%2 MB free at the destination)").arg(need >> 20).arg(free_ < 0 ? 0 : free_ >> 20));
    };
    update();
    connect(unused, &QCheckBox::toggled, &d, update);
    connect(folder, &QLineEdit::editingFinished, &d, update);
    connect(pick, &QPushButton::clicked, &d, [&] {
        const QString dir = QFileDialog::getExistingDirectory(&d, tr("Destination"), QFileInfo(QDir::fromNativeSeparators(folder->text())).absolutePath());
        if (!dir.isEmpty()) { folder->setText(QDir::toNativeSeparators(dir + "/" + name->text() + " Collected")); update(); }
    });
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Cancel, &d);
    bb->addButton(tr("Collect"), QDialogButtonBox::AcceptRole)->setProperty("primary", true);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    const QString dest = QDir::fromNativeSeparators(folder->text().trimmed());
    if (dest.isEmpty()) return;
    QProgressDialog prog(tr("Collecting files..."), tr("Cancel"), 0, 100, this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(0);
    MediaManage::CollectOptions o;
    o.includeUnused = unused->isChecked();
    o.projectName = name->text().trimmed().isEmpty() ? PRJ->displayName() : name->text().trimmed();
    o.progress = [&](int done, int total, const QString &file) {
        prog.setMaximum(qMax(1, total));
        prog.setValue(done);
        if (!file.isEmpty()) prog.setLabelText(tr("Copying %1").arg(file));
        QApplication::processEvents();
        return !prog.wasCanceled();
    };
    const auto r = MediaManage::collect(PRJ->s, dest, o);
    prog.close();
    if (!r.ok) { QMessageBox::warning(this, tr("Project Manager"), r.error); return; }
    QMessageBox box(QMessageBox::Information, tr("Project Manager"),
                    tr("Collected %1 file(s), %2 MB, into\n%3").arg(r.copied).arg(r.bytes >> 20).arg(QDir::toNativeSeparators(dest))
                        + (r.skippedOffline ? tr("\n\n%n offline file(s) could not be copied.", "", r.skippedOffline) : QString()),
                    QMessageBox::Ok, this);
    auto *open = box.addButton(tr("Open Folder"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(dest));
}

void MainWindow::projectReportDialog()
{
    QDialog d(this);
    d.setWindowTitle(tr("Project Report - %1").arg(PRJ->displayName()));
    d.resize(620, 520);
    auto *v = new QVBoxLayout(&d);
    auto *text = new QPlainTextEdit(MediaManage::reportText(PRJ->s), &d);
    text->setReadOnly(true);
    text->setFont(QFont("Consolas", 10));
    v->addWidget(text);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    auto *copy = bb->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    auto *save = bb->addButton(tr("Save..."), QDialogButtonBox::ActionRole);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(copy, &QPushButton::clicked, &d, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    connect(save, &QPushButton::clicked, &d, [&] {
        const QString p = QFileDialog::getSaveFileName(&d, tr("Save Report"), PRJ->displayName() + " report.txt", tr("Text (*.txt)"));
        QFile f(p);
        if (!p.isEmpty() && f.open(QIODevice::WriteOnly)) f.write(text->toPlainText().toUtf8());
    });
    d.exec();
}

void MainWindow::exportInterchange(const QString &kind)
{
    auto q = PRJ->active();
    if (!q) return;
    if (kind != "edl" && q->markers.isEmpty()) { QMessageBox::information(this, tr("Export Markers"), tr("This sequence has no markers. Press M to add one at the playhead.")); return; }
    const QString ext = kind == "edl" ? "edl" : kind == "csv" ? "csv" : "txt";
    const QString filter = kind == "edl" ? tr("Edit Decision List (*.edl)") : kind == "csv" ? tr("Comma-Separated Values (*.csv)") : tr("Text (*.txt)");
    const QString path = QFileDialog::getSaveFileName(this, tr("Export"), q->name + (kind == "youtube" ? " chapters." : ".") + ext, filter);
    if (path.isEmpty()) return;
    if (kind == "edl") runTool("export_edl", {{"path", path}});
    else runTool("export_markers", {{"path", path}, {"format", kind}});
    if (kind == "youtube") {
        QApplication::clipboard()->setText(Interchange::youtubeChapters(*q));
        statusBar()->showMessage(tr("Chapters saved and copied to the clipboard, ready to paste into the video description."), 8000);
    }
}

void MainWindow::autoReframeDialog()
{
    auto q = PRJ->active();
    if (!q) return;
    QDialog d(this);
    d.setWindowTitle(tr("Auto Reframe Sequence"));
    auto *f = new QFormLayout(&d);
    auto *intro = new QLabel(tr("Makes a copy of \"%1\" with a new frame size. Every picture is scaled to fill the new frame; the original sequence is not changed.").arg(q->name), &d);
    intro->setWordWrap(true);
    f->addRow(intro);
    auto *preset = new QComboBox(&d);
    struct R { const char *n; int w, h; };
    static const R sizes[] = {{"Vertical 9:16 (1080 x 1920)", 1080, 1920}, {"Square 1:1 (1080 x 1080)", 1080, 1080}, {"Portrait 4:5 (1080 x 1350)", 1080, 1350},
                              {"Widescreen 16:9 (1920 x 1080)", 1920, 1080}, {"Ultra HD 16:9 (3840 x 2160)", 3840, 2160}, {"Cinema 2.39:1 (2048 x 858)", 2048, 858}, {"Custom", 0, 0}};
    for (auto &s : sizes) preset->addItem(tr(s.n));
    auto *w = new QSpinBox(&d), *h = new QSpinBox(&d);
    for (auto *s : {w, h}) { s->setRange(16, 8192); s->setSingleStep(2); }
    w->setValue(1080); h->setValue(1920);
    auto *name = new QLineEdit(&d);
    name->setPlaceholderText(tr("%1 1080x1920").arg(q->name));
    f->addRow(tr("Target:"), preset);
    auto *wh = new QHBoxLayout;
    wh->addWidget(w); wh->addWidget(new QLabel("x", &d)); wh->addWidget(h); wh->addStretch(1);
    f->addRow(tr("Frame size:"), wh);
    f->addRow(tr("Name:"), name);
    connect(preset, &QComboBox::currentIndexChanged, &d, [&](int i) { if (sizes[i].w) { w->setValue(sizes[i].w); h->setValue(sizes[i].h); } });
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    QJsonObject args{{"width", w->value() & ~1}, {"height", h->value() & ~1}};
    if (!name->text().trimmed().isEmpty()) args["name"] = name->text().trimmed();
    runTool("auto_reframe", args);
    timeline->view->zoomFit();
}

void MainWindow::fadeDialog()
{
    auto q = PRJ->active();
    const QList<int> ids = selectedClipIds();
    if (!q || ids.isEmpty()) { statusBar()->showMessage(tr("Select one or more clips first."), 5000); return; }
    QDialog d(this);
    d.setWindowTitle(tr("Fade In / Out"));
    auto *f = new QFormLayout(&d);
    auto *in = new QDoubleSpinBox(&d), *out = new QDoubleSpinBox(&d);
    for (auto *s : {in, out}) { s->setRange(0, 60); s->setDecimals(2); s->setSingleStep(.25); s->setSuffix(tr(" s")); }
    in->setValue(QSettings().value("fade/in", .5).toDouble());
    out->setValue(QSettings().value("fade/out", .5).toDouble());
    f->addRow(tr("Fade in:"), in);
    f->addRow(tr("Fade out:"), out);
    auto *note = new QLabel(tr("Video fades with opacity, audio with volume. Set both to 0 to remove the fades."), &d);
    note->setStyleSheet("color:#8a8a8a;");
    f->addRow(note);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    QSettings().setValue("fade/in", in->value());
    QSettings().setValue("fade/out", out->value());
    const Frame fi = Frame(in->value() * q->fps() + .5), fo = Frame(out->value() * q->fps() + .5);
    const QSet<int> all = PRJ->withLinked(QSet<int>(ids.begin(), ids.end()));
    PRJ->edit(tr("Fade"), [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        for (int id : all) if (Clip *c = s.clip(id)) Ops::fadeClip(*c, fi, fo);
    });
}

void MainWindow::beatDialog()
{
    if (!PRJ->active()) return;
    QDialog d(this);
    d.setWindowTitle(tr("Add Beat Markers"));
    auto *f = new QFormLayout(&d);
    auto *intro = new QLabel(tr("Finds the tempo of the sequence's audio and puts a marker on the beats, so cuts can snap to the music."), &d);
    intro->setWordWrap(true);
    f->addRow(intro);
    auto *every = new QComboBox(&d);
    every->addItem(tr("Every beat"), 1);
    every->addItem(tr("Every 2nd beat"), 2);
    every->addItem(tr("Every 4th beat (each bar in 4/4)"), 4);
    every->addItem(tr("Every 8th beat"), 8);
    every->setCurrentIndex(2);
    f->addRow(tr("Mark:"), every);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    runTool("detect_beats", {{"add_markers", true}, {"every", every->currentData().toInt()}});
}

void MainWindow::replaceWithSource()
{
    const QList<int> ids = selectedClipIds();
    if (ids.size() != 1 || !source->mediaId) { statusBar()->showMessage(tr("Select one timeline clip and open the replacement in the Source Monitor."), 6000); return; }
    runTool("replace_clip", {{"clip_id", ids.first()}, {"media_id", source->mediaId}});
}

// Opens the frame under the playhead in the Source Monitor, at the same moment of the source.
void MainWindow::matchFrame()
{
    auto q = PRJ->active();
    if (!q) return;
    const Clip *hit = nullptr;
    for (int id : selectedClipIds())
        if (const Clip *c = q->clip(id); c && q->playhead >= c->start && q->playhead < c->end() && c->kind != MediaKind::Audio) { hit = c; break; }
    for (int i = q->video.size() - 1; i >= 0 && !hit; --i) hit = q->video[i].clipAt(q->playhead);
    for (int i = 0; i < q->audio.size() && !hit; ++i) hit = q->audio[i].clipAt(q->playhead);
    const MediaItem *m = hit ? PRJ->s.mediaById(hit->mediaId) : nullptr;
    if (!m || m->path.isEmpty()) { statusBar()->showMessage(tr("There is no source clip under the playhead."), 5000); return; }
    const double sec = hit->sourceTime(q->playhead, q->fps());
    source->setMedia(m->id);
    panels->showPanel("source");
    source->setPos(Frame(sec * (m->fps > 0 ? m->fps : q->fps()) + .5));
}

void MainWindow::selectAtPlayhead()
{
    auto q = PRJ->active();
    if (!q) return;
    QSet<int> ids;
    for (auto *t : q->allTracks())
        if (!t->locked) if (const Clip *c = t->clipAt(q->playhead)) ids << c->id;
    PRJ->setSelection(ids);
}

void MainWindow::addV3ClipMenu(QMenu *c)
{
    c->addSeparator();
    act(c, tr("Duplicate"), "Ctrl+Shift+/", [this] { if (!selectedClipIds().isEmpty()) runTool("duplicate_clips", {{"clip_ids", toArray(selectedClipIds())}}); });
    act(c, tr("Replace With Clip From Source Monitor"), "", [this] { replaceWithSource(); });
    act(c, tr("Fade In / Out..."), "Ctrl+Shift+F", [this] { fadeDialog(); });
    QMenu *lay = c->addMenu(tr("Layout"));
    for (const QString &p : Ops::layoutPresets())
        lay->addAction(p, this, [this, p] {
            const QList<int> ids = selectedClipIds();
            auto q = PRJ->active();
            int n = 0;
            if (q) PRJ->edit(tr("Layout"), [&](ProjectState &st) {
                Sequence &s = *st.seqById(st.activeSeq);
                for (int id : ids) if (Clip *cl = s.clip(id); cl && cl->kind != MediaKind::Audio) n += Ops::applyLayout(s, *cl, p);
            });
            if (!n) statusBar()->showMessage(tr("Select a video clip first."), 5000);
        });
    QMenu *ki = c->addMenu(tr("Keyframe Interpolation"));
    struct K { const char *n; const char *id; };
    for (K k : {K{"Linear", "linear"}, K{"Ease", "ease"}, K{"Ease In", "ease_in"}, K{"Ease Out", "ease_out"}, K{"Hold", "hold"}})
        ki->addAction(tr(k.n), this, [this, id = QString(k.id)] { for (int c : selectedClipIds()) runTool("set_keyframe_interpolation", {{"clip_id", c}, {"type", id}}); });
    act(c, tr("Match Frame"), "F", [this] { matchFrame(); });
}

void MainWindow::prefsDialog()
{
    QDialog d(this);
    d.setWindowTitle(tr("Preferences"));
    d.setMinimumWidth(560);
    auto *v = new QVBoxLayout(&d);
    auto *tabs = new QTabWidget(&d);
    v->addWidget(tabs);

    // --- Auto Save
    auto *sp = new QWidget(&d);
    auto *sf = new QFormLayout(sp);
    Vault::Options o = Vault::options();
    auto *autoOn = new QCheckBox(tr("Automatically save versions of the project while I work"), sp);
    autoOn->setChecked(o.autosave);
    auto *mins = new QSpinBox(sp);
    mins->setRange(1, 120); mins->setValue(o.minutes); mins->setSuffix(tr(" min"));
    auto *maxV = new QSpinBox(sp);
    maxV->setRange(1, 500); maxV->setValue(o.maxVersions);
    auto *bak = new QCheckBox(tr("Keep the previous save as a backup (.oep.bak) beside the project"), sp);
    bak->setChecked(o.backup);
    auto *rec = new QCheckBox(tr("Keep unsaved changes recoverable after a crash or power cut"), sp);
    rec->setChecked(o.recovery);
    auto *reopen = new QCheckBox(tr("Open the last project when Open Editor starts"), sp);
    reopen->setChecked(o.reopenLast);
    sf->addRow(autoOn);
    sf->addRow(tr("Save a version every:"), mins);
    sf->addRow(tr("Versions kept per project:"), maxV);
    sf->addRow(bak);
    sf->addRow(rec);
    sf->addRow(reopen);
    auto *where = new QLabel(QDir::toNativeSeparators(Vault::root() + "/AutoSave"), sp);
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    where->setWordWrap(true);
    sf->addRow(tr("Versions folder:"), where);
    auto *openV = new QPushButton(tr("Open Versions Folder"), sp);
    auto *openC = new QPushButton(tr("Open Crash Reports"), sp);
    auto *br = new QHBoxLayout;
    br->addWidget(openV); br->addWidget(openC); br->addStretch(1);
    sf->addRow(br);
    connect(openV, &QPushButton::clicked, &d, [] { QDir().mkpath(Vault::root() + "/AutoSave"); QDesktopServices::openUrl(QUrl::fromLocalFile(Vault::root() + "/AutoSave")); });
    connect(openC, &QPushButton::clicked, &d, [] { QDir().mkpath(Vault::root() + "/Crashes"); QDesktopServices::openUrl(QUrl::fromLocalFile(Vault::root() + "/Crashes")); });
    connect(autoOn, &QCheckBox::toggled, mins, &QWidget::setEnabled);
    connect(autoOn, &QCheckBox::toggled, maxV, &QWidget::setEnabled);
    mins->setEnabled(o.autosave); maxV->setEnabled(o.autosave);
    tabs->addTab(sp, tr("Auto Save"));

    // --- Performance
    auto *pp = new QWidget(&d);
    auto *f = new QFormLayout(pp);
    auto *gpuOn = new QCheckBox(tr("Use the GPU for effects"), pp);
    gpuOn->setChecked(GpuFx::enabled);
    auto *hwOn = new QCheckBox(tr("Use hardware video decoding"), pp);
    hwOn->setChecked(VideoReader::hwDecode);
    f->addRow(gpuOn);
    f->addRow(hwOn);
    if (!GpuFx::rendererName().isEmpty()) f->addRow(tr("Renderer:"), new QLabel(GpuFx::rendererName(), pp));
    auto *mem = new QSpinBox(pp);
    mem->setRange(1, 64); mem->setSuffix(" GB");
    mem->setValue(QSettings().value("cacheGB", 3).toInt());
    f->addRow(tr("Frame cache memory:"), mem);
    tabs->addTab(pp, tr("Performance"));

    // --- Media
    auto *mp = new QWidget(&d);
    auto *mf = new QFormLayout(mp);
    auto *ff = new QLabel(CACHE->ffmpeg().isEmpty() ? tr("Not found") : QDir::toNativeSeparators(CACHE->ffmpeg()), mp);
    ff->setWordWrap(true);
    ff->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mf->addRow(tr("ffmpeg:"), ff);
    auto *cd = new QLabel(QDir::toNativeSeparators(CACHE->cacheDir()), mp);
    cd->setWordWrap(true);
    cd->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mf->addRow(tr("Media cache:"), cd);
    auto *cl = new QPushButton(tr("Delete Unused Media Cache Files"), mp);
    connect(cl, &QPushButton::clicked, &d, [cl] { CACHE->clearDiskCache(); cl->setEnabled(false); });
    mf->addRow(QString(), cl);
    tabs->addTab(mp, tr("Media"));

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    o.autosave = autoOn->isChecked(); o.minutes = mins->value(); o.maxVersions = maxV->value();
    o.backup = bak->isChecked(); o.recovery = rec->isChecked(); o.reopenLast = reopen->isChecked();
    Vault::setOptions(o);
    SAFETY->applyOptions();
    QSettings().setValue("cacheGB", mem->value());
    GpuFx::enabled = gpuOn->isChecked();
    VideoReader::hwDecode = hwOn->isChecked();
    QSettings().setValue("gpu", gpuOn->isChecked());
    QSettings().setValue("hwDecode", hwOn->isChecked());
    VideoReader::setCacheBudget(qint64(mem->value()) << 30);
}
