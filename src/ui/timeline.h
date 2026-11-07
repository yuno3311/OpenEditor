#pragma once
#include "editstate.h"
#include "panels.h"
#include "widgets.h"
#include "../core/ops.h"
#include <QTabBar>
#include <QTimer>

class TimelineView : public QWidget {
    Q_OBJECT
public:
    explicit TimelineView(QWidget *p = nullptr);
    double ppf = 2.0;       // pixels per frame
    double scrollF = 0;     // left edge, in frames
    int vScroll = 0, aScroll = 0;
    static constexpr int kHeader = 196, kTicks = 30, kRuler = 44;
    QRect trackRect(bool video, int i) const;
    double frameFromX(int x) const { return (x - kHeader) / ppf + scrollF; }
    int xFromFrame(double f) const { return kHeader + int((f - scrollF) * ppf); }
    void setZoom(double p, double anchorFrame);
    void zoomFit();
    void ensurePlayheadVisible();
    Frame totalFrames() const;
    int visibleWidth() const { return width() - kHeader; }
signals:
    void viewChanged();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dragLeaveEvent(QDragLeaveEvent *) override;
    void dropEvent(QDropEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    enum class Mode { None, AutoDrag, Scrub, Move, TrimL, TrimR, Slip, Slide, Marquee, Pan, KeyDrag, BandDrag, TransEdge, Divider, TrackResize, MarkerDrag };
    struct HitT { const Track *t = nullptr; int idx = -1; bool video = true; QRect r; };
    enum class Edge { None, Left, Right };
    int dividerY() const;
    HitT trackAt(int y) const;

    const Clip *clipAtPos(const HitT &h, int x, Edge *edge = nullptr) const;
    const Transition *transitionAt(const HitT &h, int x) const;
    Frame snap(Frame f, const QSet<int> &exclude, bool *didSnap = nullptr) const;
    int bandY(const Clip &c, const QRect &r, double v) const;
    int autoY(const QRect &r, double db) const;
    double bandValue(const Clip &c, const QRect &r, int y) const;
    Param *bandParam(Clip &c);
    const Param *bandParam(const Clip &c) const;
    void paintScene(QPainter &p);
    QPixmap sceneCache;
    int lastPlayX = 0;
    void paintRuler(QPainter &p);
    void paintHeaders(QPainter &p, bool video);
    void paintTrack(QPainter &p, const Track &t, const QRect &r);
    void paintClip(QPainter &p, const Clip &c, const Track &t, const QRect &r);
    void paintWave(QPainter &p, const Clip &c, const QRect &r);
    void paintThumbs(QPainter &p, const Clip &c, const QRect &r);
    bool headerClick(QMouseEvent *e, const HitT &h);
    void clipMenu(const Clip &c, const QPoint &g);
    void applyTrim(int dx);
    void updateCursor(const QPoint &pos, Qt::KeyboardModifiers m);
    void commitMove(QMouseEvent *e);
    void endMode();
    Mode mode = Mode::None;
    Sequence base;
    QPoint pressPos, curPos;
    Frame pressFrame = 0, delta = 0, snapLine = -1;
    int pressClip = 0, pressTrack = 0;
    bool pressVideo = true;
    QSet<int> dragIds;
    int trackDelta = 0;
    bool insertDrag = false, dupDrag = false;
    Frame hoverRazor = -1;
    int hoverTrack = 0;
    int keyIndex = -1;
    QRect marquee;
    bool edgeRight = false;
    int markerIdx = -1;
    double dividerRatio = .5;
    // external drag preview
    bool dropActive = false;
    Frame dropFrame = 0, dropDur = 0;
    int dropV = -1, dropA = -1;
};

class ZoomBar : public QWidget {
    Q_OBJECT
public:
    explicit ZoomBar(TimelineView *v);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
private:
    QRect thumb() const;
    TimelineView *v;
    int mode = 0, pressX = 0;
    double pressScroll = 0, pressPpf = 0;
};

class TimelinePanel : public Panel {
    Q_OBJECT
public:
    explicit TimelinePanel(QWidget *p = nullptr);
    void fillMenu(QMenu *m) override;
    TimelineView *view;
    // Commands shared by menus, shortcuts and context menus
    void razorAtPlayhead(bool allTracks);
    void deleteSelection(bool ripple);
    void liftRange();
    void extractRange();
    void copySelection(bool cut);
    void paste(bool insert);
    void pasteAttributes();
    void toggleLink();
    void toggleEnable();
    void speedDialog();
    void nestSelection();
    void addTransitionAtPlayhead(bool video, bool audio);
    void addMarker();
    void trimToPlayhead(bool head);
    void selectAll();
    void addTextClip();
    void markSelection();
    void frameHold();
    void scaleToFrame(bool setScale);
    void addTrackDialog();
private:
    void rebuildTabs();
    QTabBar *tabs;
    TimecodeLabel *tc;
    QToolButton *snapBtn, *linkBtn;
    ZoomBar *zoom;
};
