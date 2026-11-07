// AI menu, MCP setup, proxies and multicam commands of the main window.
#include "aitools.h"
#include "assistant.h"
#include "captionspanel.h"
#include "mainwindow.h"
#include "mcpserver.h"
#include "../core/mediacache.h"
#include "../core/project.h"
#include "../engine/analysis.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QTimer>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>

static QString resultText(const QJsonObject &r)
{
    QString s;
    for (auto v : r["content"].toArray()) s += v.toObject()["text"].toString();
    return s;
}

void MainWindow::runTool(const QString &name, const QJsonObject &args)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QJsonObject r = AITOOLS->call(name, args);
    QApplication::restoreOverrideCursor();
    if (r["isError"].toBool()) QMessageBox::warning(this, tr("Open Editor"), resultText(r));
    else statusBar()->showMessage(resultText(r).left(160), 6000);
}

void MainWindow::setProxies(bool on)
{
    program->engine->setUseProxies(on);
    source->engine->setUseProxies(on);
    QSettings().setValue("useProxies", on);
    if (proxyAction) proxyAction->setChecked(on);
}

void MainWindow::createMulticam()
{
    auto ids = project->selectedMedia();
    QJsonArray a;
    for (int id : ids) if (auto *m = PRJ->s.mediaById(id)) if (m->kind == MediaKind::Video) a.append(id);
    if (a.size() < 2) { QMessageBox::information(this, tr("Multi-Camera"), tr("Select two or more video clips in the Project panel first.")); return; }
    QDialog d(this);
    d.setWindowTitle(tr("Create Multi-Camera Source Sequence"));
    auto *f = new QFormLayout(&d);
    auto *name = new QLineEdit(tr("Multicam"), &d);
    auto *sync = new QCheckBox(tr("Align angles by their audio"), &d);
    sync->setChecked(true);
    f->addRow(tr("Name:"), name);
    f->addRow(sync);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    runTool("create_multicam", {{"media_ids", a}, {"name", name->text()}, {"sync_audio", sync->isChecked()}});
    panels->showPanel("multicam");
}

void MainWindow::mcpDialog()
{
    auto *mcp = McpServer::instance();
    QDialog d(this);
    d.setWindowTitle(tr("MCP Server"));
    d.setMinimumWidth(560);
    auto *v = new QVBoxLayout(&d);
    auto *note = new QLabel(tr("The Model Context Protocol lets an AI client such as Claude Code or Claude Desktop edit this project with the same tools as the built-in assistant, "
                               "including reading media paths, importing files and exporting. It listens only on this computer and requires the token below. Leave it off when you are not using it."), &d);
    note->setWordWrap(true);
    v->addWidget(note);
    auto *on = new QCheckBox(tr("Enable MCP server"), &d);
    on->setChecked(mcp->running());
    auto *port = new QSpinBox(&d);
    port->setRange(1024, 65535);
    port->setValue(QSettings().value("mcp/port", 8765).toInt());
    auto *row = new QFormLayout;
    row->addRow(on);
    row->addRow(tr("Port:"), port);
    v->addLayout(row);
    auto *info = new QPlainTextEdit(&d);
    info->setReadOnly(true);
    info->setMinimumHeight(170);
    v->addWidget(info);
    auto refresh = [=] {
        if (!mcp->running()) { info->setPlainText(tr("Server is off.")); return; }
        info->setPlainText(tr("Claude Code:\n  claude mcp add --transport http open-editor %1 --header \"Authorization: Bearer %2\"\n\n"
                              "Clients that launch a local command (Claude Desktop):\n  \"open-editor\": { \"command\": \"%3\" }\n")
                               .arg(mcp->url(), mcp->token(), QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/oe-mcp.exe").replace("\\", "\\\\")));
    };
    refresh();
    connect(on, &QCheckBox::toggled, &d, [=, this](bool en) {
        if (en) {
            if (!mcp->start(quint16(port->value()))) { on->setChecked(false); QMessageBox::warning(this, tr("MCP Server"), tr("Could not listen on that port.")); return; }
            QSettings().setValue("mcp/port", port->value());
        } else mcp->stop();
        QSettings().setValue("mcp/enabled", en);
        refresh();
    });
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    auto *copy = bb->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    auto *regen = bb->addButton(tr("New Token"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &d, [=] { QApplication::clipboard()->setText(info->toPlainText()); });
    connect(regen, &QPushButton::clicked, &d, [=, this] {
        mcp->regenerateToken();
        if (mcp->running()) mcp->start(quint16(port->value()));
        refresh();
    });
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    d.exec();
}

void MainWindow::silenceDialog()
{
    if (!PRJ->active()) return;
    QDialog d(this);
    d.setWindowTitle(tr("Remove Silence"));
    auto *f = new QFormLayout(&d);
    auto *th = new QDoubleSpinBox(&d);
    th->setRange(-80, -10); th->setValue(-40); th->setSuffix(" dB");
    auto *mn = new QDoubleSpinBox(&d);
    mn->setRange(.1, 30); mn->setSingleStep(.1); mn->setValue(.6); mn->setSuffix(" s");
    f->addRow(tr("Quieter than:"), th);
    f->addRow(tr("For at least:"), mn);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() == QDialog::Accepted) runTool("remove_silence", {{"threshold_db", th->value()}, {"min_seconds", mn->value()}});
}

QList<int> MainWindow::selectedClipIds() const
{
    QList<int> out;
    if (auto q = PRJ->active())
        for (auto *t : const_cast<Sequence *>(q)->allTracks())
            for (auto &c : t->clips) if (PRJ->selClips.contains(c.id)) out << c.id;
    return out;
}

// Shows a background job's progress in the status bar until it ends.
void MainWindow::watchJob(const QJsonObject &r, const QString &label)
{
    if (r["isError"].toBool()) { QMessageBox::warning(this, label, resultText(r)); return; }
    const int job = QJsonDocument::fromJson(resultText(r).toUtf8()).object()["job_id"].toInt();
    auto *t = new QTimer(this);
    t->setInterval(400);
    connect(t, &QTimer::timeout, this, [this, t, job, label] {
        const QJsonObject s = QJsonDocument::fromJson(resultText(AITOOLS->call("job_status", {{"job_id", job}})).toUtf8()).object();
        const QString st = s["state"].toString();
        if (st == "running") { statusBar()->showMessage(tr("%1: %2%").arg(label).arg(int(s["progress"].toDouble() * 100))); return; }
        t->deleteLater();
        if (st == "done") statusBar()->showMessage(tr("%1: %2").arg(label, s["message"].toString()), 6000);
        else QMessageBox::warning(this, label, s["message"].toString());
    });
    t->start();
    statusBar()->showMessage(tr("%1: starting").arg(label));
}

void MainWindow::duckDialog()
{
    auto q = PRJ->active();
    if (!q || q->audio.size() < 2) { QMessageBox::information(this, tr("Auto Ducking"), tr("Put dialogue and music on separate audio tracks first.")); return; }
    QDialog d(this);
    d.setWindowTitle(tr("Auto Ducking"));
    auto *f = new QFormLayout(&d);
    f->addRow(new QLabel(tr("Lower these tracks (music) while the others have dialogue:"), &d));
    QList<QPair<QCheckBox *, int>> boxes;
    for (int i = 0; i < q->audio.size(); ++i) {
        auto *cb = new QCheckBox(q->audio[i].name.isEmpty() ? tr("A%1").arg(i + 1) : q->audio[i].name, &d);
        cb->setChecked(i == q->audio.size() - 1);
        boxes << qMakePair(cb, q->audio[i].id);
        f->addRow(cb);
    }
    auto *amt = new QDoubleSpinBox(&d);
    amt->setRange(-40, -1); amt->setValue(-15); amt->setSuffix(" dB");
    auto *th = new QDoubleSpinBox(&d);
    th->setRange(-70, -10); th->setValue(-40); th->setSuffix(" dB");
    auto *fade = new QDoubleSpinBox(&d);
    fade->setRange(.05, 3); fade->setSingleStep(.05); fade->setValue(.5); fade->setSuffix(" s");
    f->addRow(tr("Duck amount:"), amt);
    f->addRow(tr("Dialogue louder than:"), th);
    f->addRow(tr("Fade:"), fade);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    QJsonArray ids;
    for (auto &b : boxes) if (b.first->isChecked()) ids.append(b.second);
    runTool("auto_duck", {{"track_ids", ids}, {"duck_db", amt->value()}, {"threshold_db", th->value()}, {"fade_seconds", fade->value()}});
}

void MainWindow::loudnessDialog()
{
    if (!PRJ->active()) return;
    QDialog d(this);
    d.setWindowTitle(tr("Match Loudness"));
    auto *f = new QFormLayout(&d);
    auto *target = new QComboBox(&d);
    target->addItem(tr("-14 LUFS (YouTube, Spotify)"), -14.0);
    target->addItem(tr("-16 LUFS (Apple, podcasts)"), -16.0);
    target->addItem(tr("-23 LUFS (EBU R128 broadcast)"), -23.0);
    target->addItem(tr("-24 LUFS (ATSC A/85 broadcast)"), -24.0);
    f->addRow(tr("Target:"), target);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() == QDialog::Accepted) runTool("match_loudness", {{"target_lufs", target->currentData().toDouble()}});
}

void MainWindow::buildAiMenu(QMenu *a)
{
    act(a, tr("AI Assistant"), "Ctrl+Shift+Space", [this] { panels->focusPanel("assistant"); });
    act(a, tr("Transcribe Sequence"), "Ctrl+Alt+T", [this] { panels->focusPanel("captions"); captions->transcribe(); });
    a->addSeparator();
    act(a, tr("Remove Silence..."), "", [this] { silenceDialog(); });
    act(a, tr("Scene Edit Detection on Selected Clip"), "", [this] {
        auto q = PRJ->active();
        if (!q) return;
        int id = 0;
        for (auto &t : q->video) for (auto &c : t.clips) if (PRJ->selClips.contains(c.id) && !id) id = c.id;
        if (!id) { QMessageBox::information(this, tr("Scene Edit Detection"), tr("Select a video clip first.")); return; }
        runTool("cut_at_scenes", {{"clip_id", id}});
    });
    act(a, tr("Normalize Audio to -18 dB"), "", [this] { runTool("normalize_audio", {}); });
    act(a, tr("Match Loudness..."), "", [this] { loudnessDialog(); });
    a->addSeparator();
    act(a, tr("Enhance Speech on Selected Clips"), "", [this] {
        QJsonArray ids;
        for (int id : selectedClipIds()) ids.append(id);
        if (ids.isEmpty()) { QMessageBox::information(this, tr("Enhance Speech"), tr("Select the dialogue clips first.")); return; }
        runTool("enhance_speech", {{"clip_ids", ids}});
    });
    act(a, tr("Auto Ducking..."), "", [this] { duckDialog(); });
    act(a, tr("Remove Filler Words"), "", [this] { runTool("remove_filler_words", {}); });
    act(a, tr("Warp Stabilizer on Selected Clip"), "", [this] {
        auto q = PRJ->active();
        int id = 0;
        if (q) for (auto &t : q->video) for (auto &c : t.clips) if (!id && PRJ->selClips.contains(c.id)) id = c.id;
        if (!id) { QMessageBox::information(this, tr("Warp Stabilizer"), tr("Select a video clip first.")); return; }
        watchJob(AITOOLS->call("stabilize_clip", {{"clip_id", id}}), tr("Warp Stabilizer"));
    });
    a->addSeparator();
    act(a, tr("Create Multi-Camera Source Sequence..."), "", [this] { createMulticam(); });
    for (int i = 1; i <= 9; ++i) act(a, tr("Cut to Camera %1").arg(i), QString("Ctrl+%1").arg(i), [this, i] { multicam->cut(i - 1); });
    a->addSeparator();
    act(a, tr("Create Proxies for All Video"), "", [this] { runTool("make_proxies", {}); });
    proxyAction = act(a, tr("Use Proxies for Playback"), "", [this] { setProxies(proxyAction->isChecked()); });
    proxyAction->setCheckable(true);
    proxyAction->setChecked(QSettings().value("useProxies", false).toBool());
    a->addSeparator();
    act(a, tr("MCP Server..."), "", [this] { mcpDialog(); });
    act(a, tr("Claude API Key..."), "", [this] { assistant->keyDialog(); });
}
