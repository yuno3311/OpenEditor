#include "monitor.h"
#include "../engine/imagefx.h"
#include "../core/mediacache.h"
#include "../core/ops.h"
#include "../core/project.h"
#include "../engine/compositor.h"
#include <QDrag>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QtMath>
#include <cmath>

using namespace Theme;

ViewerWidget::ViewerWidget(MonitorPanel *m) : QWidget(m), mon(m)
{
    setMinimumSize(120, 80);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
}
QRectF ViewerWidget::imageRect() const
{
    QSizeF s = frameSize;
    double k = zoom > 0 ? zoom : qMin((width() - 8) / s.width(), (height() - 8) / s.height());
    QSizeF d = s * k;
    return QRectF((width() - d.width()) / 2, (height() - d.height()) / 2, d.width(), d.height());
}
const Clip *ViewerWidget::overlayClip() const
{
    if (mon->isSource) return nullptr;
    auto q = PRJ->active();
    if (!q || PRJ->selClips.isEmpty() || mon->engine->playing()) return nullptr;
    for (int i = q->video.size() - 1; i >= 0; --i) {
        const Clip *c = q->video[i].clipAt(q->playhead);
        if (c && PRJ->selClips.contains(c->id) && c->effect("motion") && c->kind != MediaKind::Adjustment) return c;
    }
    return nullptr;
}
QTransform ViewerWidget::clipTransform(const Clip &c, const MediaItem *m, Frame kt, QSizeF &native) const
{
    auto q = PRJ->active();
    QRectF ir = imageRect();
    double k = ir.width() / q->w;
    native = QSizeF(q->w, q->h);
    if (m && m->w > 0 && (c.kind == MediaKind::Video || c.kind == MediaKind::Image)) native = QSizeF(m->w, m->h);
    const Effect *mo = c.effect("motion");
    double sx = mo->val("scale", kt, 100) / 100, sy = sx;
    if (mo->val("uniform", 0, 1) < .5) sx = mo->val("scaleW", kt, 100) / 100;
    QTransform t;
    t.translate(ir.left(), ir.top());
    t.scale(k, k);
    t.translate(mo->val("posX", kt), mo->val("posY", kt));
    t.rotate(mo->val("rot", kt));
    t.scale(sx, sy);
    t.translate(-mo->val("ancX", kt), -mo->val("ancY", kt));
    return t;
}
// A title's handles hug its text; every other clip's hug the full source frame.
QRectF ViewerWidget::contentRect(const Clip &c, const QSizeF &n) const
{
    if (c.kind != MediaKind::Text) return QRectF(QPointF(0, 0), n);
    return ImageFx::textBounds(c.text, int(n.width()), int(n.height())).adjusted(-8, -4, 8, 4);
}

void ViewerWidget::paintOverlay(QPainter &p)
{
    const Clip *c = overlayClip();
    if (!c) return;
    auto q = PRJ->active();
    QSizeF n;
    QTransform t = clipTransform(*c, PRJ->s.mediaById(c->mediaId), c->kt(q->playhead), n);
    QPolygonF box = t.map(QPolygonF(contentRect(*c, n)));
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(210, 210, 210), 1));
    p.drawPolygon(box);
    p.setBrush(Qt::white);
    p.setPen(QPen(Qt::black, 1));
    for (int i = 0; i < 4; ++i) {
        QPointF a = box[i], b = box[(i + 1) % 4];
        p.drawRect(QRectF(a - QPointF(4, 4), QSizeF(8, 8)));
        p.drawRect(QRectF((a + b) / 2 - QPointF(3, 3), QSizeF(6, 6)));
    }
    p.setBrush(Qt::NoBrush);
    const Effect *mo = c->effect("motion");
    QPointF anc = t.map(QPointF(mo->val("ancX", c->kt(q->playhead)), mo->val("ancY", c->kt(q->playhead))));
    p.setPen(QPen(Qt::white, 1));
    p.drawEllipse(anc, 5, 5);
    p.drawLine(anc - QPointF(8, 0), anc + QPointF(8, 0));
    p.drawLine(anc - QPointF(0, 8), anc + QPointF(0, 8));
}
void ViewerWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor("#0f0f0f"));
    QRectF r = imageRect();
    if (!img.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(r, img);
    } else p.fillRect(r, Qt::black);
    if (safeMargins) {
        p.setPen(QPen(QColor(255, 255, 255, 110), 1));
        p.drawRect(r.adjusted(r.width() * .05, r.height() * .05, -r.width() * .05, -r.height() * .05));
        p.drawRect(r.adjusted(r.width() * .1, r.height() * .1, -r.width() * .1, -r.height() * .1));
        QPointF c = r.center();
        p.drawLine(c - QPointF(12, 0), c + QPointF(12, 0));
        p.drawLine(c - QPointF(0, 12), c + QPointF(0, 12));
    }
    paintOverlay(p);
}
void ViewerWidget::setMotion(Clip &c, const QString &id, double v)
{
    Effect *mo = c.effect("motion");
    Param *pr = mo->find(id);
    if (pr->animated) pr->setKey(c.kt(PRJ->active()->playhead), v);
    else pr->value = v;
}
void ViewerWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    if (mon->isSource) { srcPress = e->pos(); srcDrag = mon->mediaId != 0; return; }
    if (EDIT->tool == Tool::Text) { emit mon->requestText(); EDIT->setTool(Tool::Select); return; }
    const Clip *c = overlayClip();
    auto q = PRJ->active();
    if (!q) return;
    QPointF pos = e->position();
    if (!c) { // click picks the top clip under the pointer
        for (int i = q->video.size() - 1; i >= 0; --i) {
            const Clip *k = q->video[i].clipAt(q->playhead);
            if (!k || !k->effect("motion") || q->video[i].muted) continue;
            QSizeF n;
            QTransform t = clipTransform(*k, PRJ->s.mediaById(k->mediaId), k->kt(q->playhead), n);
            if (t.map(QPolygonF(contentRect(*k, n))).containsPoint(pos, Qt::OddEvenFill)) { c = k; PRJ->setSelection(PRJ->withLinked({k->id})); break; }
        }
        if (!c) { PRJ->setSelection({}); return; }
    }
    Frame kt = c->kt(q->playhead);
    QSizeF n;
    QTransform t = clipTransform(*c, PRJ->s.mediaById(c->mediaId), kt, n);
    QPolygonF box = t.map(QPolygonF(contentRect(*c, n)));
    const Effect *mo = c->effect("motion");
    pressW = pos;
    p0x = mo->val("posX", kt); p0y = mo->val("posY", kt);
    s0 = mo->val("scale", kt, 100); r0 = mo->val("rot", kt);
    QPointF anc = t.map(QPointF(mo->val("ancX", kt), mo->val("ancY", kt)));
    dist0 = qMax(1.0, QLineF(anc, pos).length());
    ang0 = QLineF(anc, pos).angle();
    dragClip = c->id;
    drag = None;
    for (int i = 0; i < 4; ++i) {
        QPointF a = box[i], b = box[(i + 1) % 4];
        if (QLineF(a, pos).length() < 9 || QLineF((a + b) / 2, pos).length() < 8) drag = Scale;
    }
    if (drag == None) {
        if (box.containsPoint(pos, Qt::OddEvenFill)) drag = Move;
        else {
            double d = 1e9;
            for (auto &pt : box) d = qMin(d, QLineF(pt, pos).length());
            if (d < 40) drag = Rotate;
        }
    }
    if (drag != None) PRJ->liveBegin();
    update();
}
void ViewerWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (srcDrag && (e->buttons() & Qt::LeftButton) && (e->pos() - srcPress).manhattanLength() > 10) {
        srcDrag = false;
        auto *d = new QDrag(this);
        auto *md = new QMimeData;
        md->setData(kMimeMedia, QByteArray::number(mon->mediaId));
        d->setMimeData(md);
        d->exec(Qt::CopyAction);
        return;
    }
    if (drag == None) {
        const Clip *c = overlayClip();
        if (c && !mon->isSource) setCursor(Qt::SizeAllCursor); else unsetCursor();
        return;
    }
    auto q = PRJ->active();
    Clip *c = q ? q->clip(dragClip) : nullptr;
    if (!c) return;
    double k = imageRect().width() / q->w;
    QPointF pos = e->position();
    if (drag == Move) {
        setMotion(*c, "posX", p0x + (pos.x() - pressW.x()) / k);
        setMotion(*c, "posY", p0y + (pos.y() - pressW.y()) / k);
    } else {
        Frame kt = c->kt(q->playhead);
        QRectF ir = imageRect();
        QPointF anc(ir.left() + p0x * k, ir.top() + p0y * k);
        (void)kt;
        if (drag == Scale) setMotion(*c, "scale", qMax(0.0, s0 * QLineF(anc, pos).length() / qMax(1.0, QLineF(anc, pressW).length())));
        else setMotion(*c, "rot", r0 - (QLineF(anc, pos).angle() - QLineF(anc, pressW).angle()));
    }
    PRJ->liveTouch();
    update();
}
void ViewerWidget::mouseReleaseEvent(QMouseEvent *)
{
    srcDrag = false;
    if (drag != None) { drag = None; PRJ->liveCommit(tr("Transform Clip")); }
}

TimeBar::TimeBar(MonitorPanel *m) : QWidget(m), mon(m) { setFixedHeight(26); }
void TimeBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), bgDark);
    Frame len = qMax<Frame>(1, mon->length());
    double k = double(width() - 16) / len;
    auto X = [&](Frame f) { return 8 + int(f * k); };
    p.setPen(QColor("#555"));
    for (int i = 0; i <= 20; ++i) {
        int x = 8 + (width() - 16) * i / 20;
        p.drawLine(x, i % 5 == 0 ? 2 : 6, x, 10);
    }
    int ia = mon->inFrame(), ib = mon->outFrame();
    if (ia > 0 || ib < len) p.fillRect(QRect(X(ia), 12, X(ib) - X(ia), 10), QColor(45, 140, 235, 120));
    else p.fillRect(QRect(8, 16, width() - 16, 2), QColor("#3a3a3a"));
    for (Frame f : mon->markerFrames()) {
        p.setBrush(QColor("#6dba4a"));
        p.setPen(Qt::NoPen);
        p.drawRect(X(f) - 2, 12, 5, 8);
    }
    int x = X(mon->position());
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath hd;
    hd.moveTo(x - 6, 2); hd.lineTo(x + 6, 2); hd.lineTo(x + 6, 11); hd.lineTo(x, 18); hd.lineTo(x - 6, 11); hd.closeSubpath();
    p.fillPath(hd, accent);
    p.setPen(accent);
    p.drawLine(x, 18, x, height());
}
void TimeBar::scrub(int x)
{
    Frame len = qMax<Frame>(1, mon->length());
    mon->setPos(qBound<Frame>(0, Frame((x - 8) * double(len) / qMax(1, width() - 16)), len - 1));
}
void TimeBar::mousePressEvent(QMouseEvent *e) { scrub(e->pos().x()); }
void TimeBar::mouseMoveEvent(QMouseEvent *e) { scrub(e->pos().x()); }

MonitorPanel::MonitorPanel(bool src, QWidget *p)
    : Panel(src ? "source" : "program", src ? tr("Source: (no clips)") : tr("Program: (no sequence)"), p), isSource(src)
{
    engine = new PlaybackEngine(this);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    viewer = new ViewerWidget(this);
    v->addWidget(viewer, 1);

    auto *row = new QWidget(this);
    row->setFixedHeight(26);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(8, 0, 8, 0);
    h->setSpacing(8);
    tc = new TimecodeLabel(row, 14);
    h->addWidget(tc);
    h->addStretch(1);
    zoomBox = new QComboBox(row);
    zoomBox->addItems({"Fit", "25%", "50%", "75%", "100%", "150%", "200%"});
    zoomBox->setFocusPolicy(Qt::NoFocus);
    resBox = new QComboBox(row);
    resBox->addItems({"Full", "1/2", "1/4", "1/8"});
    resBox->setCurrentIndex(1);
    resBox->setFocusPolicy(Qt::NoFocus);
    resBox->setToolTip(tr("Select Playback Resolution"));
    h->addWidget(zoomBox);
    h->addStretch(1);
    dropLabel = new QLabel(row);
    dropLabel->setStyleSheet("color:#d0a040;font-size:10px;");
    h->addWidget(dropLabel);
    h->addWidget(resBox);
    auto *wr = iconButton("wrench", tr("Settings"), row);
    h->addWidget(wr);
    durLabel = new QLabel("00:00:00:00", row);
    durLabel->setStyleSheet("color:#8a8a8a; font-family:Consolas; font-size:13px;");
    h->addWidget(durLabel);
    v->addWidget(row);
    bar = new TimeBar(this);
    v->addWidget(bar);
    auto *btns = new QWidget(this);
    btns->setFixedHeight(34);
    auto *bh = new QHBoxLayout(btns);
    bh->setContentsMargins(6, 2, 6, 4);
    bh->setSpacing(4);
    addTransport(bh);
    v->addWidget(btns);

    connect(wr, &QToolButton::clicked, this, [this, wr] {
        QMenu m;
        fillMenu(&m);
        m.exec(wr->mapToGlobal(QPoint(0, wr->height())));
    });
    connect(zoomBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        static const double z[] = {0, .25, .5, .75, 1, 1.5, 2};
        viewer->zoom = z[i];
        viewer->update();
    });
    connect(resBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        static const double z[] = {1, .5, .25, .125};
        engine->setScale(z[i]);
    });
    connect(tc, &TimecodeLabel::edited, this, [this](Frame f) { setPos(f); });
    connect(engine, &PlaybackEngine::frameReady, this, [this](const QImage &i, Frame) { viewer->setImage(i); });
    connect(engine, &PlaybackEngine::positionChanged, this, [this](Frame f) {
        if (isSource) srcPos = f;
        else { fromEngine = true; PRJ->setPlayhead(f); fromEngine = false; }
        refreshLabels();
    });
    connect(engine, &PlaybackEngine::stopped, this, [this] {
        playBtn->setIcon(Theme::icon("play"));
        viewer->update();
    });
    if (!isSource) {
        syncTimer.setSingleShot(true);
        syncTimer.setInterval(0);
        connect(&syncTimer, &QTimer::timeout, this, &MonitorPanel::syncProgram);
        connect(PRJ, &Project::changed, &syncTimer, qOverload<>(&QTimer::start));
        connect(PRJ, &Project::activeSeqChanged, &syncTimer, qOverload<>(&QTimer::start));
        connect(PRJ, &Project::selectionChanged, viewer, qOverload<>(&QWidget::update));
        connect(CACHE, &MediaCache::audioReady, &syncTimer, qOverload<>(&QTimer::start));
        connect(PRJ, &Project::playheadChanged, this, [this](Frame f) {
            if (!fromEngine) engine->seek(f);
            refreshLabels();
            viewer->update();
        });
    } else {
        connect(CACHE, &MediaCache::audioReady, this, [this] { refreshLabels(); });
    }
    refreshLabels();
}

void MonitorPanel::addTransport(QHBoxLayout *h)
{
    auto add = [&](const QString &ic, const QString &tip, auto fn, int size = 26) {
        auto *b = iconButton(ic, tip, this, size);
        connect(b, &QToolButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    h->addStretch(1);
    if (!isSource) add("marker", tr("Add Marker (M)"), [this] {
        auto q = PRJ->active(); if (!q) return;
        Frame f = q->playhead;
        PRJ->edit(tr("Add Marker"), [&](ProjectState &st) { Marker m; m.t = f; m.color = 13; st.seqById(st.activeSeq)->markers << m; });
    });
    add("markin", tr("Mark In (I)"), [this] { markIn(); });
    add("markout", tr("Mark Out (O)"), [this] { markOut(); });
    h->addSpacing(8);
    add("gotoin", tr("Go to In (Shift+I)"), [this] { gotoIn(); });
    add("stepback", tr("Step Back 1 Frame (Left)"), [this] { step(-1); });
    playBtn = add("play", tr("Play-Stop Toggle (Space)"), [this] { togglePlay(); }, 30);
    add("stepfwd", tr("Step Forward 1 Frame (Right)"), [this] { step(1); });
    add("gotoout", tr("Go to Out (Shift+O)"), [this] { gotoOut(); });
    h->addSpacing(8);
    if (isSource) {
        add("insert", tr("Insert (,)"), [this] { sourceEdit(true); });
        add("overwrite", tr("Overwrite (.)"), [this] { sourceEdit(false); });
    } else {
        add("lift", tr("Lift (;)"), [] { PRJ->edit(tr("Lift"), [](ProjectState &st) { auto *q = st.seqById(st.activeSeq); if (q && q->inPt >= 0 && q->outPt > q->inPt) { Ops::lift(st, *q, q->inPt, q->outPt); q->inPt = q->outPt = -1; } }); });
        add("extract", tr("Extract (')"), [] { PRJ->edit(tr("Extract"), [](ProjectState &st) { auto *q = st.seqById(st.activeSeq); if (q && q->inPt >= 0 && q->outPt > q->inPt) { Ops::extract(st, *q, q->inPt, q->outPt); q->playhead = q->inPt; q->inPt = q->outPt = -1; } }); });
    }
    add("camera", tr("Export Frame (Ctrl+Shift+E)"), [this] { exportFrame(); });
    h->addSpacing(8);
    loopBtn = iconButton("loop", tr("Loop"), this, 26, true);
    h->addWidget(loopBtn);
    connect(loopBtn, &QToolButton::toggled, this, [this](bool on) {
        Frame a = inFrame(), b = outFrame();
        engine->setRange(on ? a : 0, on ? b : -1, on);
    });
    auto *safe = iconButton("safe", tr("Safe Margins"), this, 26, true);
    h->addWidget(safe);
    connect(safe, &QToolButton::toggled, this, [this](bool on) { viewer->safeMargins = on; viewer->update(); });
    h->addStretch(1);
}

void MonitorPanel::fillMenu(QMenu *m)
{
    auto *s = m->addAction(tr("Safe Margins"), [this] { viewer->safeMargins = !viewer->safeMargins; viewer->update(); });
    s->setCheckable(true); s->setChecked(viewer->safeMargins);
    m->addAction(tr("Clear In and Out"), [this] { clearInOut(); });
}

void MonitorPanel::syncProgram()
{
    auto q = PRJ->active();
    engine->setInput(PRJ->s, PRJ->s.activeSeq);
    QString t = q ? tr("Program: %1").arg(q->name) : tr("Program: (no sequence)");
    if (t != title) { title = t; if (parentWidget()) parentWidget()->parentWidget()->update(); }
    if (q) {
        viewer->setFrameSize(QSize(q->w, q->h));
        if (!engine->playing()) engine->seek(q->playhead);
    } else viewer->setImage({});
    refreshLabels();
}

void MonitorPanel::setMedia(int id)
{
    auto *m = PRJ->s.mediaById(id);
    if (!m) return;
    engine->pause();
    mediaId = id;
    ProjectState st = PRJ->s;
    Sequence q;
    q.id = -1;
    double r = m->fps > 0 ? m->fps : 30;
    q.fpsNum = qRound(r * 1000); q.fpsDen = 1000;
    if (m->kind == MediaKind::Nested) {
        if (auto *inner = st.seqById(m->seqId)) {
            q.w = inner->w; q.h = inner->h; q.fpsNum = inner->fpsNum; q.fpsDen = inner->fpsDen;
            st.mediaById(id)->duration = inner->duration() / inner->fps();
        }
    } else if (m->w > 0) { q.w = m->w; q.h = m->h; }
    Ops::addTrack(st, q, TrackType::Video);
    Ops::addTrack(st, q, TrackType::Audio);
    const MediaItem &mi = *st.mediaById(id);
    Ops::overwriteMedia(st, q, mi, 0, mi.duration > 0 ? mi.duration : 5, 0, Ops::Patch{0, mi.hasAudio ? 0 : -1});
    if (q.duration() == 0) return;
    st.seqs << q;
    st.activeSeq = -1;
    srcSeq = q;
    srcPos = 0;
    engine->setInput(st, -1);
    engine->seek(0);
    viewer->setFrameSize(QSize(q.w, q.h));
    title = tr("Source: %1").arg(m->name);
    if (parentWidget()) parentWidget()->parentWidget()->update();
    if (m->hasAudio && !m->path.isEmpty()) CACHE->ensureAudio(m->path);
    refreshLabels();
    bar->update();
}

Frame MonitorPanel::length() const
{
    if (isSource) return srcSeq.duration();
    auto q = PRJ->active();
    return q ? q->duration() : 0;
}
double MonitorPanel::fps() const
{
    if (isSource) return srcSeq.fps();
    auto q = PRJ->active();
    return q ? q->fps() : 30;
}
bool MonitorPanel::dropFrame() const { auto q = PRJ->active(); return !isSource && q && q->dropFrame; }
Frame MonitorPanel::position() const
{
    if (isSource) return srcPos;
    auto q = PRJ->active();
    return q ? q->playhead : 0;
}
Frame MonitorPanel::inFrame() const
{
    if (isSource) { auto *m = PRJ->s.mediaById(mediaId); return m && m->inSec >= 0 ? Frame(m->inSec * fps() + .5) : 0; }
    auto q = PRJ->active();
    return q && q->inPt >= 0 ? q->inPt : 0;
}
Frame MonitorPanel::outFrame() const
{
    if (isSource) { auto *m = PRJ->s.mediaById(mediaId); return m && m->outSec >= 0 ? Frame(m->outSec * fps() + .5) : length(); }
    auto q = PRJ->active();
    return q && q->outPt >= 0 ? q->outPt : length();
}
QVector<Frame> MonitorPanel::markerFrames() const
{
    QVector<Frame> r;
    if (!isSource) if (auto q = PRJ->active()) for (auto &m : q->markers) r << m.t;
    return r;
}
void MonitorPanel::setPos(Frame f)
{
    f = qMax<Frame>(0, f);
    if (isSource) { srcPos = f; engine->seek(f); refreshLabels(); }
    else { PRJ->setPlayhead(f); if (auto q = PRJ->active()) if (q->playhead != engine->position()) engine->seek(f); }
}
void MonitorPanel::refreshLabels()
{
    tc->setFrame(position(), fps(), dropFrame());
    Frame in = inFrame(), out = outFrame();
    durLabel->setText(framesToTimecode(qMax<Frame>(0, out - in), fps(), dropFrame()));
    dropLabel->setText(engine->playing() && engine->droppedRatio() > .02 ? tr("%1% dropped").arg(int(engine->droppedRatio() * 100)) : QString());
    bar->update();
}
void MonitorPanel::togglePlay()
{
    if (engine->playing()) { engine->pause(); return; }
    if (length() <= 0) return;
    engine->play(1);
    playBtn->setIcon(Theme::icon("pause"));
}
void MonitorPanel::shuttle(int dir)
{
    if (dir == 0) { engine->pause(); return; }
    if (length() <= 0) return;
    double s = engine->playing() ? engine->speed() : 0;
    if ((dir > 0) != (s > 0) || s == 0) s = dir;
    else s = qBound(-8.0, s * 2, 8.0);
    engine->play(s);
    playBtn->setIcon(Theme::icon("pause"));
}
void MonitorPanel::step(int n)
{
    engine->pause();
    setPos(position() + n);
}
void MonitorPanel::setInOut(Frame in, Frame out)
{
    if (isSource) {
        if (auto *m = PRJ->s.mediaById(mediaId)) { m->inSec = in >= 0 ? in / fps() : -1; m->outSec = out >= 0 ? out / fps() : -1; }
    } else if (PRJ->active()) {
        PRJ->edit(tr("Mark In/Out"), [&](ProjectState &st) { auto *q = st.seqById(st.activeSeq); q->inPt = in; q->outPt = out; });
    }
    refreshLabels();
}
void MonitorPanel::markIn()
{
    Frame o = outFrame();
    bool hadOut = isSource ? (PRJ->s.mediaById(mediaId) && PRJ->s.mediaById(mediaId)->outSec >= 0) : (PRJ->active() && PRJ->active()->outPt >= 0);
    Frame p = position();
    setInOut(p, hadOut && o > p ? o : -1);
}
void MonitorPanel::markOut()
{
    Frame p = position() + 1;
    bool hadIn = isSource ? (PRJ->s.mediaById(mediaId) && PRJ->s.mediaById(mediaId)->inSec >= 0) : (PRJ->active() && PRJ->active()->inPt >= 0);
    Frame i = inFrame();
    setInOut(hadIn && i < p ? i : -1, p);
}
void MonitorPanel::clearInOut() { setInOut(-1, -1); }
void MonitorPanel::gotoIn() { engine->pause(); setPos(inFrame()); }
void MonitorPanel::gotoOut() { engine->pause(); setPos(qMax<Frame>(0, outFrame() - 1)); }

void MonitorPanel::sourceEdit(bool insert)
{
    auto *m = PRJ->s.mediaById(mediaId);
    if (!isSource || !m) return;
    MediaItem mi = *m;
    double r = fps();
    double in = inFrame() / r, out = outFrame() / r;
    mi.duration = qMax(mi.duration, length() / r);
    if (!PRJ->active()) {
        int w = mi.w > 0 ? mi.w : 1920, h = mi.h > 0 ? mi.h : 1080;
        PRJ->createSequence(mi.name, w, h, qRound(r * 1000), 1000);
    }
    Frame at = PRJ->active()->playhead;
    auto *q0 = PRJ->active();
    if (q0->inPt >= 0) at = q0->inPt;
    PRJ->edit(insert ? tr("Insert") : tr("Overwrite"), [&](ProjectState &st) {
        Sequence &q = *st.seqById(st.activeSeq);
        Ops::Patch p{mi.kind != MediaKind::Audio ? EDIT->patchV : -1, mi.hasAudio ? EDIT->patchA : -1};
        if (insert) Ops::insertMedia(st, q, mi, in, out, at, p);
        else Ops::overwriteMedia(st, q, mi, in, out, at, p);
    });
    PRJ->setPlayhead(at + qRound64((out - in) * PRJ->active()->fps()));
    if (mi.hasAudio && !mi.path.isEmpty()) CACHE->ensureAudio(mi.path);
}

void MonitorPanel::exportFrame()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export Frame"), tr("Frame.png"), tr("PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif)"));
    if (path.isEmpty()) return;
    Compositor c;
    QImage img;
    if (isSource) {
        ProjectState st = PRJ->s;
        st.seqs << srcSeq;
        img = c.render(st, -1, srcPos, 1.0, true);
    } else if (auto q = PRJ->active()) img = c.render(PRJ->s, q->id, q->playhead, 1.0, true);
    if (!img.isNull()) img.convertToFormat(QImage::Format_RGB32).save(path);
    c.releaseAll();
}
