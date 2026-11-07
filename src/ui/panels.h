// Tabbed docking system: panels live in groups inside nested splitters; dragging a tab onto another
// group adds it there, dragging it to a group's edge splits that group.
#pragma once
#include <QFrame>
#include <QHash>
#include <QMenu>
#include <QPointer>
#include <QSplitter>
#include <QStackedWidget>
#include <QVector>

class PanelGroup;
class PanelManager;

class Panel : public QWidget {
    Q_OBJECT
public:
    Panel(const QString &id, const QString &title, QWidget *p = nullptr) : QWidget(p), id(id), title(title) {}
    QString id, title;
    virtual void fillMenu(QMenu *) {}
};

class PanelTabs : public QWidget {
    Q_OBJECT
public:
    explicit PanelTabs(PanelGroup *g);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    int tabAt(int x) const;
    int tabWidth(int i) const;
    QRect menuRect() const;
    PanelGroup *g;
    int pressed = -1, hover = -1;
    QPoint pressPos;
    bool dragging = false;
};

class PanelGroup : public QWidget {
    Q_OBJECT
public:
    explicit PanelGroup(PanelManager *m);
    void addPanel(Panel *p, bool activate = true);
    void removePanel(Panel *p);
    void activate(Panel *p);
    Panel *current() const;
    QVector<Panel *> panels;
    PanelManager *mgr;
    bool active = false;
    void setActive(bool a);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    friend class PanelTabs;
    PanelTabs *tabs;
    QStackedWidget *stack;
};

class PanelManager : public QObject {
    Q_OBJECT
public:
    explicit PanelManager(QWidget *host);
    void registerPanel(Panel *p);
    Panel *panel(const QString &id) const { return all.value(id); }
    const QHash<QString, Panel *> &panels() const { return all; }
    void applyLayout(const QString &dsl);
    QString saveLayout() const;
    void showPanel(const QString &id, const QString &preferGroupOf = {});
    void togglePanel(const QString &id);
    void closePanel(Panel *p);
    bool isVisible(const QString &id) const;
    void focusPanel(const QString &id);
    Panel *activePanel() const;
    void toggleMaximize();
    void beginDrag(Panel *p);
    void dragMove(const QPoint &global);
    void dragEnd(const QPoint &global);
    void setActiveGroup(PanelGroup *g);
signals:
    void activePanelChanged(Panel *p);
private:
    enum Zone { None, Center, Left, Right, Top, Bottom };
    QWidget *parseNode(const QString &s, int &i);
    QString serialize(QWidget *w) const;
    PanelGroup *groupAt(const QPoint &global, Zone &z, QRect &rectOut) const;
    void pruneEmpty(PanelGroup *g);
    PanelGroup *anyGroup() const;
    QWidget *host;
    QWidget *root = nullptr;
    QWidget *parking;
    QWidget *overlay;
    QHash<QString, Panel *> all;
    Panel *dragPanel = nullptr;
    QPointer<PanelGroup> activeGroup;
    QPointer<PanelGroup> maximized;
    QString beforeMax;
};
