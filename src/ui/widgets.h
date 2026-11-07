// Small reusable controls that make up the editor's look.
#pragma once
#include "../core/model.h"
#include "theme.h"
#include <QColorDialog>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QWidget>

// Blue number you drag to change, click to type: the signature control of pro video apps.
class ScrubValue : public QWidget {
    Q_OBJECT
public:
    explicit ScrubValue(QWidget *p = nullptr);
    void setRange(double mn, double mx) { lo = mn; hi = mx; }
    void setStep(double s) { stepV = s; }
    void setDecimals(int d) { dec = d; update(); }
    void setSuffix(const QString &s) { suffix = s; update(); }
    void setValue(double v);
    double value() const { return val; }
    QSize sizeHint() const override;
signals:
    void edited(double v);       // continuous during drag
    void dragBegan();
    void dragEnded();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
private:
    QString text() const;
    void beginEdit();
    double val = 0, lo = -1e9, hi = 1e9, stepV = 1, pressVal = 0;
    int dec = 1, pressX = 0;
    bool dragging = false, pressed = false;
    QString suffix;
    QLineEdit *ed = nullptr;
};

class TimecodeLabel : public QWidget {
    Q_OBJECT
public:
    explicit TimecodeLabel(QWidget *p = nullptr, int px = 15, const QColor &c = Theme::scrub);
    void setFrame(Frame f, double fps, bool df);
    QSize sizeHint() const override;
signals:
    void edited(Frame f);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
private:
    Frame frame = 0, pressF = 0;
    double fps = 30;
    bool df = false, moved = false;
    int pressX = 0, pxSize;
    QColor color;
    QLineEdit *ed = nullptr;
};

class ColorSwatch : public QWidget {
    Q_OBJECT
public:
    explicit ColorSwatch(QWidget *p = nullptr);
    void setColor(const QColor &c) { col = c; update(); }
    QColor color() const { return col; }
    bool withAlpha = false;
signals:
    void changed(const QColor &c);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
private:
    QColor col = Qt::white;
};

QToolButton *iconButton(const QString &icon, const QString &tip, QWidget *parent, int size = 22, bool checkable = false);
