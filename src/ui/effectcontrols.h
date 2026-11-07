#pragma once
#include "panels.h"
#include "widgets.h"
#include <QLabel>
#include <QScrollArea>
#include <QSet>
#include <functional>

class KeyLane : public QWidget {
    Q_OBJECT
public:
    KeyLane(int clipId, int fx, const QString &param, QWidget *p);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
private:
    int xOf(Frame kt) const;
    Frame tOf(int x) const;
    int clipId, fx;
    QString param;
    int dragKey = -1;
};

class LaneRuler : public QWidget {
    Q_OBJECT
public:
    explicit LaneRuler(QWidget *p);
    int clipId = 0;
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
};

class EffectControlsPanel : public Panel {
    Q_OBJECT
public:
    explicit EffectControlsPanel(QWidget *p = nullptr);
private:
    void rebuild();
    void refresh();
    void section(class QVBoxLayout *lay, int clipId);
    void addEffect(class QVBoxLayout *lay, int clipId, int fxIndex);
    void transitionRows(class QVBoxLayout *lay);
    QString signature() const;
    QScrollArea *scroll;
    QWidget *body = nullptr;
    QLabel *head;
    QString sig;
    QVector<std::function<void()>> updaters;
    QVector<QWidget *> lanes;
    QSet<QString> collapsed;
};
