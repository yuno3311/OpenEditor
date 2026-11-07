#include "pages.h"
#include "../core/project.h"
#include "../engine/compositor.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

using namespace Theme;

// Open Editor's own mark: a film-strip frame cut by a playhead.
static QPixmap logo(int s)
{
    QPixmap px(s, s);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient g(0, 0, s, s);
    g.setColorAt(0, QColor("#16304f"));
    g.setColorAt(1, QColor("#0d1b2e"));
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(0, 0, s, s), s * .2, s * .2);
    double m = s * .2;
    QRectF r(m, m * 1.25, s - 2 * m, s - 2.5 * m);
    p.setPen(QPen(QColor("#2d8ceb"), s * .06, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, s * .05, s * .05);
    p.setPen(QPen(QColor("#2d8ceb"), s * .045));
    p.drawLine(QPointF(r.left() + r.width() * .32, r.top()), QPointF(r.left() + r.width() * .32, r.bottom()));
    p.setPen(QPen(QColor("#ffffff"), s * .06, Qt::SolidLine, Qt::RoundCap));
    double x = r.left() + r.width() * .68;
    p.drawLine(QPointF(x, s * .12), QPointF(x, s * .88));
    return px;
}
QIcon appIcon()
{
    QIcon i;
    for (int s : {16, 24, 32, 48, 64, 128, 256}) i.addPixmap(logo(s));
    return i;
}

HomePage::HomePage(QWidget *p) : QWidget(p)
{
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(60, 60, 60, 60);
    h->setSpacing(40);
    auto *left = new QVBoxLayout;
    auto *lg = new QLabel(this);
    lg->setPixmap(logo(72));
    left->addWidget(lg);
    auto *t = new QLabel("Open Editor", this);
    t->setStyleSheet("font-size:30px;font-weight:300;color:#f0f0f0;");
    left->addWidget(t);
    auto *st = new QLabel(tr("Non-linear video editing"), this);
    st->setStyleSheet("color:#8a8a8a;");
    left->addWidget(st);
    left->addSpacing(26);
    auto *np = new QPushButton(tr("New Project"), this);
    np->setProperty("primary", true);
    np->setMinimumHeight(34);
    auto *op = new QPushButton(tr("Open Project"), this);
    op->setMinimumHeight(34);
    left->addWidget(np);
    left->addWidget(op);
    left->addStretch(1);
    h->addLayout(left, 0);
    auto *right = new QVBoxLayout;
    auto *rl = new QLabel(tr("Recent"), this);
    rl->setStyleSheet("font-size:18px;color:#e0e0e0;");
    right->addWidget(rl);
    recent = new QListWidget(this);
    recent->setStyleSheet("QListWidget{background:#1f1f1f;} QListWidget::item{padding:9px 8px;}");
    right->addWidget(recent, 1);
    h->addLayout(right, 1);
    connect(np, &QPushButton::clicked, this, &HomePage::newProject);
    connect(op, &QPushButton::clicked, this, &HomePage::openProject);
    connect(recent, &QListWidget::itemActivated, this, [this](QListWidgetItem *i) { emit openPath(i->data(Qt::UserRole).toString()); });
    connect(recent, &QListWidget::itemClicked, this, [this](QListWidgetItem *i) { emit openPath(i->data(Qt::UserRole).toString()); });
    refreshRecent();
}
void HomePage::refreshRecent()
{
    recent->clear();
    QSettings s;
    for (auto &f : s.value("recent").toStringList()) {
        QFileInfo fi(f);
        if (!fi.exists()) continue;
        auto *i = new QListWidgetItem(Theme::icon("sequence", textDim), QString("%1    %2").arg(fi.completeBaseName(), fi.lastModified().toString("yyyy-MM-dd hh:mm")), recent);
        i->setData(Qt::UserRole, f);
        i->setToolTip(f);
    }
}
void HomePage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), bg);
}

ImportPage::ImportPage(QWidget *p) : QWidget(p)
{
    setAutoFillBackground(true);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto *sp = new QSplitter(this);
    sp->setHandleWidth(2);
    auto *loc = new QListWidget(sp);
    loc->setMaximumWidth(260);
    auto addLoc = [&](const QString &name, const QString &path, const QString &ic) {
        auto *i = new QListWidgetItem(Theme::icon(ic, textDim), name, loc);
        i->setData(Qt::UserRole, path);
    };
    addLoc(tr("Home"), QDir::homePath(), "home");
    addLoc(tr("Videos"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation), "video");
    addLoc(tr("Music"), QStandardPaths::writableLocation(QStandardPaths::MusicLocation), "music");
    addLoc(tr("Pictures"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation), "photo");
    addLoc(tr("Desktop"), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation), "folder");
    addLoc(tr("Documents"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), "folder");
    addLoc(tr("Downloads"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), "import");
    for (auto &d : QDir::drives()) addLoc(d.absolutePath(), d.absolutePath(), "folder");
    auto *mid = new QWidget(sp);
    auto *mv = new QVBoxLayout(mid);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(0);
    auto *nav = new QWidget(mid);
    auto *nh = new QHBoxLayout(nav);
    nh->setContentsMargins(8, 6, 8, 6);
    auto *up = iconButton("stepback", tr("Up one level"), nav, 24);
    pathLabel = new QLabel(nav);
    nh->addWidget(up);
    nh->addWidget(pathLabel, 1);
    mv->addWidget(nav);
    model = new QFileSystemModel(this);
    model->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Files);
    model->setNameFilters({"*.mp4", "*.mov", "*.mkv", "*.avi", "*.webm", "*.m4v", "*.mxf", "*.mts", "*.m2ts", "*.wmv", "*.mpg", "*.wav", "*.mp3", "*.aac", "*.m4a", "*.flac", "*.ogg", "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tif", "*.tiff", "*.webp", "*.gif"});
    model->setNameFilterDisables(false);
    model->setRootPath(QString());
    view = new QListView(mid);
    view->setModel(model);
    view->setViewMode(QListView::IconMode);
    view->setGridSize(QSize(120, 96));
    view->setIconSize(QSize(48, 48));
    view->setResizeMode(QListView::Adjust);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setWordWrap(true);
    mv->addWidget(view, 1);
    sp->addWidget(loc);
    sp->addWidget(mid);
    sp->setStretchFactor(1, 1);
    v->addWidget(sp, 1);
    auto *bot = new QWidget(this);
    bot->setFixedHeight(48);
    auto *bh = new QHBoxLayout(bot);
    bh->setContentsMargins(12, 6, 12, 6);
    selLabel = new QLabel(tr("Select media to import."), bot);
    makeSeq = new QCheckBox(tr("Create new sequence"), bot);
    makeSeq->setChecked(true);
    importBtn = new QPushButton(tr("Import"), bot);
    importBtn->setProperty("primary", true);
    importBtn->setEnabled(false);
    bh->addWidget(selLabel, 1);
    bh->addWidget(makeSeq);
    bh->addWidget(importBtn);
    v->addWidget(bot);

    connect(loc, &QListWidget::itemClicked, this, [this](QListWidgetItem *i) { go(i->data(Qt::UserRole).toString()); });
    connect(up, &QToolButton::clicked, this, [this] { QDir d(pathLabel->text()); if (d.cdUp()) go(d.absolutePath()); });
    connect(view, &QListView::doubleClicked, this, [this](const QModelIndex &i) {
        if (model->isDir(i)) go(model->filePath(i));
        else emit importFiles({model->filePath(i)}, makeSeq->isChecked());
    });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        int n = 0;
        for (auto &i : view->selectionModel()->selectedIndexes()) if (!model->isDir(i)) ++n;
        selLabel->setText(n ? tr("%n file(s) selected", "", n) : tr("Select media to import."));
        importBtn->setEnabled(n > 0);
    });
    connect(importBtn, &QPushButton::clicked, this, [this] {
        QStringList f;
        for (auto &i : view->selectionModel()->selectedIndexes()) if (!model->isDir(i)) f << model->filePath(i);
        if (!f.isEmpty()) emit importFiles(f, makeSeq->isChecked());
    });
    go(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
}
void ImportPage::go(const QString &path)
{
    pathLabel->setText(QDir::toNativeSeparators(path));
    view->setRootIndex(model->index(path));
}

static const struct { const char *name; const char *fmt; int w, h, crf, kbps; } kExportPresets[] = {
    {"Match Source - High Bitrate", "H.264", 0, 0, 18, 0}, {"Match Source - Medium Bitrate", "H.264", 0, 0, 23, 0},
    {"YouTube 1080p Full HD", "H.264", 1920, 1080, 0, 16000}, {"YouTube 2160p 4K Ultra HD", "H.264", 3840, 2160, 0, 45000},
    {"Vimeo 1080p Full HD", "H.264", 1920, 1080, 0, 20000}, {"TikTok / Reels 1080x1920", "H.264", 1080, 1920, 0, 12000},
    {"Web HEVC 1080p", "HEVC (H.265)", 1920, 1080, 26, 0}, {"Mastering ProRes 422", "QuickTime ProRes 422", 0, 0, 0, 0},
    {"Custom", "", 0, 0, 0, 0}};

ExportPage::ExportPage(QWidget *p) : QWidget(p)
{
    setAutoFillBackground(true);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(14, 12, 14, 12);
    h->setSpacing(14);
    auto *leftW = new QWidget(this);
    leftW->setFixedWidth(370);
    auto *left = new QVBoxLayout(leftW);
    left->setContentsMargins(0, 0, 0, 0);
    auto *outBox = new QGroupBox(tr("Output"), leftW);
    auto *of = new QFormLayout(outBox);
    fileName = new QLineEdit(outBox);
    folder = new QLineEdit(outBox);
    auto *fb = iconButton("open", tr("Choose folder"), outBox, 24);
    auto *fr = new QHBoxLayout;
    fr->addWidget(folder, 1);
    fr->addWidget(fb);
    of->addRow(tr("File Name"), fileName);
    of->addRow(tr("Location"), fr);
    left->addWidget(outBox);
    auto *setBox = new QGroupBox(tr("Settings"), leftW);
    auto *sf = new QFormLayout(setBox);
    preset = new QComboBox(setBox);
    for (auto &k : kExportPresets) preset->addItem(k.name);
    format = new QComboBox(setBox);
    for (auto &f : exportFormats()) format->addItem(f.name);
    sf->addRow(tr("Preset"), preset);
    sf->addRow(tr("Format"), format);
    left->addWidget(setBox);
    auto *vBox = new QGroupBox(tr("Video"), leftW);
    auto *vf = new QFormLayout(vBox);
    withV = new QCheckBox(tr("Export Video"), vBox); withV->setChecked(true);
    wEd = new QSpinBox(vBox); wEd->setRange(16, 8192); wEd->setSingleStep(2);
    hEd = new QSpinBox(vBox); hEd->setRange(16, 8192); hEd->setSingleStep(2);
    auto *wh = new QHBoxLayout;
    wh->addWidget(wEd); wh->addWidget(new QLabel("x", vBox)); wh->addWidget(hEd);
    crf = new QSpinBox(vBox); crf->setRange(0, 51); crf->setValue(20); crf->setToolTip(tr("Constant quality: lower is better. Used when bitrate is 0."));
    kbps = new QSpinBox(vBox); kbps->setRange(0, 400000); kbps->setSingleStep(1000); kbps->setSuffix(" kbps"); kbps->setSpecialValueText(tr("Constant Quality"));
    speed = new QComboBox(vBox);
    speed->addItems({"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"});
    speed->setCurrentText("medium");
    vf->addRow(withV);
    vf->addRow(tr("Frame Size"), wh);
    vf->addRow(tr("Quality (CRF)"), crf);
    vf->addRow(tr("Target Bitrate"), kbps);
    vf->addRow(tr("Encoding Speed"), speed);
    left->addWidget(vBox);
    auto *aBox = new QGroupBox(tr("Audio"), leftW);
    auto *af = new QFormLayout(aBox);
    withA = new QCheckBox(tr("Export Audio"), aBox); withA->setChecked(true);
    aKbps = new QSpinBox(aBox); aKbps->setRange(64, 512); aKbps->setValue(320); aKbps->setSuffix(" kbps");
    af->addRow(withA);
    af->addRow(tr("Bitrate"), aKbps);
    sidecar = new QCheckBox(tr("Write Captions Beside the File (.srt)"), aBox);
    af->addRow(sidecar);
    left->addWidget(aBox);
    left->addStretch(1);
    h->addWidget(leftW);

    auto *mid = new QVBoxLayout;
    auto *ph = new QLabel(tr("Preview"), this);
    ph->setStyleSheet("color:#aaa;");
    mid->addWidget(ph);
    preview = new QLabel(this);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet("background:#0f0f0f;");
    preview->setMinimumSize(320, 180);
    preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mid->addWidget(preview, 1);
    auto *rh = new QHBoxLayout;
    rh->addWidget(new QLabel(tr("Range"), this));
    range = new QComboBox(this);
    range->addItems({tr("Entire Sequence"), tr("Sequence In/Out")});
    rh->addWidget(range);
    rh->addStretch(1);
    mid->addLayout(rh);
    summary = new QLabel(this);
    summary->setStyleSheet("color:#aaa;");
    summary->setWordWrap(true);
    mid->addWidget(summary);
    bar = new QProgressBar(this);
    bar->setRange(0, 1000);
    bar->setTextVisible(false);
    bar->hide();
    mid->addWidget(bar);
    status = new QLabel(this);
    mid->addWidget(status);
    auto *bh = new QHBoxLayout;
    bh->addStretch(1);
    cancelBtn = new QPushButton(tr("Cancel"), this);
    cancelBtn->hide();
    exportBtn = new QPushButton(tr("Export"), this);
    exportBtn->setProperty("primary", true);
    exportBtn->setMinimumWidth(110);
    bh->addWidget(cancelBtn);
    bh->addWidget(exportBtn);
    mid->addLayout(bh);
    h->addLayout(mid, 1);

    connect(fb, &QToolButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, tr("Save To"), folder->text());
        if (!d.isEmpty()) folder->setText(QDir::toNativeSeparators(d));
    });
    connect(preset, &QComboBox::currentIndexChanged, this, [this] { fromPreset(); });
    for (auto *c : {format, range}) connect(c, &QComboBox::currentIndexChanged, this, [this] { updateSummary(); });
    for (auto *s : {wEd, hEd, crf, kbps, aKbps}) connect(s, &QSpinBox::valueChanged, this, [this] { updateSummary(); });
    connect(exportBtn, &QPushButton::clicked, this, &ExportPage::start);
    connect(cancelBtn, &QPushButton::clicked, this, [this] { if (job) job->cancel(); });
}
void ExportPage::prepare(const QImage &img)
{
    auto q = PRJ->active();
    if (q) {
        if (fileName->text().isEmpty()) fileName->setText(q->name);
        wEd->setValue(q->w); hEd->setValue(q->h);
    }
    if (folder->text().isEmpty()) folder->setText(QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)));
    QImage still = img;
    if (q) { Compositor c; still = c.render(PRJ->s, q->id, q->playhead, 1.0, true); c.releaseAll(); }
    if (!still.isNull()) {
        const qreal dpr = devicePixelRatioF();
        QSize box = QSize(qMax(320, width() - 430), qMax(180, height() - 190)) * dpr;
        QPixmap px = QPixmap::fromImage(still).scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        px.setDevicePixelRatio(dpr);
        preview->setPixmap(px);
    }
    fromPreset();
}
void ExportPage::fromPreset()
{
    auto &k = kExportPresets[preset->currentIndex()];
    auto q = PRJ->active();
    if (QString(k.fmt).isEmpty()) return;
    format->setCurrentText(k.fmt);
    wEd->setValue(k.w ? k.w : (q ? q->w : 1920));
    hEd->setValue(k.h ? k.h : (q ? q->h : 1080));
    if (k.crf) crf->setValue(k.crf);
    kbps->setValue(k.kbps);
    updateSummary();
}
void ExportPage::updateSummary()
{
    auto q = PRJ->active();
    if (!q) { summary->setText(tr("No sequence to export.")); exportBtn->setEnabled(false); return; }
    exportBtn->setEnabled(!job);
    Frame n = q->duration();
    if (range->currentIndex() == 1 && q->inPt >= 0) n = (q->outPt >= 0 ? q->outPt : q->duration()) - q->inPt;
    double sec = n / q->fps();
    QString s = tr("%1 - %2 x %3, %4 fps - Duration %5").arg(format->currentText()).arg(wEd->value()).arg(hEd->value()).arg(q->fps(), 0, 'f', 2).arg(framesToTimecode(n, q->fps(), q->dropFrame));
    if (kbps->value() > 0) s += tr(" - Estimated size %1 MB").arg(int((kbps->value() + aKbps->value()) * sec / 8000));
    summary->setText(s);
}
void ExportPage::start()
{
    auto q = PRJ->active();
    if (!q || job) return;
    if (q->duration() <= 0) { status->setText(tr("The sequence is empty.")); return; }
    const ExportFormat *fmt = nullptr;
    for (auto &f : exportFormats()) if (f.name == format->currentText()) fmt = &f;
    ExportSettings c;
    c.format = format->currentText();
    c.path = QDir(QDir::fromNativeSeparators(folder->text())).filePath(fileName->text() + "." + fmt->ext);
    if (QFileInfo::exists(c.path) && QMessageBox::question(this, tr("Export"), tr("%1 already exists. Replace it?").arg(QFileInfo(c.path).fileName())) != QMessageBox::Yes) return;
    c.width = wEd->value(); c.height = hEd->value(); c.crf = crf->value(); c.kbps = kbps->value();
    c.sidecarSrt = sidecar->isChecked();
    c.preset = speed->currentText(); c.audioKbps = aKbps->value(); c.video = withV->isChecked(); c.audio = withA->isChecked();
    if (range->currentIndex() == 1) { c.first = qMax<Frame>(0, q->inPt); c.last = q->outPt; }
    job = new Exporter(PRJ->s, q->id, c, this);
    bar->show(); bar->setValue(0); cancelBtn->show(); exportBtn->setEnabled(false);
    connect(job, &Exporter::progress, this, [this](double fr, const QString &st) { bar->setValue(int(fr * 1000)); status->setText(st); });
    auto done = [this] { bar->hide(); cancelBtn->hide(); job->deleteLater(); job = nullptr; updateSummary(); };
    connect(job, &Exporter::finishedOk, this, [this, done](const QString &path) {
        done();
        status->setText(tr("Export complete: %1").arg(QDir::toNativeSeparators(path)));
    });
    connect(job, &Exporter::failed, this, [this, done](const QString &w) { done(); status->setText(w); });
    job->start();
}
