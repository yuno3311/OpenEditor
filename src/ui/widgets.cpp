#include "widgets.h"
#include <QMouseEvent>
#include <QPainter>
#include <cmath>

QToolButton *iconButton(const QString &icon, const QString &tip, QWidget *parent, int size, bool checkable)
{
    auto *b = new QToolButton(parent);
    b->setIcon(Theme::icon(icon));
    b->setIconSize(QSize(size - 6, size - 6));
    b->setFixedSize(size, size);
    b->setToolTip(tip);
    b->setCheckable(checkable);
    b->setFocusPolicy(Qt::NoFocus);
    b->setAutoRaise(true);
    return b;
}

ScrubValue::ScrubValue(QWidget *p) : QWidget(p)
{
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}
QString ScrubValue::text() const { return QString::number(val, 'f', dec) + suffix; }
QSize ScrubValue::sizeHint() const
{
    return QSize(fontMetrics().horizontalAdvance(text()) + 6, 18);
}
void ScrubValue::setValue(double v)
{
    if (qFuzzyCompare(val + 1e-9, v + 1e-9)) return;
    val = v;
    updateGeometry();
    update();
}
void ScrubValue::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setPen(isEnabled() ? Theme::scrub : QColor("#555"));
    p.drawText(rect().adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
    p.setPen(QPen(isEnabled() ? QColor(63, 162, 255, 120) : QColor(90, 90, 90), 1, Qt::DotLine));
    int w = fontMetrics().horizontalAdvance(text());
    p.drawLine(2, height() - 3, 2 + w, height() - 3);
}
void ScrubValue::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    pressed = true;
    dragging = false;
    pressX = e->globalPosition().x();
    pressVal = val;
}
void ScrubValue::mouseMoveEvent(QMouseEvent *e)
{
    if (!pressed) return;
    int dx = e->globalPosition().x() - pressX;
    if (!dragging && qAbs(dx) < 3) return;
    if (!dragging) { dragging = true; emit dragBegan(); }
    double k = stepV;
    if (e->modifiers() & Qt::ShiftModifier) k *= 10;
    if (e->modifiers() & Qt::ControlModifier) k /= 10;
    double v = qBound(lo, pressVal + dx * k, hi);
    if (v != val) { val = v; update(); emit edited(v); }
}
void ScrubValue::mouseReleaseEvent(QMouseEvent *)
{
    if (!pressed) return;
    pressed = false;
    if (dragging) { dragging = false; emit dragEnded(); }
    else beginEdit();
}
void ScrubValue::beginEdit()
{
    if (ed) return;
    ed = new QLineEdit(this);
    ed->setText(QString::number(val, 'f', dec));
    ed->setGeometry(rect().adjusted(0, -1, 16, 1));
    ed->setStyleSheet("QLineEdit{padding:0 2px;font-size:11px;}");
    ed->setFixedWidth(qMax(width(), 54));
    ed->show();
    ed->selectAll();
    ed->setFocus();
    auto done = [this] {
        if (!ed) return;
        auto *e = ed;
        ed = nullptr;
        bool ok;
        double v = e->text().toDouble(&ok);
        e->deleteLater();
        if (ok) {
            val = qBound(lo, v, hi);
            update();
            emit dragBegan();
            emit edited(val);
            emit dragEnded();
        }
    };
    connect(ed, &QLineEdit::editingFinished, this, done);
}

TimecodeLabel::TimecodeLabel(QWidget *p, int px, const QColor &c) : QWidget(p), pxSize(px), color(c)
{
    setCursor(Qt::SizeHorCursor);
}
QSize TimecodeLabel::sizeHint() const { return QSize(pxSize * 7, pxSize + 8); }
void TimecodeLabel::setFrame(Frame f, double r, bool d)
{
    if (frame == f && fps == r && df == d) return;
    frame = f; fps = r; df = d;
    update();
}
void TimecodeLabel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QFont f = font();
    f.setPixelSize(pxSize);
    f.setFamilies({"Consolas", "Segoe UI"});
    p.setFont(f);
    p.setPen(color);
    p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, framesToTimecode(frame, fps, df));
}
void TimecodeLabel::mousePressEvent(QMouseEvent *e)
{
    pressX = e->globalPosition().x();
    pressF = frame;
    moved = false;
}
void TimecodeLabel::mouseMoveEvent(QMouseEvent *e)
{
    int dx = e->globalPosition().x() - pressX;
    if (qAbs(dx) > 2) moved = true;
    if (moved) emit edited(qMax<Frame>(0, pressF + dx));
}
void TimecodeLabel::mouseReleaseEvent(QMouseEvent *)
{
    if (moved || ed) return;
    ed = new QLineEdit(this);
    ed->setText(framesToTimecode(frame, fps, df));
    ed->setGeometry(rect());
    ed->show();
    ed->selectAll();
    ed->setFocus();
    connect(ed, &QLineEdit::editingFinished, this, [this] {
        if (!ed) return;
        auto *e = ed;
        ed = nullptr;
        bool ok;
        Frame f = timecodeToFrames(e->text(), fps, df, &ok);
        e->deleteLater();
        if (ok) emit edited(f);
    });
}

ColorSwatch::ColorSwatch(QWidget *p) : QWidget(p)
{
    setFixedSize(34, 16);
    setCursor(Qt::PointingHandCursor);
}
void ColorSwatch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor("#555"));
    p.fillRect(rect().adjusted(1, 1, -1, -1), col);
}
void ColorSwatch::mousePressEvent(QMouseEvent *)
{
    auto opts = withAlpha ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions();
    QColor c = QColorDialog::getColor(col, this, tr("Select Color"), opts);
    if (c.isValid()) { col = c; update(); emit changed(c); }
}
