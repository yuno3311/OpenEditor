#include "projectpanel.h"
#include "../core/mediacache.h"
#include "../core/project.h"
#include <QApplication>
#include <QDir>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace Theme;
enum { RoleMedia = Qt::UserRole, RoleBin };

ProjectTree::ProjectTree(QWidget *p) : QTreeWidget(p)
{
    setHeaderLabels({tr("Name"), tr("Frame Rate"), tr("Media Duration"), tr("Video Info"), tr("Audio Info")});
    setRootIsDecorated(true);
    setSelectionMode(ExtendedSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(DragDrop);
    setAlternatingRowColors(true);
    setIconSize(QSize(16, 16));
    setUniformRowHeights(true);
    setSortingEnabled(false);
    header()->setSectionResizeMode(QHeaderView::Interactive);
    header()->resizeSection(0, 210);
    header()->resizeSection(1, 70);
    header()->resizeSection(2, 95);
}
QStringList ProjectTree::mimeTypes() const { return {kMimeMedia}; }
QMimeData *ProjectTree::mimeData(const QList<QTreeWidgetItem *> &items) const
{
    QStringList ids;
    for (auto *i : items) if (int id = i->data(0, RoleMedia).toInt()) ids << QString::number(id);
    auto *md = new QMimeData;
    md->setData(kMimeMedia, ids.join(',').toUtf8());
    return md;
}
void ProjectTree::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasFormat(kMimeMedia)) e->acceptProposedAction();
}
void ProjectTree::dragMoveEvent(QDragMoveEvent *e)
{
    e->acceptProposedAction();
    QTreeWidget::dragMoveEvent(e);
    e->acceptProposedAction();
}
void ProjectTree::dropEvent(QDropEvent *e)
{
    if (e->mimeData()->hasUrls()) {
        QStringList paths;
        for (auto &u : e->mimeData()->urls()) if (u.isLocalFile()) paths << u.toLocalFile();
        emit filesDropped(paths);
        e->acceptProposedAction();
        return;
    }
    auto *it = itemAt(e->position().toPoint());
    int bin = 0;
    if (it) bin = it->data(0, RoleBin).toInt() ? it->data(0, RoleBin).toInt() : (it->parent() ? it->parent()->data(0, RoleBin).toInt() : 0);
    QList<int> ids;
    for (auto &s : QString::fromUtf8(e->mimeData()->data(kMimeMedia)).split(',', Qt::SkipEmptyParts)) ids << s.toInt();
    emit moved(ids, bin);
    e->ignore();
}

ProjectGrid::ProjectGrid(QWidget *p) : QListWidget(p)
{
    setViewMode(IconMode);
    setResizeMode(Adjust);
    setIconSize(QSize(170, 96));
    setGridSize(QSize(182, 134));
    setSelectionMode(ExtendedSelection);
    setDragEnabled(true);
    setMovement(Static);
    setWordWrap(false);
    setUniformItemSizes(true);
}
QStringList ProjectGrid::mimeTypes() const { return {kMimeMedia}; }
QMimeData *ProjectGrid::mimeData(const QList<QListWidgetItem *> &items) const
{
    QStringList ids;
    for (auto *i : items) if (int id = i->data(RoleMedia).toInt()) ids << QString::number(id);
    auto *md = new QMimeData;
    md->setData(kMimeMedia, ids.join(',').toUtf8());
    return md;
}

ProjectPanel::ProjectPanel(QWidget *p) : Panel("project", tr("Project"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto *top = new QWidget(this);
    auto *h = new QHBoxLayout(top);
    h->setContentsMargins(6, 4, 6, 4);
    h->setSpacing(6);
    search = new QLineEdit(top);
    search->setPlaceholderText(tr("Search"));
    search->addAction(Theme::icon("search", textDim), QLineEdit::LeadingPosition);
    search->setClearButtonEnabled(true);
    h->addWidget(search, 1);
    count = new QLabel(top);
    count->setStyleSheet("color:#8a8a8a;");
    h->addWidget(count);
    v->addWidget(top);
    tree = new ProjectTree(this);
    grid = new ProjectGrid(this);
    auto *st = new QStackedWidget(this);
    st->addWidget(tree);
    st->addWidget(grid);
    v->addWidget(st, 1);
    auto *bot = new QWidget(this);
    bot->setFixedHeight(28);
    auto *b = new QHBoxLayout(bot);
    b->setContentsMargins(6, 2, 6, 2);
    b->setSpacing(2);
    listBtn = iconButton("list", tr("List View"), bot, 22, true);
    gridBtn = iconButton("grid", tr("Icon View"), bot, 22, true);
    listBtn->setChecked(true);
    b->addWidget(listBtn);
    b->addWidget(gridBtn);
    b->addStretch(1);
    auto *nb = iconButton("newbin", tr("New Bin (Ctrl+B)"), bot);
    auto *ni = iconButton("newitem", tr("New Item"), bot);
    auto *tr_ = iconButton("trash", tr("Clear (Backspace)"), bot);
    b->addWidget(nb); b->addWidget(ni); b->addWidget(tr_);
    v->addWidget(bot);

    connect(nb, &QToolButton::clicked, this, &ProjectPanel::newBin);
    connect(tr_, &QToolButton::clicked, this, &ProjectPanel::deleteSelected);
    connect(ni, &QToolButton::clicked, this, [this, ni] {
        QMenu m;
        m.addAction(tr("Sequence..."), [this] { newSequenceDialog(); });
        m.addSeparator();
        m.addAction(tr("Bars and Tone"), [this] { newBars(); });
        m.addAction(tr("Black Video"), [this] { newBlack(); });
        m.addAction(tr("Color Matte..."), [this] { newColorMatte(); });
        m.addAction(tr("Adjustment Layer"), [this] { newAdjustment(); });
        m.exec(ni->mapToGlobal(QPoint(0, -m.sizeHint().height())));
    });
    connect(listBtn, &QToolButton::clicked, this, [=, this] { gridMode = false; st->setCurrentIndex(0); listBtn->setChecked(true); gridBtn->setChecked(false); rebuild(); });
    connect(gridBtn, &QToolButton::clicked, this, [=, this] { gridMode = true; st->setCurrentIndex(1); gridBtn->setChecked(true); listBtn->setChecked(false); rebuild(); });
    connect(search, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(PRJ, &Project::changed, this, &ProjectPanel::rebuild);
    connect(CACHE, &MediaCache::thumbReady, this, [this] { if (gridMode) rebuild(); });
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *i, int) { activated(i->data(0, RoleMedia).toInt()); });
    connect(grid, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *i) { activated(i->data(RoleMedia).toInt()); });
    connect(tree, &ProjectTree::filesDropped, this, &ProjectPanel::importFiles);
    connect(tree, &ProjectTree::moved, this, [](const QList<int> &ids, int bin) {
        PRJ->edit(tr("Move to Bin"), [&](ProjectState &st) { for (int id : ids) if (auto *m = st.mediaById(id)) m->bin = bin; });
    });
    connect(tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *i, int col) {
        if (rebuilding || col) return;
        QString n = i->text(0);
        int bid = i->data(0, RoleBin).toInt(), mid = i->data(0, RoleMedia).toInt();
        PRJ->edit(tr("Rename"), [&](ProjectState &st) {
            if (bid) for (auto &b : st.bins) if (b.id == bid) b.name = n;
            if (auto *m = st.mediaById(mid)) {
                m->name = n;
                if (m->kind == MediaKind::Nested) if (auto *q = st.seqById(m->seqId)) q->name = n;
            }
        });
    });
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { contextMenu(tree->viewport()->mapToGlobal(p)); });
    connect(grid, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { contextMenu(grid->viewport()->mapToGlobal(p)); });
    rebuild();
}

void ProjectPanel::fillMenu(QMenu *m)
{
    m->addAction(tr("New Bin"), [this] { newBin(); });
    m->addAction(tr("Import..."), [this] { importDialog(); });
}

QIcon ProjectPanel::iconFor(const MediaItem &m) const
{
    QString n;
    switch (m.kind) {
    case MediaKind::Video: n = "video"; break;
    case MediaKind::Audio: n = "music"; break;
    case MediaKind::Image: n = "photo"; break;
    case MediaKind::Nested: n = "sequence"; break;
    default: n = "color"; break;
    }
    return Theme::icon(n, labelColor(m.label >= 0 ? m.label : 0));
}

void ProjectPanel::addTreeChildren(QTreeWidgetItem *parent, int bin, const QString &filter)
{
    for (auto &b : PRJ->s.bins) {
        if (b.parent != bin) continue;
        auto *it = new QTreeWidgetItem(QStringList{b.name});
        it->setIcon(0, Theme::icon("folder", QColor("#c9a55c")));
        it->setData(0, RoleBin, b.id);
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        if (parent) parent->addChild(it); else tree->addTopLevelItem(it);
        addTreeChildren(it, b.id, filter);
        it->setExpanded(true);
    }
    for (auto &m : PRJ->s.media) {
        if (m.bin != bin) continue;
        if (!filter.isEmpty() && !m.name.contains(filter, Qt::CaseInsensitive)) continue;
        double fps = m.fps > 0 ? m.fps : 30;
        QString v = m.hasVideo && m.w > 0 ? QString("%1 x %2").arg(m.w).arg(m.h) : QString();
        QString a = m.hasAudio && m.sampleRate ? QString("%1 Hz - %2 ch").arg(m.sampleRate).arg(m.channels) : QString();
        QString dur = m.duration > 0 ? framesToTimecode(Frame(m.duration * fps), fps) : QString();
        if (m.kind == MediaKind::Nested) if (auto *q = PRJ->s.seqById(m.seqId)) dur = framesToTimecode(q->duration(), q->fps(), q->dropFrame);
        auto *it = new QTreeWidgetItem(QStringList{m.name, m.fps > 0 ? QString("%1 fps").arg(m.fps, 0, 'f', 2) : QString(), dur, v, a});
        it->setIcon(0, iconFor(m));
        it->setData(0, RoleMedia, m.id);
        it->setFlags((it->flags() | Qt::ItemIsEditable) & ~Qt::ItemIsDropEnabled);
        if (m.offline) it->setForeground(0, QColor("#e05a5a"));
        if (parent) parent->addChild(it); else tree->addTopLevelItem(it);
    }
}

void ProjectPanel::rebuild()
{
    rebuilding = true;
    QString f = search->text();
    QSet<int> keep;
    for (int id : selectedMedia()) keep << id;
    tree->clear();
    addTreeChildren(nullptr, 0, f);
    grid->clear();
    int n = 0;
    for (auto &m : PRJ->s.media) {
        if (!f.isEmpty() && !m.name.contains(f, Qt::CaseInsensitive)) continue;
        ++n;
        if (!gridMode) continue;
        QPixmap px(170, 96);
        px.fill(Qt::black);
        {
            QPainter p(&px);
            if (m.kind == MediaKind::Video || m.kind == MediaKind::Image) {
                QImage t = CACHE->thumbnail(m.path, qMin(1.0, m.duration * .1), 320, m.kind == MediaKind::Image);
                if (!t.isNull()) {
                    QSize s = t.size().scaled(px.size(), Qt::KeepAspectRatio);
                    p.drawImage(QRect((170 - s.width()) / 2, (96 - s.height()) / 2, s.width(), s.height()), t);
                }
            } else if (m.kind == MediaKind::Color) p.fillRect(px.rect(), m.color);
            else {
                p.drawPixmap(69, 32, Theme::pixmap(m.kind == MediaKind::Audio ? "music" : m.kind == MediaKind::Nested ? "sequence" : "color", 32, QColor("#777")));
            }
            if (m.duration > 0) {
                QString d = framesToTimecode(Frame(m.duration * (m.fps > 0 ? m.fps : 30)), m.fps > 0 ? m.fps : 30);
                QFont ft = p.font(); ft.setPixelSize(10); p.setFont(ft);
                QRect r(170 - 62, 96 - 15, 60, 13);
                p.fillRect(r, QColor(0, 0, 0, 170));
                p.setPen(Qt::white);
                p.drawText(r, Qt::AlignCenter, d);
            }
        }
        auto *it = new QListWidgetItem(QIcon(px), m.name, grid);
        it->setData(RoleMedia, m.id);
        if (keep.contains(m.id)) it->setSelected(true);
    }
    count->setText(tr("%n item(s)", "", n));
    rebuilding = false;
}

QList<int> ProjectPanel::selectedMedia() const
{
    QList<int> r;
    if (gridMode) { for (auto *i : grid->selectedItems()) if (int id = i->data(RoleMedia).toInt()) r << id; }
    else for (auto *i : tree->selectedItems()) if (int id = i->data(0, RoleMedia).toInt()) r << id;
    return r;
}

void ProjectPanel::activated(int id)
{
    auto *m = PRJ->s.mediaById(id);
    if (!m) return;
    if (m->kind == MediaKind::Nested) emit openSequence(m->seqId);
    else emit openInSource(id);
}

void ProjectPanel::contextMenu(const QPoint &g)
{
    QMenu m;
    auto sel = selectedMedia();
    if (!sel.isEmpty()) {
        int first = sel.first();
        m.addAction(tr("Open in Source Monitor"), [this, first] { emit openInSource(first); });
        m.addAction(tr("New Sequence From Clip"), [this, first] {
            auto *mi = PRJ->s.mediaById(first);
            if (!mi) return;
            MediaItem c = *mi;
            double r = c.fps > 0 ? c.fps : 30;
            PRJ->createSequence(c.name, c.w > 0 ? c.w : 1920, c.h > 0 ? c.h : 1080, qRound(r * 1000), 1000);
        });
        auto *lm = m.addMenu(tr("Label"));
        for (int i = 0; i < kLabelCount; ++i) {
            QPixmap px(12, 12); px.fill(labelColor(i));
            lm->addAction(QIcon(px), labelName(i), [sel, i] {
                PRJ->edit(tr("Label"), [&](ProjectState &st) { for (int id : sel) if (auto *x = st.mediaById(id)) x->label = i; });
            });
        }
        m.addAction(tr("Create Multi-Camera Source Sequence..."), [this] { emit multicamRequested(); });
        m.addAction(tr("Create Proxies"), [this, sel] {
            for (int id : sel) if (auto *x = PRJ->s.mediaById(id)) if (x->kind == MediaKind::Video) CACHE->makeProxy(x->path, x->hdr);
        });
        m.addAction(tr("Clear"), [this] { deleteSelected(); });
        m.addSeparator();
    }
    m.addAction(tr("New Bin"), [this] { newBin(); });
    m.addAction(tr("New Sequence..."), [this] { newSequenceDialog(); });
    m.addAction(tr("Import..."), [this] { importDialog(); });
    m.exec(g);
}

void ProjectPanel::importDialog()
{
    QStringList f = QFileDialog::getOpenFileNames(this, tr("Import"), {},
        tr("All Supported Media (*.mp4 *.mov *.mkv *.avi *.webm *.m4v *.mxf *.mts *.m2ts *.wmv *.flv *.mpg *.mpeg *.ts *.wav *.mp3 *.aac *.m4a *.flac *.ogg *.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.gif);;All Files (*)"));
    if (!f.isEmpty()) importFiles(f);
}

void ProjectPanel::importFiles(const QStringList &paths)
{
    if (!CACHE->available()) {
        QMessageBox::warning(this, tr("Import"), tr("ffmpeg was not found. Install ffmpeg and make sure it is on PATH, or place ffmpeg.exe and ffprobe.exe next to OpenEditor.exe."));
        return;
    }
    QStringList files;
    for (auto &p : paths) {
        QFileInfo fi(p);
        if (fi.isDir()) { for (auto &e : QDir(p).entryInfoList(QDir::Files)) files << e.absoluteFilePath(); }
        else files << p;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    struct R { bool ok; MediaItem m; QString path; };
    QList<R> res = QtConcurrent::blockingMapped<QList<R>>(files, [](const QString &p) { R r; r.path = p; r.ok = CACHE->probe(p, r.m); return r; });
    QApplication::restoreOverrideCursor();
    QStringList bad;
    QVector<MediaItem> items;
    for (auto &r : res) { if (r.ok) items << r.m; else bad << QFileInfo(r.path).fileName(); }
    if (!items.isEmpty())
        PRJ->edit(tr("Import"), [&](ProjectState &st) {
            for (auto m : items) { m.id = st.newId(); st.media << m; }
        });
    for (auto &m : items) if (m.hasAudio) CACHE->ensureAudio(m.path);
    if (!bad.isEmpty()) QMessageBox::warning(this, tr("Import"), tr("These files could not be read:\n%1").arg(bad.join('\n')));
}

void ProjectPanel::newBin()
{
    PRJ->edit(tr("New Bin"), [](ProjectState &st) { Bin b; b.id = st.newId(); b.name = tr("Bin %1").arg(st.bins.size() + 1); st.bins << b; });
}
void ProjectPanel::newSequenceDialog()
{
    NewSequenceDialog d(this);
    if (d.exec() != QDialog::Accepted) return;
    int num, den;
    d.rate(num, den);
    PRJ->createSequence(d.name(), d.widthPx(), d.heightPx(), num, den, d.videoTracks(), d.audioTracks());
}
void ProjectPanel::addGenerated(MediaKind k, const QString &name, const QColor &c, int w, int h)
{
    auto q = PRJ->active();
    PRJ->edit(tr("New Item"), [&](ProjectState &st) {
        MediaItem m;
        m.id = st.newId(); m.kind = k; m.name = name; m.color = c; m.duration = 5; m.hasVideo = true;
        m.w = w ? w : (q ? q->w : 1920); m.h = h ? h : (q ? q->h : 1080);
        m.label = defaultLabel(k);
        st.media << m;
    });
}
void ProjectPanel::newColorMatte()
{
    QColor c = QColorDialog::getColor(Qt::darkBlue, this, tr("Color Picker"));
    if (c.isValid()) addGenerated(MediaKind::Color, tr("Color Matte"), c);
}
void ProjectPanel::newBars() { addGenerated(MediaKind::Bars, tr("Bars"), Qt::black); }
void ProjectPanel::newBlack() { addGenerated(MediaKind::Black, tr("Black Video"), Qt::black); }
void ProjectPanel::newAdjustment() { addGenerated(MediaKind::Adjustment, tr("Adjustment Layer"), Qt::transparent); }

void ProjectPanel::deleteSelected()
{
    auto ids = selectedMedia();
    QList<int> bins;
    if (!gridMode) for (auto *i : tree->selectedItems()) if (int b = i->data(0, RoleBin).toInt()) bins << b;
    if (ids.isEmpty() && bins.isEmpty()) return;
    int used = 0;
    for (auto &q : PRJ->s.seqs) for (auto *t : const_cast<Sequence &>(q).allTracks()) for (auto &c : t->clips) if (ids.contains(c.mediaId)) ++used;
    if (used && QMessageBox::question(this, tr("Clear"), tr("%n clip(s) in sequences use this media and will be removed. Continue?", "", used)) != QMessageBox::Yes) return;
    PRJ->edit(tr("Clear"), [&](ProjectState &st) {
        for (auto &q : st.seqs) for (auto *t : q.allTracks())
            t->clips.erase(std::remove_if(t->clips.begin(), t->clips.end(), [&](const Clip &c) { return ids.contains(c.mediaId); }), t->clips.end());
        for (int id : ids) {
            auto *m = st.mediaById(id);
            if (m && m->kind == MediaKind::Nested) {
                int sid = m->seqId;
                for (int i = 0; i < st.seqs.size(); ++i) if (st.seqs[i].id == sid) st.seqs.removeAt(i);
                if (st.activeSeq == sid) st.activeSeq = st.seqs.isEmpty() ? 0 : st.seqs.first().id;
            }
            for (int i = 0; i < st.media.size(); ++i) if (st.media[i].id == id) st.media.removeAt(i);
        }
        for (int b : bins) {
            for (auto &m : st.media) if (m.bin == b) m.bin = 0;
            for (int i = 0; i < st.bins.size(); ++i) if (st.bins[i].id == b) st.bins.removeAt(i);
        }
    });
    PRJ->setSelection({});
}

static const struct { const char *name; int w, h, num, den; } kPresets[] = {
    {"HD 1080p 23.976", 1920, 1080, 24000, 1001}, {"HD 1080p 24", 1920, 1080, 24, 1}, {"HD 1080p 25", 1920, 1080, 25, 1},
    {"HD 1080p 29.97", 1920, 1080, 30000, 1001}, {"HD 1080p 30", 1920, 1080, 30, 1}, {"HD 1080p 59.94", 1920, 1080, 60000, 1001},
    {"HD 1080p 60", 1920, 1080, 60, 1}, {"HD 720p 30", 1280, 720, 30, 1}, {"UHD 4K 23.976", 3840, 2160, 24000, 1001},
    {"UHD 4K 30", 3840, 2160, 30, 1}, {"UHD 4K 60", 3840, 2160, 60, 1}, {"DCI 4K 24", 4096, 2160, 24, 1},
    {"Vertical 1080x1920 30", 1080, 1920, 30, 1}, {"Square 1080x1080 30", 1080, 1080, 30, 1}};
static const struct { const char *name; int num, den; } kRates[] = {
    {"23.976", 24000, 1001}, {"24", 24, 1}, {"25", 25, 1}, {"29.97", 30000, 1001}, {"30", 30, 1}, {"50", 50, 1}, {"59.94", 60000, 1001}, {"60", 60, 1}};

NewSequenceDialog::NewSequenceDialog(QWidget *p) : QDialog(p)
{
    setWindowTitle(tr("New Sequence"));
    setMinimumWidth(420);
    auto *f = new QFormLayout(this);
    preset = new QComboBox(this);
    for (auto &k : kPresets) preset->addItem(k.name);
    preset->setCurrentIndex(4);
    wEd = new QSpinBox(this); wEd->setRange(16, 8192); wEd->setSingleStep(2); wEd->setValue(1920);
    hEd = new QSpinBox(this); hEd->setRange(16, 8192); hEd->setSingleStep(2); hEd->setValue(1080);
    rateBox = new QComboBox(this);
    for (auto &k : kRates) rateBox->addItem(QString("%1 frames/second").arg(k.name));
    rateBox->setCurrentIndex(4);
    vt = new QSpinBox(this); vt->setRange(1, 32); vt->setValue(3);
    at = new QSpinBox(this); at->setRange(1, 32); at->setValue(3);
    nameEd = new QLineEdit(tr("Sequence %1").arg(PRJ->s.seqs.size() + 1), this);
    f->addRow(tr("Preset:"), preset);
    f->addRow(tr("Frame Width:"), wEd);
    f->addRow(tr("Frame Height:"), hEd);
    f->addRow(tr("Timebase:"), rateBox);
    f->addRow(tr("Video Tracks:"), vt);
    f->addRow(tr("Audio Tracks:"), at);
    f->addRow(tr("Sequence Name:"), nameEd);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(preset, &QComboBox::currentIndexChanged, this, [this](int i) {
        auto &k = kPresets[i];
        wEd->setValue(k.w); hEd->setValue(k.h);
        for (int r = 0; r < rateBox->count(); ++r) if (kRates[r].num == k.num && kRates[r].den == k.den) rateBox->setCurrentIndex(r);
    });
}
QString NewSequenceDialog::name() const { return nameEd->text(); }
int NewSequenceDialog::widthPx() const { return wEd->value() & ~1; }
int NewSequenceDialog::heightPx() const { return hEd->value() & ~1; }
void NewSequenceDialog::rate(int &n, int &d) const { auto &k = kRates[rateBox->currentIndex()]; n = k.num; d = k.den; }
int NewSequenceDialog::videoTracks() const { return vt->value(); }
int NewSequenceDialog::audioTracks() const { return at->value(); }
