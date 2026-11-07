#include "sidepanels.h"
#include "../core/effectdefs.h"
#include "../core/mediacache.h"
#include "../core/project.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDial>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMimeData>
#include <QPainter>
#include <QScrollArea>
#include <QSlider>
#include <QStandardPaths>
#include <QUndoView>
#include <QVBoxLayout>
#include <cmath>

using namespace Theme;

ToolsPanel::ToolsPanel(QWidget *p) : Panel("tools", tr("Tools"), p)
{
    auto *v = new QGridLayout(this);
    v->setContentsMargins(4, 6, 4, 6);
    v->setSpacing(2);
    group = new QButtonGroup(this);
    group->setExclusive(true);
    struct T { Tool t; const char *ic; const char *tip; };
    const T tools[] = {
        {Tool::Select, "t_select", "Selection Tool (V)"}, {Tool::TrackFwd, "t_track", "Track Select Forward Tool (A)"},
        {Tool::Ripple, "t_ripple", "Ripple Edit Tool (B)"}, {Tool::Roll, "t_roll", "Rolling Edit Tool (N)"},
        {Tool::Rate, "t_rate", "Rate Stretch Tool (R)"}, {Tool::Razor, "t_razor", "Razor Tool (C)"},
        {Tool::Slip, "t_slip", "Slip Tool (Y)"}, {Tool::Slide, "t_slide", "Slide Tool (U)"},
        {Tool::Pen, "t_pen", "Pen Tool (P)"}, {Tool::Hand, "t_hand", "Hand Tool (H)"},
        {Tool::Zoom, "t_zoom", "Zoom Tool (Z)"}, {Tool::Text, "t_text", "Type Tool (T)"}};
    int i = 0;
    for (auto &t : tools) {
        auto *b = iconButton(t.ic, tr(t.tip), this, 30, true);
        b->setIcon(Theme::icon(t.ic, text, Qt::white));
        b->setStyleSheet("QToolButton:checked{background:#2d8ceb;}");
        group->addButton(b, int(t.t));
        v->addWidget(b, i, 0, Qt::AlignHCenter);
        ++i;
    }
    v->setRowStretch(i, 1);
    group->button(int(Tool::Select))->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, [](int id) { EDIT->setTool(Tool(id)); });
    connect(EDIT, &EditState::toolChanged, this, [this](Tool t) { group->button(int(t))->setChecked(true); });
}

EffectsTree::EffectsTree(QWidget *p) : QTreeWidget(p)
{
    setHeaderHidden(true);
    setDragEnabled(true);
    setIconSize(QSize(16, 16));
    setSelectionMode(SingleSelection);
}
QStringList EffectsTree::mimeTypes() const { return {kMimeEffect, kMimeTransition}; }
QMimeData *EffectsTree::mimeData(const QList<QTreeWidgetItem *> &items) const
{
    auto *md = new QMimeData;
    if (items.isEmpty()) return md;
    QString id = items.first()->data(0, Qt::UserRole).toString();
    bool trans = items.first()->data(0, Qt::UserRole + 1).toBool();
    if (!id.isEmpty()) md->setData(trans ? kMimeTransition : kMimeEffect, id.toUtf8());
    return md;
}

EffectsPanel::EffectsPanel(QWidget *p) : Panel("effects", tr("Effects"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    search = new QLineEdit(this);
    search->setPlaceholderText(tr("Search effects and transitions"));
    search->addAction(Theme::icon("search", textDim), QLineEdit::LeadingPosition);
    search->setClearButtonEnabled(true);
    auto *w = new QWidget(this);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(6, 4, 6, 4);
    h->addWidget(search);
    v->addWidget(w);
    tree = new EffectsTree(this);
    v->addWidget(tree, 1);
    connect(search, &QLineEdit::textChanged, this, &EffectsPanel::build);
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *i, int) { applyItem(i); });
    build({});
}
void EffectsPanel::build(const QString &f)
{
    tree->clear();
    auto root = [&](const QString &name) {
        auto *i = new QTreeWidgetItem(tree, {name});
        i->setIcon(0, Theme::icon("folder", QColor("#c9a55c")));
        i->setFlags(Qt::ItemIsEnabled);
        return i;
    };
    QHash<QString, QTreeWidgetItem *> cats;
    auto cat = [&](QTreeWidgetItem *r, const QString &k) {
        QString key = r->text(0) + k;
        if (!cats.contains(key)) {
            auto *i = new QTreeWidgetItem(r, {k});
            i->setIcon(0, Theme::icon("folder", QColor("#c9a55c")));
            i->setFlags(Qt::ItemIsEnabled);
            cats[key] = i;
        }
        return cats[key];
    };
    auto *ae = root(tr("Audio Effects")), *at = root(tr("Audio Transitions")), *ve = root(tr("Video Effects")), *vt = root(tr("Video Transitions"));
    for (auto &d : EffectRegistry::all()) {
        if (d.intrinsic || (!f.isEmpty() && !d.name.contains(f, Qt::CaseInsensitive))) continue;
        auto *i = new QTreeWidgetItem(d.audio ? cat(ae, d.category) : cat(ve, d.category), {d.name});
        i->setIcon(0, Theme::icon(d.audio ? "audio" : "fxicon", QColor("#9ab8d8")));
        i->setData(0, Qt::UserRole, d.id);
    }
    for (auto &d : EffectRegistry::transitions()) {
        if (!f.isEmpty() && !d.name.contains(f, Qt::CaseInsensitive)) continue;
        auto *i = new QTreeWidgetItem(d.audio ? cat(at, d.category) : cat(vt, d.category), {d.name});
        i->setIcon(0, Theme::icon("transition", QColor("#b8a0d8")));
        i->setData(0, Qt::UserRole, d.id);
        i->setData(0, Qt::UserRole + 1, true);
        if (d.id == EDIT->defaultVideoTransition || d.id == EDIT->defaultAudioTransition) i->setForeground(0, accent);
    }
    if (!f.isEmpty()) tree->expandAll();
    else { ve->setExpanded(true); vt->setExpanded(true); }
}
void EffectsPanel::applyItem(QTreeWidgetItem *i)
{
    QString id = i->data(0, Qt::UserRole).toString();
    if (id.isEmpty() || i->data(0, Qt::UserRole + 1).toBool()) return;
    auto *d = EffectRegistry::find(id);
    QSet<int> ids = PRJ->selClips;
    if (!d || ids.isEmpty()) return;
    PRJ->edit(tr("Apply Effect: %1").arg(d->name), [&](ProjectState &st) {
        for (auto *t : st.seqById(st.activeSeq)->allTracks())
            for (auto &c : t->clips)
                if (ids.contains(c.id) && d->audio == (c.kind == MediaKind::Audio)) c.fx << EffectRegistry::make(id);
    });
}

InfoPanel::InfoPanel(QWidget *p) : Panel("info", tr("Info"), p)
{
    auto *v = new QVBoxLayout(this);
    lab = new QLabel(this);
    lab->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    lab->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(lab);
    connect(PRJ, &Project::selectionChanged, this, &InfoPanel::refresh);
    connect(PRJ, &Project::changed, this, &InfoPanel::refresh);
    connect(PRJ, &Project::playheadChanged, this, &InfoPanel::refresh);
    refresh();
}
void InfoPanel::refresh()
{
    if (!isVisible()) return;
    auto q = PRJ->active();
    QString s;
    if (q) {
        s += tr("<b>%1</b><br>Current: %2<br>Duration: %3<br>%4 x %5, %6 fps<br><br>")
                 .arg(q->name.toHtmlEscaped(), framesToTimecode(q->playhead, q->fps(), q->dropFrame), framesToTimecode(q->duration(), q->fps(), q->dropFrame))
                 .arg(q->w).arg(q->h).arg(q->fps(), 0, 'f', 2);
        for (int id : std::as_const(PRJ->selClips)) {
            if (auto *c = q->clip(id)) {
                s += tr("<b>%1</b><br>Start: %2<br>End: %3<br>Duration: %4<br>Speed: %5%<br><br>")
                         .arg(c->name.toHtmlEscaped(), framesToTimecode(c->start, q->fps(), q->dropFrame), framesToTimecode(c->end(), q->fps(), q->dropFrame), framesToTimecode(c->dur, q->fps(), q->dropFrame))
                         .arg(c->speed * 100, 0, 'f', 1);
                break;
            }
        }
    } else s = tr("No sequence open.");
    lab->setText(s);
}

HistoryPanel::HistoryPanel(QWidget *p) : Panel("history", tr("History"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto *u = new QUndoView(&PRJ->undo, this);
    u->setEmptyLabel(tr("New/Open"));
    v->addWidget(u);
}

MarkersPanel::MarkersPanel(QWidget *p) : Panel("markers", tr("Markers"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    list = new QTreeWidget(this);
    list->setHeaderLabels({tr("In"), tr("Name"), tr("Comment")});
    list->setRootIsDecorated(false);
    v->addWidget(list);
    connect(PRJ, &Project::changed, this, &MarkersPanel::refresh);
    connect(list, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem *i) { PRJ->setPlayhead(i->data(0, Qt::UserRole).toLongLong()); });
    connect(list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *i, int col) {
        if (busy) return;
        int idx = list->indexOfTopLevelItem(i);
        QString s = i->text(col);
        PRJ->edit(tr("Edit Marker"), [&](ProjectState &st) {
            auto &m = st.seqById(st.activeSeq)->markers[idx];
            if (col == 1) m.name = s; else if (col == 2) m.comment = s;
        });
    });
}
void MarkersPanel::refresh()
{
    busy = true;
    list->clear();
    if (auto q = PRJ->active())
        for (auto &m : q->markers) {
            auto *i = new QTreeWidgetItem(list, {framesToTimecode(m.t, q->fps(), q->dropFrame), m.name, m.comment});
            i->setData(0, Qt::UserRole, m.t);
            i->setFlags(i->flags() | Qt::ItemIsEditable);
            QPixmap px(12, 12); px.fill(labelColor(m.color));
            i->setIcon(0, QIcon(px));
        }
    busy = false;
}

MediaBrowserPanel::MediaBrowserPanel(QWidget *p) : Panel("mediabrowser", tr("Media Browser"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    model = new QFileSystemModel(this);
    model->setRootPath(QString());
    model->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Files);
    view = new QTreeView(this);
    view->setModel(model);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setColumnWidth(0, 240);
    view->setExpanded(model->index(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)), true);
    v->addWidget(view);
    connect(view, &QTreeView::doubleClicked, this, [this](const QModelIndex &i) {
        if (!model->isDir(i)) emit importRequested({model->filePath(i)});
    });
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this, [this](const QPoint &pt) {
        QStringList f;
        for (auto &i : view->selectionModel()->selectedRows()) f << model->filePath(i);
        if (f.isEmpty()) return;
        QMenu m;
        m.addAction(tr("Import"), [this, f] { emit importRequested(f); });
        m.exec(view->viewport()->mapToGlobal(pt));
    });
}

MetadataPanel::MetadataPanel(QWidget *p) : Panel("metadata", tr("Metadata"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    list = new QTreeWidget(this);
    list->setHeaderLabels({tr("Property"), tr("Value")});
    list->setRootIsDecorated(false);
    list->setColumnWidth(0, 150);
    v->addWidget(list);
}
void MetadataPanel::show(int id)
{
    list->clear();
    auto *m = PRJ->s.mediaById(id);
    if (!m) return;
    auto add = [&](const QString &k, const QString &val) { if (!val.isEmpty()) new QTreeWidgetItem(list, {k, val}); };
    add(tr("Name"), m->name);
    add(tr("File Path"), m->path);
    add(tr("Duration"), m->duration > 0 ? QString::number(m->duration, 'f', 3) + " s" : QString());
    add(tr("Frame Size"), m->w ? QString("%1 x %2").arg(m->w).arg(m->h) : QString());
    add(tr("Frame Rate"), m->fps > 0 ? QString::number(m->fps, 'f', 3) : QString());
    add(tr("Video Codec"), m->vcodec);
    add(tr("Audio Codec"), m->acodec);
    add(tr("Sample Rate"), m->sampleRate ? QString("%1 Hz").arg(m->sampleRate) : QString());
    add(tr("Channels"), m->channels ? QString::number(m->channels) : QString());
    add(tr("Bit Rate"), m->bitrate ? QString("%1 kb/s").arg(m->bitrate / 1000) : QString());
    add(tr("File Size"), m->size ? QString("%1 MB").arg(m->size / 1048576.0, 0, 'f', 1) : QString());
}

MeterBar::MeterBar(QWidget *p, bool s) : QWidget(p), scale(s)
{
    setMinimumWidth(s ? 46 : 20);
    setMaximumWidth(s ? 16777215 : 24);
}
void MeterBar::setLevels(float l, float r)
{
    float in[2] = {l, r};
    for (int c = 0; c < 2; ++c) {
        cur[c] = qMax(in[c], cur[c] * .82f);
        if (cur[c] >= hold[c]) { hold[c] = cur[c]; holdT[c] = 45; }
        else if (--holdT[c] < 0) hold[c] *= .93f;
        if (in[c] >= 1.f) clip[c] = true;
    }
    update();
}
void MeterBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const int top = 12, scaleW = scale ? 24 : 0;
    int h = height() - top - 4;
    auto yOf = [&](double db) { return top + int((0 - db) / 60.0 * h); };
    int barW = (width() - scaleW - 3) / 2;
    if (scale) {
        QFont f = font(); f.setPixelSize(9); p.setFont(f);
        p.setPen(textDim);
        for (int db = 0; db >= -54; db -= 6) {
            int y = yOf(db);
            p.drawText(QRect(width() - scaleW + 3, y - 6, scaleW - 3, 12), Qt::AlignLeft | Qt::AlignVCenter, QString::number(db));
        }
    }
    QLinearGradient g(0, top, 0, top + h);
    g.setColorAt(0, QColor("#e53935"));
    g.setColorAt(.06, QColor("#e8c040"));
    g.setColorAt(.30, QColor("#4fc34f"));
    g.setColorAt(1, QColor("#2a8f3a"));
    for (int c = 0; c < 2; ++c) {
        int x = 1 + c * (barW + 1);
        QRect r(x, top, barW, h);
        p.fillRect(r, QColor("#111"));
        double db = cur[c] > 1e-5f ? 20 * std::log10(cur[c]) : -96;
        int y = qBound(top, yOf(db), top + h);
        p.fillRect(QRect(x, y, barW, top + h - y), g);
        double hd = hold[c] > 1e-5f ? 20 * std::log10(hold[c]) : -96;
        if (hd > -60) { int yh = qBound(top, yOf(hd), top + h - 1); p.fillRect(x, yh, barW, 2, hd > -.5 ? QColor("#ff5050") : QColor("#e8e8e8")); }
        p.fillRect(QRect(x, 2, barW, 7), clip[c] ? QColor("#e53935") : QColor("#333"));
    }
}

AudioMetersPanel::AudioMetersPanel(QWidget *p) : Panel("audiometers", tr("Audio Meters"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(4, 2, 2, 4);
    bar = new MeterBar(this, true);
    v->addWidget(bar);
    timer.setInterval(33);
    connect(&timer, &QTimer::timeout, this, [this] {
        PlaybackEngine *e = source ? source() : nullptr;
        if (e && e->playing()) bar->setLevels(e->mixer.masterPeak[0], e->mixer.masterPeak[1]);
        else bar->setLevels(0, 0);
    });
    timer.start();
}

static double sliderToDb(int v) { return v / 10.0; }

AudioMixerPanel::AudioMixerPanel(QWidget *p) : Panel("audiomixer", tr("Audio Track Mixer"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    v->addWidget(scroll);
    connect(PRJ, &Project::changed, this, &AudioMixerPanel::rebuild);
    timer.setInterval(33);
    connect(&timer, &QTimer::timeout, this, [this] {
        PlaybackEngine *e = source ? source() : nullptr;
        bool on = e && e->playing();
        for (int i = 0; i < meters.size() && i < AudioMixer::kMaxTracks; ++i)
            meters[i]->setLevels(on ? e->mixer.trackPeak[i][0].load() : 0, on ? e->mixer.trackPeak[i][1].load() : 0);
        if (master) master->setLevels(on ? e->mixer.masterPeak[0].load() : 0, on ? e->mixer.masterPeak[1].load() : 0);
    });
    timer.start();
    rebuild();
}

void AudioMixerPanel::rebuild()
{
    auto q = PRJ->active();
    QString s;
    if (q) {
        s = QString::number(q->id);
        for (auto &t : q->audio) s += QString("|%1:%2:%3:%4:%5:%6").arg(t.id).arg(t.name).arg(t.muted).arg(t.solo).arg(t.volumeDb).arg(t.pan) + QString("|%1:%2").arg(t.fx.size()).arg(t.writeAuto);
        s += QString("|m%1").arg(q->masterDb);
    }
    if (s == sig || PRJ->inLive()) return;
    sig = s;
    meters.clear();
    master = nullptr;
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(6, 6, 6, 6);
    h->setSpacing(4);
    if (!q) { delete scroll->takeWidget(); scroll->setWidget(w); return; }
    auto strip = [&](const QString &name, int tid, double db, double pan, bool mute, bool solo, bool isMaster) {
        auto *st = new QFrame(w);
        st->setFixedWidth(92);
        st->setStyleSheet("QFrame{background:#2a2a2a;border-radius:2px;}");
        auto *v = new QVBoxLayout(st);
        v->setContentsMargins(4, 4, 4, 4);
        v->setSpacing(3);
        if (!isMaster) {
            auto *d = new PanKnob(st);
            d->setRange(-100, 100);
            d->setValue(int(pan * 100));
            d->setFixedSize(38, 38);
            d->setToolTip(tr("Pan"));
            v->addWidget(d, 0, Qt::AlignHCenter);
            connect(d, &QDial::sliderPressed, this, [] { PRJ->liveBegin(); });
            connect(d, &QDial::valueChanged, this, [tid](int x) { if (auto *t = PRJ->active()->track(tid)) { t->pan = x / 100.0; if (PRJ->inLive()) PRJ->liveTouch(); } });
            connect(d, &QDial::sliderReleased, this, [] { PRJ->liveCommit(tr("Track Pan")); });
            auto *ms = new QHBoxLayout;
            auto btn = [&](const QString &txt, bool on, const char *col, std::function<void(Track &)> fn) {
                auto *b = new QToolButton(st);
                b->setText(txt);
                b->setCheckable(true);
                b->setChecked(on);
                b->setFixedSize(22, 20);
                b->setFocusPolicy(Qt::NoFocus);
                b->setStyleSheet(QString("QToolButton{background:#3a3a3a;}QToolButton:checked{background:%1;color:black;}").arg(col));
                connect(b, &QToolButton::clicked, this, [tid, fn] {
                    PRJ->edit(tr("Track Mixer"), [&](ProjectState &stt) { if (auto *t = stt.seqById(stt.activeSeq)->track(tid)) fn(*t); });
                });
                ms->addWidget(b);
            };
            btn("M", mute, "#3fbf5f", [](Track &t) { t.muted = !t.muted; });
            btn("S", solo, "#e8c040", [](Track &t) { t.solo = !t.solo; });
            btn("W", PRJ->active()->track(tid)->writeAuto, "#e0604a", [](Track &t) { t.writeAuto = !t.writeAuto; });
            auto *fxBtn = new QToolButton(st);
            fxBtn->setText("fx");
            fxBtn->setFixedSize(22, 20);
            fxBtn->setFocusPolicy(Qt::NoFocus);
            fxBtn->setToolTip(tr("Track Effects"));
            fxBtn->setStyleSheet(PRJ->active()->track(tid)->fx.isEmpty() ? "QToolButton{background:#3a3a3a;}" : "QToolButton{background:#2d8ceb;color:white;}");
            connect(fxBtn, &QToolButton::clicked, this, [this, tid, fxBtn] { trackFxMenu(tid, fxBtn); });
            ms->addWidget(fxBtn);
            v->addLayout(ms);
        } else v->addSpacing(68);
        auto *fh = new QHBoxLayout;
        auto *sl = new QSlider(Qt::Vertical, st);
        sl->setRange(-600, 60);
        sl->setValue(int(db * 10));
        sl->setToolTip(tr("Volume"));
        auto *mb = new MeterBar(st, false);
        fh->addWidget(sl);
        fh->addWidget(mb);
        v->addLayout(fh, 1);
        auto *lb = new QLabel(QString::number(db, 'f', 1), st);
        lb->setAlignment(Qt::AlignCenter);
        lb->setStyleSheet("color:#3fa2ff;");
        v->addWidget(lb);
        auto *nm = new QLabel(name, st);
        nm->setAlignment(Qt::AlignCenter);
        nm->setStyleSheet("color:#ddd;font-weight:600;");
        v->addWidget(nm);
        connect(sl, &QSlider::sliderPressed, this, [] { PRJ->liveBegin(); });
        connect(sl, &QSlider::valueChanged, this, [=](int x) {
            lb->setText(QString::number(sliderToDb(x), 'f', 1));
            auto *sq = PRJ->active();
            if (isMaster) sq->masterDb = sliderToDb(x);
            else if (auto *t = sq->track(tid)) {
                t->volumeDb = sliderToDb(x);
                if (t->writeAuto && source && source()->playing()) {
                    t->volAuto.animated = true;
                    t->volAuto.setKey(sq->playhead, sliderToDb(x));
                }
            }
            if (PRJ->inLive()) PRJ->liveTouch();
        });
        connect(sl, &QSlider::sliderReleased, this, [] { PRJ->liveCommit(tr("Track Volume")); });
        if (isMaster) master = mb; else meters << mb;
        h->addWidget(st);
    };
    for (auto &t : q->audio) strip(t.name, t.id, t.volumeDb, t.pan, t.muted, t.solo, false);
    h->addSpacing(8);
    strip(tr("Mix"), 0, q->masterDb, 0, false, false, true);
    h->addStretch(1);
    delete scroll->takeWidget();
    scroll->setWidget(w);
}

void PanKnob::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = QRectF(rect()).adjusted(4, 4, -4, -4);
    p.setPen(QPen(QColor("#111"), 3, Qt::SolidLine, Qt::FlatCap));
    p.drawArc(r, -60 * 16, 300 * 16);
    const double u = double(value() - minimum()) / qMax(1, maximum() - minimum());
    p.setPen(QPen(scrub, 3, Qt::SolidLine, Qt::FlatCap));
    p.drawArc(r, 90 * 16, int((.5 - u) * 300 * 16));
    p.setBrush(QColor("#3a3a3a"));
    p.setPen(QPen(QColor("#555"), 1));
    QRectF in = r.adjusted(5, 5, -5, -5);
    p.drawEllipse(in);
    p.translate(in.center());
    p.rotate((u - .5) * 300);
    p.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(0, -in.height() / 2 + 2), QPointF(0, -in.height() / 2 + 7));
}
