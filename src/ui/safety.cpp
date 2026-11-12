#include "safety.h"
#include "../core/mediacache.h"
#include "../core/project.h"
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSessionManager>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

SafetyNet *SafetyNet::instance() { static SafetyNet s; return &s; }

SafetyNet::SafetyNet(QObject *p) : QObject(p)
{
    recoveryTimer.setSingleShot(true);
    connect(&recoveryTimer, &QTimer::timeout, this, &SafetyNet::recoveryTick);
    connect(&autoTimer, &QTimer::timeout, this, [this] { autosaveNow(false); });
    connect(&writer, &QFutureWatcher<bool>::finished, this, [this] {
        // Saved while the write was in flight: the file it left is already stale.
        if (!PRJ->isDirty()) session.clearRecovery();
        else if (again) scheduleRecovery();
        again = false;
    });
}

void SafetyNet::start()
{
    if (started) return;
    started = true;
    if (!session.begin()) emit problem(tr("Crash recovery is off: the session folder %1 cannot be written.").arg(QDir::toNativeSeparators(Vault::Session::dir())));
    Crash::install(Vault::root() + "/Crashes", QApplication::applicationVersion(), session.id());
    connect(PRJ, &Project::changed, this, &SafetyNet::scheduleRecovery);
    connect(PRJ, &Project::dirtyChanged, this, [this] {
        if (!PRJ->isDirty()) { recoveryTimer.stop(); if (!writer.isRunning()) session.clearRecovery(); }
        else scheduleRecovery();
    });
    // Windows is logging off or shutting down: whatever is unsaved goes to the recovery file now.
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this](QSessionManager &) { writeRecoveryNow(); }, Qt::DirectConnection);
    applyOptions();
}

void SafetyNet::applyOptions()
{
    const Vault::Options o = Vault::options();
    autoTimer.stop();
    if (o.autosave) autoTimer.start(o.minutes * 60 * 1000);
    if (!o.recovery) { recoveryTimer.stop(); session.clearRecovery(); }
}

void SafetyNet::projectSwitched()
{
    session.setProject(PRJ->filePath, PRJ->displayName());
    autosavedRev = PRJ->revision;
    if (!PRJ->isDirty()) session.clearRecovery();
}

void SafetyNet::scheduleRecovery()
{
    if (!started || !Vault::options().recovery || !PRJ->loaded || !PRJ->isDirty()) return;
    if (!recoveryTimer.isActive()) recoveryTimer.start(recoveryDelayMs);
}

void SafetyNet::recoveryTick()
{
    if (!PRJ->loaded || !PRJ->isDirty() || recoveredRev == PRJ->revision) return;
    // A drag in progress has not been committed yet; wait for it rather than save half an edit.
    if (PRJ->inLive()) { recoveryTimer.start(500); return; }
    if (writer.isRunning()) { again = true; return; }
    recoveredRev = PRJ->revision;
    session.setProject(PRJ->filePath, PRJ->displayName());
    // The state is a cheap implicitly shared copy; encoding and writing happen off the UI thread.
    const ProjectState snap = PRJ->s;
    Vault::Session *s = &session;
    writer.setFuture(QtConcurrent::run([snap, s] { return s->writeRecoveryData(Vault::encode(snap)); }));
}

bool SafetyNet::writeRecoveryNow()
{
    if (!started || !PRJ->loaded || !PRJ->isDirty()) return false;
    writer.waitForFinished();
    recoveredRev = PRJ->revision;
    session.setProject(PRJ->filePath, PRJ->displayName());
    return session.writeRecovery(PRJ->s, &lastError);
}

bool SafetyNet::autosaveNow(bool force, QString *path)
{
    if (!PRJ->loaded) return false;
    if (!force && (!PRJ->isDirty() || PRJ->revision == autosavedRev || PRJ->inLive())) return false;
    QString err;
    const QString p = Vault::saveVersion(PRJ->s, PRJ->filePath, PRJ->displayName(), Vault::options().maxVersions, &err);
    if (p.isEmpty()) {
        lastError = err;
        emit problem(tr("Auto-save failed: %1").arg(err));
        return false;
    }
    autosavedRev = PRJ->revision;
    lastAutosave = p;
    lastAutosaveTime = QDateTime::currentDateTime();
    if (path) *path = p;
    emit autosaved(p);
    return true;
}

void SafetyNet::shutdown()
{
    if (!started) return;
    autoTimer.stop();
    recoveryTimer.stop();
    writer.waitForFinished();
    session.end();
    started = false;
}

bool SafetyNet::recover(const Vault::Orphan &o, QString *err)
{
    ProjectState st;
    const Vault::LoadResult r = Vault::read(o.recoverFile, &st);
    if (r != Vault::LoadResult::Ok) { if (err) *err = Vault::describe(r); return false; }
    PRJ->adopt(st, o.projectPath, true);
    projectSwitched();
    // Our own recovery file must exist before the old one is thrown away.
    writeRecoveryNow();
    Vault::Session::discard(o);
    for (auto &m : PRJ->s.media) if (m.hasAudio && !m.path.isEmpty() && !m.offline) CACHE->ensureAudio(m.path);
    return true;
}

bool SafetyNet::restoreVersion(const Vault::Version &v, QString *err)
{
    ProjectState st;
    const Vault::LoadResult r = Vault::read(v.path, &st);
    if (r != Vault::LoadResult::Ok) { if (err) *err = Vault::describe(r); return false; }
    const QString keep = PRJ->filePath;
    PRJ->adopt(st, keep, true);
    projectSwitched();
    for (auto &m : PRJ->s.media) if (m.hasAudio && !m.path.isEmpty() && !m.offline) CACHE->ensureAudio(m.path);
    return true;
}

static QString when(const QDateTime &t)
{
    const qint64 secs = t.secsTo(QDateTime::currentDateTime());
    const QString clock = QLocale().toString(t, QLocale::ShortFormat);
    if (secs < 90) return SafetyNet::tr("%1 (just now)").arg(clock);
    if (secs < 3600) return SafetyNet::tr("%1 (%2 minutes ago)").arg(clock).arg(secs / 60);
    if (secs < 86400) return SafetyNet::tr("%1 (%2 hours ago)").arg(clock).arg(secs / 3600);
    return SafetyNet::tr("%1 (%2 days ago)").arg(clock).arg(secs / 86400);
}

bool SafetyNet::offerRecovery(QWidget *parent)
{
    const QVector<Vault::Orphan> found = Vault::Session::orphans();
    if (found.isEmpty()) return false;
    QDialog d(parent);
    d.setWindowTitle(tr("Recover Unsaved Work"));
    d.resize(680, 360);
    auto *v = new QVBoxLayout(&d);
    auto *head = new QLabel(tr("<b style='font-size:15px'>Open Editor did not close properly last time.</b><br>"
                               "Your unsaved changes were kept. Choose a project to continue where you left off."), &d);
    head->setWordWrap(true);
    v->addWidget(head);
    auto *tb = new QTableWidget(found.size(), 3, &d);
    tb->setHorizontalHeaderLabels({tr("Project"), tr("Last change"), tr("Saved file")});
    tb->verticalHeader()->hide();
    tb->setSelectionBehavior(QAbstractItemView::SelectRows);
    tb->setSelectionMode(QAbstractItemView::SingleSelection);
    tb->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tb->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    tb->setColumnWidth(0, 170);
    tb->setColumnWidth(1, 220);
    for (int i = 0; i < found.size(); ++i) {
        const auto &o = found[i];
        tb->setItem(i, 0, new QTableWidgetItem(o.name.isEmpty() ? tr("Untitled") : o.name));
        tb->setItem(i, 1, new QTableWidgetItem(when(o.time)));
        tb->setItem(i, 2, new QTableWidgetItem(o.projectPath.isEmpty() ? tr("Never saved") : QDir::toNativeSeparators(o.projectPath)));
        if (!o.crashReport.isEmpty()) tb->item(i, 0)->setToolTip(tr("A crash report was saved:\n%1").arg(QDir::toNativeSeparators(o.crashReport)));
    }
    tb->selectRow(0);
    v->addWidget(tb, 1);
    bool anyCrash = false;
    for (auto &o : found) anyCrash |= !o.crashReport.isEmpty();
    auto *note = new QLabel(anyCrash ? tr("A crash report was saved in %1.").arg(QDir::toNativeSeparators(Vault::root() + "/Crashes"))
                                     : tr("Recovering does not change the saved file until you press Save."), &d);
    note->setStyleSheet("color:#8a8a8a;");
    note->setWordWrap(true);
    v->addWidget(note);
    auto *bb = new QDialogButtonBox(&d);
    auto *rec = bb->addButton(tr("Recover"), QDialogButtonBox::AcceptRole);
    auto *del = bb->addButton(tr("Discard"), QDialogButtonBox::DestructiveRole);
    auto *later = bb->addButton(tr("Decide Later"), QDialogButtonBox::RejectRole);
    rec->setProperty("primary", true);
    rec->setDefault(true);
    v->addWidget(bb);
    bool recovered = false;
    connect(later, &QPushButton::clicked, &d, &QDialog::reject);
    connect(rec, &QPushButton::clicked, &d, [&] {
        const int row = tb->currentRow();
        if (row < 0) return;
        QString err;
        if (!recover(found[row], &err)) { QMessageBox::warning(&d, tr("Recover"), tr("That recovery file could not be opened: %1").arg(err)); return; }
        recovered = true;
        d.accept();
    });
    connect(del, &QPushButton::clicked, &d, [&] {
        const int row = tb->currentRow();
        if (row < 0 || tb->isRowHidden(row)) return;
        if (QMessageBox::question(&d, tr("Discard"), tr("Throw away the unsaved changes to \"%1\"? This cannot be undone.").arg(tb->item(row, 0)->text())) != QMessageBox::Yes) return;
        Vault::Session::discard(found[row]);
        tb->setRowHidden(row, true);
        int left = 0, next = -1;
        for (int i = 0; i < tb->rowCount(); ++i) if (!tb->isRowHidden(i)) { ++left; if (next < 0) next = i; }
        if (!left) d.reject();
        else tb->selectRow(next);
    });
    d.exec();
    return recovered;
}

bool SafetyNet::restoreVersionDialog(QWidget *parent)
{
    const QVector<Vault::Version> list = Vault::versions(PRJ->filePath, PRJ->displayName());
    if (list.isEmpty()) {
        QMessageBox::information(parent, tr("Restore Version"), tr("There are no earlier versions of this project yet.\n\nAuto-save keeps a version every %1 minute(s) while you work, "
                                                                     "and Save keeps the previous file as a backup.").arg(Vault::options().minutes));
        return false;
    }
    QDialog d(parent);
    d.setWindowTitle(tr("Restore Version - %1").arg(PRJ->displayName()));
    d.resize(640, 420);
    auto *v = new QVBoxLayout(&d);
    v->addWidget(new QLabel(tr("Earlier versions of this project, newest first. Restoring opens the version as unsaved changes; your project file changes only when you save."), &d));
    static_cast<QLabel *>(v->itemAt(0)->widget())->setWordWrap(true);
    auto *tb = new QTableWidget(list.size(), 4, &d);
    tb->setHorizontalHeaderLabels({tr("When"), tr("Kind"), tr("Contents"), tr("Size")});
    tb->verticalHeader()->hide();
    tb->setSelectionBehavior(QAbstractItemView::SelectRows);
    tb->setSelectionMode(QAbstractItemView::SingleSelection);
    tb->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tb->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    tb->setColumnWidth(0, 230);
    tb->setColumnWidth(1, 110);
    for (int i = 0; i < list.size(); ++i) {
        const auto &ver = list[i];
        ProjectState st;
        QString what = tr("Damaged");
        if (Vault::read(ver.path, &st) == Vault::LoadResult::Ok) {
            int clips = 0;
            for (auto &q : st.seqs) { for (auto &t : q.video) clips += t.clips.size(); for (auto &t : q.audio) clips += t.clips.size(); }
            what = tr("%1 sequence(s), %2 clip(s), %3 media").arg(st.seqs.size()).arg(clips).arg(st.media.size());
        }
        tb->setItem(i, 0, new QTableWidgetItem(when(ver.time)));
        tb->setItem(i, 1, new QTableWidgetItem(ver.kind == "backup" ? tr("Previous save") : tr("Auto-save")));
        tb->setItem(i, 2, new QTableWidgetItem(what));
        tb->setItem(i, 3, new QTableWidgetItem(tr("%1 KB").arg(qMax<qint64>(1, ver.size / 1024))));
    }
    tb->selectRow(0);
    v->addWidget(tb, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Cancel, &d);
    auto *open = bb->addButton(tr("Restore"), QDialogButtonBox::AcceptRole);
    auto *copy = bb->addButton(tr("Save a Copy..."), QDialogButtonBox::ActionRole);
    auto *show = bb->addButton(tr("Show in Folder"), QDialogButtonBox::ActionRole);
    open->setProperty("primary", true);
    v->addWidget(bb);
    bool restored = false;
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(show, &QPushButton::clicked, &d, [&] { if (tb->currentRow() >= 0) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(list[tb->currentRow()].path).absolutePath())); });
    connect(copy, &QPushButton::clicked, &d, [&] {
        if (tb->currentRow() < 0) return;
        const QString to = QFileDialog::getSaveFileName(&d, tr("Save a Copy"), PRJ->displayName() + " (restored).oep", tr("Open Editor Project (*.oep)"));
        if (to.isEmpty()) return;
        QFile::remove(to);
        if (!QFile::copy(list[tb->currentRow()].path, to)) QMessageBox::warning(&d, tr("Save a Copy"), tr("Could not write %1.").arg(QDir::toNativeSeparators(to)));
    });
    connect(open, &QPushButton::clicked, &d, [&] {
        if (tb->currentRow() < 0) return;
        if (PRJ->isDirty() && QMessageBox::question(&d, tr("Restore Version"), tr("The project has unsaved changes that restoring will replace. An auto-save of them is kept first. Continue?")) != QMessageBox::Yes) return;
        autosaveNow(true);
        QString err;
        if (!restoreVersion(list[tb->currentRow()], &err)) { QMessageBox::warning(&d, tr("Restore Version"), err); return; }
        restored = true;
        d.accept();
    });
    d.exec();
    return restored;
}

bool SafetyNet::offerFallback(QWidget *parent, const QString &projectPath, Vault::LoadResult why)
{
    const QString name = QFileInfo(projectPath).completeBaseName();
    Vault::Version good;
    const bool have = (why == Vault::LoadResult::Corrupt || why == Vault::LoadResult::Unreadable || why == Vault::LoadResult::Missing) && Vault::newestGood(projectPath, name, &good);
    if (!have) {
        QMessageBox::warning(parent, tr("Open Project"), tr("%1 could not be opened.\n\n%2").arg(QDir::toNativeSeparators(projectPath), Vault::describe(why)));
        return false;
    }
    QMessageBox box(QMessageBox::Warning, tr("Open Project"),
                    tr("%1 could not be opened.\n\n%2\n\nAn earlier version from %3 (%4) is available. Open it instead? "
                       "The damaged file is left untouched until you save.")
                        .arg(QDir::toNativeSeparators(projectPath), Vault::describe(why), when(good.time), good.kind == "backup" ? tr("previous save") : tr("auto-save")),
                    QMessageBox::NoButton, parent);
    auto *yes = box.addButton(tr("Open Earlier Version"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != yes) return false;
    ProjectState st;
    if (Vault::read(good.path, &st) != Vault::LoadResult::Ok) return false;
    PRJ->adopt(st, projectPath, true);
    projectSwitched();
    for (auto &m : PRJ->s.media) if (m.hasAudio && !m.path.isEmpty() && !m.offline) CACHE->ensureAudio(m.path);
    return true;
}
