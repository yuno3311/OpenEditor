#include "mainwindow.h"
#include "../core/mediacache.h"
#include "../core/ops.h"
#include "../core/project.h"
#include "../engine/videoreader.h"
#include "aitools.h"
#include "mcpserver.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFile>
#include <QFileInfo>
#include <QCheckBox>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QMenuBar>
#include <QMessageBox>
#include "setupwizard.h"
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

using namespace Theme;

static const QStringList kWorkspaces{"Assembly", "Editing", "Color", "Effects", "Audio", "Graphics", "Text", "AI"};

MainWindow::MainWindow()
{
    setWindowIcon(appIcon());
    resize(1600, 960);
    wsLayouts = {
        {"Editing", "v(56 h(40 [*effectcontrols source audiomixer multicam metadata] 60 [program]) 44 h(26 [*project captions mediabrowser effects markers history info] 3 [tools] 67 [timeline] 4 [audiometers]))"},
        {"Assembly", "v(58 h(40 [*project mediabrowser info] 30 [source] 30 [program]) 42 h(3 [tools] 93 [timeline] 4 [audiometers]))"},
        {"Color", "h(73 v(58 h(55 [program] 45 [scopes]) 42 h(3 [tools] 93 [timeline] 4 [audiometers])) 27 [*color effectcontrols])"},
        {"Effects", "h(75 v(56 h(45 [*effectcontrols source] 55 [program]) 44 h(3 [tools] 93 [timeline] 4 [audiometers])) 25 [*effects project history])"},
        {"Audio", "v(56 h(50 [*audiomixer source] 50 [program]) 44 h(24 [*project effects] 3 [tools] 69 [timeline] 4 [audiometers]))"},
        {"Graphics", "h(75 v(60 h(62 [program] 38 [project]) 40 h(3 [tools] 93 [timeline] 4 [audiometers])) 25 [*graphics effectcontrols])"},
        {"Text", "h(30 [*captions project] 70 v(56 [program] 44 h(3 [tools] 93 [timeline] 4 [audiometers])))"},
        {"AI", "h(30 [*assistant captions] 70 v(56 h(35 [*project effects] 65 [program]) 44 h(3 [tools] 93 [timeline] 4 [audiometers])))"}};

    auto *central = new QWidget(this);
    auto *v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    setCentralWidget(central);
    buildHeader(v);

    stack = new QStackedWidget(central);
    v->addWidget(stack, 1);
    home = new HomePage(stack);
    importPage = new ImportPage(stack);
    editor = new QWidget(stack);
    auto *el = new QVBoxLayout(editor);
    el->setContentsMargins(3, 3, 3, 3);
    exportPage = new ExportPage(stack);
    stack->addWidget(home);
    stack->addWidget(importPage);
    stack->addWidget(editor);
    stack->addWidget(exportPage);

    panels = new PanelManager(editor);
    project = new ProjectPanel;
    timeline = new TimelinePanel;
    source = new MonitorPanel(true);
    program = new MonitorPanel(false);
    scopes = new ScopesPanel;
    captions = new CaptionsPanel;
    multicam = new MulticamPanel;
    assistant = new AssistantPanel;
    auto *ec = new EffectControlsPanel;
    auto *meters = new AudioMetersPanel;
    auto *mixer = new AudioMixerPanel;
    auto *browser = new MediaBrowserPanel;
    auto *meta = new MetadataPanel;
    auto *tools = new ToolsPanel;
    tools->setMaximumWidth(46);
    tools->setMinimumWidth(40);
    meters->setMaximumWidth(112);
    for (Panel *p : std::initializer_list<Panel *>{project, timeline, source, program, scopes, ec, meters, mixer, browser, meta, tools, new EffectsPanel, new InfoPanel, new HistoryPanel, new MarkersPanel, new ColorPanel, new GraphicsPanel, captions, multicam, assistant})
        panels->registerPanel(p);
    auto engineFn = [this] { return activeMonitor()->engine; };
    meters->source = engineFn;
    mixer->source = engineFn;

    connect(project, &ProjectPanel::openInSource, this, [=, this](int id) { source->setMedia(id); panels->showPanel("source"); meta->show(id); });
    connect(project, &ProjectPanel::openSequence, this, [](int id) { PRJ->setActiveSequence(id); });
    connect(project, &ProjectPanel::multicamRequested, this, &MainWindow::createMulticam);
    connect(browser, &MediaBrowserPanel::importRequested, project, &ProjectPanel::importFiles);
    connect(program->engine, &PlaybackEngine::frameReady, scopes, &ScopesPanel::feed);
    scopes->currentImage = [this] { return program->engine->currentImage(); };
    connect(program, &MonitorPanel::requestText, timeline, &TimelinePanel::addTextClip);
    connect(importPage, &ImportPage::importFiles, this, [this](const QStringList &f, bool seq) {
        ensurePolished();
        if (!PRJ->loaded) { PRJ->loaded = true; updateTitle(); }
        int before = PRJ->s.media.size();
        project->importFiles(f);
        if (seq && PRJ->s.media.size() > before) {
            const MediaItem m = PRJ->s.media[before];
            double r = m.fps > 0 ? m.fps : 30;
            int w = m.w > 0 ? m.w : 1920, h = m.h > 0 ? m.h : 1080;
            PRJ->createSequence(QFileInfo(m.name).completeBaseName(), w, h, qRound(r * 1000), 1000);
            PRJ->edit(tr("Add Clips"), [&](ProjectState &st) {
                Sequence &q = *st.seqById(st.activeSeq);
                Frame at = 0;
                for (int i = before; i < st.media.size(); ++i) {
                    const MediaItem mi = st.media[i];
                    if (mi.kind == MediaKind::Nested) continue;
                    Ops::overwriteMedia(st, q, mi, 0, mi.duration, at, Ops::Patch{mi.kind != MediaKind::Audio ? 0 : -1, mi.hasAudio ? 0 : -1});
                    at += qRound64(mi.duration * q.fps());
                }
            });
        }
        showPage(PageEdit);
        timeline->view->zoomFit();
    });
    connect(home, &HomePage::newProject, this, &MainWindow::newProject);
    connect(home, &HomePage::openProject, this, [this] { openProject(); });
    connect(home, &HomePage::openPath, this, [this](const QString &p) { openProject(p); });
    connect(PRJ, &Project::dirtyChanged, this, &MainWindow::updateTitle);
    connect(PRJ, &Project::changed, this, &MainWindow::updateTitle);

    buildMenus();
    loadShortcuts();
    jobLabel = new QLabel(this);
    statusBar()->addPermanentWidget(jobLabel);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setFixedHeight(22);
    connect(CACHE, &MediaCache::jobsChanged, this, [this](int n) { jobLabel->setText(n ? tr("Conforming audio: %n file(s) remaining", "", n) : QString()); });
    if (!CACHE->available())
        statusBar()->showMessage(tr("ffmpeg not found: install it or place ffmpeg.exe and ffprobe.exe next to the application."));

    auto *auto_ = new QTimer(this);
    auto_->setInterval(5 * 60 * 1000);
    connect(auto_, &QTimer::timeout, this, [] {
        if (!PRJ->loaded || PRJ->undo.isClean()) return;
        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/AutoSave";
        QDir().mkpath(dir);
        QString keep = PRJ->filePath;
        bool clean = PRJ->undo.isClean();
        QFile f(dir + "/" + PRJ->displayName() + ".autosave.oep");
        if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(stateToJson(PRJ->s)).toJson(QJsonDocument::Compact));
        Q_UNUSED(keep); Q_UNUSED(clean);
    });
    auto_->start();

    AITOOLS->setProxiesEnabled = [this](bool on) { setProxies(on); };
    if (QSettings().value("useProxies", false).toBool()) setProxies(true);
    if (QSettings().value("mcp/enabled", false).toBool()) McpServer::instance()->start(quint16(QSettings().value("mcp/port", 8765).toInt()));
    setWorkspace(QSettings().value("workspace", "Editing").toString());
    showPage(PageHome);
    updateTitle();
}

void MainWindow::buildHeader(QVBoxLayout *v)
{
    header = new QWidget(this);
    header->setFixedHeight(36);
    header->setStyleSheet("background:#1b1b1b;");
    auto *h = new QHBoxLayout(header);
    h->setContentsMargins(8, 0, 8, 0);
    h->setSpacing(2);
    auto *hm = iconButton("home", tr("Home"), header, 28);
    connect(hm, &QToolButton::clicked, this, [this] { showPage(PageHome); });
    h->addWidget(hm);
    h->addSpacing(8);
    auto tab = [&](const QString &t, QHash<QString, QPushButton *> &map, QHBoxLayout *lay, std::function<void()> fn) {
        auto *b = new QPushButton(t, header);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFlat(true);
        b->setStyleSheet("QPushButton{background:transparent;border:0;border-radius:0;padding:8px 10px;color:#9a9a9a;}"
                         "QPushButton:hover{color:#f0f0f0;}"
                         "QPushButton:checked{color:#ffffff;border-bottom:2px solid #2d8ceb;}");
        connect(b, &QPushButton::clicked, this, fn);
        map[t] = b;
        lay->addWidget(b);
    };
    tab(tr("Import"), pageTabs, h, [this] { showPage(PageImport); });
    tab(tr("Edit"), pageTabs, h, [this] { showPage(PageEdit); });
    tab(tr("Export"), pageTabs, h, [this] { showPage(PageExport); });
    h->addStretch(1);
    for (auto &w : kWorkspaces) tab(w, wsTabs, h, [this, w] { setWorkspace(w); showPage(PageEdit); });
    h->addStretch(1);
    titleLabel = new QLabel(header);
    titleLabel->setStyleSheet("color:#9a9a9a;");
    h->addWidget(titleLabel);
    h->addSpacing(8);
    auto *fs = iconButton("fullscreen", tr("Full Screen"), header, 28);
    connect(fs, &QToolButton::clicked, this, [this] { isFullScreen() ? showNormal() : showFullScreen(); });
    h->addWidget(fs);
    v->addWidget(header);
}

void MainWindow::showPage(Page p)
{
    stack->setCurrentIndex(p);
    pageTabs[tr("Import")]->setChecked(p == PageImport);
    pageTabs[tr("Edit")]->setChecked(p == PageEdit);
    pageTabs[tr("Export")]->setChecked(p == PageExport);
    for (auto *b : std::as_const(wsTabs)) b->setVisible(p == PageEdit);
    if (p == PageExport) exportPage->prepare(program->engine->currentImage());
    if (p == PageHome) home->refreshRecent();
}

void MainWindow::setWorkspace(const QString &name)
{
    if (!wsLayouts.contains(name)) return;
    QSettings s;
    if (!workspace.isEmpty()) s.setValue("layout2/" + workspace, panels->saveLayout());
    workspace = name;
    s.setValue("workspace", name);
    QString saved = s.value("layout2/" + name).toString();
    panels->applyLayout(saved.isEmpty() ? wsLayouts[name] : saved);
    for (auto it = wsTabs.begin(); it != wsTabs.end(); ++it) it.value()->setChecked(it.key() == name);
}

QAction *MainWindow::act(QMenu *m, const QString &text, const QString &key, const std::function<void()> &fn)
{
    auto *a = m->addAction(text);
    if (!key.isEmpty()) a->setShortcut(QKeySequence(key));
    a->setData(key);
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, fn);
    addAction(a);
    allActions << a;
    return a;
}

MonitorPanel *MainWindow::activeMonitor() const
{
    return panels->activePanel() == source ? source : program;
}

void MainWindow::buildMenus()
{
    auto *tl = timeline;
    QMenu *f = menuBar()->addMenu(tr("&File"));
    act(f, tr("New Project..."), "Ctrl+Alt+N", [this] { newProject(); });
    act(f, tr("New Sequence..."), "Ctrl+N", [this] { project->newSequenceDialog(); showPage(PageEdit); });
    act(f, tr("New Bin"), "Ctrl+B", [this] { project->newBin(); });
    act(f, tr("Open Project..."), "Ctrl+O", [this] { openProject(); });
    f->addSeparator();
    act(f, tr("Save"), "Ctrl+S", [this] { saveProject(false); });
    act(f, tr("Save As..."), "Ctrl+Shift+S", [this] { saveProject(true); });
    f->addSeparator();
    act(f, tr("Import..."), "Ctrl+I", [this] { project->importDialog(); });
    act(f, tr("Import Page"), "Ctrl+Shift+I", [this] { showPage(PageImport); });
    f->addSeparator();
    act(f, tr("Export Media..."), "Ctrl+M", [this] { showPage(PageExport); });
    act(f, tr("Export Frame..."), "Ctrl+Shift+E", [this] { activeMonitor()->exportFrame(); });
    f->addSeparator();
    act(f, tr("Exit"), "Ctrl+Q", [this] { close(); });

    QMenu *e = menuBar()->addMenu(tr("&Edit"));
    auto *undo = PRJ->undo.createUndoAction(this, tr("Undo"));
    undo->setShortcut(QKeySequence("Ctrl+Z"));
    auto *redo = PRJ->undo.createRedoAction(this, tr("Redo"));
    redo->setShortcut(QKeySequence("Ctrl+Shift+Z"));
    undo->setData("Ctrl+Z"); redo->setData("Ctrl+Shift+Z");
    e->addAction(undo); e->addAction(redo);
    addAction(undo); addAction(redo);
    allActions << undo << redo;
    e->addSeparator();
    act(e, tr("Cut"), "Ctrl+X", [tl] { tl->copySelection(true); });
    act(e, tr("Copy"), "Ctrl+C", [tl] { tl->copySelection(false); });
    act(e, tr("Paste"), "Ctrl+V", [tl] { tl->paste(false); });
    act(e, tr("Paste Insert"), "Ctrl+Shift+V", [tl] { tl->paste(true); });
    act(e, tr("Paste Attributes"), "Ctrl+Alt+V", [tl] { tl->pasteAttributes(); });
    e->addSeparator();
    act(e, tr("Clear"), "Del", [this, tl] {
        if (panels->activePanel() == project) project->deleteSelected();
        else tl->deleteSelection(false);
    });
    act(e, tr("Ripple Delete"), "Shift+Del", [tl] { tl->deleteSelection(true); });
    e->addSeparator();
    act(e, tr("Select All"), "Ctrl+A", [tl] { tl->selectAll(); });
    act(e, tr("Deselect All"), "Ctrl+Shift+A", [] { PRJ->setSelection({}); });
    e->addSeparator();
    act(e, tr("Keyboard Shortcuts..."), "Ctrl+Alt+K", [this] { shortcutsDialog(); });
    act(e, tr("Preferences..."), "", [this] { prefsDialog(); });

    QMenu *c = menuBar()->addMenu(tr("&Clip"));
    act(c, tr("Speed/Duration..."), "Ctrl+R", [tl] { tl->speedDialog(); });
    act(c, tr("Frame Hold Segment"), "", [tl] { tl->frameHold(); });
    act(c, tr("Scale to Frame Size"), "", [tl] { tl->scaleToFrame(true); });
    c->addSeparator();
    act(c, tr("Insert"), ",", [this] { source->sourceEdit(true); });
    act(c, tr("Overwrite"), ".", [this] { source->sourceEdit(false); });
    c->addSeparator();
    act(c, tr("Enable"), "Shift+E", [tl] { tl->toggleEnable(); });
    act(c, tr("Link / Unlink"), "Ctrl+L", [tl] { tl->toggleLink(); });
    act(c, tr("Nest..."), "", [tl] { tl->nestSelection(); });

    QMenu *s = menuBar()->addMenu(tr("&Sequence"));
    act(s, tr("Add Edit"), "Ctrl+K", [tl] { tl->razorAtPlayhead(false); });
    act(s, tr("Add Edit to All Tracks"), "Ctrl+Shift+K", [tl] { tl->razorAtPlayhead(true); });
    act(s, tr("Trim Previous Edit to Playhead"), "Q", [tl] { tl->trimToPlayhead(true); });
    act(s, tr("Trim Next Edit to Playhead"), "W", [tl] { tl->trimToPlayhead(false); });
    s->addSeparator();
    act(s, tr("Apply Video Transition"), "Ctrl+D", [tl] { tl->addTransitionAtPlayhead(true, false); });
    act(s, tr("Apply Audio Transition"), "Ctrl+Shift+D", [tl] { tl->addTransitionAtPlayhead(false, true); });
    act(s, tr("Apply Default Transitions to Selection"), "Shift+D", [tl] { tl->addTransitionAtPlayhead(true, true); });
    s->addSeparator();
    act(s, tr("Lift"), ";", [tl] { tl->liftRange(); });
    act(s, tr("Extract"), "'", [tl] { tl->extractRange(); });
    s->addSeparator();
    auto *sn = act(s, tr("Snap in Timeline"), "S", [] { PRJ->snapping = !PRJ->snapping; });
    sn->setCheckable(true); sn->setChecked(true);
    act(s, tr("Zoom In"), "=", [tl] { tl->view->setZoom(tl->view->ppf * 1.4, PRJ->active() ? PRJ->active()->playhead : 0); });
    act(s, tr("Zoom Out"), "-", [tl] { tl->view->setZoom(tl->view->ppf / 1.4, PRJ->active() ? PRJ->active()->playhead : 0); });
    act(s, tr("Zoom to Sequence"), "\\", [tl] { tl->view->zoomFit(); });
    s->addSeparator();
    act(s, tr("Add Tracks"), "", [tl] { tl->addTrackDialog(); });

    QMenu *m = menuBar()->addMenu(tr("&Markers"));
    act(m, tr("Mark In"), "I", [this] { activeMonitor()->markIn(); });
    act(m, tr("Mark Out"), "O", [this] { activeMonitor()->markOut(); });
    act(m, tr("Mark Selection"), "/", [tl] { tl->markSelection(); });
    act(m, tr("Go to In"), "Shift+I", [this] { activeMonitor()->gotoIn(); });
    act(m, tr("Go to Out"), "Shift+O", [this] { activeMonitor()->gotoOut(); });
    act(m, tr("Clear In and Out"), "Ctrl+Shift+X", [this] { activeMonitor()->clearInOut(); });
    m->addSeparator();
    act(m, tr("Add Marker"), "M", [tl] { tl->addMarker(); });
    act(m, tr("Go to Next Marker"), "Shift+M", [this] { nextMarker(true); });
    act(m, tr("Go to Previous Marker"), "Ctrl+Shift+M", [this] { nextMarker(false); });

    QMenu *g = menuBar()->addMenu(tr("&Graphics and Titles"));
    act(g, tr("New Text Layer"), "Ctrl+T", [tl] { tl->addTextClip(); });
    act(g, tr("New Color Matte..."), "", [this] { project->newColorMatte(); });
    act(g, tr("New Adjustment Layer"), "", [this] { project->newAdjustment(); });

    QMenu *t = menuBar()->addMenu(tr("&Tools"));
    struct T { const char *n; const char *k; Tool tool; };
    for (T x : {T{"Selection", "V", Tool::Select}, T{"Track Select Forward", "A", Tool::TrackFwd}, T{"Ripple Edit", "B", Tool::Ripple}, T{"Rolling Edit", "N", Tool::Roll},
                T{"Rate Stretch", "R", Tool::Rate}, T{"Razor", "C", Tool::Razor}, T{"Slip", "Y", Tool::Slip}, T{"Slide", "U", Tool::Slide}, T{"Pen", "P", Tool::Pen},
                T{"Hand", "H", Tool::Hand}, T{"Zoom", "Z", Tool::Zoom}, T{"Type", "T", Tool::Text}})
        act(t, tr(x.n), x.k, [tool = x.tool] { EDIT->setTool(tool); });

    QMenu *pl = menuBar()->addMenu(tr("&Playback"));
    act(pl, tr("Play/Stop"), "Space", [this] { activeMonitor()->togglePlay(); });
    act(pl, tr("Shuttle Left"), "J", [this] { activeMonitor()->shuttle(-1); });
    act(pl, tr("Shuttle Stop"), "K", [this] { activeMonitor()->shuttle(0); });
    act(pl, tr("Shuttle Right"), "L", [this] { activeMonitor()->shuttle(1); });
    pl->addSeparator();
    act(pl, tr("Step Back"), "Left", [this] { activeMonitor()->step(-1); });
    act(pl, tr("Step Forward"), "Right", [this] { activeMonitor()->step(1); });
    act(pl, tr("Step Back Many Frames"), "Shift+Left", [this] { activeMonitor()->step(-5); });
    act(pl, tr("Step Forward Many Frames"), "Shift+Right", [this] { activeMonitor()->step(5); });
    act(pl, tr("Go to Previous Edit"), "Up", [] { if (auto q = PRJ->active()) PRJ->setPlayhead(Ops::nextEdit(*q, q->playhead, false, true)); });
    act(pl, tr("Go to Next Edit"), "Down", [] { if (auto q = PRJ->active()) PRJ->setPlayhead(Ops::nextEdit(*q, q->playhead, true, true)); });
    act(pl, tr("Go to Sequence Start"), "Home", [this] { activeMonitor()->setPos(0); });
    act(pl, tr("Go to Sequence End"), "End", [this] { auto *mn = activeMonitor(); mn->setPos(qMax<Frame>(0, mn->length() - 1)); });

    buildAiMenu(menuBar()->addMenu(tr("&AI")));

    QMenu *w = menuBar()->addMenu(tr("&Window"));
    QMenu *wm = w->addMenu(tr("Workspaces"));
    int n = 1;
    for (auto &name : kWorkspaces) act(wm, name, QString("Alt+Shift+%1").arg(n++), [this, name] { setWorkspace(name); showPage(PageEdit); });
    wm->addSeparator();
    act(wm, tr("Reset to Saved Layout"), "", [this] {
        QSettings().remove("layout2/" + workspace);
        panels->applyLayout(wsLayouts[workspace]);
    });
    w->addSeparator();
    act(w, tr("Maximize Frame"), "`", [this] { panels->toggleMaximize(); });
    w->addSeparator();
    struct P { const char *id; const char *n; };
    for (P x : {P{"assistant", "AI Assistant"}, P{"audiomixer", "Audio Track Mixer"}, P{"audiometers", "Audio Meters"}, P{"color", "Color"}, P{"effectcontrols", "Effect Controls"}, P{"effects", "Effects"},
                P{"graphics", "Graphics"}, P{"history", "History"}, P{"info", "Info"}, P{"markers", "Markers"}, P{"mediabrowser", "Media Browser"}, P{"metadata", "Metadata"}, P{"multicam", "Multi-Camera"},
                P{"program", "Program Monitor"}, P{"project", "Project"}, P{"scopes", "Scopes"}, P{"source", "Source Monitor"}, P{"captions", "Text"}, P{"timeline", "Timeline"}, P{"tools", "Tools"}}) {
        auto *a = w->addAction(tr(x.n));
        a->setCheckable(true);
        QString id = x.id;
        connect(a, &QAction::triggered, this, [this, id] { panels->togglePanel(id); });
        connect(w, &QMenu::aboutToShow, a, [this, a, id] { a->setChecked(panels->isVisible(id)); });
    }
    act(w, tr("Focus Project"), "Shift+1", [this] { panels->focusPanel("project"); });
    act(w, tr("Focus Source Monitor"), "Shift+2", [this] { panels->focusPanel("source"); });
    act(w, tr("Focus Timeline"), "Shift+3", [this] { panels->focusPanel("timeline"); });
    act(w, tr("Focus Program Monitor"), "Shift+4", [this] { panels->focusPanel("program"); });
    act(w, tr("Focus Effect Controls"), "Shift+5", [this] { panels->focusPanel("effectcontrols"); });

    QMenu *h = menuBar()->addMenu(tr("&Help"));
    act(h, tr("Keyboard Shortcuts"), "", [this] { shortcutsDialog(); });
    act(h, tr("Setup Wizard..."), "", [] { Setup::run(false); });
    act(h, tr("About Open Editor"), "", [this] {
        QMessageBox::about(this, tr("About Open Editor"),
                           tr("<b>Open Editor 2.1</b><br>A non-linear video editor built with Qt.<br><br>"
                              "Decoding and encoding by FFmpeg (separate executable).<br>Icons: Tabler Icons, MIT License."));
    });
}

void MainWindow::loadShortcuts()
{
    QSettings s;
    s.beginGroup("shortcuts");
    for (auto *a : std::as_const(allActions))
        if (s.contains(a->text())) a->setShortcut(QKeySequence(s.value(a->text()).toString()));
}

void MainWindow::shortcutsDialog()
{
    QDialog d(this);
    d.setWindowTitle(tr("Keyboard Shortcuts"));
    d.resize(560, 640);
    auto *v = new QVBoxLayout(&d);
    auto *filter = new QLineEdit(&d);
    filter->setPlaceholderText(tr("Search"));
    v->addWidget(filter);
    auto *tb = new QTableWidget(allActions.size(), 2, &d);
    tb->setHorizontalHeaderLabels({tr("Command"), tr("Shortcut")});
    tb->verticalHeader()->hide();
    tb->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tb->setColumnWidth(1, 200);
    QVector<QKeySequenceEdit *> eds;
    for (int i = 0; i < allActions.size(); ++i) {
        auto *a = allActions[i];
        tb->setItem(i, 0, new QTableWidgetItem(a->text()));
        tb->item(i, 0)->setFlags(Qt::ItemIsEnabled);
        auto *ed = new QKeySequenceEdit(a->shortcut(), tb);
        tb->setCellWidget(i, 1, ed);
        eds << ed;
    }
    v->addWidget(tb);
    connect(filter, &QLineEdit::textChanged, &d, [=](const QString &s) {
        for (int i = 0; i < tb->rowCount(); ++i) tb->setRowHidden(i, !tb->item(i, 0)->text().contains(s, Qt::CaseInsensitive));
    });
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, &d);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(bb->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &d, [&] {
        for (int i = 0; i < allActions.size(); ++i) eds[i]->setKeySequence(QKeySequence(allActions[i]->data().toString()));
    });
    if (d.exec() != QDialog::Accepted) return;
    QSettings s;
    s.beginGroup("shortcuts");
    QHash<QString, QString> used;
    QStringList clashes;
    for (int i = 0; i < allActions.size(); ++i) {
        QString k = eds[i]->keySequence().toString();
        if (!k.isEmpty()) { if (used.contains(k)) clashes << QString("%1: %2 / %3").arg(k, used[k], allActions[i]->text()); used[k] = allActions[i]->text(); }
        allActions[i]->setShortcut(eds[i]->keySequence());
        s.setValue(allActions[i]->text(), k);
    }
    if (!clashes.isEmpty()) QMessageBox::warning(this, tr("Keyboard Shortcuts"), tr("These shortcuts are assigned twice:\n%1").arg(clashes.join('\n')));
}

void MainWindow::prefsDialog()
{
    QDialog d(this);
    d.setWindowTitle(tr("Preferences"));
    d.setMinimumWidth(480);
    auto *f = new QFormLayout(&d);
    auto *gpuOn = new QCheckBox(tr("Use the GPU for effects"), &d);
    gpuOn->setChecked(GpuFx::enabled);
    auto *hwOn = new QCheckBox(tr("Use hardware video decoding"), &d);
    hwOn->setChecked(VideoReader::hwDecode);
    f->addRow(gpuOn);
    f->addRow(hwOn);
    if (!GpuFx::rendererName().isEmpty()) f->addRow(tr("Renderer:"), new QLabel(GpuFx::rendererName(), &d));
    auto *mem = new QSpinBox(&d);
    mem->setRange(1, 64); mem->setSuffix(" GB");
    mem->setValue(QSettings().value("cacheGB", 3).toInt());
    f->addRow(tr("Frame cache memory:"), mem);
    auto *ff = new QLabel(CACHE->ffmpeg().isEmpty() ? tr("Not found") : CACHE->ffmpeg(), &d);
    ff->setWordWrap(true);
    ff->setTextInteractionFlags(Qt::TextSelectableByMouse);
    f->addRow(tr("ffmpeg:"), ff);
    auto *cd = new QLabel(CACHE->cacheDir(), &d);
    cd->setWordWrap(true);
    cd->setTextInteractionFlags(Qt::TextSelectableByMouse);
    f->addRow(tr("Media cache:"), cd);
    auto *cl = new QPushButton(tr("Delete Unused Media Cache Files"), &d);
    connect(cl, &QPushButton::clicked, &d, [cl] { CACHE->clearDiskCache(); cl->setEnabled(false); });
    f->addRow(QString(), cl);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    QSettings().setValue("cacheGB", mem->value());
    GpuFx::enabled = gpuOn->isChecked();
    VideoReader::hwDecode = hwOn->isChecked();
    QSettings().setValue("gpu", gpuOn->isChecked());
    QSettings().setValue("hwDecode", hwOn->isChecked());
    VideoReader::setCacheBudget(qint64(mem->value()) << 30);
}

void MainWindow::nextMarker(bool fwd)
{
    auto q = PRJ->active();
    if (!q) return;
    Frame ph = q->playhead, best = -1;
    for (auto &m : q->markers) if (fwd ? m.t > ph && (best < 0 || m.t < best) : m.t < ph && m.t > best) best = m.t;
    if (best >= 0) PRJ->setPlayhead(best);
}

void MainWindow::updateTitle()
{
    QString n = PRJ->displayName() + (PRJ->undo.isClean() ? "" : " *");
    titleLabel->setText(PRJ->loaded ? n : QString());
    setWindowTitle(PRJ->loaded ? QString("Open Editor - %1").arg(n) : QString("Open Editor"));
}

bool MainWindow::confirmClose()
{
    if (PRJ->undo.isClean()) return true;
    auto r = QMessageBox::question(this, tr("Save Changes"), tr("Save changes to \"%1\"?").arg(PRJ->displayName()),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    return r == QMessageBox::Discard || saveProject(false);
}
bool MainWindow::saveProject(bool as)
{
    QString path = PRJ->filePath;
    if (as || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save Project"), PRJ->displayName() + ".oep", tr("Open Editor Project (*.oep)"));
        if (path.isEmpty()) return false;
    }
    if (!PRJ->save(path)) { QMessageBox::warning(this, tr("Save"), tr("Could not write %1.").arg(path)); return false; }
    addRecent(path);
    updateTitle();
    return true;
}
void MainWindow::addRecent(const QString &path)
{
    QSettings s;
    auto l = s.value("recent").toStringList();
    l.removeAll(path);
    l.prepend(path);
    while (l.size() > 12) l.removeLast();
    s.setValue("recent", l);
}
void MainWindow::openProject(const QString &given)
{
    if (!confirmClose()) return;
    QString path = given.isEmpty() ? QFileDialog::getOpenFileName(this, tr("Open Project"), {}, tr("Open Editor Project (*.oep)")) : given;
    if (path.isEmpty()) return;
    if (!PRJ->load(path)) { QMessageBox::warning(this, tr("Open Project"), tr("This is not a valid Open Editor project.")); return; }
    addRecent(path);
    for (auto &m : PRJ->s.media) if (m.hasAudio && !m.path.isEmpty() && !m.offline) CACHE->ensureAudio(m.path);
    int off = 0;
    for (auto &m : PRJ->s.media) off += m.offline;
    if (off) QMessageBox::warning(this, tr("Open Project"), tr("%n media file(s) could not be found and are offline.", "", off));
    showPage(PageEdit);
    updateTitle();
}
void MainWindow::newProject()
{
    if (!confirmClose()) return;
    bool ok;
    QString name = QInputDialog::getText(this, tr("New Project"), tr("Project name:"), QLineEdit::Normal, tr("Untitled Project"), &ok);
    if (!ok) return;
    PRJ->reset();
    PRJ->loaded = true;
    QString path = QFileDialog::getSaveFileName(this, tr("Project Location"), QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(name + ".oep"), tr("Open Editor Project (*.oep)"));
    if (!path.isEmpty()) { PRJ->save(path); addRecent(path); }
    updateTitle();
    showPage(PageImport);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!confirmClose()) { e->ignore(); return; }
    QSettings().setValue("layout2/" + workspace, panels->saveLayout());
    e->accept();
}
