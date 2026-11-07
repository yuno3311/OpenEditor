#include "colorpanel.h"
#include "../core/effectdefs.h"
#include "../core/project.h"
#include "../engine/imagefx.h"
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QtMath>
#include <cmath>

using namespace Theme;

ColorWheel::ColorWheel(QWidget *p) : QWidget(p)
{
    setMinimumSize(110, 110);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}
void ColorWheel::resizeEvent(QResizeEvent *)
{
    int s = qMin(width(), height()) - 6;
    if (s < 8) return;
    disc = QImage(s, s, QImage::Format_ARGB32_Premultiplied);
    disc.fill(0);
    double R = s / 2.0;
    for (int y = 0; y < s; ++y) {
        auto *l = reinterpret_cast<QRgb *>(disc.scanLine(y));
        for (int x = 0; x < s; ++x) {
            double dx = x + .5 - R, dy = y + .5 - R, d = std::hypot(dx, dy) / R;
            if (d > 1) continue;
            double a = std::atan2(-dy, dx) / (2 * M_PI);
            if (a < 0) a += 1;
            QColor c = QColor::fromHslF(a, 1, .5 + (1 - d) * .1);
            int k = int(d * d * 150);
            int al = d > .98 ? int((1 - d) * 50 * 255) : 255;
            int base = 40;
            int r = (base * (150 - k) + c.red() * k) / 150, g = (base * (150 - k) + c.green() * k) / 150, b = (base * (150 - k) + c.blue() * k) / 150;
            l[x] = qPremultiply(qRgba(r, g, b, al));
        }
    }
}
void ColorWheel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPointF c(width() / 2.0, height() / 2.0);
    if (!disc.isNull()) p.drawImage(c - QPointF(disc.width() / 2.0, disc.height() / 2.0), disc);
    double R = disc.width() / 2.0;
    p.setPen(QPen(QColor(0, 0, 0, 90), 1));
    p.drawLine(c - QPointF(R, 0), c + QPointF(R, 0));
    p.drawLine(c - QPointF(0, R), c + QPointF(0, R));
    QPointF pk = c + QPointF(ox * R, oy * R);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::white, 1.5));
    p.drawEllipse(pk, 5, 5);
}
void ColorWheel::pick(const QPointF &pos)
{
    double R = disc.width() / 2.0;
    QPointF d = (pos - QPointF(width() / 2.0, height() / 2.0)) / R;
    double m = std::hypot(d.x(), d.y());
    if (m > 1) d /= m;
    ox = d.x(); oy = d.y();
    double a = std::atan2(-oy, ox), s = qMin(1.0, m);
    double r = std::cos(a) * s, g = std::cos(a - 2 * M_PI / 3) * s, b = std::cos(a + 2 * M_PI / 3) * s;
    update();
    emit moved(r * .5, g * .5, b * .5);
}
void ColorWheel::setValue(double r, double g, double b)
{
    r *= 2; g *= 2; b *= 2;
    // Inverse of the three-phase projection in pick()
    double x = (2 * r - g - b) / 3, y = (g - b) / std::sqrt(3.0);
    ox = x; oy = -y;
    update();
}
void ColorWheel::mousePressEvent(QMouseEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier || e->button() == Qt::RightButton) {
        emit pressed(); ox = oy = 0; update(); emit moved(0, 0, 0); emit released();
        return;
    }
    emit pressed();
    pick(e->position());
}
void ColorWheel::mouseMoveEvent(QMouseEvent *e) { if (e->buttons() & Qt::LeftButton) pick(e->position()); }
void ColorWheel::mouseReleaseEvent(QMouseEvent *) { emit released(); }

CurveEditor::CurveEditor(QWidget *p) : QWidget(p) { setFixedHeight(200); setMouseTracking(false); }
void CurveEditor::setPoints(const QVariantList &v)
{
    pts.clear();
    for (int i = 0; i + 1 < v.size(); i += 2) pts << QPointF(v[i].toDouble(), v[i + 1].toDouble());
    if (pts.size() < 2) pts = {{0, 0}, {1, 1}};
    update();
}
QVariantList CurveEditor::points() const
{
    QVariantList v;
    for (auto &p : pts) v << p.x() << p.y();
    return v;
}
void CurveEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QRect r = rect().adjusted(2, 2, -3, -3);
    p.fillRect(r, QColor("#161616"));
    p.setPen(QColor("#303030"));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(r.left() + r.width() * i / 4, r.top(), r.left() + r.width() * i / 4, r.bottom());
        p.drawLine(r.left(), r.top() + r.height() * i / 4, r.right(), r.top() + r.height() * i / 4);
    }
    p.setPen(QColor("#444"));
    p.drawLine(r.bottomLeft(), r.topRight());
    p.setRenderHint(QPainter::Antialiasing);
    auto lut = ImageFx::curveLut(points());
    QPolygonF poly;
    for (int i = 0; i < 256; ++i) poly << QPointF(r.left() + r.width() * i / 255.0, r.bottom() - r.height() * lut[i]);
    p.setPen(QPen(color, 1.6));
    p.drawPolyline(poly);
    p.setBrush(Qt::white);
    p.setPen(Qt::NoPen);
    for (auto &pt : pts) p.drawEllipse(QPointF(r.left() + pt.x() * r.width(), r.bottom() - pt.y() * r.height()), 3.5, 3.5);
}
void CurveEditor::mousePressEvent(QMouseEvent *e)
{
    QRect r = rect().adjusted(2, 2, -3, -3);
    QPointF n((e->position().x() - r.left()) / r.width(), (r.bottom() - e->position().y()) / r.height());
    drag = -1;
    for (int i = 0; i < pts.size(); ++i)
        if (std::hypot((pts[i].x() - n.x()) * r.width(), (pts[i].y() - n.y()) * r.height()) < 9) drag = i;
    emit pressed();
    if (drag < 0) {
        int i = 0;
        while (i < pts.size() && pts[i].x() < n.x()) ++i;
        pts.insert(i, QPointF(qBound(0.0, n.x(), 1.0), qBound(0.0, n.y(), 1.0)));
        drag = i;
        emit edited();
        update();
    }
}
void CurveEditor::mouseMoveEvent(QMouseEvent *e)
{
    if (drag < 0) return;
    QRect r = rect().adjusted(2, 2, -3, -3);
    double x = (e->position().x() - r.left()) / r.width(), y = (r.bottom() - e->position().y()) / r.height();
    double lo = drag > 0 ? pts[drag - 1].x() + .01 : 0, hi = drag + 1 < pts.size() ? pts[drag + 1].x() - .01 : 1;
    pts[drag] = QPointF(qBound(lo, x, hi), qBound(0.0, y, 1.0));
    update();
    emit edited();
}
void CurveEditor::mouseReleaseEvent(QMouseEvent *) { if (drag >= -1) emit released(); drag = -1; }
void CurveEditor::mouseDoubleClickEvent(QMouseEvent *e)
{
    QRect r = rect().adjusted(2, 2, -3, -3);
    for (int i = 1; i + 1 < pts.size(); ++i)
        if (std::hypot(r.left() + pts[i].x() * r.width() - e->position().x(), r.bottom() - pts[i].y() * r.height() - e->position().y()) < 9) {
            emit pressed();
            pts.removeAt(i);
            emit edited();
            emit released();
            update();
            return;
        }
}

ColorPanel::ColorPanel(QWidget *p) : Panel("color", tr("Color"), p)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    clipLabel = new QLabel(this);
    clipLabel->setContentsMargins(8, 4, 8, 4);
    clipLabel->setStyleSheet("background:#1f1f1f;color:#aaa;");
    outer->addWidget(clipLabel);
    auto *sa = new QScrollArea(this);
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    outer->addWidget(sa, 1);
    content = new QWidget;
    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(1);

    auto *basic = new QWidget(content);
    auto *bv = new QVBoxLayout(basic);
    bv->setContentsMargins(10, 6, 10, 6);
    bv->setSpacing(2);
    auto *lutRow = new QHBoxLayout;
    lutRow->addWidget(new QLabel(tr("Input LUT"), basic));
    lutLabel = new QLabel(tr("None"), basic);
    lutLabel->setStyleSheet("color:#999;");
    lutRow->addWidget(lutLabel, 1);
    auto *lutBtn = new QPushButton(tr("Browse..."), basic);
    auto *lutClr = iconButton("close", tr("Remove LUT"), basic);
    lutRow->addWidget(lutBtn);
    lutRow->addWidget(lutClr);
    bv->addLayout(lutRow);
    auto setLut = [this](const QString &path) {
        ensureGrade();
        int cid = targetClip();
        PRJ->edit(tr("Set LUT"), [&](ProjectState &st) {
            if (auto *e = st.seqById(st.activeSeq)->clip(cid)->effect("colorgrade")) { if (path.isEmpty()) e->extra.remove("lut"); else e->extra["lut"] = path; }
        });
    };
    connect(lutBtn, &QPushButton::clicked, this, [=, this] {
        if (!targetClip()) return;
        QString f = QFileDialog::getOpenFileName(this, tr("Load LUT"), {}, tr("Cube LUT (*.cube)"));
        if (!f.isEmpty()) setLut(f);
    });
    connect(lutClr, &QToolButton::clicked, this, [=, this] { if (targetClip()) setLut({}); });
    struct S { const char *id; const char *n; double lo, hi, def, sc; };
    for (S s : {S{"temp", "Temperature", -100, 100, 0, 1}, S{"tint", "Tint", -100, 100, 0, 1}, S{"exposure", "Exposure", -5, 5, 0, 100}, S{"contrast", "Contrast", -100, 100, 0, 1},
                S{"highlights", "Highlights", -100, 100, 0, 1}, S{"shadows", "Shadows", -100, 100, 0, 1}, S{"whites", "Whites", -100, 100, 0, 1}, S{"blacks", "Blacks", -100, 100, 0, 1},
                S{"sat", "Saturation", 0, 200, 100, 1}})
        bv->addWidget(sliderRow(s.id, tr(s.n), s.lo, s.hi, s.def, s.sc));
    section(tr("Basic Correction"), basic, lay);

    auto *cre = new QWidget(content);
    auto *cv = new QVBoxLayout(cre);
    cv->setContentsMargins(10, 6, 10, 6);
    cv->setSpacing(2);
    for (S s : {S{"faded", "Faded Film", 0, 100, 0, 1}, S{"sharpen", "Sharpen", 0, 100, 0, 1}, S{"vibrance", "Vibrance", -100, 100, 0, 1}})
        cv->addWidget(sliderRow(s.id, tr(s.n), s.lo, s.hi, s.def, s.sc));
    section(tr("Creative"), cre, lay);

    auto *cu = new QWidget(content);
    auto *cuv = new QVBoxLayout(cu);
    cuv->setContentsMargins(10, 6, 10, 6);
    auto *chs = new QHBoxLayout;
    for (auto [k, col] : std::initializer_list<std::pair<const char *, const char *>>{{"curveM", "#ffffff"}, {"curveR", "#ff5a5a"}, {"curveG", "#5adc5a"}, {"curveB", "#5a8cff"}}) {
        auto *b = new QPushButton(QString(k).right(1) == "M" ? tr("Luma") : QString(k).right(1), cu);
        b->setStyleSheet(QString("color:%1;padding:2px 8px;").arg(col));
        QString key = k; QColor c(col);
        connect(b, &QPushButton::clicked, this, [=, this] {
            curveKey = key;
            curve->color = c;
            int cid = targetClip();
            auto q = PRJ->active();
            const Effect *e = cid && q ? q->clip(cid)->effect("colorgrade") : nullptr;
            curve->setPoints(e ? e->extra.value(key).toList() : QVariantList{});
        });
        chs->addWidget(b);
    }
    chs->addStretch(1);
    cuv->addLayout(chs);
    curve = new CurveEditor(cu);
    cuv->addWidget(curve);
    connect(curve, &CurveEditor::pressed, this, [this] { ensureGrade(); PRJ->liveBegin(); });
    connect(curve, &CurveEditor::edited, this, [this] {
        if (auto *q = PRJ->active()) if (auto *c = q->clip(targetClip())) if (auto *e = c->effect("colorgrade")) { e->extra[curveKey] = curve->points(); PRJ->liveTouch(); }
    });
    connect(curve, &CurveEditor::released, this, [] { PRJ->liveCommit(tr("Adjust Curve")); });
    section(tr("Curves"), cu, lay);

    auto *wh = new QWidget(content);
    auto *wl = new QHBoxLayout(wh);
    wl->setContentsMargins(6, 6, 6, 6);
    wl->addWidget(wheel(tr("Shadows"), "liftR", "liftG", "liftB", "liftY"));
    wl->addWidget(wheel(tr("Midtones"), "gammaR", "gammaG", "gammaB", "gammaY"));
    wl->addWidget(wheel(tr("Highlights"), "gainR", "gainG", "gainB", "gainY"));
    section(tr("Color Wheels"), wh, lay);

    auto *vg = new QWidget(content);
    auto *vv = new QVBoxLayout(vg);
    vv->setContentsMargins(10, 6, 10, 6);
    vv->setSpacing(2);
    for (S s : {S{"vigAmount", "Amount", -5, 5, 0, 100}, S{"vigMid", "Midpoint", 0, 100, 50, 1}, S{"vigRound", "Roundness", -100, 100, 0, 1}, S{"vigFeather", "Feather", 0, 100, 50, 1}})
        vv->addWidget(sliderRow(s.id, tr(s.n), s.lo, s.hi, s.def, s.sc));
    section(tr("Vignette"), vg, lay);
    lay->addStretch(1);
    sa->setWidget(content);
    connect(PRJ, &Project::selectionChanged, this, &ColorPanel::bind);
    connect(PRJ, &Project::changed, this, &ColorPanel::bind);
    bind();
}

void ColorPanel::section(const QString &title, QWidget *c, QVBoxLayout *lay)
{
    auto *b = new QToolButton(content);
    b->setText(title);
    b->setCheckable(true);
    b->setChecked(true);
    b->setIcon(Theme::icon("chevdown"));
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setIconSize(QSize(14, 14));
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setFixedHeight(24);
    b->setStyleSheet("QToolButton{background:#2e2e2e;color:#e0e0e0;font-weight:600;text-align:left;border-radius:0;}");
    b->setFocusPolicy(Qt::NoFocus);
    connect(b, &QToolButton::toggled, c, &QWidget::setVisible);
    lay->addWidget(b);
    lay->addWidget(c);
}

int ColorPanel::targetClip() const
{
    auto q = PRJ->active();
    if (!q) return 0;
    for (auto &t : q->video) for (auto &c : t.clips) if (PRJ->selClips.contains(c.id)) return c.id;
    // no selection: grade whatever is topmost under the playhead
    for (int i = q->video.size() - 1; i >= 0; --i) if (auto *c = q->video[i].clipAt(q->playhead)) return c->id;
    return 0;
}
void ColorPanel::ensureGrade()
{
    int id = targetClip();
    auto q = PRJ->active();
    if (!id || !q || q->clip(id)->effect("colorgrade")) return;
    PRJ->edit(tr("Add Color Grade"), [&](ProjectState &st) { st.seqById(st.activeSeq)->clip(id)->fx << EffectRegistry::make("colorgrade"); });
}
double ColorPanel::get(const QString &id, double def) const
{
    int cid = targetClip();
    auto q = PRJ->active();
    if (!cid || !q) return def;
    auto *c = q->clip(cid);
    auto *e = c->effect("colorgrade");
    auto *pr = e ? e->find(id) : nullptr;
    return pr ? pr->at(c->kt(q->playhead)) : def;
}
void ColorPanel::set(const QString &id, double v)
{
    auto q = PRJ->active();
    int cid = targetClip();
    if (!q || !cid) return;
    auto *c = q->clip(cid);
    auto *e = c->effect("colorgrade");
    if (!e) return;
    if (auto *pr = e->find(id)) {
        if (pr->animated) pr->setKey(c->kt(q->playhead), v); else pr->value = v;
    }
    PRJ->liveTouch();
}
QWidget *ColorPanel::sliderRow(const QString &id, const QString &name, double lo, double hi, double def, double scale)
{
    auto *w = new QWidget(content);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 1, 0, 1);
    auto *l = new QLabel(name, w);
    l->setFixedWidth(82);
    auto *s = new QSlider(Qt::Horizontal, w);
    s->setRange(int(lo * scale), int(hi * scale));
    s->setValue(int(def * scale));
    s->setFocusPolicy(Qt::NoFocus);
    auto *v = new ScrubValue(w);
    v->setRange(lo, hi);
    v->setStep(1 / scale * (scale > 1 ? 2 : 1));
    v->setDecimals(scale > 1 ? 2 : 1);
    v->setValue(def);
    v->setFixedWidth(46);
    h->addWidget(l); h->addWidget(s, 1); h->addWidget(v);
    auto begin = [this] { ensureGrade(); PRJ->liveBegin(); };
    QString nm = name;
    auto end = [nm] { PRJ->liveCommit(tr("Color: %1").arg(nm)); };
    connect(s, &QSlider::sliderPressed, this, begin);
    connect(s, &QSlider::sliderMoved, this, [=, this](int x) { v->setValue(x / scale); set(id, x / scale); });
    connect(s, &QSlider::sliderReleased, this, end);
    connect(v, &ScrubValue::dragBegan, this, begin);
    connect(v, &ScrubValue::edited, this, [=, this](double x) { s->setValue(int(x * scale)); set(id, x); });
    connect(v, &ScrubValue::dragEnded, this, end);
    updaters << [=, this] {
        double x = get(id, def);
        v->setValue(x);
        if (!s->isSliderDown()) s->setValue(int(x * scale));
    };
    return w;
}
QWidget *ColorPanel::wheel(const QString &title, const char *r, const char *g, const char *b, const char *y)
{
    auto *w = new QWidget(content);
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto *l = new QLabel(title, w);
    l->setAlignment(Qt::AlignCenter);
    v->addWidget(l);
    auto *wh = new ColorWheel(w);
    v->addWidget(wh);
    auto *lv = new QSlider(Qt::Horizontal, w);
    lv->setRange(-100, 100);
    lv->setFocusPolicy(Qt::NoFocus);
    lv->setToolTip(tr("Level"));
    v->addWidget(lv);
    QString R = r, G = g, B = b, Y = y, T = title;
    connect(wh, &ColorWheel::pressed, this, [this] { ensureGrade(); PRJ->liveBegin(); });
    connect(wh, &ColorWheel::moved, this, [=, this](double a, double c, double d) { set(R, a); set(G, c); set(B, d); });
    connect(wh, &ColorWheel::released, this, [T] { PRJ->liveCommit(tr("Color Wheel: %1").arg(T)); });
    connect(lv, &QSlider::sliderPressed, this, [this] { ensureGrade(); PRJ->liveBegin(); });
    connect(lv, &QSlider::sliderMoved, this, [=, this](int x) { set(Y, x / 100.0); });
    connect(lv, &QSlider::sliderReleased, this, [T] { PRJ->liveCommit(tr("Color Wheel: %1").arg(T)); });
    updaters << [=, this] {
        wh->setValue(get(R, 0), get(G, 0), get(B, 0));
        if (!lv->isSliderDown()) lv->setValue(int(get(Y, 0) * 100));
    };
    return w;
}
void ColorPanel::bind()
{
    if (PRJ->inLive()) return;
    int cid = targetClip();
    auto q = PRJ->active();
    auto *c = cid && q ? q->clip(cid) : nullptr;
    clipLabel->setText(c ? tr("Grading: %1").arg(c->name) : tr("Select or park on a video clip to grade it."));
    content->setEnabled(c != nullptr);
    for (auto &u : updaters) u();
    const Effect *e = c ? c->effect("colorgrade") : nullptr;
    curve->setPoints(e ? e->extra.value(curveKey).toList() : QVariantList{});
    lutLabel->setText(e && e->extra.contains("lut") ? QFileInfo(e->extra["lut"].toString()).fileName() : tr("None"));
}

ScopesPanel::ScopesPanel(QWidget *p) : Panel("scopes", tr("Scopes"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    mode = new QComboBox(this);
    mode->addItems({tr("Waveform (Luma)"), tr("Waveform (RGB Parade)"), tr("Vectorscope"), tr("Histogram")});
    mode->setFocusPolicy(Qt::NoFocus);
    auto *h = new QHBoxLayout;
    h->addWidget(mode);
    h->addStretch(1);
    v->addLayout(h);
    v->addStretch(1);
    connect(mode, &QComboBox::currentIndexChanged, this, [this] { compute(); });
}
void ScopesPanel::showEvent(QShowEvent *)
{
    if (currentImage) { src = currentImage(); compute(); }
}
void ScopesPanel::feed(const QImage &img)
{
    if (!isVisible()) return;
    src = img;
    if (busy) return;
    busy = true;
    QTimer::singleShot(66, this, [this] { busy = false; compute(); });
}
void ScopesPanel::compute()
{
    if (src.isNull()) return;
    QImage in = src.scaled(360, 200, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);
    int m = mode->currentIndex();
    const int W = 512, H = 256;
    QVector<int> buf(W * H * 3, 0);
    auto add = [&](int x, int y, int c, int n = 1) { if (x >= 0 && y >= 0 && x < W && y < H) buf[(y * W + x) * 3 + c] += n; };
    for (int y = 0; y < in.height(); ++y) {
        auto *l = reinterpret_cast<const QRgb *>(in.constScanLine(y));
        for (int x = 0; x < in.width(); ++x) {
            int r = qRed(l[x]), g = qGreen(l[x]), b = qBlue(l[x]);
            int Y = (r * 54 + g * 183 + b * 19) >> 8;
            int px = x * W / in.width();
            if (m == 0) { for (int c = 0; c < 3; ++c) add(px, 255 - Y, c, 3); }
            else if (m == 1) {
                add(px / 3, 255 - r, 0, 6); add(W / 3 + px / 3, 255 - g, 1, 6); add(2 * W / 3 + px / 3, 255 - b, 2, 6);
            } else if (m == 2) {
                double cb = -.1146 * r - .3854 * g + .5 * b, cr = .5 * r - .4542 * g - .0458 * b;
                int vx = 128 + int(cb), vy = 128 - int(cr);
                add(vx + 128, vy, 0, 5); add(vx + 128, vy, 1, 5); add(vx + 128, vy, 2, 5);
            }
        }
    }
    QImage out(W, H, QImage::Format_RGB32);
    out.fill(qRgb(14, 14, 14));
    if (m == 3) {
        int hist[3][256] = {};
        for (int y = 0; y < in.height(); ++y) {
            auto *l = reinterpret_cast<const QRgb *>(in.constScanLine(y));
            for (int x = 0; x < in.width(); ++x) { ++hist[0][qRed(l[x])]; ++hist[1][qGreen(l[x])]; ++hist[2][qBlue(l[x])]; }
        }
        int mx = 1;
        for (int c = 0; c < 3; ++c) for (int i = 2; i < 254; ++i) mx = qMax(mx, hist[c][i]);
        for (int i = 0; i < 256; ++i)
            for (int c = 0; c < 3; ++c) {
                int top = H - qMin(H, hist[c][i] * H / mx);
                for (int y = top; y < H; ++y) for (int k = 0; k < 2; ++k) buf[(y * W + i * 2 + k) * 3 + c] += 60;
            }
    }
    for (int y = 0; y < H; ++y) {
        auto *l = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < W; ++x) {
            int *c = &buf[(y * W + x) * 3];
            if (c[0] || c[1] || c[2]) l[x] = qRgb(qMin(255, 20 + c[0] * 8), qMin(255, 20 + c[1] * 8), qMin(255, 20 + c[2] * 8));
        }
    }
    scope = out;
    update();
}
void ScopesPanel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), bgDark);
    QRect r = rect().adjusted(6, 34, -6, -6);
    p.fillRect(r, QColor("#0e0e0e"));
    if (!scope.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (mode->currentIndex() == 2) {
            int s = qMin(r.width(), r.height());
            QRect sq(r.center().x() - s / 2, r.center().y() - s / 2, s, s);
            p.drawImage(sq, scope, QRect(128, 0, 256, 256));
            p.setPen(QColor("#555"));
            p.setRenderHint(QPainter::Antialiasing);
            p.drawEllipse(sq.adjusted(4, 4, -4, -4));
            p.drawLine(sq.center().x(), sq.top(), sq.center().x(), sq.bottom());
            p.drawLine(sq.left(), sq.center().y(), sq.right(), sq.center().y());
        } else {
            p.drawImage(r, scope);
            if (mode->currentIndex() < 2) {
                QFont f = font(); f.setPixelSize(9); p.setFont(f);
                for (int i = 0; i <= 4; ++i) {
                    int y = r.top() + r.height() * i / 4;
                    p.setPen(QColor(120, 120, 120, 90));
                    p.drawLine(r.left(), y, r.right(), y);
                    p.setPen(textDim);
                    p.drawText(r.left() + 2, y + (i == 0 ? 10 : -2), QString::number(100 - i * 25));
                }
            }
        }
    }
}
