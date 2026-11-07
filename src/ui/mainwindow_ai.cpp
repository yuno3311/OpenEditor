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
