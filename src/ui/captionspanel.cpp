#include "captionspanel.h"
#include "../core/ops.h"
#include "../core/project.h"
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QVBoxLayout>

using namespace Theme;

CaptionsPanel::CaptionsPanel(QWidget *p) : Panel("captions", tr("Text"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);
    auto *h = new QHBoxLayout;
    modelBox = new QComboBox(this);
    for (auto &n : Analysis::whisperModels()) modelBox->addItem(QString("%1 (%2 MB)").arg(Analysis::whisperModelLabel(n)).arg(Analysis::whisperModelSizeMB(n)), n);
    modelBox->setCurrentIndex(qMax(0, modelBox->findData(QSettings().value("transcribe/model"))));
    modelBox->setFocusPolicy(Qt::NoFocus);
    modelBox->setToolTip(tr("Speech model. Larger models are slower and more accurate."));
    langBox = new QComboBox(this);
    for (auto &l : std::initializer_list<std::pair<const char *, const char *>>{{"Auto Detect", ""}, {"English", "en"}, {"Korean", "ko"}, {"Japanese", "ja"}, {"Chinese", "zh"}, {"Spanish", "es"}, {"French", "fr"}, {"German", "de"}})
        langBox->addItem(l.first, l.second);
    langBox->setFocusPolicy(Qt::NoFocus);
    goBtn = new QPushButton(tr("Transcribe Sequence"), this);
    goBtn->setProperty("primary", true);
    cancelBtn = new QPushButton(tr("Cancel"), this);
    cancelBtn->hide();
    h->addWidget(modelBox, 1);
    auto *h2 = new QHBoxLayout;
    h2->addWidget(langBox, 1);
    h2->addWidget(cancelBtn);
    h2->addWidget(goBtn);
    v->addLayout(h);
    v->addLayout(h2);
    bar = new QProgressBar(this);
    bar->setRange(0, 1000);
    bar->setTextVisible(false);
    bar->hide();
    v->addWidget(bar);
    status = new QLabel(this);
    status->setStyleSheet("color:#8a8a8a;");
    status->hide();
    v->addWidget(status);
    table = new QTableWidget(0, 3, this);
    table->setHorizontalHeaderLabels({tr("In"), tr("Out"), tr("Text")});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->setColumnWidth(0, 88);
    table->setColumnWidth(1, 88);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setWordWrap(true);
    tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    auto *tw = new QWidget(tabs);
    auto *tv = new QVBoxLayout(tw);
    tv->setContentsMargins(0, 4, 0, 0);
    tv->setSpacing(4);
    searchBox = new QLineEdit(tw);
    searchBox->setPlaceholderText(tr("Find in transcript"));
    searchBox->addAction(Theme::icon("search", textDim), QLineEdit::LeadingPosition);
    searchBox->setClearButtonEnabled(true);
    tv->addWidget(searchBox);
    transcript = new TranscriptView(tw);
    tv->addWidget(transcript, 1);
    tabs->addTab(tw, tr("Transcript"));
    tabs->addTab(table, tr("Captions"));
    v->addWidget(tabs, 1);
    connect(searchBox, &QLineEdit::returnPressed, this, [this] { transcript->find(searchBox->text(), true); transcript->setFocus(); });
    auto *b = new QHBoxLayout;
    auto *add = iconButton("plus", tr("Add Caption at Playhead"), this, 24);
    auto *del = iconButton("trash", tr("Delete Selected Captions"), this, 24);
    auto *cutBtn = new QPushButton(tr("Delete Selected Text from Video"), this);
    cutBtn->setToolTip(tr("Text-based editing: remove the footage under the selected words or lines and close the gaps."));
    auto *imp = new QPushButton(tr("Import..."), this);
    auto *exp = new QPushButton(tr("Export..."), this);
    burn = new QCheckBox(tr("Show in Video"), this);
    burn->setChecked(true);
    size = new QSpinBox(this);
    size->setRange(12, 200);
    size->setSuffix(" px");
    size->setToolTip(tr("Caption size"));
    b->addWidget(add); b->addWidget(del); b->addWidget(cutBtn, 1);
    auto *b2 = new QHBoxLayout;
    b2->addWidget(burn); b2->addWidget(size); b2->addStretch(1); b2->addWidget(imp); b2->addWidget(exp);
    v->addLayout(b);
    v->addLayout(b2);

    connect(goBtn, &QPushButton::clicked, this, &CaptionsPanel::transcribe);
    connect(cancelBtn, &QPushButton::clicked, this, [this] { if (job) job->cancel(); });
    connect(add, &QToolButton::clicked, this, &CaptionsPanel::addCaption);
    connect(del, &QToolButton::clicked, this, &CaptionsPanel::deleteSelected);
    connect(cutBtn, &QPushButton::clicked, this, [this] {
        if (tabs->currentIndex() == 0) transcript->deleteSelection(); else removeSelectedFootage();
    });
    connect(imp, &QPushButton::clicked, this, &CaptionsPanel::importFile);
    connect(exp, &QPushButton::clicked, this, &CaptionsPanel::exportFile);
    connect(burn, &QCheckBox::clicked, this, [](bool on) { if (PRJ->active()) PRJ->edit(tr("Show Captions"), [&](ProjectState &st) { st.seqById(st.activeSeq)->burnCaptions = on; }); });
    connect(size, &QSpinBox::valueChanged, this, [this](int v2) {
        if (busy || !PRJ->active()) return;
        PRJ->edit(tr("Caption Size"), [&](ProjectState &st) { st.seqById(st.activeSeq)->captionSize = v2; });
    });
    connect(PRJ, &Project::changed, this, &CaptionsPanel::refresh);
    connect(table, &QTableWidget::cellClicked, this, [this](int r, int) {
        if (auto q = PRJ->active()) if (r < q->captions.size()) PRJ->setPlayhead(q->captions[r].start);
    });
    connect(table, &QTableWidget::cellChanged, this, [this](int r, int c) {
        if (busy) return;
        QString t = table->item(r, c)->text();
        auto q = PRJ->active();
        if (!q || r >= q->captions.size()) return;
        bool ok = true;
        Frame f = c < 2 ? timecodeToFrames(t, q->fps(), q->dropFrame, &ok) : 0;
        if (!ok) { refresh(); return; }
        PRJ->edit(tr("Edit Caption"), [&](ProjectState &st) {
            auto &cap = st.seqById(st.activeSeq)->captions[r];
            if (c == 0) cap.start = f;
            else if (c == 1) cap.end = qMax(cap.start + 1, f);
            else { cap.text = t; cap.words = Analysis::retimeWords(cap.words, t, cap.start, cap.end); }
        });
    });
    refresh();
}

void CaptionsPanel::refresh()
{
    busy = true;
    auto q = PRJ->active();
    int n = q ? q->captions.size() : 0;
    table->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const Caption &c = q->captions[i];
        auto set = [&](int col, const QString &s) {
            auto *it = table->item(i, col);
            if (!it) { it = new QTableWidgetItem; table->setItem(i, col, it); }
            if (it->text() != s) it->setText(s);
        };
        set(0, framesToTimecode(c.start, q->fps(), q->dropFrame));
        set(1, framesToTimecode(c.end, q->fps(), q->dropFrame));
        set(2, c.text);
    }
    if (q) { burn->setChecked(q->burnCaptions); size->setValue(q->captionSize); }
    busy = false;
}

void CaptionsPanel::transcribe()
{
    auto q = PRJ->active();
    if (!q || q->duration() <= 0) { QMessageBox::information(this, tr("Transcribe"), tr("Add clips to a sequence first.")); return; }
    if (job) return;
    const QString name = modelBox->currentData().toString(), path = Analysis::whisperModelPath(name);
    if (QFileInfo::exists(path)) { beginTranscribe(path); return; }
    if (QMessageBox::question(this, tr("Download Speech Model"),
                              tr("Transcription runs on this computer and needs the \"%1\" speech model, a %2 MB download from huggingface.co. Download it now?")
                                  .arg(name).arg(Analysis::whisperModelSizeMB(name))) != QMessageBox::Yes) return;
    auto *dl = new ModelDownloader(this);
    bar->show(); status->show(); bar->setValue(0);
    goBtn->setEnabled(false);
    connect(dl, &ModelDownloader::progress, this, [this](qint64 g, qint64 t) {
        if (t > 0) { bar->setValue(int(g * 1000 / t)); status->setText(tr("Downloading model: %1 of %2 MB").arg(g >> 20).arg(t >> 20)); }
    });
    connect(dl, &ModelDownloader::finished, this, [this, dl](const QString &p) { dl->deleteLater(); goBtn->setEnabled(true); beginTranscribe(p); });
    connect(dl, &ModelDownloader::failed, this, [this, dl](const QString &e) { dl->deleteLater(); goBtn->setEnabled(true); bar->hide(); status->setText(tr("Download failed: %1").arg(e)); });
    dl->start(name);
}

void CaptionsPanel::beginTranscribe(const QString &model)
{
    auto q = PRJ->active();
    if (!q) return;
    int sid = q->id;
    job = new Transcriber(PRJ->s, sid, model, langBox->currentData().toString(), this);
    bar->show(); status->show(); cancelBtn->show(); goBtn->setEnabled(false);
    connect(job, &Transcriber::progress, this, [this](double f, const QString &s) { bar->setValue(int(f * 1000)); status->setText(s); });
    connect(job, &Transcriber::finishedOk, this, [this, sid](const QVector<Caption> &c) {
        PRJ->edit(tr("Transcribe"), [&](ProjectState &st) { if (auto *s = st.seqById(sid)) s->captions = c; });
        status->setText(c.isEmpty() ? tr("No speech found.") : tr("%n caption(s) added.", "", int(c.size())));
    });
    connect(job, &Transcriber::failed, this, [this](const QString &e) { status->setText(e); });
    connect(job, &QThread::finished, this, [this] {
        bar->hide(); cancelBtn->hide(); goBtn->setEnabled(true);
        job->deleteLater();
        job = nullptr;
    });
    job->start();
}

void CaptionsPanel::addCaption()
{
    auto q = PRJ->active();
    if (!q) return;
    Caption c{q->playhead, q->playhead + Frame(q->fps() * 3), tr("New caption")};
    PRJ->edit(tr("Add Caption"), [&](ProjectState &st) {
        auto &s = *st.seqById(st.activeSeq);
        s.captions << c;
        std::sort(s.captions.begin(), s.captions.end(), [](const Caption &a, const Caption &b) { return a.start < b.start; });
    });
}
static QList<int> selectedRows(QTableWidget *t)
{
    QList<int> r;
    for (auto &i : t->selectionModel()->selectedRows()) r << i.row();
    std::sort(r.begin(), r.end());
    return r;
}
void CaptionsPanel::deleteSelected()
{
    auto rows = selectedRows(table);
    if (rows.isEmpty()) return;
    PRJ->edit(tr("Delete Captions"), [&](ProjectState &st) {
        auto &s = *st.seqById(st.activeSeq);
        for (int i = rows.size() - 1; i >= 0; --i) s.captions.removeAt(rows[i]);
    });
}
void CaptionsPanel::removeSelectedFootage()
{
    auto q = PRJ->active();
    auto rows = selectedRows(table);
    if (!q || rows.isEmpty()) return;
    QVector<QPair<Frame, Frame>> ranges;
    for (int r : rows) ranges << qMakePair(q->captions[r].start, q->captions[r].end);
    PRJ->edit(tr("Delete Transcript Text"), [&](ProjectState &st) { Ops::rippleRemoveRanges(st, *st.seqById(st.activeSeq), ranges); });
}
void CaptionsPanel::importFile()
{
    auto q = PRJ->active();
    if (!q) return;
    QString f = QFileDialog::getOpenFileName(this, tr("Import Captions"), {}, tr("Subtitles (*.srt *.vtt)"));
    if (f.isEmpty()) return;
    QFile in(f);
    if (!in.open(QIODevice::ReadOnly)) return;
    auto caps = Analysis::parseSubtitles(QString::fromUtf8(in.readAll()), q->fps());
    if (caps.isEmpty()) { QMessageBox::warning(this, tr("Import Captions"), tr("No captions were found in that file.")); return; }
    PRJ->edit(tr("Import Captions"), [&](ProjectState &st) { st.seqById(st.activeSeq)->captions = caps; });
}
void CaptionsPanel::exportFile()
{
    auto q = PRJ->active();
    if (!q || q->captions.isEmpty()) return;
    QString f = QFileDialog::getSaveFileName(this, tr("Export Captions"), q->name + ".srt", tr("SubRip (*.srt);;WebVTT (*.vtt)"));
    if (f.isEmpty()) return;
    QFile out(f);
    if (!out.open(QIODevice::WriteOnly)) return;
    out.write((f.endsWith(".vtt", Qt::CaseInsensitive) ? Analysis::writeVtt(q->captions, q->fps()) : Analysis::writeSrt(q->captions, q->fps())).toUtf8());
}

// ---------------------------------------------------------------- multicam
MulticamPanel::MulticamPanel(QWidget *p) : Panel("multicam", tr("Multi-Camera"), p)
{
    setMinimumSize(160, 120);
    th = std::thread([this] { worker(); });
    timer.setInterval(120);
    connect(&timer, &QTimer::timeout, this, [this] { if (isVisible()) request(); });
    timer.start();
}
MulticamPanel::~MulticamPanel()
{
    { std::lock_guard<std::mutex> l(m); quit = true; }
    cv.notify_all();
    th.join();
}
const Clip *MulticamPanel::currentClip() const
{
    auto q = PRJ->active();
    if (!q) return nullptr;
    for (auto &t : q->video)
        if (auto *c = t.clipAt(q->playhead))
            if (c->cam >= 0) return c;
    return nullptr;
}
void MulticamPanel::request()
{
    auto q = PRJ->active();
    const Clip *c = currentClip();
    if (!q || !c) { if (angles) { angles = 0; update(); } return; }
    auto *mi = PRJ->s.mediaById(c->mediaId);
    auto *inner = mi ? PRJ->s.seqById(mi->seqId) : nullptr;
    if (!inner) return;
    std::lock_guard<std::mutex> l(m);
    snap = std::make_shared<const ProjectState>(PRJ->s);
    seqId = inner->id;
    frame = Frame(c->sourceTime(q->playhead, q->fps()) * inner->fps() + 1e-4);
    angles = inner->video.size();
    want = true;
    cv.notify_all();
}
void MulticamPanel::worker()
{
    Compositor comp;
    std::unique_lock<std::mutex> l(m);
    for (;;) {
        cv.wait(l, [this] { return quit || want; });
        if (quit) break;
        want = false;
        auto s = snap;
        int sid = seqId, n = angles;
        Frame f = frame;
        l.unlock();
        QVector<QImage> out;
        for (int i = 0; i < n; ++i) out << comp.renderAngle(*s, sid, f, .25, i);
        l.lock();
        QMetaObject::invokeMethod(this, [this, out] { images = out; update(); }, Qt::QueuedConnection);
    }
    l.unlock();
    comp.releaseAll();
}
QRect MulticamPanel::cell(int i, int n) const
{
    int cols = n <= 1 ? 1 : n <= 4 ? 2 : n <= 9 ? 3 : 4, rows = (n + cols - 1) / cols;
    int w = width() / cols, h = height() / rows;
    return QRect((i % cols) * w, (i / cols) * h, w, h).adjusted(2, 2, -2, -2);
}
void MulticamPanel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), bgDark);
    const Clip *c = currentClip();
    if (!c || !angles) {
        p.setPen(textDim);
        p.drawText(rect().adjusted(16, 0, -16, 0), Qt::AlignCenter | Qt::TextWordWrap, tr("Park the playhead on a multicam clip. Select clips in the Project panel and choose Create Multi-Camera Source Sequence to make one."));
        return;
    }
    for (int i = 0; i < angles; ++i) {
        QRect r = cell(i, angles);
        p.fillRect(r, Qt::black);
        if (i < images.size() && !images[i].isNull()) {
            QSize s = images[i].size().scaled(r.size(), Qt::KeepAspectRatio);
            p.drawImage(QRect(r.center().x() - s.width() / 2, r.center().y() - s.height() / 2, s.width(), s.height()), images[i]);
        }
        p.setPen(i == c->cam ? QPen(QColor("#e53935"), 3) : QPen(QColor("#444"), 1));
        p.drawRect(r);
        p.setPen(Qt::white);
        p.drawText(r.adjusted(6, 4, 0, 0), Qt::AlignLeft | Qt::AlignTop, QString::number(i + 1));
    }
}
void MulticamPanel::mousePressEvent(QMouseEvent *e)
{
    for (int i = 0; i < angles; ++i) if (cell(i, angles).contains(e->pos())) { cut(i); return; }
}
void MulticamPanel::cut(int angle)
{
    auto q = PRJ->active();
    if (!q || !currentClip() || angle >= angles) return;
    Frame f = q->playhead;
    PRJ->edit(tr("Multicam Cut"), [&](ProjectState &st) { Ops::cutToAngle(st, *st.seqById(st.activeSeq), f, angle); });
    update();
}
