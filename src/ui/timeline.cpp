#include "timeline.h"
#include "../core/effectdefs.h"
#include "../core/mediacache.h"
#include "../core/project.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <cmath>

using namespace Theme;

static const QColor kSnap("#f4d03f");
static QColor clipColor(const Clip &c)
{
    int l = c.label >= 0 ? c.label : defaultLabel(c.kind);
    return labelColor(l);
}

TimelineView::TimelineView(QWidget *p) : QWidget(p)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(120);
    connect(PRJ, &Project::changed, this, [this] { emit viewChanged(); update(); });
    connect(PRJ, &Project::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(PRJ, &Project::playheadChanged, this, [this] {
        double before = scrollF;
        ensurePlayheadVisible();
        auto q = PRJ->active();
        if (scrollF != before || !q) { update(); return; }
        update(QRect(lastPlayX - 8, 0, 16, height()));
        update(QRect(xFromFrame(q->playhead) - 8, 0, 16, height()));
    });
    connect(CACHE, &MediaCache::audioReady, this, qOverload<>(&QWidget::update));
    connect(CACHE, &MediaCache::thumbReady, this, qOverload<>(&QWidget::update));
    connect(CACHE, &MediaCache::proxyReady, this, qOverload<>(&QWidget::update));
    connect(EDIT, &EditState::toolChanged, this, [this] { updateCursor(mapFromGlobal(QCursor::pos()), {}); });
}

Frame TimelineView::totalFrames() const
{
    auto q = PRJ->active();
    return q ? qMax<Frame>(q->duration(), Frame(q->fps() * 30)) : 900;
}
void TimelineView::setZoom(double np, double anchor)
{
    double minP = visibleWidth() / double(qMax<Frame>(1, totalFrames()) * 1.1);
    np = qBound(qMin(minP, .01), np, 40.0);
    double ax = (anchor - scrollF) * ppf;
    ppf = np;
    scrollF = qMax(0.0, anchor - ax / ppf);
    emit viewChanged();
    update();
}
void TimelineView::zoomFit()
{
    auto q = PRJ->active();
    if (!q) return;
    Frame d = qMax<Frame>(q->duration(), 30);
    ppf = qMax(.005, visibleWidth() * .95 / d);
    scrollF = 0;
    emit viewChanged();
    update();
}
void TimelineView::ensurePlayheadVisible()
{
    auto q = PRJ->active();
    if (!q || mode == Mode::Scrub) return;
    int x = xFromFrame(q->playhead);
    if (x > width() - 12 || x < kHeader) {
        scrollF = qMax(0.0, double(q->playhead) - 8 / ppf);
        emit viewChanged();
    }
}

int TimelineView::dividerY() const { return kRuler + int((height() - kRuler) * dividerRatio); }
QRect TimelineView::trackRect(bool video, int i) const
{
    auto q = PRJ->active();
    if (!q) return {};
    if (video) {
        int y = dividerY() + vScroll;
        for (int k = 0; k <= i; ++k) y -= q->video[k].height + 1;
        return QRect(0, y, width(), q->video[i].height);
    }
    int y = dividerY() + 3 + aScroll;
    for (int k = 0; k < i; ++k) y += q->audio[k].height + 1;
    return QRect(0, y, width(), q->audio[i].height);
}
TimelineView::HitT TimelineView::trackAt(int y) const
{
    HitT h;
    auto q = PRJ->active();
    if (!q || y < kRuler) return h;
    bool video = y < dividerY();
    auto &list = video ? q->video : q->audio;
    for (int i = 0; i < list.size(); ++i) {
        QRect r = trackRect(video, i);
        if (y >= r.top() && y <= r.bottom() + 1) { h.t = &list[i]; h.idx = i; h.video = video; h.r = r; return h; }
    }
    h.video = video;
    return h;
}
const Clip *TimelineView::clipAtPos(const HitT &h, int x, Edge *edge) const
{
    if (!h.t) return nullptr;
    const int grab = 7;
    const Clip *found = nullptr;
    for (auto &c : h.t->clips) {
        int x0 = xFromFrame(c.start), x1 = xFromFrame(c.end());
        if (x >= x0 - 2 && x <= x1 + 2) {
            found = &c;
            if (edge) {
                int g = qMin(grab, qMax(2, (x1 - x0) / 3));
                *edge = x < x0 + g ? Edge::Left : x > x1 - g ? Edge::Right : Edge::None;
            }
            if (x >= x0 && x <= x1) break;
        }
    }
    return found;
}
const Transition *TimelineView::transitionAt(const HitT &h, int x) const
{
    if (!h.t) return nullptr;
    for (auto &t : h.t->transitions) {
        int x0 = xFromFrame(t.start()), x1 = xFromFrame(t.end());
        if (x >= x0 && x <= x1) return &t;
    }
    return nullptr;
}

Frame TimelineView::snap(Frame f, const QSet<int> &exclude, bool *did) const
{
    if (did) *did = false;
    auto q = PRJ->active();
    bool on = PRJ->snapping != bool(QApplication::keyboardModifiers() & Qt::ShiftModifier && false);
    if (!q || !on) return f;
    double best = 9.0 / ppf;
    Frame out = f;
    auto consider = [&](Frame e) {
        double d = std::fabs(double(e - f));
        if (d < best) { best = d; out = e; if (did) *did = true; }
    };
    if (mode != Mode::Scrub) consider(q->playhead);
    consider(0);
    if (q->inPt >= 0) consider(q->inPt);
    if (q->outPt >= 0) consider(q->outPt);
    for (auto &m : q->markers) consider(m.t);
    for (auto *l : {&q->video, &q->audio})
        for (auto &t : *l)
            for (auto &c : t.clips) {
                if (exclude.contains(c.id)) continue;
                consider(c.start);
                consider(c.end());
            }
    return out;
}

const Param *TimelineView::bandParam(const Clip &c) const
{
    if (c.kind == MediaKind::Audio) { auto e = c.effect("volume"); return e ? e->find("level") : nullptr; }
    auto e = c.effect("opacity");
    return e ? e->find("opacity") : nullptr;
}
Param *TimelineView::bandParam(Clip &c)
{
    // Non-const path on purpose: it must detach shared data, or edits leak into undo snapshots.
    auto e = c.effect(c.kind == MediaKind::Audio ? "volume" : "opacity");
    return e ? e->find(c.kind == MediaKind::Audio ? "level" : "opacity") : nullptr;
}
int TimelineView::autoY(const QRect &r, double db) const { return r.top() + int((15 - qBound(-60.0, db, 15.0)) / 75 * r.height()); }
int TimelineView::bandY(const Clip &c, const QRect &r, double v) const
{
    double u = c.kind == MediaKind::Audio ? (15 - v) / 75.0 : 1 - v / 100.0;
    int top = r.top() + 16, bot = r.bottom() - 2;
    return top + int(qBound(0.0, u, 1.0) * (bot - top));
}
double TimelineView::bandValue(const Clip &c, const QRect &r, int y) const
{
    int top = r.top() + 16, bot = r.bottom() - 2;
    double u = qBound(0.0, double(y - top) / qMax(1, bot - top), 1.0);
    return c.kind == MediaKind::Audio ? 15 - u * 75 : (1 - u) * 100;
}

// ------------------------------------------------------------------ painting
void TimelineView::paintRuler(QPainter &p)
{
    auto q = PRJ->active();
    QRect r(kHeader, 0, width() - kHeader, kRuler);
    p.fillRect(r, bgDark);
    p.setPen(QColor("#111"));
    p.drawLine(0, kRuler - 1, width(), kRuler - 1);
    if (!q) return;
    // Caption lane under the ruler
    {
        p.save();
        p.setClipRect(r);
        QFont cf = font(); cf.setPixelSize(9); p.setFont(cf);
        for (auto &c : q->captions) {
            int a = xFromFrame(c.start), b = xFromFrame(c.end);
            if (b < kHeader || a > width()) continue;
            QRect cr(a, kTicks + 1, qMax(2, b - a - 1), kRuler - kTicks - 3);
            p.fillRect(cr, QColor("#7a5a1f"));
            p.setPen(QColor("#f4e3bd"));
            p.drawText(cr.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, c.text.simplified());
        }
        p.restore();
    }
    p.setClipRect(r);
    double fps = q->fps();
    int nominal = qRound(fps);
    static const int secs[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
    Frame major = nominal;
    if (ppf * nominal >= 200 && nominal >= 2) {
        for (int d : {1, 2, 5, 10, 15}) { if (d < nominal && ppf * d >= 80) { major = d; break; } }
    } else {
        for (int s : secs) { major = Frame(s) * nominal; if (ppf * major >= 80) break; }
    }
    Frame minor = qMax<Frame>(1, major / (major % 10 == 0 || major % nominal == 0 ? 5 : 4));
    Frame first = Frame(scrollF) / major * major;
    p.setPen(QColor("#6a6a6a"));
    for (Frame f = first; xFromFrame(f) < width(); f += minor) {
        int x = xFromFrame(f);
        bool mj = f % major == 0;
        p.drawLine(x, kTicks - (mj ? 11 : 5), x, kTicks - 2);
    }
    p.setPen(textDim);
    QFont ft = font(); ft.setPixelSize(10); p.setFont(ft);
    for (Frame f = first; xFromFrame(f) < width(); f += major)
        p.drawText(xFromFrame(f) + 3, 12, framesToTimecode(f, fps, q->dropFrame));
    // work area / in-out
    if (q->inPt >= 0 || q->outPt >= 0) {
        int a = xFromFrame(qMax<Frame>(0, q->inPt)), b = q->outPt >= 0 ? xFromFrame(q->outPt) : width();
        p.fillRect(QRect(a, kTicks - 8, b - a, 7), QColor(45, 140, 235, 110));
    }
    for (auto &m : q->markers) {
        int x = xFromFrame(m.t);
        QPainterPath path; path.moveTo(x - 5, 14); path.lineTo(x + 5, 14); path.lineTo(x + 5, 20); path.lineTo(x, 25); path.lineTo(x - 5, 20); path.closeSubpath();
        if (m.dur > 0) p.fillRect(QRect(x, 14, int(m.dur * ppf), 6), labelColor(m.color).darker(160));
        p.fillPath(path, labelColor(m.color));
    }
    p.setClipping(false);
}

void TimelineView::paintThumbs(QPainter &p, const Clip &c, const QRect &r)
{
    auto *m = PRJ->s.mediaById(c.mediaId);
    if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image) || r.height() < 36) return;
    int h = r.height() - 16, w = h * 16 / 9;
    QRect area(r.x(), r.y() + 15, r.width(), h);
    double tl = c.srcIn, tr = c.srcOut(PRJ->active()->fps());
    auto draw = [&](int x, double t) {
        QImage img = CACHE->thumbnail(m->path, t, 160, m->kind == MediaKind::Image);
        if (!img.isNull()) p.drawImage(QRect(x, area.y(), w, h), img);
    };
    p.save();
    p.setClipRect(area.intersected(rect()));
    if (r.width() > w * 2 + 6) {
        // head and tail frames, more when the clip is long enough, like a filmstrip
        int n = qMin(12, r.width() / w);
        if (n >= 4) {
            for (int i = 0; i < n; ++i) {
                int x = r.x() + i * w;
                if (x + w < 0 || x > width()) continue;
                draw(x, tl + (tr - tl) * double(i) / n);
            }
        } else {
            draw(r.x(), tl);
            draw(r.right() - w, qMax(tl, tr - .1));
        }
    } else if (r.width() > 30) draw(r.x(), tl);
    p.restore();
}

void TimelineView::paintWave(QPainter &p, const Clip &c, const QRect &r)
{
    auto *m = PRJ->s.mediaById(c.mediaId);
    if (!m || m->path.isEmpty()) return;
    auto pk = CACHE->peaks(m->path);
    if (!pk) { CACHE->ensureAudio(m->path); return; }
    QRect area = r.adjusted(0, 14, 0, -1);
    if (area.height() < 6) return;
    double fps = PRJ->active()->fps();
    const auto *b = reinterpret_cast<const qint8 *>(pk->bytes.constData());
    const qint64 n = pk->count();
    const double perSec = double(MediaCache::kRate) / PeakData::kSamplesPerPeak;
    int x0 = qMax(area.left(), kHeader), x1 = qMin(area.right(), width() - 1);
    const double secPerPx = c.speed / (fps * ppf);
    const double xStartSec = c.srcIn;
    const bool stereo = area.height() >= 40;
    QColor col = clipColor(c).lighter(150);
    p.setPen(col);
    auto lane = [&](int ch, int top, int ht) {
        int mid = top + ht / 2;
        for (int x = x0; x <= x1; ++x) {
            double t0 = xStartSec + (x - r.left()) * secPerPx, t1 = t0 + secPerPx;
            qint64 a = qBound<qint64>(0, qint64(t0 * perSec), n - 1), z = qBound<qint64>(a + 1, qint64(t1 * perSec), n);
            z = qMin(z, a + 64);
            int mn = 0, mx = 0;
            for (qint64 i = a; i < z; ++i) {
                if (ch == 2) { mn = qMin(mn, qMin<int>(b[i * 4], b[i * 4 + 2])); mx = qMax(mx, qMax<int>(b[i * 4 + 1], b[i * 4 + 3])); }
                else { mn = qMin<int>(mn, b[i * 4 + ch * 2]); mx = qMax<int>(mx, b[i * 4 + ch * 2 + 1]); }
            }
            p.drawLine(x, mid - mx * ht / 256, x, mid - mn * ht / 256);
        }
    };
    if (stereo) { lane(0, area.top(), area.height() / 2); lane(1, area.top() + area.height() / 2, area.height() / 2); }
    else lane(2, area.top(), area.height());
}

void TimelineView::paintClip(QPainter &p, const Clip &c, const Track &t, const QRect &r)
{
    bool sel = PRJ->selClips.contains(c.id);
    QColor base = clipColor(c);
    QColor fill = c.kind == MediaKind::Audio ? base.darker(230) : base.darker(170);
    if (!c.enabled) fill = QColor("#3c3c3c");
    p.fillRect(r, fill);
    QRect head(r.x(), r.y(), r.width(), qMin(14, r.height()));
    p.fillRect(head, c.enabled ? base.darker(115) : QColor("#555"));
    p.save();
    p.setClipRect(r.intersected(QRect(kHeader, 0, width() - kHeader, height())));
    if (c.enabled) {
        if (c.kind == MediaKind::Audio) paintWave(p, c, r);
        else paintThumbs(p, c, r);
    }
    int fxCount = 0;
    for (auto &e : c.fx) if (e.type != "motion" && e.type != "opacity" && e.type != "volume" && e.type != "panner") ++fxCount;
    int tx = qMax(r.x(), kHeader) + 3;
    if (fxCount) {
        QRect b(tx, r.y() + 1, 13, 12);
        p.fillRect(b, QColor("#d8a83a"));
        QFont f = font(); f.setPixelSize(9); f.setBold(true); p.setFont(f);
        p.setPen(Qt::black);
        p.drawText(b, Qt::AlignCenter, "fx");
        tx += 16;
    }
    QFont f = font(); f.setPixelSize(10); p.setFont(f);
    p.setPen(c.enabled ? QColor(10, 10, 10) : QColor("#999"));
    QString label = c.name;
    if (!qFuzzyCompare(c.speed, 1.0) || c.reverse) label += QString(" [%1%]").arg((c.reverse ? -100 : 100) * c.speed, 0, 'f', 0);
    p.drawText(QRect(tx, r.y(), r.right() - tx - 2, 14), Qt::AlignVCenter | Qt::AlignLeft, label);
    // rubber band: on expanded tracks, or whenever the Pen is out
    if (c.enabled && r.height() >= 34 && (r.height() >= 60 || EDIT->tool == Tool::Pen)) {
        if (auto *bp = bandParam(c)) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(255, 255, 255, 190), 1));
            if (bp->animated && !bp->keys.isEmpty()) {
                QPolygonF poly;
                poly << QPointF(r.left(), bandY(c, r, bp->keys.first().v));
                for (auto &k : bp->keys) poly << QPointF(xFromFrame(k.t - c.kfOffset + c.start), bandY(c, r, k.v));
                poly << QPointF(r.right(), bandY(c, r, bp->keys.last().v));
                p.drawPolyline(poly);
                p.setBrush(Qt::white);
                for (auto &k : bp->keys) {
                    QPointF pt(xFromFrame(k.t - c.kfOffset + c.start), bandY(c, r, k.v));
                    QPolygonF d; d << pt + QPointF(0, -4) << pt + QPointF(4, 0) << pt + QPointF(0, 4) << pt + QPointF(-4, 0);
                    p.drawPolygon(d);
                }
                p.setBrush(Qt::NoBrush);
            } else {
                int y = bandY(c, r, bp->value);
                p.drawLine(r.left(), y, r.right(), y);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    }
    p.restore();
    p.setPen(QColor(0, 0, 0, 200));
    p.drawRect(r.adjusted(0, 0, -1, -1));
    if (sel) {
        p.setPen(QPen(selection, 1));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        p.drawRect(r.adjusted(1, 1, -2, -2));
    }
    (void)t;
}

void TimelineView::paintTrack(QPainter &p, const Track &t, const QRect &r)
{
    p.fillRect(QRect(kHeader, r.y(), width() - kHeader, r.height()), t.target ? QColor("#2b2b2b") : trackBg);
    for (auto &c : t.clips) {
        int x0 = xFromFrame(c.start), x1 = xFromFrame(c.end());
        if (x1 < kHeader || x0 > width()) continue;
        paintClip(p, c, t, QRect(x0, r.y(), qMax(2, x1 - x0), r.height()));
    }
    for (auto &x : t.transitions) {
        int a = xFromFrame(x.start()), b = xFromFrame(x.end());
        if (b < kHeader || a > width()) continue;
        QRect tr(a, r.y(), qMax(4, b - a), qMin(r.height(), 22));
        bool sel = PRJ->selTransition.first == t.id && PRJ->selTransition.second == x.cut;
        p.fillRect(tr, sel ? QColor("#6db3f2") : QColor("#cfcfcf"));
        p.setPen(QColor("#555"));
        p.drawLine(tr.bottomLeft(), tr.topRight());
        p.setPen(Qt::black);
        p.drawRect(tr.adjusted(0, 0, -1, -1));
    }
    if (t.showAuto && t.type == TrackType::Audio) {
        p.save();
        p.setClipRect(QRect(kHeader, r.y(), width() - kHeader, r.height()));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#f4d03f"), 1.4));
        const Param &au = t.volAuto;
        if (au.animated && !au.keys.isEmpty()) {
            QPolygonF poly;
            poly << QPointF(kHeader, autoY(r, au.keys.first().v));
            for (auto &k : au.keys) poly << QPointF(xFromFrame(k.t), autoY(r, k.v));
            poly << QPointF(width(), autoY(r, au.keys.last().v));
            p.drawPolyline(poly);
            p.setBrush(QColor("#f4d03f"));
            for (auto &k : au.keys) {
                QPointF pt(xFromFrame(k.t), autoY(r, k.v));
                QPolygonF d; d << pt + QPointF(0, -4) << pt + QPointF(4, 0) << pt + QPointF(0, 4) << pt + QPointF(-4, 0);
                p.drawPolygon(d);
            }
        } else p.drawLine(kHeader, autoY(r, t.volumeDb), width(), autoY(r, t.volumeDb));
        p.restore();
    }
    if (t.locked) {
        p.save();
        p.setClipRect(QRect(kHeader, r.y(), width() - kHeader, r.height()));
        p.setPen(QPen(QColor(255, 255, 255, 26), 1));
        for (int x = kHeader - r.height(); x < width(); x += 10) p.drawLine(x, r.bottom(), x + r.height(), r.top());
        p.restore();
    }
}

void TimelineView::paintHeaders(QPainter &p, bool video)
{
    auto q = PRJ->active();
    if (!q) return;
    auto &list = video ? q->video : q->audio;
    QFont f = font(); f.setPixelSize(11);
    for (int i = 0; i < list.size(); ++i) {
        const Track &t = list[i];
        QRect r = trackRect(video, i);
        QRect hr(0, r.y(), kHeader, r.height());
        p.fillRect(hr, bgLight);
        // source patch column
        bool patched = (video ? EDIT->patchV : EDIT->patchA) == i;
        QRect pc(3, r.y() + 2, 24, qMin(r.height() - 4, 18));
        p.setFont(f);
        if (patched) { p.fillRect(pc, accent); p.setPen(Qt::white); }
        else { p.fillRect(pc, QColor("#3a3a3a")); p.setPen(textDim); }
        p.drawText(pc, Qt::AlignCenter, QString("%1%2").arg(video ? "V" : "A").arg(i + 1));
        // target
        QRect tc(30, r.y() + 2, 24, qMin(r.height() - 4, 18));
        p.fillRect(tc, t.target ? QColor("#d8d8d8") : QColor("#444"));
        p.setPen(t.target ? QColor("#111") : textDim);
        p.drawText(tc, Qt::AlignCenter, QString("%1%2").arg(video ? "V" : "A").arg(i + 1));
        int x = 60;
        auto btn = [&](const QString &ic, bool on, const QColor &oc) {
            QRect b(x, r.y() + 3, 16, 16);
            p.drawPixmap(b, Theme::pixmap(ic, 16, on ? oc : QColor("#9a9a9a")));
            x += 20;
        };
        auto txtBtn = [&](const QString &s, bool on, const QColor &oc) {
            QRect b(x, r.y() + 3, 17, 16);
            p.fillRect(b, on ? oc : QColor("#3a3a3a"));
            p.setPen(on ? Qt::black : QColor("#bbb"));
            p.drawText(b, Qt::AlignCenter, s);
            x += 20;
        };
        btn(t.locked ? "lock" : "unlock", t.locked, QColor("#e0a030"));
        btn("link", t.sync, QColor("#7fbf7f"));
        if (video) btn(t.muted ? "eyeoff" : "eye", !t.muted, QColor("#d0d0d0"));
        else { txtBtn("M", t.muted, QColor("#3fbf5f")); txtBtn("S", t.solo, QColor("#e8c040")); }
        if (r.height() >= 44 || x < kHeader - 40) {
            p.setPen(text);
            QRect nr = r.height() >= 44 ? QRect(60, r.y() + 22, kHeader - 64, 18) : QRect(x, r.y(), kHeader - x - 4, 22);
            p.drawText(nr, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(t.name, Qt::ElideRight, nr.width()));
        }
        p.setPen(bgDeep);
        p.drawLine(0, r.bottom() + 1, kHeader, r.bottom() + 1);
    }
}

void TimelineView::paintEvent(QPaintEvent *e)
{
    // During playback only the playhead moves: repaint it over a cached scene instead of every clip.
    bool narrow = true;
    for (const QRect &r : e->region()) narrow &= r.width() <= 20;
    const qreal dpr = devicePixelRatioF();
    if (!narrow || sceneCache.size() != size() * dpr) {
        if (sceneCache.size() != size() * dpr) { sceneCache = QPixmap(size() * dpr); sceneCache.setDevicePixelRatio(dpr); }
        QPainter sp(&sceneCache);
        paintScene(sp);
    }
    QPainter p(this);
    p.drawPixmap(0, 0, sceneCache);
    auto q = PRJ->active();
    if (!q) return;
    p.setClipRect(QRect(kHeader, 0, width() - kHeader, height()));
    int x = xFromFrame(q->playhead);
    lastPlayX = x;
    if (x >= kHeader - 6) {
        p.setPen(QPen(accent, 1));
        p.drawLine(x, kRuler, x, height());
        QPainterPath hd; hd.moveTo(x - 6, 4); hd.lineTo(x + 6, 4); hd.lineTo(x + 6, 16); hd.lineTo(x, 24); hd.lineTo(x - 6, 16); hd.closeSubpath();
        p.setRenderHint(QPainter::Antialiasing);
        p.fillPath(hd, accent);
    }
}

void TimelineView::paintScene(QPainter &p)
{
    p.fillRect(rect(), bgDark);
    auto q = PRJ->active();
    p.fillRect(QRect(0, 0, kHeader, kRuler), bg);
    if (!q) {
        p.setPen(textDim);
        p.drawText(rect(), Qt::AlignCenter, tr("Drop media here to create a sequence."));
        return;
    }
    int div = dividerY();
    // video area
    p.setClipRect(QRect(0, kRuler, width(), div - kRuler));
    for (int i = 0; i < q->video.size(); ++i) paintTrack(p, q->video[i], trackRect(true, i));
    paintHeaders(p, true);
    // audio area
    p.setClipRect(QRect(0, div + 3, width(), height() - div - 3));
    for (int i = 0; i < q->audio.size(); ++i) paintTrack(p, q->audio[i], trackRect(false, i));
    paintHeaders(p, false);
    p.setClipping(false);
    p.fillRect(QRect(0, div, width(), 3), bgDeep);
    paintRuler(p);

    p.setClipRect(QRect(kHeader, 0, width() - kHeader, height()));
    // drag ghosts
    if (mode == Mode::Move) {
        for (int id : std::as_const(dragIds)) {
            const Clip *c = base.clip(id);
            if (!c) continue;
            bool vid = c->kind != MediaKind::Audio;
            int ti = -1;
            auto &ls = vid ? base.video : base.audio;
            for (int i = 0; i < ls.size(); ++i) if (ls[i].clipById(id)) ti = i;
            if (vid == pressVideo) ti = qBound(0, ti + trackDelta, ls.size() - 1);
            QRect tr = trackRect(vid, ti);
            QRect g(xFromFrame(c->start + delta), tr.y(), int(c->dur * ppf), tr.height());
            p.fillRect(g, QColor(255, 255, 255, 40));
            p.setPen(QPen(insertDrag ? kSnap : Qt::white, 1, Qt::DashLine));
            p.drawRect(g.adjusted(0, 0, -1, -1));
        }
    }
    if (dropActive) {
        auto drawDrop = [&](bool vid, int idx) {
            if (idx < 0) return;
            QRect tr = trackRect(vid, idx);
            QRect g(xFromFrame(dropFrame), tr.y(), qMax(2, int(dropDur * ppf)), tr.height());
            p.fillRect(g, QColor(45, 140, 235, 80));
            p.setPen(QPen(accent, 1));
            p.drawRect(g.adjusted(0, 0, -1, -1));
        };
        drawDrop(true, dropV);
        drawDrop(false, dropA);
    }
    if (mode == Mode::Marquee) {
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 20));
        p.drawRect(marquee);
        p.setBrush(Qt::NoBrush);
    }
    if (snapLine >= 0) {
        int x = xFromFrame(snapLine);
        p.setPen(QPen(kSnap, 1));
        p.drawLine(x, kRuler, x, height());
    }
    if (hoverRazor >= 0) {
        int x = xFromFrame(hoverRazor);
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        p.drawLine(x, kRuler, x, height());
    }
}

// ------------------------------------------------------------------ interaction
void TimelineView::updateCursor(const QPoint &pos, Qt::KeyboardModifiers)
{
    Qt::CursorShape cs = Qt::ArrowCursor;
    HitT h = trackAt(pos.y());
    Tool tool = EDIT->tool;
    hoverRazor = -1;
    if (pos.x() >= kHeader && pos.y() >= kRuler) {
        Edge e = Edge::None;
        const Clip *c = clipAtPos(h, pos.x(), &e);
        switch (tool) {
        case Tool::Select: if (c && e != Edge::None) cs = Qt::SizeHorCursor; break;
        case Tool::Ripple: case Tool::Roll: case Tool::Rate: if (c) cs = Qt::SplitHCursor; break;
        case Tool::Razor:
            cs = Qt::CrossCursor;
            if (c) {
                bool did; Frame f = Frame(frameFromX(pos.x()) + .5);
                f = snap(f, {}, &did);
                hoverRazor = f;
            }
            break;
        case Tool::Slip: case Tool::Slide: if (c) cs = Qt::SizeHorCursor; break;
        case Tool::Pen: cs = Qt::CrossCursor; break;
        case Tool::Hand: cs = Qt::OpenHandCursor; break;
        case Tool::Zoom: cs = Qt::PointingHandCursor; break;
        case Tool::Text: cs = Qt::IBeamCursor; break;
        default: break;
        }
    }
    if (std::abs(pos.y() - dividerY() - 1) <= 3) cs = Qt::SplitVCursor;
    if (pos.x() < kHeader && h.t && pos.y() >= h.r.bottom() - 2) cs = Qt::SizeVerCursor;
    setCursor(cs);
    update();
}

bool TimelineView::headerClick(QMouseEvent *e, const HitT &h)
{
    if (!h.t) return false;
    int x = e->pos().x();
    auto setT = [&](const QString &label, std::function<void(Track &)> f) {
        int id = h.t->id;
        PRJ->edit(label, [&](ProjectState &st) { if (auto *q = st.seqById(st.activeSeq)) if (auto *t = q->track(id)) f(*t); });
    };
    if (x >= 3 && x < 27) {
        int &pv = h.video ? EDIT->patchV : EDIT->patchA;
        pv = pv == h.idx ? -1 : h.idx;
        update();
        return true;
    }
    if (x >= 30 && x < 54) { setT(tr("Toggle Track Targeting"), [](Track &t) { t.target = !t.target; }); return true; }
    if (e->pos().y() - h.r.y() > 21) return true;
    if (x >= 60 && x < 78) { setT(tr("Toggle Track Lock"), [](Track &t) { t.locked = !t.locked; }); return true; }
    if (x >= 80 && x < 98) { setT(tr("Toggle Sync Lock"), [](Track &t) { t.sync = !t.sync; }); return true; }
    if (x >= 100 && x < 118) { setT(h.video ? tr("Toggle Track Output") : tr("Mute Track"), [](Track &t) { t.muted = !t.muted; }); return true; }
    if (!h.video && x >= 120 && x < 138) { setT(tr("Solo Track"), [](Track &t) { t.solo = !t.solo; }); return true; }
    return true;
}

void TimelineView::mousePressEvent(QMouseEvent *e)
{
    auto q = PRJ->active();
    if (!q) return;
    const QPoint pos = e->pos();
    pressPos = curPos = pos;
    if (e->button() == Qt::MiddleButton) { mode = Mode::Pan; base = *q; pressFrame = Frame(scrollF); return; }
    if (e->button() != Qt::LeftButton) return;
    if (std::abs(pos.y() - dividerY() - 1) <= 3) { mode = Mode::Divider; return; }
    if (pos.y() < kRuler) {
        if (pos.x() < kHeader) return;
        // marker grab
        for (int i = 0; i < q->markers.size(); ++i)
            if (std::abs(xFromFrame(q->markers[i].t) - pos.x()) <= 5 && pos.y() >= 13) {
                mode = Mode::MarkerDrag; markerIdx = i; PRJ->liveBegin(); return;
            }
        mode = Mode::Scrub;
        PRJ->setPlayhead(snap(Frame(frameFromX(pos.x()) + .5), {}));
        return;
    }
    HitT h = trackAt(pos.y());
    if (pos.x() < kHeader) {
        if (h.t && pos.y() >= h.r.bottom() - 2) {
            mode = Mode::TrackResize; pressTrack = h.t->id; pressFrame = h.t->height; PRJ->liveBegin(); return;
        }
        headerClick(e, h);
        return;
    }
    // Track volume automation: Ctrl-click the yellow line to add a point, drag a point to move it.
    if (h.t && h.t->showAuto && !h.t->locked) {
        const Param &au = h.t->volAuto;
        int hit = -1;
        if (au.animated)
            for (int i = 0; i < au.keys.size(); ++i)
                if ((QPoint(xFromFrame(au.keys[i].t), autoY(h.r, au.keys[i].v)) - pos).manhattanLength() <= 7) hit = i;
        const double here = au.animated ? au.at(Frame(frameFromX(pos.x()))) : h.t->volumeDb;
        const bool onLine = std::abs(autoY(h.r, here) - pos.y()) <= 8;
        if (hit >= 0 || (onLine && (e->modifiers() & Qt::ControlModifier))) {
            pressTrack = h.t->id;
            pressVideo = false;
            PRJ->liveBegin();
            Track *t = PRJ->active()->track(pressTrack);
            if (hit < 0) {
                if (!t->volAuto.animated) { t->volAuto.animated = true; t->volAuto.keys.clear(); }
                Frame kf = qMax<Frame>(0, Frame(frameFromX(pos.x()) + .5));
                t->volAuto.setKey(kf, here);
                hit = t->volAuto.keyIndexAt(kf);
            }
            keyIndex = hit;
            mode = Mode::AutoDrag;
            PRJ->liveTouch();
            return;
        }
    }
    Tool tool = EDIT->tool;
    if (tool == Tool::Hand) { mode = Mode::Pan; pressFrame = Frame(scrollF); setCursor(Qt::ClosedHandCursor); return; }
    if (tool == Tool::Zoom) {
        setZoom(ppf * (e->modifiers() & Qt::AltModifier ? .5 : 2), frameFromX(pos.x()));
        return;
    }
    if (tool == Tool::Text) {
        PRJ->setPlayhead(Frame(frameFromX(pos.x())));
        if (auto *tp = qobject_cast<TimelinePanel *>(parentWidget())) tp->addTextClip();
        EDIT->setTool(Tool::Select);
        return;
    }
    Edge edge = Edge::None;
    const Clip *c = clipAtPos(h, pos.x(), &edge);
    if (h.t && h.t->locked) c = nullptr;

    if (tool == Tool::Razor) {
        if (!c) return;
        Frame f = snap(Frame(frameFromX(pos.x()) + .5), {});
        QSet<int> tracks;
        if (!(e->modifiers() & Qt::ShiftModifier)) {
            tracks << h.t->id;
            for (int id : PRJ->withLinked({c->id})) { Track *t = nullptr; q->clip(id, &t); if (t) tracks << t->id; }
        }
        PRJ->edit(tr("Razor"), [&](ProjectState &st) { Ops::razor(st, *st.seqById(st.activeSeq), f, tracks); });
        return;
    }
    if (tool == Tool::TrackFwd) {
        Frame f = Frame(frameFromX(pos.x()));
        QSet<int> sel;
        auto add = [&](const Track &t) { for (auto &k : t.clips) if (k.end() > f) sel << k.id; };
        if (e->modifiers() & Qt::ShiftModifier) { if (h.t) add(*h.t); }
        else { for (auto &t : q->video) add(t); for (auto &t : q->audio) add(t); }
        PRJ->setSelection(sel);
        if (sel.isEmpty()) return;
        c = nullptr;
        base = *q; dragIds = sel; mode = Mode::Move; delta = 0; trackDelta = 0;
        pressFrame = Frame(frameFromX(pos.x())); pressVideo = h.video; insertDrag = dupDrag = false;
        return;
    }
    if (!c) {
        // transition selection/resize
        if (auto *tx = transitionAt(h, pos.x())) { PRJ->selectTransition(h.t->id, tx->cut); return; }
        if (!(e->modifiers() & Qt::ShiftModifier)) PRJ->setSelection({});
        mode = Mode::Marquee;
        marquee = QRect(pos, QSize(1, 1));
        return;
    }
    if (auto *tx = transitionAt(h, pos.x())) {
        if (pos.y() < h.r.y() + 22) {
            PRJ->selectTransition(h.t->id, tx->cut);
            int x0 = xFromFrame(tx->start()), x1 = xFromFrame(tx->end());
            if (std::abs(pos.x() - x0) < 5 || std::abs(pos.x() - x1) < 5) {
                mode = Mode::TransEdge; base = *q; pressTrack = h.t->id; pressFrame = tx->cut; edgeRight = std::abs(pos.x() - x1) < 5;
                PRJ->liveBegin();
            }
            return;
        }
    }
    bool ctrlKey = e->modifiers() & Qt::ControlModifier;
    if (tool == Tool::Pen || (tool == Tool::Select && ctrlKey && h.r.height() >= 60)) {
        const Param *bp = bandParam(*c);
        if (bp) {
            QRect r(xFromFrame(c->start), h.r.y(), int(c->dur * ppf), h.r.height());
            keyIndex = -1;
            for (int i = 0; bp->animated && i < bp->keys.size(); ++i) {
                QPoint kp(xFromFrame(bp->keys[i].t - c->kfOffset + c->start), bandY(*c, r, bp->keys[i].v));
                if ((kp - pos).manhattanLength() <= 7) keyIndex = i;
            }
            // Decide before touching the model: liveBegin() detaches the state q and c point into.
            const int atY = bandY(*c, r, bp->at(c->kt(Frame(frameFromX(pos.x())))));
            const bool onBand = std::abs(atY - pos.y()) <= 8;
            const bool addKey = onBand && (tool == Tool::Select || (e->modifiers() & Qt::ControlModifier) || bp->animated);
            const bool dragBand = onBand && !addKey;
            if (keyIndex >= 0 || addKey || dragBand) {
                const int cid = c->id;
                const Clip snapshot = *c;
                pressClip = cid;
                pressTrack = h.t->id;
                base = *q;
                PRJ->liveBegin();
                Clip *mc = PRJ->active()->clip(cid);
                Param *mp = bandParam(*mc);
                if (keyIndex >= 0) { mode = Mode::KeyDrag; return; }
                if (dragBand) { mode = Mode::BandDrag; return; }
                Frame kt = mc->kt(Frame(frameFromX(pos.x()) + .5));
                if (!mp->animated) { mp->animated = true; mp->keys.clear(); }
                mp->setKey(kt, bandValue(snapshot, r, pos.y()));
                keyIndex = mp->keyIndexAt(kt);
                mode = Mode::KeyDrag;
                PRJ->liveTouch();
                return;
            }
        }
    }
    // Selection
    QSet<int> ids = (e->modifiers() & Qt::AltModifier && tool == Tool::Select) ? QSet<int>{c->id} : PRJ->withLinked({c->id});
    if (tool == Tool::Select || tool == Tool::Pen) {
        if (e->modifiers() & Qt::ShiftModifier) {
            QSet<int> s = PRJ->selClips;
            if (s.contains(c->id)) s -= ids; else s += ids;
            PRJ->setSelection(s);
            ids = s;
        } else if (!PRJ->selClips.contains(c->id)) {
            PRJ->setSelection(ids);
        } else ids = PRJ->selClips;
    } else PRJ->setSelection(ids);
    if (tool == Tool::Pen) return;

    base = *q;
    pressClip = c->id;
    pressTrack = h.t->id;
    pressVideo = h.video;
    dragIds = ids;
    pressFrame = Frame(frameFromX(pos.x()) + .5);
    delta = 0; trackDelta = 0;
    insertDrag = dupDrag = false;
    if (tool == Tool::Slip) { mode = Mode::Slip; dragIds = PRJ->withLinked({c->id}); PRJ->liveBegin(); return; }
    if (tool == Tool::Slide) { mode = Mode::Slide; dragIds = PRJ->withLinked({c->id}); PRJ->liveBegin(); return; }
    bool trimTool = tool == Tool::Ripple || tool == Tool::Roll || tool == Tool::Rate;
    if (edge != Edge::None || trimTool) {
        if (trimTool && edge == Edge::None) edge = (pos.x() - xFromFrame(c->start)) < (xFromFrame(c->end()) - pos.x()) ? Edge::Left : Edge::Right;
        dragIds = PRJ->withLinked({c->id});
        mode = edge == Edge::Left ? Mode::TrimL : Mode::TrimR;
        PRJ->liveBegin();
        return;
    }
    mode = Mode::Move;
}

// All trim modes recompute from the press-time sequence, so a drag can never accumulate error.
void TimelineView::applyTrim(int)
{
    auto q = PRJ->active();
    if (!q) return;
    const bool left = mode == Mode::TrimL;
    const Tool tool = EDIT->tool;
    const double fps = base.fps();
    const Clip *pc = base.clip(pressClip);
    if (!pc) return;
    Frame edgeF = left ? pc->start : pc->end();
    Frame want = Frame(frameFromX(curPos.x()) + .5);
    bool did = false;
    QSet<int> ex = dragIds;
    Frame snapped = snap(want, ex, &did);
    snapLine = did ? snapped : -1;
    Frame d = snapped - edgeF;
    Sequence n = base;
    auto clipLimits = [&](const Clip &c, const Track &t, Frame &lo, Frame &hi) {
        auto *m = PRJ->s.mediaById(c.mediaId);
        Frame extL = c.kind == MediaKind::Text ? Frame(1) << 40 : Ops::maxExtendLeft(c, fps, m);
        Frame extR = c.kind == MediaKind::Text ? Frame(1) << 40 : Ops::maxExtendRight(c, fps, m);
        int idx = -1;
        for (int i = 0; i < t.clips.size(); ++i) if (t.clips[i].id == c.id) idx = i;
        Frame prevEnd = idx > 0 ? t.clips[idx - 1].end() : 0;
        Frame nextStart = idx + 1 < t.clips.size() ? t.clips[idx + 1].start : Frame(1) << 50;
        if (tool == Tool::Rate) { extL = extR = Frame(1) << 40; }
        if (left) {
            lo = qMax(lo, tool == Tool::Ripple ? -extL : qMax(prevEnd - c.start, -extL));
            hi = qMin(hi, c.dur - 1);
        } else {
            lo = qMax(lo, 1 - c.dur);
            hi = qMin(hi, tool == Tool::Ripple ? extR : qMin(nextStart - c.end(), extR));
        }
    };
    Frame lo = -(Frame(1) << 50), hi = Frame(1) << 50;
    if (tool == Tool::Roll) {
        for (int id : std::as_const(dragIds)) {
            Track *t = nullptr; Clip *c = n.clip(id, &t);
            if (!c) continue;
            auto *m = PRJ->s.mediaById(c->mediaId);
            for (auto &o : t->clips) {
                if (left && o.end() == c->start) {
                    auto *om = PRJ->s.mediaById(o.mediaId);
                    lo = qMax(lo, 1 - o.dur); lo = qMax(lo, -Ops::maxExtendLeft(*c, fps, m));
                    hi = qMin(hi, c->dur - 1); hi = qMin(hi, Ops::maxExtendRight(o, fps, om));
                } else if (!left && o.start == c->end()) {
                    auto *om = PRJ->s.mediaById(o.mediaId);
                    lo = qMax(lo, 1 - c->dur); lo = qMax(lo, -Ops::maxExtendLeft(o, fps, om));
                    hi = qMin(hi, o.dur - 1); hi = qMin(hi, Ops::maxExtendRight(*c, fps, m));
                }
            }
        }
        d = qBound(lo, d, hi);
        for (int id : std::as_const(dragIds)) {
            Track *t = nullptr; Clip *c = n.clip(id, &t);
            if (!c) continue;
            Frame cut = left ? c->start : c->end();
            for (auto &o : t->clips) {
                if (left && o.end() == cut) Ops::trimOut(o, d);
                if (!left && o.start == cut) Ops::trimIn(o, d, fps);
            }
            c = n.clip(id, &t);
            if (left) Ops::trimIn(*c, d, fps); else Ops::trimOut(*c, d);
            for (auto &x : t->transitions) if (x.cut == cut) x.cut += d;
        }
    } else {
        for (int id : std::as_const(dragIds)) {
            Track *t = nullptr; const Clip *c = n.clip(id, &t);
            if (c) clipLimits(*c, *t, lo, hi);
        }
        d = qBound(lo, d, hi);
        QSet<int> rippled;
        for (int id : std::as_const(dragIds)) {
            Track *t = nullptr; Clip *c = n.clip(id, &t);
            if (!c) continue;
            Frame oldEnd = c->end();
            if (tool == Tool::Rate) {
                Frame oldDur = c->dur;
                if (left) { c->start += d; c->dur -= d; } else c->dur += d;
                c->speed = qBound(.01, c->speed * double(oldDur) / qMax<Frame>(1, c->dur), 100.0);
                auto *m = PRJ->s.mediaById(c->mediaId);
                if (m && m->duration > 0 && (c->kind == MediaKind::Video || c->kind == MediaKind::Audio)) {
                    double maxS = (m->duration - c->srcIn) * fps / c->dur;
                    c->speed = qMin(c->speed, qMax(.01, maxS));
                }
            } else if (tool == Tool::Ripple) {
                Frame shift;
                if (left) {
                    c->srcIn += d / fps * c->speed; c->dur -= d; c->kfOffset += d;
                    shift = -d;
                } else { c->dur += d; shift = d; }
                if (!rippled.contains(t->id)) rippled << t->id;
                for (auto &o : t->clips) if (o.id != id && o.start >= oldEnd) o.start += shift;
                for (auto &x : t->transitions) if (x.cut >= oldEnd) x.cut += shift;
                // keep other sync-locked tracks in step
                if (rippled.size() == 1) {
                    for (auto *ot : n.allTracks()) {
                        bool own = false;
                        for (int k : std::as_const(dragIds)) if (ot->clipById(k)) own = true;
                        if (own || ot->locked || !ot->sync) continue;
                        for (auto &o : ot->clips) if (o.start >= oldEnd) o.start += shift;
                        for (auto &x : ot->transitions) if (x.cut >= oldEnd) x.cut += shift;
                    }
                }
            } else {
                if (left) Ops::trimIn(*c, d, fps); else Ops::trimOut(*c, d);
            }
        }
    }
    *q = [&] { Sequence s = n; s.playhead = q->playhead; s.inPt = q->inPt; s.outPt = q->outPt; return s; }();
    PRJ->liveTouch();
}

void TimelineView::mouseMoveEvent(QMouseEvent *e)
{
    auto q = PRJ->active();
    curPos = e->pos();
    if (mode == Mode::None) { updateCursor(e->pos(), e->modifiers()); return; }
    if (!q) return;
    const int dx = curPos.x() - pressPos.x();
    switch (mode) {
    case Mode::Scrub: {
        Frame f = Frame(frameFromX(curPos.x()) + .5);
        PRJ->setPlayhead(qMax<Frame>(0, f));
        if (curPos.x() > width() - 8) { scrollF += 10 / ppf; emit viewChanged(); }
        else if (curPos.x() < kHeader + 8) { scrollF = qMax(0.0, scrollF - 10 / ppf); emit viewChanged(); }
        break;
    }
    case Mode::Pan:
        scrollF = qMax(0.0, double(pressFrame) - dx / ppf);
        emit viewChanged();
        update();
        break;
    case Mode::Divider:
        dividerRatio = qBound(.1, double(curPos.y() - kRuler) / qMax(1, height() - kRuler), .9);
        update();
        break;
    case Mode::TrackResize:
        if (auto *t = q->track(pressTrack)) {
            int dy = curPos.y() - pressPos.y();
            bool up = t->type == TrackType::Video;
            t->height = qBound(22, int(pressFrame + (up ? -dy : dy)), 240);
            update();
        }
        break;
    case Mode::MarkerDrag:
        if (markerIdx >= 0 && markerIdx < q->markers.size()) {
            q->markers[markerIdx].t = qMax<Frame>(0, Frame(frameFromX(curPos.x()) + .5));
            update();
        }
        break;
    case Mode::Marquee:
        marquee = QRect(pressPos, curPos).normalized();
        {
            QSet<int> s;
            auto scan = [&](bool video, const QVector<Track> &ls) {
                for (int i = 0; i < ls.size(); ++i) {
                    QRect tr = trackRect(video, i);
                    if (ls[i].locked) continue;
                    for (auto &c : ls[i].clips) {
                        QRect r(xFromFrame(c.start), tr.y(), int(c.dur * ppf), tr.height());
                        if (r.intersects(marquee)) s << c.id;
                    }
                }
            };
            scan(true, q->video); scan(false, q->audio);
            PRJ->setSelection(PRJ->withLinked(s));
        }
        update();
        break;
    case Mode::Move: {
        if (dx == 0 && curPos.y() == pressPos.y() && delta == 0) break;
        insertDrag = e->modifiers() & Qt::ControlModifier;
        dupDrag = e->modifiers() & Qt::AltModifier;
        Frame d = Frame(std::round(dx / ppf));
        Frame minStart = Frame(1) << 50;
        for (int id : std::as_const(dragIds)) if (auto *c = base.clip(id)) minStart = qMin(minStart, c->start);
        d = qMax(d, -minStart);
        // Snap both edges of the grabbed clip
        snapLine = -1;
        if (auto *pc = base.clip(pressClip)) {
            bool ds, de;
            Frame a = snap(pc->start + d, dragIds, &ds), b = snap(pc->end() + d, dragIds, &de);
            if (ds) { d = a - pc->start; snapLine = a; }
            else if (de) { d = b - pc->end(); snapLine = b; }
        }
        delta = d;
        HitT h = trackAt(curPos.y());
        if (h.t && h.video == pressVideo) {
            auto &ls = pressVideo ? base.video : base.audio;
            int src = 0;
            for (int i = 0; i < ls.size(); ++i) if (ls[i].id == pressTrack) src = i;
            trackDelta = h.idx - src;
        }
        update();
        break;
    }
    case Mode::TrimL: case Mode::TrimR:
        applyTrim(dx);
        break;
    case Mode::Slip: {
        Frame d = Frame(std::round(dx / ppf));
        for (int id : std::as_const(dragIds)) {
            const Clip *b = base.clip(id);
            Clip *c = q->clip(id);
            if (!b || !c) continue;
            auto *m = PRJ->s.mediaById(b->mediaId);
            double ns = b->srcIn - d / base.fps() * b->speed;
            if (m && m->duration > 0) ns = qBound(0.0, ns, qMax(0.0, m->duration - b->dur / base.fps() * b->speed));
            c->srcIn = qMax(0.0, ns);
        }
        PRJ->liveTouch();
        break;
    }
    case Mode::Slide: {
        Frame d = Frame(std::round(dx / ppf));
        Sequence n = base;
        Frame lo = -(Frame(1) << 50), hi = Frame(1) << 50;
        for (int id : std::as_const(dragIds)) {
            Track *t = nullptr; Clip *c = n.clip(id, &t);
            if (!c) continue;
            for (auto &o : t->clips) {
                auto *om = PRJ->s.mediaById(o.mediaId);
                if (o.end() == c->start) { lo = qMax(lo, 1 - o.dur); hi = qMin(hi, Ops::maxExtendRight(o, n.fps(), om)); }
                if (o.start == c->end()) { hi = qMin(hi, o.dur - 1); lo = qMax(lo, -Ops::maxExtendLeft(o, n.fps(), om)); }
            }
            lo = qMax(lo, -c->start);
        }
        d = qBound(lo, d, hi);
        for (int id : std::as_const(dragIds)) {
            Track *t = nullptr; Clip *c = n.clip(id, &t);
            if (!c) continue;
            Frame s = c->start, en = c->end();
            for (auto &o : t->clips) {
                if (o.end() == s) Ops::trimOut(o, d);
                else if (o.start == en) Ops::trimIn(o, d, n.fps());
            }
            n.clip(id)->start += d;
        }
        Frame ph = q->playhead;
        *q = n;
        q->playhead = ph;
        PRJ->liveTouch();
        break;
    }
    case Mode::KeyDrag: case Mode::BandDrag: {
        Clip *c = q->clip(pressClip);
        Track *t = q->track(pressTrack);
        if (!c || !t) break;
        Param *mp = bandParam(*c);
        QRect tr;
        for (int i = 0; i < q->video.size(); ++i) if (q->video[i].id == pressTrack) tr = trackRect(true, i);
        for (int i = 0; i < q->audio.size(); ++i) if (q->audio[i].id == pressTrack) tr = trackRect(false, i);
        QRect r(xFromFrame(c->start), tr.y(), int(c->dur * ppf), tr.height());
        double v = bandValue(*c, r, curPos.y());
        if (mode == Mode::BandDrag) { mp->value = v; }
        else if (keyIndex >= 0 && keyIndex < mp->keys.size()) {
            Frame nt = qBound(c->kfOffset, c->kt(Frame(frameFromX(curPos.x()) + .5)), c->kfOffset + c->dur - 1);
            if (keyIndex > 0) nt = qMax(nt, mp->keys[keyIndex - 1].t + 1);
            if (keyIndex + 1 < mp->keys.size()) nt = qMin(nt, mp->keys[keyIndex + 1].t - 1);
            mp->keys[keyIndex].t = nt;
            mp->keys[keyIndex].v = v;
        }
        PRJ->liveTouch();
        break;
    }
    case Mode::AutoDrag: {
        Track *t = q->track(pressTrack);
        if (!t || keyIndex < 0 || keyIndex >= t->volAuto.keys.size()) break;
        QRect tr;
        for (int i = 0; i < q->audio.size(); ++i) if (q->audio[i].id == pressTrack) tr = trackRect(false, i);
        auto &keys = t->volAuto.keys;
        Frame nt = qMax<Frame>(0, Frame(frameFromX(curPos.x()) + .5));
        if (keyIndex > 0) nt = qMax(nt, keys[keyIndex - 1].t + 1);
        if (keyIndex + 1 < keys.size()) nt = qMin(nt, keys[keyIndex + 1].t - 1);
        keys[keyIndex].t = nt;
        keys[keyIndex].v = qBound(-60.0, 15 - double(curPos.y() - tr.top()) / qMax(1, tr.height()) * 75, 15.0);
        PRJ->liveTouch();
        break;
    }
    case Mode::TransEdge: {
        Track *t = q->track(pressTrack);
        if (!t) break;
        for (auto &x : t->transitions)
            if (x.cut == pressFrame) {
                Frame f = Frame(frameFromX(curPos.x()) + .5);
                Frame half = qMax<Frame>(1, std::abs(f - x.cut));
                x.dur = qBound<Frame>(2, Frame(half / qMax(.5, edgeRight ? 1 - x.align : x.align)), Frame(q->fps() * 30));
            }
        PRJ->liveTouch();
        break;
    }
    default: break;
    }
}

void TimelineView::commitMove(QMouseEvent *)
{
    if (delta == 0 && trackDelta == 0 && !dupDrag) return;
    bool ins = insertDrag, dup = dupDrag;
    Frame d = delta;
    int td = trackDelta;
    bool pv = pressVideo;
    QSet<int> ids = dragIds;
    QSet<int> newSel;
    PRJ->edit(dup ? tr("Duplicate Clips") : ins ? tr("Insert Clips") : tr("Move Clips"), [&](ProjectState &st) {
        Sequence &q = *st.seqById(st.activeSeq);
        struct Mv { Clip c; bool vid; int ti; };
        QVector<Mv> mv;
        auto collect = [&](QVector<Track> &ls, bool vid) {
            for (int i = 0; i < ls.size(); ++i) {
                Track &t = ls[i];
                if (t.locked) continue;
                for (int k = t.clips.size() - 1; k >= 0; --k)
                    if (ids.contains(t.clips[k].id)) {
                        Clip c = t.clips[k];
                        Ops::dropTransitionsInRange(t, c.start, c.start);
                        Ops::dropTransitionsInRange(t, c.end(), c.end());
                        mv << Mv{c, vid, i};
                        if (!dup) t.clips.removeAt(k);
                    }
            }
        };
        collect(q.video, true); collect(q.audio, false);
        if (mv.isEmpty()) return;
        QHash<int, int> newLinks;
        Frame first = Frame(1) << 50, last = 0;
        for (auto &m : mv) {
            m.c.start += d;
            if (m.vid == pv) m.ti += td;
            auto &ls = m.vid ? q.video : q.audio;
            m.ti = qBound(0, m.ti, ls.size() - 1);
            first = qMin(first, m.c.start); last = qMax(last, m.c.end());
            if (dup) {
                m.c.id = st.newId();
                if (m.c.link) { if (!newLinks.contains(m.c.link)) newLinks[m.c.link] = st.newId(); m.c.link = newLinks[m.c.link]; }
            }
            newSel << m.c.id;
        }
        if (ins) Ops::insertGap(q, st, first, last - first);
        for (auto &m : mv) {
            auto &t = (m.vid ? q.video : q.audio)[m.ti];
            Ops::clearRange(q, t, m.c.start, m.c.end(), &st);
            Ops::placeClip(t, m.c);
        }
    });
    PRJ->setSelection(newSel);
}

void TimelineView::endMode()
{
    mode = Mode::None;
    snapLine = -1;
    update();
}

void TimelineView::mouseReleaseEvent(QMouseEvent *e)
{
    Mode m = mode;
    mode = Mode::None;
    snapLine = -1;
    switch (m) {
    case Mode::Move: mode = Mode::Move; commitMove(e); mode = Mode::None; break;
    case Mode::TrimL: case Mode::TrimR: {
        const char *n = "Trim";
        switch (EDIT->tool) { case Tool::Ripple: n = "Ripple Trim"; break; case Tool::Roll: n = "Rolling Edit"; break; case Tool::Rate: n = "Rate Stretch"; break; default: break; }
        PRJ->liveCommit(tr(n));
        break;
    }
    case Mode::Slip: PRJ->liveCommit(tr("Slip")); break;
    case Mode::Slide: PRJ->liveCommit(tr("Slide")); break;
    case Mode::AutoDrag: PRJ->liveCommit(tr("Track Automation")); break;
    case Mode::KeyDrag: PRJ->liveCommit(tr("Add/Move Keyframe")); break;
    case Mode::BandDrag: PRJ->liveCommit(tr("Adjust Level")); break;
    case Mode::TransEdge: PRJ->liveCommit(tr("Resize Transition")); break;
    case Mode::TrackResize: PRJ->liveCommit(tr("Resize Track")); break;
    case Mode::MarkerDrag: PRJ->liveCommit(tr("Move Marker")); break;
    default: break;
    }
    update();
    updateCursor(e->pos(), e->modifiers());
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent *e)
{
    auto q = PRJ->active();
    if (!q) return;
    if (e->pos().y() < kRuler) {
        for (int i = 0; i < q->markers.size(); ++i)
            if (std::abs(xFromFrame(q->markers[i].t) - e->pos().x()) <= 6) {
                Marker m = q->markers[i];
                bool ok;
                QString nm = QInputDialog::getText(this, tr("Marker"), tr("Name:"), QLineEdit::Normal, m.name, &ok);
                if (ok) PRJ->edit(tr("Edit Marker"), [&](ProjectState &st) { st.seqById(st.activeSeq)->markers[i].name = nm; });
                return;
            }
    }
    if (e->pos().x() < kHeader) {
        HitT h = trackAt(e->pos().y());
        if (h.t) {
            int id = h.t->id;
            int nh = h.t->height < 60 ? 90 : 44;
            PRJ->edit(tr("Resize Track"), [&](ProjectState &st) { st.seqById(st.activeSeq)->track(id)->height = nh; });
        }
    }
}

void TimelineView::wheelEvent(QWheelEvent *e)
{
    int d = e->angleDelta().y();
    QPoint pos = e->position().toPoint();
    if (e->modifiers() & Qt::AltModifier) {
        int y = e->angleDelta().x() ? e->angleDelta().x() : d;
        setZoom(ppf * (y > 0 ? 1.2 : 1 / 1.2), frameFromX(pos.x()));
    } else if (e->modifiers() & Qt::ControlModifier) {
        if (pos.y() < dividerY()) vScroll = qMax(0, vScroll - d / 3);
        else aScroll = qMin(0, aScroll + d / 3);
        update();
    } else {
        scrollF = qMax(0.0, scrollF - d / 2.0 / ppf);
        emit viewChanged();
        update();
    }
}

void TimelineView::leaveEvent(QEvent *) { hoverRazor = -1; update(); }

void TimelineView::contextMenuEvent(QContextMenuEvent *e)
{
    auto q = PRJ->active();
    if (!q) return;
    auto *tp = qobject_cast<TimelinePanel *>(parentWidget());
    HitT h = trackAt(e->pos().y());
    if (e->pos().y() < kRuler) {
        QMenu m(this);
        m.addAction(tr("Clear In and Out"), [] { PRJ->edit(tr("Clear In/Out"), [](ProjectState &st) { auto *s = st.seqById(st.activeSeq); s->inPt = s->outPt = -1; }); });
        m.addAction(tr("Clear All Markers"), [] { PRJ->edit(tr("Clear Markers"), [](ProjectState &st) { st.seqById(st.activeSeq)->markers.clear(); }); });
        m.exec(e->globalPos());
        return;
    }
    if (e->pos().x() < kHeader) {
        QMenu m(this);
        m.addAction(tr("Add Video Track"), [] { PRJ->edit(tr("Add Track"), [](ProjectState &st) { Ops::addTrack(st, *st.seqById(st.activeSeq), TrackType::Video); }); });
        m.addAction(tr("Add Audio Track"), [] { PRJ->edit(tr("Add Track"), [](ProjectState &st) { Ops::addTrack(st, *st.seqById(st.activeSeq), TrackType::Audio); }); });
        if (h.t) {
            int id = h.t->id;
            auto *del = m.addAction(tr("Delete This Track"), [id] { PRJ->edit(tr("Delete Track"), [=](ProjectState &st) { Ops::removeTrack(*st.seqById(st.activeSeq), id); }); });
            del->setEnabled(h.t->clips.isEmpty() && (h.video ? q->video.size() : q->audio.size()) > 1);
            if (!h.video) {
                auto *sa = m.addAction(tr("Show Track Volume Automation"), [id] {
                    PRJ->edit(tr("Show Automation"), [=](ProjectState &st) { auto *t = st.seqById(st.activeSeq)->track(id); t->showAuto = !t->showAuto; });
                });
                sa->setCheckable(true);
                sa->setChecked(h.t->showAuto);
                m.addAction(tr("Clear Track Automation"), [id] {
                    PRJ->edit(tr("Clear Automation"), [=](ProjectState &st) { auto *t = st.seqById(st.activeSeq)->track(id); t->volAuto = Param(); t->panAuto = Param(); });
                })->setEnabled(h.t->volAuto.animated || h.t->panAuto.animated);
            }
            QString cur = h.t->name;
            m.addAction(tr("Rename..."), [this, id, cur] {
                bool ok;
                QString n = QInputDialog::getText(this, tr("Rename Track"), tr("Name:"), QLineEdit::Normal, cur, &ok);
                if (ok && !n.isEmpty()) PRJ->edit(tr("Rename Track"), [=](ProjectState &st) { st.seqById(st.activeSeq)->track(id)->name = n; });
            });
        }
        m.exec(e->globalPos());
        return;
    }
    if (auto *tx = transitionAt(h, e->pos().x())) {
        if (e->pos().y() < h.r.y() + 22) {
            PRJ->selectTransition(h.t->id, tx->cut);
            QMenu m(this);
            int id = h.t->id; Frame cut = tx->cut;
            m.addAction(tr("Clear"), [=] { PRJ->edit(tr("Delete Transition"), [=](ProjectState &st) { Ops::dropTransitionsInRange(*st.seqById(st.activeSeq)->track(id), cut, cut); }); });
            m.exec(e->globalPos());
            return;
        }
    }
    if (auto *c = clipAtPos(h, e->pos().x())) { clipMenu(*c, e->globalPos()); return; }
    QMenu m(this);
    Frame f = Frame(frameFromX(e->pos().x()));
    if (h.t) {
        // Empty space between clips: offer the classic gap delete.
        Frame a = 0, b = Frame(1) << 50;
        for (auto &c : h.t->clips) { if (c.end() <= f) a = qMax(a, c.end()); if (c.start > f) b = qMin(b, c.start); }
        if (b < (Frame(1) << 50)) {
            m.addAction(tr("Ripple Delete"), [=] {
                PRJ->edit(tr("Ripple Delete Gap"), [=](ProjectState &st) {
                    auto &s = *st.seqById(st.activeSeq);
                    for (auto *t : s.allTracks()) if (!t->locked && t->sync) Ops::shiftAfter(*t, b, a - b);
                });
            });
        }
    }
    if (tp) m.addAction(tr("Paste"), [tp] { tp->paste(false); })->setEnabled(!EDIT->clipboard.isEmpty());
    if (!m.isEmpty()) m.exec(e->globalPos());
}

void TimelineView::clipMenu(const Clip &clip, const QPoint &g)
{
    auto *tp = qobject_cast<TimelinePanel *>(parentWidget());
    if (!tp) return;
    if (!PRJ->selClips.contains(clip.id)) PRJ->setSelection(PRJ->withLinked({clip.id}));
    QMenu m(this);
    m.addAction(tr("Cut"), [tp] { tp->copySelection(true); });
    m.addAction(tr("Copy"), [tp] { tp->copySelection(false); });
    m.addAction(tr("Paste Attributes"), [tp] { tp->pasteAttributes(); })->setEnabled(!EDIT->clipboard.isEmpty());
    m.addSeparator();
    auto *en = m.addAction(tr("Enable"), [tp] { tp->toggleEnable(); });
    en->setCheckable(true); en->setChecked(clip.enabled);
    m.addAction(clip.link ? tr("Unlink") : tr("Link"), [tp] { tp->toggleLink(); });
    m.addAction(tr("Nest..."), [tp] { tp->nestSelection(); });
    m.addSeparator();
    m.addAction(tr("Speed/Duration..."), [tp] { tp->speedDialog(); });
    if (clip.kind != MediaKind::Audio) {
        m.addAction(tr("Frame Hold Segment"), [tp] { tp->frameHold(); });
        m.addAction(tr("Scale to Frame Size"), [tp] { tp->scaleToFrame(true); });
    }
    m.addSeparator();
    m.addAction(tr("Ripple Delete"), [tp] { tp->deleteSelection(true); });
    m.addAction(tr("Clear"), [tp] { tp->deleteSelection(false); });
    m.addSeparator();
    auto *lm = m.addMenu(tr("Label"));
    for (int i = 0; i < kLabelCount; ++i) {
        QPixmap px(12, 12); px.fill(labelColor(i));
        lm->addAction(QIcon(px), labelName(i), [i] {
            QSet<int> ids = PRJ->selClips;
            PRJ->edit(tr("Label"), [&](ProjectState &st) {
                for (auto *t : st.seqById(st.activeSeq)->allTracks()) for (auto &c : t->clips) if (ids.contains(c.id)) c.label = i;
            });
        });
    }
    int cid = clip.id;
    QString nm = clip.name;
    m.addAction(tr("Rename..."), [this, cid, nm] {
        bool ok;
        QString n = QInputDialog::getText(this, tr("Rename Clip"), tr("Clip name:"), QLineEdit::Normal, nm, &ok);
        if (ok && !n.isEmpty()) PRJ->edit(tr("Rename Clip"), [=](ProjectState &st) { if (auto *c = st.seqById(st.activeSeq)->clip(cid)) c->name = n; });
    });
    m.exec(g);
}

void TimelineView::dragEnterEvent(QDragEnterEvent *e)
{
    auto *md = e->mimeData();
    if (md->hasFormat(kMimeMedia) || md->hasFormat(kMimeEffect) || md->hasFormat(kMimeTransition)) e->acceptProposedAction();
}
void TimelineView::dragMoveEvent(QDragMoveEvent *e)
{
    e->acceptProposedAction();
    auto q = PRJ->active();
    if (!q || !e->mimeData()->hasFormat(kMimeMedia)) return;
    QPoint pos = e->position().toPoint();
    auto ids = QString::fromUtf8(e->mimeData()->data(kMimeMedia)).split(',');
    double tot = 0;
    bool anyV = false, anyA = false;
    for (auto &s : ids)
        if (auto *m = PRJ->s.mediaById(s.toInt())) {
            double in = m->inSec >= 0 ? m->inSec : 0, out = m->outSec >= 0 ? m->outSec : m->duration;
            tot += out - in;
            anyV |= m->kind != MediaKind::Audio; anyA |= m->hasAudio;
        }
    dropDur = Frame(tot * q->fps());
    bool did;
    dropFrame = snap(qMax<Frame>(0, Frame(frameFromX(pos.x()) + .5)), {}, &did);
    snapLine = did ? dropFrame : -1;
    HitT h = trackAt(pos.y());
    dropV = anyV ? (h.t && h.video ? h.idx : 0) : -1;
    dropA = anyA ? (h.t && !h.video ? h.idx : 0) : -1;
    dropActive = true;
    update();
}
void TimelineView::dragLeaveEvent(QDragLeaveEvent *) { dropActive = false; snapLine = -1; update(); }

void TimelineView::dropEvent(QDropEvent *e)
{
    dropActive = false;
    snapLine = -1;
    auto *md = e->mimeData();
    QPoint pos = e->position().toPoint();
    auto q = PRJ->active();
    if (md->hasFormat(kMimeMedia)) {
        auto ids = QString::fromUtf8(md->data(kMimeMedia)).split(',');
        if (!q && !ids.isEmpty()) { // first clip dropped defines the sequence settings
            auto *m = PRJ->s.mediaById(ids.first().toInt());
            if (!m) return;
            int w = m->w > 0 ? m->w : 1920, h = m->h > 0 ? m->h : 1080;
            int num = m->fps > 0 ? qRound(m->fps * 1000) : 30000, den = 1000;
            if (std::fabs(m->fps - 29.97) < .01) { num = 30000; den = 1001; }
            else if (std::fabs(m->fps - 23.976) < .01) { num = 24000; den = 1001; }
            else if (std::fabs(m->fps - 59.94) < .01) { num = 60000; den = 1001; }
            PRJ->createSequence(m->name, w, h, num, den);
            q = PRJ->active();
            dropV = m->kind != MediaKind::Audio ? 0 : -1;
            dropA = m->hasAudio ? 0 : -1;
            dropFrame = 0;
        }
        bool ins = e->modifiers() & Qt::ControlModifier;
        Frame at = dropFrame;
        int v = dropV, a = dropA;
        PRJ->edit(ins ? tr("Insert") : tr("Overwrite"), [&](ProjectState &st) {
            Sequence &s = *st.seqById(st.activeSeq);
            for (auto &idS : ids) {
                auto *m = st.mediaById(idS.toInt());
                if (!m || (m->kind == MediaKind::Nested && m->seqId == s.id)) continue;
                double in = m->inSec >= 0 ? m->inSec : 0, out = m->outSec >= 0 ? m->outSec : m->duration;
                if (out <= in) { if (auto *inner = st.seqById(m->seqId)) out = inner->duration() / inner->fps(); else out = in + 5; }
                Ops::Patch p;
                p.videoTrack = m->kind != MediaKind::Audio ? v : -1;
                p.audioTrack = m->hasAudio ? a : -1;
                MediaItem mi = *m;
                mi.duration = qMax(mi.duration, out);
                if (ins) Ops::insertMedia(st, s, mi, in, out, at, p);
                else Ops::overwriteMedia(st, s, mi, in, out, at, p);
                at += qMax<Frame>(1, qRound64((out - in) * s.fps()));
            }
        });
        for (auto &idS : ids) if (auto *m = PRJ->s.mediaById(idS.toInt())) if (m->hasAudio && !m->path.isEmpty()) CACHE->ensureAudio(m->path);
        e->acceptProposedAction();
        update();
        return;
    }
    if (!q) return;
    HitT h = trackAt(pos.y());
    if (md->hasFormat(kMimeEffect)) {
        QString id = QString::fromUtf8(md->data(kMimeEffect));
        auto *def = EffectRegistry::find(id);
        const Clip *c = clipAtPos(h, pos.x());
        if (!c || !def) return;
        if (def->audio != (c->kind == MediaKind::Audio)) return;
        int cid = c->id;
        PRJ->edit(tr("Apply Effect: %1").arg(def->name), [&](ProjectState &st) {
            if (auto *mc = st.seqById(st.activeSeq)->clip(cid)) mc->fx << EffectRegistry::make(id);
        });
        PRJ->setSelection({cid});
    } else if (md->hasFormat(kMimeTransition)) {
        QString id = QString::fromUtf8(md->data(kMimeTransition));
        auto *def = EffectRegistry::findTransition(id);
        if (!h.t || !def || def->audio == h.video) return;
        Frame f = Frame(frameFromX(pos.x()));
        Frame best = -1; double bd = 1e18;
        for (auto &c : h.t->clips)
            for (Frame k : {c.start, c.end()})
                if (std::fabs(double(k - f)) < bd) { bd = std::fabs(double(k - f)); best = k; }
        if (best < 0) return;
        int tid = h.t->id;
        Frame dur = Frame(q->fps());
        // Align on the clip when dropped on a single-sided edit
        bool hasA = false, hasB = false;
        for (auto &c : h.t->clips) { hasA |= c.end() == best; hasB |= c.start == best; }
        double align = hasA && hasB ? .5 : hasB ? 0 : 1;
        PRJ->edit(tr("Add Transition"), [&](ProjectState &st) { Ops::addTransition(*st.seqById(st.activeSeq)->track(tid), id, best, dur, align); });
    }
    update();
}

// ------------------------------------------------------------------ zoom bar
ZoomBar::ZoomBar(TimelineView *view) : v(view)
{
    setFixedHeight(14);
    setMouseTracking(true);
    connect(view, &TimelineView::viewChanged, this, qOverload<>(&QWidget::update));
}
QRect ZoomBar::thumb() const
{
    double tot = qMax(1.0, double(v->totalFrames()) * 1.1);
    int w = width() - TimelineView::kHeader;
    double vis = v->visibleWidth() / v->ppf;
    int x0 = TimelineView::kHeader + int(v->scrollF / tot * w);
    int tw = qMax(24, int(vis / tot * w));
    return QRect(x0, 2, qMin(tw, width() - x0), height() - 4);
}
void ZoomBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), bgDark);
    p.setRenderHint(QPainter::Antialiasing);
    QRect t = thumb();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#555"));
    p.drawRoundedRect(t, 5, 5);
    p.setBrush(QColor("#9a9a9a"));
    p.drawEllipse(QRect(t.left() + 1, t.top() + 1, t.height() - 2, t.height() - 2));
    p.drawEllipse(QRect(t.right() - t.height() + 2, t.top() + 1, t.height() - 2, t.height() - 2));
}
void ZoomBar::mousePressEvent(QMouseEvent *e)
{
    QRect t = thumb();
    pressX = e->pos().x();
    pressScroll = v->scrollF;
    pressPpf = v->ppf;
    if (!t.contains(e->pos())) { mode = 1; return; }
    mode = e->pos().x() < t.left() + 12 ? 2 : e->pos().x() > t.right() - 12 ? 3 : 1;
}
void ZoomBar::mouseMoveEvent(QMouseEvent *e)
{
    if (!mode) return;
    double tot = qMax(1.0, double(v->totalFrames()) * 1.1);
    int w = width() - TimelineView::kHeader;
    double df = (e->pos().x() - pressX) / double(w) * tot;
    double visWas = v->visibleWidth() / pressPpf;
    if (mode == 1) v->scrollF = qMax(0.0, pressScroll + df);
    else if (mode == 3) v->ppf = qBound(.005, v->visibleWidth() / qMax(10.0, visWas + df), 40.0);
    else {
        double nv = qMax(10.0, visWas - df);
        v->ppf = qBound(.005, v->visibleWidth() / nv, 40.0);
        v->scrollF = qMax(0.0, pressScroll + df);
    }
    emit v->viewChanged();
    v->update();
}
void ZoomBar::mouseReleaseEvent(QMouseEvent *) { mode = 0; }

// ------------------------------------------------------------------ panel
TimelinePanel::TimelinePanel(QWidget *p) : Panel("timeline", tr("Timeline"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    tabs = new QTabBar(this);
    tabs->setExpanding(false);
    tabs->setTabsClosable(false);
    tabs->setDrawBase(false);
    v->addWidget(tabs);
    auto *top = new QWidget(this);
    top->setFixedHeight(32);
    auto *h = new QHBoxLayout(top);
    h->setContentsMargins(8, 2, 6, 2);
    h->setSpacing(6);
    tc = new TimecodeLabel(top, 17);
    h->addWidget(tc);
    h->addSpacing(24);
    auto *nest = iconButton("nest", tr("Nest Selected Clips"), top, 22);
    snapBtn = iconButton("snap", tr("Snap in Timeline (S)"), top, 22, true);
    snapBtn->setChecked(true);
    linkBtn = iconButton("link", tr("Linked Selection"), top, 22, true);
    linkBtn->setChecked(true);
    auto *mark = iconButton("marker", tr("Add Marker (M)"), top);
    auto *wr = iconButton("wrench", tr("Timeline Display Settings"), top);
    h->addWidget(nest); h->addWidget(snapBtn); h->addWidget(linkBtn); h->addWidget(mark); h->addWidget(wr);
    h->addStretch(1);
    v->addWidget(top);
    view = new TimelineView(this);
    v->addWidget(view, 1);
    zoom = new ZoomBar(view);
    v->addWidget(zoom);

    connect(snapBtn, &QToolButton::toggled, this, [](bool on) { PRJ->snapping = on; });
    connect(linkBtn, &QToolButton::toggled, this, [](bool on) { PRJ->linkedSelection = on; });
    connect(mark, &QToolButton::clicked, this, &TimelinePanel::addMarker);
    connect(nest, &QToolButton::clicked, this, &TimelinePanel::nestSelection);
    connect(wr, &QToolButton::clicked, this, [this, wr] {
        QMenu m;
        m.addAction(tr("Expand All Tracks"), [] { PRJ->edit(tr("Resize Tracks"), [](ProjectState &st) { for (auto *t : st.seqById(st.activeSeq)->allTracks()) t->height = 90; }); });
        m.addAction(tr("Minimize All Tracks"), [] { PRJ->edit(tr("Resize Tracks"), [](ProjectState &st) { for (auto *t : st.seqById(st.activeSeq)->allTracks()) t->height = 26; }); });
        m.exec(wr->mapToGlobal(QPoint(0, wr->height())));
    });
    connect(tc, &TimecodeLabel::edited, this, [](Frame f) { PRJ->setPlayhead(f); });
    connect(PRJ, &Project::playheadChanged, this, [this](Frame f) { if (auto q = PRJ->active()) tc->setFrame(f, q->fps(), q->dropFrame); });
    connect(PRJ, &Project::changed, this, [this] { rebuildTabs(); if (auto q = PRJ->active()) tc->setFrame(q->playhead, q->fps(), q->dropFrame); });
    connect(tabs, &QTabBar::tabBarClicked, this, [this](int i) { if (i >= 0) PRJ->setActiveSequence(tabs->tabData(i).toInt()); });
    rebuildTabs();
}

void TimelinePanel::rebuildTabs()
{
    QSignalBlocker b(tabs);
    bool same = tabs->count() == PRJ->s.seqs.size();
    for (int i = 0; same && i < tabs->count(); ++i)
        same = tabs->tabData(i).toInt() == PRJ->s.seqs[i].id && tabs->tabText(i) == PRJ->s.seqs[i].name;
    if (!same) {
        while (tabs->count()) tabs->removeTab(0);
        for (auto &q : PRJ->s.seqs) { int i = tabs->addTab(q.name); tabs->setTabData(i, q.id); }
    }
    for (int i = 0; i < tabs->count(); ++i) if (tabs->tabData(i).toInt() == PRJ->s.activeSeq) tabs->setCurrentIndex(i);
    tabs->setVisible(tabs->count() > 1);
    auto q = PRJ->active();
    QString t = q ? tr("Timeline: %1").arg(q->name) : tr("Timeline: (no sequences)");
    if (t != title) {
        title = t;
        if (parentWidget() && parentWidget()->parentWidget()) parentWidget()->parentWidget()->update();
    }
}

void TimelinePanel::fillMenu(QMenu *m)
{
    m->addAction(tr("Add Track..."), [this] { addTrackDialog(); });
    m->addAction(tr("Zoom to Sequence"), [this] { view->zoomFit(); });
}

void TimelinePanel::razorAtPlayhead(bool all)
{
    auto q = PRJ->active();
    if (!q) return;
    Frame f = q->playhead;
    QSet<int> tr_;
    if (!all) for (auto *t : q->allTracks()) if (t->target) tr_ << t->id;
    if (!all && tr_.isEmpty()) return;
    PRJ->edit(tr("Add Edit"), [&](ProjectState &st) { Ops::razor(st, *st.seqById(st.activeSeq), f, tr_); });
}
void TimelinePanel::deleteSelection(bool ripple)
{
    auto q = PRJ->active();
    if (!q) return;
    if (PRJ->selTransition.first) {
        auto sel = PRJ->selTransition;
        PRJ->edit(tr("Delete Transition"), [&](ProjectState &st) { Ops::dropTransitionsInRange(*st.seqById(st.activeSeq)->track(sel.first), sel.second, sel.second); });
        PRJ->setSelection({});
        return;
    }
    if (PRJ->selClips.isEmpty()) return;
    QSet<int> ids = PRJ->selClips;
    PRJ->edit(ripple ? tr("Ripple Delete") : tr("Delete"), [&](ProjectState &st) { Ops::deleteClips(*st.seqById(st.activeSeq), ids, ripple); });
    PRJ->setSelection({});
}
void TimelinePanel::liftRange()
{
    auto q = PRJ->active();
    if (!q || q->inPt < 0 || q->outPt <= q->inPt) return;
    PRJ->edit(tr("Lift"), [](ProjectState &st) { auto &s = *st.seqById(st.activeSeq); Ops::lift(st, s, s.inPt, s.outPt); s.inPt = s.outPt = -1; });
}
void TimelinePanel::extractRange()
{
    auto q = PRJ->active();
    if (!q || q->inPt < 0 || q->outPt <= q->inPt) return;
    PRJ->edit(tr("Extract"), [](ProjectState &st) { auto &s = *st.seqById(st.activeSeq); Ops::extract(st, s, s.inPt, s.outPt); s.outPt = -1; s.playhead = s.inPt; s.inPt = -1; });
}
void TimelinePanel::copySelection(bool cut)
{
    auto q = PRJ->active();
    if (!q || PRJ->selClips.isEmpty()) return;
    EDIT->clipboard.clear();
    auto scan = [&](const QVector<Track> &ls, bool vid) {
        for (int i = 0; i < ls.size(); ++i)
            for (auto &c : ls[i].clips)
                if (PRJ->selClips.contains(c.id)) EDIT->clipboard << ClipboardEntry{c, vid, i};
    };
    scan(q->video, true); scan(q->audio, false);
    if (cut) deleteSelection(false);
}
void TimelinePanel::paste(bool insert)
{
    auto q = PRJ->active();
    if (!q || EDIT->clipboard.isEmpty()) return;
    Frame first = Frame(1) << 50, last = 0;
    for (auto &e : EDIT->clipboard) { first = qMin(first, e.clip.start); last = qMax(last, e.clip.end()); }
    QSet<int> sel;
    Frame at = q->playhead;
    PRJ->edit(insert ? tr("Paste Insert") : tr("Paste"), [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        if (insert) Ops::insertGap(s, st, at, last - first);
        QHash<int, int> links;
        for (auto e : EDIT->clipboard) {
            e.clip.id = st.newId();
            e.clip.start += at - first;
            if (e.clip.link) { if (!links.contains(e.clip.link)) links[e.clip.link] = st.newId(); e.clip.link = links[e.clip.link]; }
            auto &ls = e.video ? s.video : s.audio;
            if (ls.isEmpty()) continue;
            Track &t = ls[qBound(0, e.trackIndex, ls.size() - 1)];
            if (t.locked) continue;
            Ops::clearRange(s, t, e.clip.start, e.clip.end(), &st);
            Ops::placeClip(t, e.clip);
            sel << e.clip.id;
        }
    });
    PRJ->setSelection(sel);
    PRJ->setPlayhead(at + last - first);
}
void TimelinePanel::pasteAttributes()
{
    if (EDIT->clipboard.isEmpty() || PRJ->selClips.isEmpty()) return;
    Clip srcV, srcA;
    bool hasV = false, hasA = false;
    for (auto &e : EDIT->clipboard) {
        if (e.video && !hasV) { srcV = e.clip; hasV = true; }
        if (!e.video && !hasA) { srcA = e.clip; hasA = true; }
    }
    QSet<int> ids = PRJ->selClips;
    PRJ->edit(tr("Paste Attributes"), [&](ProjectState &st) {
        for (auto *t : st.seqById(st.activeSeq)->allTracks())
            for (auto &c : t->clips) {
                if (!ids.contains(c.id)) continue;
                bool aud = c.kind == MediaKind::Audio;
                if (aud && hasA) c.fx = srcA.fx;
                if (!aud && hasV) c.fx = srcV.fx;
            }
    });
}
void TimelinePanel::toggleLink()
{
    auto q = PRJ->active();
    if (!q || PRJ->selClips.isEmpty()) return;
    QSet<int> ids = PRJ->selClips;
    bool anyLinked = false;
    for (int id : ids) if (auto *c = q->clip(id)) anyLinked |= c->link != 0;
    PRJ->edit(anyLinked ? tr("Unlink") : tr("Link"), [&](ProjectState &st) {
        int l = anyLinked ? 0 : st.newId();
        auto &s = *st.seqById(st.activeSeq);
        for (auto *t : s.allTracks()) for (auto &c : t->clips) if (ids.contains(c.id)) c.link = l;
    });
}
void TimelinePanel::toggleEnable()
{
    QSet<int> ids = PRJ->selClips;
    if (ids.isEmpty()) return;
    PRJ->edit(tr("Enable Clip"), [&](ProjectState &st) {
        for (auto *t : st.seqById(st.activeSeq)->allTracks()) for (auto &c : t->clips) if (ids.contains(c.id)) c.enabled = !c.enabled;
    });
}

void TimelinePanel::speedDialog()
{
    auto q = PRJ->active();
    if (!q || PRJ->selClips.isEmpty()) return;
    const Clip *first = q->clip(*PRJ->selClips.begin());
    if (!first) return;
    QDialog d(this);
    d.setWindowTitle(tr("Clip Speed / Duration"));
    auto *form = new QFormLayout(&d);
    auto *sp = new QDoubleSpinBox(&d);
    sp->setRange(1, 10000); sp->setSuffix(" %"); sp->setValue(first->speed * 100);
    auto *du = new QLineEdit(framesToTimecode(first->dur, q->fps(), q->dropFrame), &d);
    auto *rev = new QCheckBox(tr("Reverse Speed"), &d); rev->setChecked(first->reverse);
    auto *rip = new QCheckBox(tr("Ripple Edit, Shifting Trailing Clips"), &d);
    form->addRow(tr("Speed:"), sp);
    form->addRow(tr("Duration:"), du);
    form->addRow(rev);
    form->addRow(rip);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    double base = first->dur * first->speed;
    connect(sp, &QDoubleSpinBox::valueChanged, &d, [&](double v) {
        du->setText(framesToTimecode(Frame(base / (v / 100) + .5), q->fps(), q->dropFrame));
    });
    if (d.exec() != QDialog::Accepted) return;
    double s = sp->value() / 100;
    bool r = rev->isChecked(), ripple = rip->isChecked();
    QSet<int> ids = PRJ->selClips;
    PRJ->edit(tr("Speed/Duration"), [&](ProjectState &st) {
        Sequence &sq = *st.seqById(st.activeSeq);
        for (auto *t : sq.allTracks()) {
            for (int i = 0; i < t->clips.size(); ++i) {
                Clip &c = t->clips[i];
                if (!ids.contains(c.id)) continue;
                Frame nd = qMax<Frame>(1, Frame(c.dur * c.speed / s + .5));
                Frame oldEnd = c.end();
                Frame limit = i + 1 < t->clips.size() ? t->clips[i + 1].start - c.start : Frame(1) << 50;
                if (!ripple) nd = qMin(nd, limit);
                c.speed = s; c.reverse = r; c.dur = nd;
                if (ripple) {
                    Frame d2 = c.end() - oldEnd;
                    for (int k = i + 1; k < t->clips.size(); ++k) t->clips[k].start += d2;
                }
            }
        }
    });
}

void TimelinePanel::nestSelection()
{
    auto q = PRJ->active();
    if (!q || PRJ->selClips.isEmpty()) return;
    bool ok;
    QString name = QInputDialog::getText(this, tr("Nested Sequence Name"), tr("Name:"), QLineEdit::Normal, tr("Nested Sequence %1").arg(PRJ->s.seqs.size()), &ok);
    if (!ok || name.isEmpty()) return;
    QSet<int> ids = PRJ->selClips;
    PRJ->edit(tr("Nest"), [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        Sequence n;
        n.id = st.newId(); n.name = name; n.w = s.w; n.h = s.h; n.fpsNum = s.fpsNum; n.fpsDen = s.fpsDen;
        Frame first = Frame(1) << 50, last = 0;
        for (auto *t : s.allTracks()) for (auto &c : t->clips) if (ids.contains(c.id)) { first = qMin(first, c.start); last = qMax(last, c.end()); }
        int vLow = -1, aLow = -1;
        auto move = [&](QVector<Track> &ls, QVector<Track> &outL, TrackType ty, int &low) {
            for (int i = 0; i < ls.size(); ++i) {
                Track nt; nt.id = st.newId(); nt.type = ty; nt.name = ls[i].name;
                for (int k = ls[i].clips.size() - 1; k >= 0; --k)
                    if (ids.contains(ls[i].clips[k].id)) {
                        Clip c = ls[i].clips[k];
                        c.start -= first;
                        nt.clips.prepend(c);
                        ls[i].clips.removeAt(k);
                        if (low < 0 || i < low) low = i;
                    }
                outL << nt;
            }
        };
        move(s.video, n.video, TrackType::Video, vLow);
        move(s.audio, n.audio, TrackType::Audio, aLow);
        st.seqs << n;
        MediaItem m;
        m.id = st.newId(); m.kind = MediaKind::Nested; m.name = name; m.seqId = n.id; m.w = n.w; m.h = n.h; m.fps = n.fps();
        m.hasVideo = vLow >= 0; m.hasAudio = aLow >= 0; m.duration = (last - first) / n.fps(); m.label = defaultLabel(MediaKind::Nested);
        st.media << m;
        Sequence &s2 = *st.seqById(st.activeSeq);
        Ops::Patch p{qMax(vLow, -1), qMax(aLow, -1)};
        Ops::overwriteMedia(st, s2, m, 0, m.duration, first, p);
    });
    PRJ->setSelection({});
}

void TimelinePanel::addTransitionAtPlayhead(bool video, bool audio)
{
    auto q = PRJ->active();
    if (!q) return;
    Frame ph = q->playhead;
    Frame dur = Frame(q->fps());
    QString vid = EDIT->defaultVideoTransition, aud = EDIT->defaultAudioTransition;
    PRJ->edit(tr("Add Default Transition"), [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        // Applies at the selected clips' edges if any, otherwise at the edit nearest the playhead on targeted tracks.
        QSet<int> ids = PRJ->selClips;
        for (auto *t : s.allTracks()) {
            bool isV = t->type == TrackType::Video;
            if ((isV && !video) || (!isV && !audio) || t->locked) continue;
            const QString &ty = isV ? vid : aud;
            // Centred on a two-sided cut; on a clip's open end it stays inside the clip.
            auto addAt = [&](Frame k) {
                bool hasA = false, hasB = false;
                for (auto &c : t->clips) { hasA |= c.end() == k; hasB |= c.start == k; }
                Ops::addTransition(*t, ty, k, dur, hasA && hasB ? .5 : hasB ? 0 : 1);
            };
            if (!ids.isEmpty()) {
                QVector<Frame> edges;
                for (auto &c : t->clips) if (ids.contains(c.id)) edges << c.start << c.end();
                for (Frame k : edges) addAt(k);
            } else if (t->target) {
                Frame best = -1; double bd = 1e18;
                for (auto &c : t->clips) for (Frame k : {c.start, c.end()}) if (std::fabs(double(k - ph)) < bd) { bd = std::fabs(double(k - ph)); best = k; }
                if (best >= 0) addAt(best);
            }
        }
    });
}
void TimelinePanel::addMarker()
{
    auto q = PRJ->active();
    if (!q) return;
    Frame f = q->playhead;
    PRJ->edit(tr("Add Marker"), [&](ProjectState &st) {
        auto &s = *st.seqById(st.activeSeq);
        Marker m; m.t = f; m.color = 13;
        s.markers << m;
        std::sort(s.markers.begin(), s.markers.end(), [](const Marker &a, const Marker &b) { return a.t < b.t; });
    });
}
void TimelinePanel::trimToPlayhead(bool head)
{
    auto q = PRJ->active();
    if (!q) return;
    Frame ph = q->playhead;
    PRJ->edit(head ? tr("Ripple Trim Previous Edit") : tr("Ripple Trim Next Edit"), [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        Frame bestD = 0;
        for (auto *t : s.allTracks()) {
            if (t->locked || !t->target) continue;
            for (auto &c : t->clips) {
                if (ph <= c.start || ph >= c.end()) continue;
                if (head) { Frame d = ph - c.start; Ops::trimIn(c, d, s.fps()); c.start -= d; bestD = -d; }
                else { c.dur = ph - c.start; bestD = 0; }
            }
        }
        if (bestD != 0)
            for (auto *t : s.allTracks()) {
                if (t->locked) continue;
                for (auto &c : t->clips) if (c.start > ph) c.start += bestD;
            }
    });
    if (head) PRJ->setPlayhead(ph - (ph - Ops::nextEdit(*PRJ->active(), ph + 1, false, true)));
}
void TimelinePanel::selectAll()
{
    auto q = PRJ->active();
    if (!q) return;
    QSet<int> s;
    for (auto *t : q->allTracks()) if (!t->locked) for (auto &c : t->clips) s << c.id;
    PRJ->setSelection(s);
}
void TimelinePanel::addTextClip()
{
    auto q = PRJ->active();
    if (!q) return;
    Frame at = q->playhead;
    int cid = 0;
    PRJ->edit(tr("New Title"), [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        // Use the first target track above V1 with free space, otherwise add a track.
        Frame dur = Frame(s.fps() * 5);
        Track *dest = nullptr;
        for (int i = s.video.size() - 1; i >= 0; --i) {
            Track &t = s.video[i];
            bool free = !t.locked;
            for (auto &c : t.clips) if (c.start < at + dur && c.end() > at) free = false;
            if (free) dest = &t;
            else if (dest) break;
        }
        if (!dest) { Ops::addTrack(st, s, TrackType::Video); dest = &s.video.last(); }
        Clip c;
        c.id = cid = st.newId(); c.kind = MediaKind::Text; c.name = tr("Title"); c.start = at; c.dur = dur; c.label = defaultLabel(MediaKind::Text);
        EffectRegistry::defaultsFor(c);
        c.fx[0].find("posX")->value = s.w / 2.0; c.fx[0].find("posY")->value = s.h / 2.0;
        c.fx[0].find("ancX")->value = s.w / 2.0; c.fx[0].find("ancY")->value = s.h / 2.0;
        Ops::placeClip(*dest, c);
    });
    PRJ->setSelection({cid});
}
void TimelinePanel::markSelection()
{
    auto q = PRJ->active();
    if (!q || PRJ->selClips.isEmpty()) return;
    Frame a = Frame(1) << 50, b = 0;
    for (int id : std::as_const(PRJ->selClips)) if (auto *c = q->clip(id)) { a = qMin(a, c->start); b = qMax(b, c->end()); }
    PRJ->edit(tr("Mark Selection"), [&](ProjectState &st) { auto &s = *st.seqById(st.activeSeq); s.inPt = a; s.outPt = b; });
}
void TimelinePanel::frameHold()
{
    auto q = PRJ->active();
    if (!q) return;
    Frame ph = q->playhead;
    QSet<int> ids = PRJ->selClips;
    PRJ->edit(tr("Frame Hold Segment"), [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        for (auto *t : s.allTracks()) {
            if (t->type != TrackType::Video) continue;
            for (auto &c : t->clips)
                if (ids.contains(c.id) && ph >= c.start && ph < c.end()) {
                    double held = c.sourceTime(ph, s.fps());
                    c.srcIn = held; c.speed = .0001;
                    c.reverse = false;
                }
        }
    });
}
void TimelinePanel::scaleToFrame(bool)
{
    auto q = PRJ->active();
    if (!q) return;
    QSet<int> ids = PRJ->selClips;
    PRJ->edit(tr("Scale to Frame Size"), [&](ProjectState &st) {
        Sequence &s = *st.seqById(st.activeSeq);
        for (auto &t : s.video)
            for (auto &c : t.clips) {
                if (!ids.contains(c.id)) continue;
                auto *m = st.mediaById(c.mediaId);
                if (!m || m->w <= 0) continue;
                if (auto *mo = c.effect("motion")) {
                    mo->find("scale")->value = 100.0 * qMin(double(s.w) / m->w, double(s.h) / m->h);
                    mo->find("posX")->value = s.w / 2.0; mo->find("posY")->value = s.h / 2.0;
                }
            }
    });
}
void TimelinePanel::addTrackDialog()
{
    if (!PRJ->active()) return;
    PRJ->edit(tr("Add Tracks"), [](ProjectState &st) { auto &s = *st.seqById(st.activeSeq); Ops::addTrack(st, s, TrackType::Video); Ops::addTrack(st, s, TrackType::Audio); });
}
