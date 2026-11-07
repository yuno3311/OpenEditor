#include "panels.h"
#include "theme.h"
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

static const int kTabH = 26;

PanelTabs::PanelTabs(PanelGroup *gr) : QWidget(gr), g(gr)
{
    setFixedHeight(kTabH);
    setMouseTracking(true);
}
int PanelTabs::tabWidth(int i) const { return fontMetrics().horizontalAdvance(g->panels[i]->title) + 22; }
QRect PanelTabs::menuRect() const { return QRect(width() - 24, 3, 20, 20); }
int PanelTabs::tabAt(int mx) const
{
    int x = 0;
    for (int i = 0; i < g->panels.size(); ++i) {
        int w = tabWidth(i);
        if (mx >= x && mx < x + w) return i;
        x += w;
    }
    return -1;
}
void PanelTabs::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Theme::bgDark);
    int x = 0;
    auto *cur = g->current();
    // Tabs that do not fit are reachable from the panel menu, as is every open panel.
    p.setClipRect(0, 0, width() > 70 ? width() - 26 : width(), kTabH);
    for (int i = 0; i < g->panels.size(); ++i) {
        int w = tabWidth(i);
        bool on = g->panels[i] == cur;
        QRect r(x, 0, w, kTabH);
        if (on) p.fillRect(r, Theme::bg);
        else if (i == hover) p.fillRect(r, QColor("#262626"));
        QFont f = font();
        f.setBold(on && g->active);
        p.setFont(f);
        p.setPen(on ? Theme::textBright : Theme::textDim);
        p.drawText(r, Qt::AlignCenter, g->panels[i]->title);
        if (on) {
            p.fillRect(QRect(x + 8, kTabH - 3, w - 16, 2), g->active ? Theme::accent : QColor("#6a6a6a"));
        }
        x += w;
    }
    p.setClipping(false);
    if (cur && width() > 70) p.drawPixmap(menuRect().adjusted(3, 3, -3, -3), Theme::pixmap("menu", 14, Theme::textDim));
}
void PanelTabs::mousePressEvent(QMouseEvent *e)
{
    auto *cur = g->current();
    if (cur && width() > 70 && menuRect().contains(e->pos())) {
        QMenu m;
        m.addAction(tr("Close Panel"), [this, cur] { g->mgr->closePanel(cur); });
        m.addAction(tr("Maximize Frame"), [this] { g->mgr->toggleMaximize(); });
        m.addSeparator();
        cur->fillMenu(&m);
        if (g->panels.size() > 1) {
            m.addSeparator();
            for (auto *pn : std::as_const(g->panels)) {
                auto *a = m.addAction(pn->title, [this, pn] { g->activate(pn); });
                a->setCheckable(true);
                a->setChecked(pn == cur);
            }
        }
        m.exec(mapToGlobal(menuRect().bottomLeft()));
        return;
    }
    pressed = tabAt(e->pos().x());
    pressPos = e->pos();
    dragging = false;
    if (pressed >= 0 && e->button() == Qt::LeftButton) {
        g->activate(g->panels[pressed]);
        g->mgr->setActiveGroup(g);
    }
}
void PanelTabs::mouseMoveEvent(QMouseEvent *e)
{
    int h = tabAt(e->pos().x());
    if (h != hover) { hover = h; update(); }
    if (pressed < 0 || !(e->buttons() & Qt::LeftButton)) return;
    if (!dragging && (e->pos() - pressPos).manhattanLength() > 10) {
        dragging = true;
        g->mgr->beginDrag(g->panels[pressed]);
    }
    if (dragging) g->mgr->dragMove(e->globalPosition().toPoint());
}
void PanelTabs::mouseReleaseEvent(QMouseEvent *e)
{
    bool d = dragging;
    dragging = false;
    pressed = -1;
    if (d) g->mgr->dragEnd(e->globalPosition().toPoint()); // may delete this group: touch nothing after
}
void PanelTabs::mouseDoubleClickEvent(QMouseEvent *) { g->mgr->toggleMaximize(); }
void PanelTabs::leaveEvent(QEvent *) { hover = -1; update(); }

PanelGroup::PanelGroup(PanelManager *m) : mgr(m)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(1, 0, 1, 1);
    lay->setSpacing(0);
    tabs = new PanelTabs(this);
    stack = new QStackedWidget(this);
    lay->addWidget(tabs);
    lay->addWidget(stack, 1);
    setMinimumSize(60, 50);
}
void PanelGroup::addPanel(Panel *p, bool act)
{
    panels << p;
    stack->addWidget(p);
    if (act) activate(p);
    tabs->update();
}
void PanelGroup::removePanel(Panel *p)
{
    panels.removeAll(p);
    stack->removeWidget(p);
    tabs->update();
}
void PanelGroup::activate(Panel *p)
{
    stack->setCurrentWidget(p);
    // Narrow utility panels (Tools, Audio Meters) keep their group narrow.
    setMaximumWidth(panels.size() == 1 ? qMin(p->maximumWidth() + 2, QWIDGETSIZE_MAX) : QWIDGETSIZE_MAX);
    tabs->update();
}
Panel *PanelGroup::current() const { return qobject_cast<Panel *>(stack->currentWidget()); }
void PanelGroup::setActive(bool a) { active = a; update(); tabs->update(); }
void PanelGroup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Theme::bg);
    if (active) {
        p.setPen(QPen(Theme::accent, 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

PanelManager::PanelManager(QWidget *h) : QObject(h), host(h)
{
    parking = new QWidget(h);
    parking->hide();
    overlay = new QWidget(h);
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay->setStyleSheet("background: rgba(45,140,235,70); border: 2px solid #2d8ceb;");
    overlay->hide();
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (QWidget *w = now; w; w = w->parentWidget())
            if (auto *g = qobject_cast<PanelGroup *>(w)) { setActiveGroup(g); return; }
    });
}
void PanelManager::registerPanel(Panel *p)
{
    all[p->id] = p;
    p->setParent(parking);
}
void PanelManager::setActiveGroup(PanelGroup *g)
{
    if (activeGroup == g) return;
    if (activeGroup) activeGroup->setActive(false);
    activeGroup = g;
    if (g) g->setActive(true);
    emit activePanelChanged(activePanel());
}
Panel *PanelManager::activePanel() const { return activeGroup ? activeGroup->current() : nullptr; }

// layout grammar:  h(30 [a b] 70 v(50 [c] 50 [d]))   first id in brackets may be prefixed * to activate
QWidget *PanelManager::parseNode(const QString &s, int &i)
{
    auto ws = [&] { while (i < s.size() && s[i].isSpace()) ++i; };
    ws();
    if (i >= s.size()) return nullptr;
    QChar c = s[i];
    if (c == '[') {
        ++i;
        auto *g = new PanelGroup(this);
        Panel *act = nullptr;
        QString tok;
        auto flush = [&] {
            if (tok.isEmpty()) return;
            bool star = tok.startsWith('*');
            auto *p = all.value(star ? tok.mid(1) : tok);
            tok.clear();
            if (!p) return;
            g->addPanel(p, false);
            if (star) act = p;
        };
        while (i < s.size() && s[i] != ']') {
            if (s[i].isSpace()) flush(); else tok += s[i];
            ++i;
        }
        flush();
        ++i;
        if (act) g->activate(act);
        else if (!g->panels.isEmpty()) g->activate(g->panels.first());
        return g;
    }
    bool horiz = c == 'h';
    i += 2; // skip h(
    auto *sp = new QSplitter(horiz ? Qt::Horizontal : Qt::Vertical);
    sp->setHandleWidth(3);
    sp->setChildrenCollapsible(false);
    QList<int> sizes;
    for (;;) {
        ws();
        if (i >= s.size() || s[i] == ')') { ++i; break; }
        QString num;
        while (i < s.size() && (s[i].isDigit() || s[i] == '.')) num += s[i++];
        sizes << qMax(1, int(num.toDouble() * 100));
        QWidget *child = parseNode(s, i);
        if (child) sp->addWidget(child);
    }
    sp->setProperty("wanted", QVariant::fromValue(sizes));
    for (int k = 0; k < sizes.size() && k < sp->count(); ++k) sp->setStretchFactor(k, sizes[k]);
    return sp;
}

static void applySizes(QWidget *w)
{
    auto *sp = qobject_cast<QSplitter *>(w);
    if (!sp) return;
    auto want = sp->property("wanted").value<QList<int>>();
    long long tot = 0;
    for (int v : want) tot += v;
    int avail = sp->orientation() == Qt::Horizontal ? sp->width() : sp->height();
    if (tot > 0 && want.size() == sp->count()) {
        QList<int> s;
        for (int v : want) s << int(avail * double(v) / tot);
        sp->setSizes(s);
    }
    for (int i = 0; i < sp->count(); ++i) applySizes(sp->widget(i));
}

void PanelManager::applyLayout(const QString &dsl)
{
    maximized = nullptr;
    for (auto *p : std::as_const(all)) { p->hide(); p->setParent(parking); }
    activeGroup = nullptr;
    if (root) { root->hide(); root->deleteLater(); root = nullptr; }
    int i = 0;
    root = parseNode(dsl, i);
    if (!root) return;
    auto *lay = host->layout();
    lay->addWidget(root);
    root->show(); // each group's stack shows only its current page
    QTimer::singleShot(0, this, [r = QPointer<QWidget>(root)] { if (r) applySizes(r); });
}

QString PanelManager::serialize(QWidget *w) const
{
    if (auto *g = qobject_cast<PanelGroup *>(w)) {
        QStringList ids;
        for (auto *p : g->panels) ids << (p == g->current() ? "*" + p->id : p->id);
        return "[" + ids.join(' ') + "]";
    }
    auto *sp = qobject_cast<QSplitter *>(w);
    if (!sp) return "[]";
    QString r = sp->orientation() == Qt::Horizontal ? "h(" : "v(";
    auto sizes = sp->sizes();
    double tot = 0;
    for (int v : sizes) tot += v;
    for (int i = 0; i < sp->count(); ++i)
        r += QString::number(sizes[i] / qMax(1.0, tot) * 100, 'f', 2) + " " + serialize(sp->widget(i)) + " ";
    return r + ")";
}
QString PanelManager::saveLayout() const { return root ? serialize(root) : QString(); }

PanelGroup *PanelManager::anyGroup() const { return root ? root->findChild<PanelGroup *>() : nullptr; }

void PanelManager::showPanel(const QString &id, const QString &prefer)
{
    auto *p = all.value(id);
    if (!p) return;
    if (auto *g = qobject_cast<PanelGroup *>(p->parentWidget() ? p->parentWidget()->parentWidget() : nullptr)) {
        g->activate(p);
        return;
    }
    PanelGroup *g = nullptr;
    if (!prefer.isEmpty() && all.value(prefer) && all[prefer]->parentWidget())
        g = qobject_cast<PanelGroup *>(all[prefer]->parentWidget()->parentWidget());
    if (!g) g = activeGroup ? activeGroup.data() : anyGroup();
    if (!g) return;
    g->addPanel(p);
    p->show();
}
bool PanelManager::isVisible(const QString &id) const
{
    auto *p = all.value(id);
    return p && p->parent() != parking;
}
void PanelManager::closePanel(Panel *p)
{
    auto *g = qobject_cast<PanelGroup *>(p->parentWidget() ? p->parentWidget()->parentWidget() : nullptr);
    if (!g) return;
    g->removePanel(p);
    p->hide();
    p->setParent(parking);
    pruneEmpty(g);
}
void PanelManager::togglePanel(const QString &id)
{
    if (isVisible(id)) closePanel(all[id]);
    else showPanel(id);
}
void PanelManager::focusPanel(const QString &id)
{
    showPanel(id);
    if (auto *p = all.value(id)) {
        if (auto *g = qobject_cast<PanelGroup *>(p->parentWidget() ? p->parentWidget()->parentWidget() : nullptr)) setActiveGroup(g);
        p->setFocus();
    }
}

void PanelManager::pruneEmpty(PanelGroup *g)
{
    if (!g->panels.isEmpty() || g == root) return;
    if (activeGroup == g) activeGroup = nullptr;
    auto *sp = qobject_cast<QSplitter *>(g->parentWidget());
    g->hide();
    g->setParent(nullptr);
    g->deleteLater();
    // collapse splitters left with a single child
    while (sp && sp->count() == 1) {
        QWidget *only = sp->widget(0);
        auto *parentSp = qobject_cast<QSplitter *>(sp->parentWidget());
        if (parentSp) {
            int idx = parentSp->indexOf(sp);
            auto sizes = parentSp->sizes();
            parentSp->insertWidget(idx, only);
            sp->hide();
            sp->setParent(nullptr);
            sp->deleteLater();
            parentSp->setSizes(sizes);
            sp = parentSp;
        } else {
            if (sp != root) break;
            host->layout()->removeWidget(sp);
            only->setParent(nullptr);
            host->layout()->addWidget(only);
            only->show();
            sp->hide();
            sp->deleteLater();
            root = only;
            break;
        }
    }
}

PanelGroup *PanelManager::groupAt(const QPoint &global, Zone &z, QRect &r) const
{
    z = None;
    QWidget *w = QApplication::widgetAt(global);
    PanelGroup *g = nullptr;
    for (; w; w = w->parentWidget())
        if ((g = qobject_cast<PanelGroup *>(w))) break;
    if (!g) return nullptr;
    QPoint l = g->mapFromGlobal(global);
    r = QRect(g->mapTo(host, QPoint(0, 0)), g->size());
    double u = double(l.x()) / g->width(), v = double(l.y()) / g->height();
    if (l.y() < kTabH + 2) { z = Center; return g; }
    if (u < .22) { z = Left; r.setWidth(r.width() / 2); }
    else if (u > .78) { z = Right; r.setLeft(r.left() + r.width() / 2); }
    else if (v < .25) { z = Top; r.setHeight(r.height() / 2); }
    else if (v > .78) { z = Bottom; r.setTop(r.top() + r.height() / 2); }
    else z = Center;
    return g;
}
void PanelManager::beginDrag(Panel *p) { dragPanel = p; QApplication::setOverrideCursor(Qt::ClosedHandCursor); }
void PanelManager::dragMove(const QPoint &global)
{
    Zone z; QRect r;
    groupAt(global, z, r);
    if (z == None) { overlay->hide(); return; }
    overlay->setGeometry(r);
    overlay->show();
    overlay->raise();
}
void PanelManager::dragEnd(const QPoint &global)
{
    QApplication::restoreOverrideCursor();
    overlay->hide();
    Panel *p = dragPanel;
    dragPanel = nullptr;
    if (!p) return;
    Zone z; QRect r;
    PanelGroup *dst = groupAt(global, z, r);
    auto *src = qobject_cast<PanelGroup *>(p->parentWidget() ? p->parentWidget()->parentWidget() : nullptr);
    if (!dst || !src || z == None) return;
    if (dst == src && (z == Center || src->panels.size() == 1)) return;
    // Deferred: the drag came from a child of src, which may be deleted by pruneEmpty.
    QTimer::singleShot(0, this, [=, this] {
        src->removePanel(p);
        if (z == Center) {
            dst->addPanel(p);
            p->show();
        } else {
            auto *ng = new PanelGroup(this);
            ng->addPanel(p);
            p->show();
            auto o = (z == Left || z == Right) ? Qt::Horizontal : Qt::Vertical;
            bool before = z == Left || z == Top;
            auto *sp = qobject_cast<QSplitter *>(dst->parentWidget());
            if (sp && sp->orientation() == o) {
                int idx = sp->indexOf(dst);
                auto sizes = sp->sizes();
                int half = sizes[idx] / 2;
                sizes[idx] = half;
                sp->insertWidget(before ? idx : idx + 1, ng);
                sizes.insert(before ? idx : idx + 1, half);
                sp->setSizes(sizes);
            } else {
                auto *nsp = new QSplitter(o);
                nsp->setHandleWidth(3);
                nsp->setChildrenCollapsible(false);
                if (sp) {
                    int idx = sp->indexOf(dst);
                    auto sizes = sp->sizes();
                    sp->insertWidget(idx, nsp);
                    sp->setSizes(sizes);
                } else {
                    host->layout()->removeWidget(root);
                    host->layout()->addWidget(nsp);
                    root = nsp;
                }
                nsp->addWidget(before ? static_cast<QWidget *>(ng) : dst);
                nsp->addWidget(before ? dst : static_cast<QWidget *>(ng));
                nsp->setSizes({1000, 1000});
                nsp->show();
            }
            ng->show();
        }
        pruneEmpty(src);
    });
}

void PanelManager::toggleMaximize()
{
    if (!root) return;
    if (maximized) {
        QString l = beforeMax;
        maximized = nullptr;
        applyLayout(l);
        return;
    }
    if (!activeGroup) return;
    beforeMax = saveLayout();
    QStringList ids;
    for (auto *p : activeGroup->panels) ids << (p == activeGroup->current() ? "*" + p->id : p->id);
    applyLayout("[" + ids.join(' ') + "]");
    maximized = root->findChild<PanelGroup *>();
    if (!maximized) maximized = qobject_cast<PanelGroup *>(root);
    setActiveGroup(maximized);
}
