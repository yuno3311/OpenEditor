#pragma once
#include "editstate.h"
#include "panels.h"
#include "widgets.h"
#include "../engine/playback.h"
#include <QComboBox>
#include <QLabel>
#include <QTimer>

class MonitorPanel;

class ViewerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(MonitorPanel *m);
    void setImage(const QImage &i) { img = i; update(); }
    void setFrameSize(QSize s) { frameSize = s; update(); }
    QRectF imageRect() const;
    double zoom = 0; // 0 = fit
    bool safeMargins = false;
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
private:
    QTransform clipTransform(const Clip &c, const MediaItem *m, Frame kt, QSizeF &native) const;
    const Clip *overlayClip() const;
    QRectF contentRect(const Clip &c, const QSizeF &native) const;
    void paintOverlay(QPainter &p);
    void setMotion(Clip &c, const QString &id, double v);
    MonitorPanel *mon;
    QImage img;
    QSize frameSize{1920, 1080};
    enum { None, Move, Scale, Rotate, Anchor } drag = None;
    QPointF pressW;
    double p0x = 0, p0y = 0, s0 = 0, r0 = 0, dist0 = 1, ang0 = 0;
    int dragClip = 0;
    QPoint srcPress;
    bool srcDrag = false;
};

class TimeBar : public QWidget {
    Q_OBJECT
public:
    explicit TimeBar(MonitorPanel *m);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
private:
    void scrub(int x);
    MonitorPanel *mon;
};

class MonitorPanel : public Panel {
    Q_OBJECT
public:
    MonitorPanel(bool source, QWidget *p = nullptr);
    void fillMenu(QMenu *m) override;
    bool isSource;
    PlaybackEngine *engine;
    ViewerWidget *viewer;
    int mediaId = 0;
    void setMedia(int id);
    Frame length() const;
    double fps() const;
    bool dropFrame() const;
    Frame position() const;
    Frame inFrame() const;
    Frame outFrame() const;
    void setPos(Frame f);
    void setInOut(Frame in, Frame out);
    void togglePlay();
    void shuttle(int dir);  // -1 J, 0 K, +1 L
    void step(int frames);
    void markIn();
    void markOut();
    void clearInOut();
    void gotoIn();
    void gotoOut();
    void sourceEdit(bool insert);
    void exportFrame();
    QVector<Frame> markerFrames() const;
signals:
    void requestText();
private:
    void syncProgram();
    void refreshLabels();
    void addTransport(class QHBoxLayout *h);
    QTimer syncTimer;
    bool fromEngine = false;
    TimecodeLabel *tc;
    QLabel *durLabel, *dropLabel;
    QComboBox *zoomBox, *resBox;
    TimeBar *bar;
    QToolButton *playBtn, *loopBtn;
    Sequence srcSeq;
    Frame srcPos = 0;
    QString srcTitle;
};
