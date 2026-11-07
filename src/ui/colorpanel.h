// Color grading panel (wheels, curves, LUTs), video scopes and the text/graphics panel.
#pragma once
#include "panels.h"
#include "widgets.h"
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollArea>
#include <QSlider>
#include <functional>

class ColorWheel : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheel(QWidget *p);
    void setValue(double r, double g, double b);
    QSize sizeHint() const override { return QSize(130, 130); }
    double ox = 0, oy = 0;
signals:
    void pressed();
    void moved(double r, double g, double b);
    void released();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
private:
    void pick(const QPointF &p);
    QImage disc;
};

class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget *p);
    void setPoints(const QVariantList &v);
    QVariantList points() const;
    QColor color = Qt::white;
    QSize sizeHint() const override { return QSize(220, 220); }
signals:
    void pressed();
    void edited();
    void released();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
private:
    QVector<QPointF> pts{{0, 0}, {1, 1}};
    int drag = -1;
};

class ColorPanel : public Panel {
    Q_OBJECT
public:
    explicit ColorPanel(QWidget *p = nullptr);
private:
    void bind();
    void section(const QString &title, QWidget *content, class QVBoxLayout *lay);
    QWidget *sliderRow(const QString &id, const QString &name, double lo, double hi, double def, double scale);
    QWidget *wheel(const QString &title, const char *r, const char *g, const char *b, const char *y);
    void ensureGrade();
    double get(const QString &id, double def) const;
    void set(const QString &id, double v);
    int targetClip() const;
    QVector<std::function<void()>> updaters;
    CurveEditor *curve;
    QString curveKey = "curveM";
    QLabel *clipLabel, *lutLabel;
    QWidget *content;
};

class ScopesPanel : public Panel {
    Q_OBJECT
public:
    explicit ScopesPanel(QWidget *p = nullptr);
    void feed(const QImage &img);
    std::function<QImage()> currentImage;
protected:
    void showEvent(QShowEvent *) override;
    void paintEvent(QPaintEvent *) override;
private:
    QComboBox *mode;
    QImage scope;
    QImage src;
    bool busy = false;
    void compute();
};

class GraphicsPanel : public Panel {
    Q_OBJECT
public:
    explicit GraphicsPanel(QWidget *p = nullptr);
    static QStringList animationNames();
    void importFont();
private:
    void load();
    void store(const std::function<void(TextStyle &)> &f, const QString &label);
    void applyPreset(int index);
    void savePreset();
    void reloadPresets();
    int clipId = 0;
    bool loading = false;
    QWidget *form, *scrollArea;
    QLabel *empty, *fontNote;
    QPlainTextEdit *text;
    class QFontComboBox *font;
    class QComboBox *preset, *weight, *caps, *valign, *fillType, *strokePos, *animIn, *animOut;
    class QPushButton *delPre;
    ScrubValue *size, *tracking, *leading, *skew, *boxW, *gradA, *strokeW, *shadowD, *shadowA, *shadowB, *glowS, *glowK, *bgPad, *bgR, *animInS, *animOutS;
    QToolButton *ital, *under, *strike, *al[4];
    ColorSwatch *fill, *fill2, *strokeC, *shadowC, *glowC, *bgC;
    class QCheckBox *kern, *stroke, *shadow, *glow, *bg, *bgLine;
};
