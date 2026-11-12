#include "effectcontrols.h"
#include "../core/effectdefs.h"
#include "../core/ops.h"
#include "../core/project.h"
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

using namespace Theme;
static const int kRowH = 23, kLeft = 310;

static Clip *liveClip(int id) { auto q = PRJ->active(); return q ? q->clip(id) : nullptr; }
static Param *liveParam(int clipId, int fx, const QString &id)
{
    auto *c = liveClip(clipId);
    return c && fx < c->fx.size() ? c->fx[fx].find(id) : nullptr;
}

KeyLane::KeyLane(int c, int f, const QString &pr, QWidget *p) : QWidget(p), clipId(c), fx(f), param(pr)
{
    setFixedHeight(kRowH);
    setMinimumWidth(100);
}
int KeyLane::xOf(Frame kt) const
{
    auto *c = liveClip(clipId);
    if (!c || c->dur <= 0) return 0;
    return 6 + int(double(kt - c->kfOffset) / c->dur * (width() - 12));
}
Frame KeyLane::tOf(int x) const
{
    auto *c = liveClip(clipId);
    if (!c) return 0;
    return c->kfOffset + Frame(double(x - 6) / qMax(1, width() - 12) * c->dur + .5);
}
void KeyLane::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), bgDark);
    auto *q = PRJ->active();
    auto *c = liveClip(clipId);
    if (!q || !c) return;
    int xp = 6 + int(double(q->playhead - c->start) / qMax<Frame>(1, c->dur) * (width() - 12));
    if (!param.isEmpty()) {
        if (auto *pr = liveParam(clipId, fx, param)) {
            if (pr->animated) {
                p.setRenderHint(QPainter::Antialiasing);
                for (auto &k : pr->keys) {
                    int x = xOf(k.t), y = height() / 2;
                    QPolygon d;
                    bool now = k.t == c->kt(q->playhead);
                    if (k.i == Interp::Hold) { p.fillRect(x - 3, y - 4, 6, 8, now ? accent : QColor("#ddd")); continue; }
                    d << QPoint(x, y - 5) << QPoint(x + 5, y) << QPoint(x, y + 5) << QPoint(x - 5, y);
                    p.setBrush(now ? accent : (k.i == Interp::Ease || k.i == Interp::EaseIn || k.i == Interp::EaseOut) ? QColor("#e8c040") : QColor("#ddd"));
                    p.setPen(Qt::black);
                    p.drawPolygon(d);
                }
            }
        }
    }
    p.setPen(accent);
    p.drawLine(xp, 0, xp, height());
}
void KeyLane::mousePressEvent(QMouseEvent *e)
{
    auto *pr = liveParam(clipId, fx, param);
    dragKey = -1;
    auto *c = liveClip(clipId);
    if (!c) return;
    if (pr && pr->animated)
        for (int i = 0; i < pr->keys.size(); ++i)
            if (std::abs(xOf(pr->keys[i].t) - e->pos().x()) <= 6) dragKey = i;
    if (e->button() == Qt::RightButton && dragKey >= 0) {
        int k = dragKey;
        dragKey = -1;
        auto set = [=](Interp in) {
            PRJ->edit(tr("Keyframe Interpolation"), [&](ProjectState &st) {
                if (auto *x = st.seqById(st.activeSeq)->clip(clipId)) if (fx < x->fx.size()) if (auto *pa = x->fx[fx].find(param)) if (k < pa->keys.size()) pa->keys[k].i = in;
            });
        };
        QMenu m;
        m.addAction(tr("Linear"), [=] { set(Interp::Linear); });
        m.addAction(tr("Ease (Bezier)"), [=] { set(Interp::Ease); });
        m.addAction(tr("Ease In"), [=] { set(Interp::EaseIn); });
        m.addAction(tr("Ease Out"), [=] { set(Interp::EaseOut); });
        m.addAction(tr("Hold"), [=] { set(Interp::Hold); });
        m.addSeparator();
        m.addAction(tr("Delete"), [=] {
            PRJ->edit(tr("Delete Keyframe"), [&](ProjectState &st) {
                if (auto *x = st.seqById(st.activeSeq)->clip(clipId)) if (auto *pa = x->fx[fx].find(param)) { pa->value = pa->keys[k].v; pa->keys.removeAt(k); if (pa->keys.isEmpty()) pa->animated = false; }
            });
        });
        m.exec(e->globalPosition().toPoint());
        return;
    }
    if (dragKey >= 0) { PRJ->liveBegin(); return; }
    PRJ->setPlayhead(c->start + Frame(double(e->pos().x() - 6) / qMax(1, width() - 12) * c->dur));
}
void KeyLane::mouseMoveEvent(QMouseEvent *e)
{
    auto *pr = liveParam(clipId, fx, param);
    auto *c = liveClip(clipId);
    if (!c) return;
    if (dragKey < 0 || !pr) {
        if (e->buttons() & Qt::LeftButton) PRJ->setPlayhead(c->start + Frame(double(e->pos().x() - 6) / qMax(1, width() - 12) * c->dur));
        return;
    }
    Frame t = tOf(e->pos().x());
    if (dragKey > 0) t = qMax(t, pr->keys[dragKey - 1].t + 1);
    if (dragKey + 1 < pr->keys.size()) t = qMin(t, pr->keys[dragKey + 1].t - 1);
    pr->keys[dragKey].t = t;
    PRJ->liveTouch();
    update();
}
void KeyLane::mouseReleaseEvent(QMouseEvent *)
{
    if (dragKey >= 0) { dragKey = -1; PRJ->liveCommit(tr("Move Keyframe")); }
}

LaneRuler::LaneRuler(QWidget *p) : QWidget(p) { setFixedHeight(22); }
void LaneRuler::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), bgDark);
    auto *q = PRJ->active();
    auto *c = clipId ? liveClip(clipId) : nullptr;
    if (!q || !c) return;
    QFont f = font(); f.setPixelSize(10); p.setFont(f);
    p.setPen(textDim);
    p.drawText(rect().adjusted(6, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, framesToTimecode(c->start, q->fps(), q->dropFrame));
    p.drawText(rect().adjusted(0, 0, -6, 0), Qt::AlignVCenter | Qt::AlignRight, framesToTimecode(c->end(), q->fps(), q->dropFrame));
    int x = 6 + int(double(q->playhead - c->start) / qMax<Frame>(1, c->dur) * (width() - 12));
    p.setBrush(accent);
    p.setPen(Qt::NoPen);
    p.drawPolygon(QPolygon({QPoint(x - 5, 8), QPoint(x + 5, 8), QPoint(x, 18)}));
    p.setPen(accent);
    p.drawLine(x, 18, x, height());
}
void LaneRuler::mousePressEvent(QMouseEvent *e)
{
    if (auto *c = clipId ? liveClip(clipId) : nullptr)
        PRJ->setPlayhead(c->start + Frame(double(e->pos().x() - 6) / qMax(1, width() - 12) * c->dur));
}
void LaneRuler::mouseMoveEvent(QMouseEvent *e) { mousePressEvent(e); }

EffectControlsPanel::EffectControlsPanel(QWidget *p) : Panel("effectcontrols", tr("Effect Controls"), p)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    head = new QLabel(this);
    head->setContentsMargins(8, 3, 8, 3);
    head->setStyleSheet("color:#aaa; background:#1f1f1f;");
    v->addWidget(head);
    scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    v->addWidget(scroll, 1);
    connect(PRJ, &Project::selectionChanged, this, &EffectControlsPanel::rebuild);
    connect(PRJ, &Project::changed, this, [this] { if (signature() != sig) rebuild(); else refresh(); });
    connect(PRJ, &Project::playheadChanged, this, &EffectControlsPanel::refresh);
    rebuild();
}

QString EffectControlsPanel::signature() const
{
    QString s;
    auto q = PRJ->active();
    if (!q) return s;
    auto sel = PRJ->selClips.values();
    std::sort(sel.begin(), sel.end());
    for (int id : sel) if (auto *c = q->clip(id)) {
        s += QString::number(id) + ":";
        for (auto &e : c->fx) s += e.type + (e.enabled ? "1," : "0,");
    }
    s += QString("|t%1:%2").arg(PRJ->selTransition.first).arg(PRJ->selTransition.second);
    return s;
}

void EffectControlsPanel::refresh()
{
    for (auto &u : updaters) u();
    for (auto *w : std::as_const(lanes)) w->update();
}

void EffectControlsPanel::rebuild()
{
    sig = signature();
    updaters.clear();
    lanes.clear();
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto q = PRJ->active();
    QVector<int> clips;
    if (q) {
        for (auto &t : q->video) for (auto &c : t.clips) if (PRJ->selClips.contains(c.id)) clips << c.id;
        for (auto &t : q->audio) for (auto &c : t.clips) if (PRJ->selClips.contains(c.id)) clips << c.id;
    }
    if (PRJ->selTransition.first) {
        head->setText(tr("Transition"));
        transitionRows(lay);
    } else if (clips.isEmpty()) {
        head->setText(q ? tr("Source: (no clips)  /  %1").arg(q->name) : tr("(no sequence)"));
        auto *l = new QLabel(tr("Select a clip in the timeline to edit its effects."), w);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet("color:#777;padding:30px;");
        lay->addWidget(l);
    } else {
        auto *c0 = q->clip(clips.first());
        head->setText(QString("%1  /  %2").arg(c0->name, q->name));
        auto *rr = new QWidget(w);
        auto *rh = new QHBoxLayout(rr);
        rh->setContentsMargins(kLeft, 0, 0, 0);
        auto *ruler = new LaneRuler(rr);
        ruler->clipId = clips.first();
        rh->addWidget(ruler);
        lanes << ruler;
        lay->addWidget(rr);
        // At most one video and one audio clip: the linked pair. Multi-select edits stay readable.
        bool v = false, a = false;
        for (int id : clips) {
            bool isA = q->clip(id)->kind == MediaKind::Audio;
            if ((isA && a) || (!isA && v)) continue;
            (isA ? a : v) = true;
            section(lay, id);
        }
    }
    lay->addStretch(1);
    delete scroll->takeWidget();
    scroll->setWidget(w);
    body = w;
}

void EffectControlsPanel::section(QVBoxLayout *lay, int clipId)
{
    auto *c = liveClip(clipId);
    if (!c) return;
    auto *h = new QLabel(c->kind == MediaKind::Audio ? tr("Audio") : tr("Video"), body);
    h->setStyleSheet("background:#2e2e2e;color:#e0e0e0;font-weight:600;padding:3px 8px;");
    lay->addWidget(h);
    for (int i = 0; i < c->fx.size(); ++i) addEffect(lay, clipId, i);
}

void EffectControlsPanel::addEffect(QVBoxLayout *lay, int clipId, int fxIndex)
{
    auto *c = liveClip(clipId);
    const Effect &e = c->fx[fxIndex];
    const EffectDef *def = EffectRegistry::find(e.type);
    if (!def) return;
    QString key = QString("%1/%2").arg(clipId).arg(e.type);
    if (e.type == "colorgrade" && !collapsed.contains("!" + key)) collapsed << key;
    bool isCollapsed = collapsed.contains(key);

    auto *hdr = new QWidget;
    hdr->setFixedHeight(kRowH);
    hdr->setStyleSheet("background:#262626;");
    auto *hl = new QHBoxLayout(hdr);
    hl->setContentsMargins(4, 0, 0, 0);
    hl->setSpacing(4);
    auto *chev = iconButton(isCollapsed ? "chevright" : "chevdown", tr("Expand"), hdr, 18);
    connect(chev, &QToolButton::clicked, this, [=, this] {
        if (collapsed.contains(key)) { collapsed.remove(key); collapsed << "!" + key; } else { collapsed << key; collapsed.remove("!" + key); }
        rebuild();
    });
    auto *fxb = new QToolButton(hdr);
    fxb->setText("fx");
    fxb->setCheckable(true);
    fxb->setChecked(e.enabled);
    fxb->setFixedSize(20, 17);
    fxb->setFocusPolicy(Qt::NoFocus);
    fxb->setToolTip(tr("Toggle the effect on or off"));
    fxb->setStyleSheet("QToolButton{font:italic bold 11px 'Segoe UI';color:#777;background:#1a1a1a;border:1px solid #444;border-radius:2px;}"
                       "QToolButton:checked{color:#2d8ceb;border-color:#2d8ceb;}");
    connect(fxb, &QToolButton::clicked, this, [=](bool on) {
        PRJ->edit(tr("Toggle Effect"), [&](ProjectState &st) { if (auto *x = st.seqById(st.activeSeq)->clip(clipId)) x->fx[fxIndex].enabled = on; });
    });
    auto *nm = new QLabel(def->name, hdr);
    nm->setStyleSheet("color:#e0e0e0;");
    hl->addWidget(chev); hl->addWidget(fxb); hl->addWidget(nm); hl->addStretch(1);
    auto *rst = iconButton("reset", tr("Reset Parameters"), hdr, 18);
    connect(rst, &QToolButton::clicked, this, [=] {
        PRJ->edit(tr("Reset Effect"), [&](ProjectState &st) {
            auto &q = *st.seqById(st.activeSeq);
            auto *x = q.clip(clipId);
            Effect f = EffectRegistry::make(x->fx[fxIndex].type);
            if (f.type == "motion") {
                f.find("posX")->value = q.w / 2.0; f.find("posY")->value = q.h / 2.0;
                f.find("ancX")->value = x->fx[fxIndex].find("ancX")->value; f.find("ancY")->value = x->fx[fxIndex].find("ancY")->value;
            }
            x->fx[fxIndex] = f;
        });
    });
    hl->addWidget(rst);
    if (!def->intrinsic) {
        auto *del = iconButton("trash", tr("Remove Effect"), hdr, 18);
        connect(del, &QToolButton::clicked, this, [=] {
            PRJ->edit(tr("Remove Effect"), [&](ProjectState &st) { if (auto *x = st.seqById(st.activeSeq)->clip(clipId)) x->fx.removeAt(fxIndex); });
        });
        hl->addWidget(del);
    }
    hl->setContentsMargins(4, 0, qMax(0, scroll->viewport()->width() - kLeft) + 4, 0);
    hdr->setMaximumWidth(16777215);
    auto *hw = new QWidget;
    auto *hwl = new QHBoxLayout(hw);
    hwl->setContentsMargins(0, 0, 0, 0);
    hwl->setSpacing(0);
    hl->setContentsMargins(4, 0, 4, 0);
    hdr->setFixedWidth(kLeft);
    hwl->addWidget(hdr);
    auto *lane0 = new KeyLane(clipId, fxIndex, QString(), hw);
    lanes << lane0;
    hwl->addWidget(lane0, 1);
    lay->addWidget(hw);
    if (isCollapsed) return;

    for (int pi = 0; pi < def->params.size(); ++pi) {
        const ParamDef &pd = def->params[pi];
        if (pd.kind == ParamKind::PointY) continue;
        if (e.type == "motion" && pd.id == "scaleW") continue;
        auto *row = new QWidget;
        row->setFixedHeight(kRowH);
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(0);
        auto *left = new QWidget(row);
        left->setFixedWidth(kLeft);
        auto *h = new QHBoxLayout(left);
        h->setContentsMargins(22, 0, 4, 0);
        h->setSpacing(4);
        const QString pid = pd.id;
        QToolButton *sw = nullptr;
        if (pd.animatable) {
            sw = iconButton("stopwatch", tr("Toggle animation"), left, 18, true);
            connect(sw, &QToolButton::clicked, this, [=](bool on) {
                PRJ->edit(on ? tr("Enable Keyframes") : tr("Disable Keyframes"), [&](ProjectState &st) {
                    auto &q = *st.seqById(st.activeSeq);
                    auto *x = q.clip(clipId);
                    auto apply = [&](const QString &id) {
                        auto *pa = x->fx[fxIndex].find(id);
                        if (!pa) return;
                        double cur = pa->at(x->kt(q.playhead));
                        pa->animated = on;
                        pa->keys.clear();
                        pa->value = cur;
                        if (on) pa->setKey(x->kt(q.playhead), cur);
                    };
                    apply(pid);
                    if (pd.kind == ParamKind::PointX && pi + 1 < def->params.size()) apply(def->params[pi + 1].id);
                });
            });
            h->addWidget(sw);
        } else h->addSpacing(18);
        auto *lb = new QLabel(pd.name, left);
        lb->setFixedWidth(118);
        h->addWidget(lb);
        auto value = [=](const QString &id) {
            auto *x = liveClip(clipId);
            auto *pa = liveParam(clipId, fxIndex, id);
            auto q = PRJ->active();
            return pa && x ? pa->at(x->kt(q->playhead)) : 0.0;
        };
        auto setV = [=](const QString &id, double v) {
            auto *x = liveClip(clipId);
            auto *pa = liveParam(clipId, fxIndex, id);
            if (!pa || !x) return;
            if (pa->animated) pa->setKey(x->kt(PRJ->active()->playhead), v);
            else pa->value = v;
            PRJ->liveTouch();
        };
        auto addScrub = [&](const ParamDef &d) {
            auto *s = new ScrubValue(left);
            s->setRange(d.min, d.max);
            s->setStep(d.step);
            s->setDecimals(d.step >= 1 ? 1 : 2);
            s->setSuffix(d.unit);
            QString id = d.id;
            s->setValue(value(id));
            connect(s, &ScrubValue::dragBegan, this, [] { PRJ->liveBegin(); });
            connect(s, &ScrubValue::edited, this, [=](double v) { setV(id, v); });
            connect(s, &ScrubValue::dragEnded, this, [n = d.name] { PRJ->liveCommit(tr("Change %1").arg(n)); });
            updaters << [=] { s->setValue(value(id)); };
            h->addWidget(s);
        };
        switch (pd.kind) {
        case ParamKind::Toggle: {
            auto *cb = new QCheckBox(left);
            cb->setChecked(value(pid) > .5);
            connect(cb, &QCheckBox::clicked, this, [=](bool on) {
                PRJ->edit(tr("Change %1").arg(pd.name), [&](ProjectState &st) { if (auto *x = st.seqById(st.activeSeq)->clip(clipId)) x->fx[fxIndex].find(pid)->value = on; });
            });
            h->addWidget(cb);
            break;
        }
        case ParamKind::Choice: {
            auto *cb = new QComboBox(left);
            cb->addItems(pd.choices);
            cb->setCurrentIndex(int(value(pid)));
            cb->setFocusPolicy(Qt::NoFocus);
            connect(cb, &QComboBox::activated, this, [=](int i) {
                PRJ->edit(tr("Change %1").arg(pd.name), [&](ProjectState &st) { if (auto *x = st.seqById(st.activeSeq)->clip(clipId)) x->fx[fxIndex].find(pid)->value = i; });
            });
            h->addWidget(cb, 1);
            break;
        }
        case ParamKind::Color: {
            auto *sw2 = new ColorSwatch(left);
            sw2->setColor(QColor::fromRgb(QRgb(int(value(pid)))));
            connect(sw2, &ColorSwatch::changed, this, [=](const QColor &col) {
                PRJ->edit(tr("Change %1").arg(pd.name), [&](ProjectState &st) { if (auto *x = st.seqById(st.activeSeq)->clip(clipId)) x->fx[fxIndex].find(pid)->value = double(col.rgb() & 0xFFFFFF); });
            });
            h->addWidget(sw2);
            break;
        }
        case ParamKind::PointX:
            addScrub(pd);
            addScrub(def->params[pi + 1]);
            break;
        default:
            addScrub(pd);
        }
        h->addStretch(1);
        if (sw) {
            auto *prev = iconButton("stepback", tr("Go to Previous Keyframe"), left, 16);
            auto *add = iconButton("keyframe", tr("Add/Remove Keyframe"), left, 16);
            auto *next = iconButton("stepfwd", tr("Go to Next Keyframe"), left, 16);
            auto jump = [=](bool fwd) {
                auto *x = liveClip(clipId);
                auto *pa = liveParam(clipId, fxIndex, pid);
                if (!x || !pa) return;
                Frame now = x->kt(PRJ->active()->playhead);
                Frame best = -1;
                for (auto &k : pa->keys) if (fwd ? k.t > now && (best < 0 || k.t < best) : k.t < now && k.t > best) best = k.t;
                if (best >= 0) PRJ->setPlayhead(best - x->kfOffset + x->start);
            };
            connect(prev, &QToolButton::clicked, this, [=] { jump(false); });
            connect(next, &QToolButton::clicked, this, [=] { jump(true); });
            connect(add, &QToolButton::clicked, this, [=] {
                PRJ->edit(tr("Add/Remove Keyframe"), [&](ProjectState &st) {
                    auto &q = *st.seqById(st.activeSeq);
                    auto *x = q.clip(clipId);
                    Frame t = x->kt(q.playhead);
                    auto tog = [&](const QString &id) {
                        auto *pa = x->fx[fxIndex].find(id);
                        if (!pa) return;
                        if (pa->animated && pa->keyIndexAt(t) >= 0) { pa->removeKeyAt(t); return; }
                        double cur = pa->at(t);
                        if (!pa->animated) { pa->animated = true; pa->keys.clear(); }
                        pa->setKey(t, cur);
                    };
                    tog(pid);
                    if (pd.kind == ParamKind::PointX) tog(def->params[pi + 1].id);
                });
            });
            h->addWidget(prev); h->addWidget(add); h->addWidget(next);
            updaters << [=] {
                auto *pa = liveParam(clipId, fxIndex, pid);
                if (pa) sw->setChecked(pa->animated);
            };
            auto *pa0 = liveParam(clipId, fxIndex, pid);
            sw->setChecked(pa0 && pa0->animated);
        }
        rl->addWidget(left);
        auto *lane = new KeyLane(clipId, fxIndex, pd.animatable ? pid : QString(), row);
        lanes << lane;
        rl->addWidget(lane, 1);
        lay->addWidget(row);
    }
}

void EffectControlsPanel::transitionRows(QVBoxLayout *lay)
{
    auto sel = PRJ->selTransition;
    auto q = PRJ->active();
    if (!q) return;
    auto *t = q->track(sel.first);
    if (!t) return;
    const Transition *x = nullptr;
    for (auto &k : t->transitions) if (k.cut == sel.second) x = &k;
    if (!x) return;
    auto *w = new QWidget;
    auto *f = new QVBoxLayout(w);
    f->setContentsMargins(10, 8, 10, 8);
    f->setSpacing(6);
    auto *d = EffectRegistry::findTransition(x->type);
    auto *n = new QLabel(d ? d->name : x->type, w);
    n->setStyleSheet("font-weight:600;color:#e0e0e0;");
    f->addWidget(n);
    auto *h = new QHBoxLayout;
    h->addWidget(new QLabel(tr("Duration"), w));
    auto *dur = new TimecodeLabel(w, 13);
    dur->setFrame(x->dur, q->fps(), q->dropFrame);
    h->addWidget(dur);
    h->addStretch(1);
    f->addLayout(h);
    auto *h2 = new QHBoxLayout;
    h2->addWidget(new QLabel(tr("Alignment"), w));
    auto *al = new QComboBox(w);
    al->addItems({tr("Center at Cut"), tr("Start at Cut"), tr("End at Cut")});
    al->setCurrentIndex(x->align == 0 ? 1 : x->align == 1 ? 2 : 0);
    h2->addWidget(al);
    h2->addStretch(1);
    f->addLayout(h2);
    connect(dur, &TimecodeLabel::edited, this, [sel](Frame nv) {
        PRJ->edit(tr("Transition Duration"), [&](ProjectState &st) {
            for (auto &k : st.seqById(st.activeSeq)->track(sel.first)->transitions) if (k.cut == sel.second) k.dur = qMax<Frame>(2, nv);
        });
    });
    connect(al, &QComboBox::activated, this, [sel](int i) {
        PRJ->edit(tr("Transition Alignment"), [&](ProjectState &st) {
            for (auto &k : st.seqById(st.activeSeq)->track(sel.first)->transitions) if (k.cut == sel.second) k.align = i == 0 ? .5 : i == 1 ? 0 : 1;
        });
    });
    lay->addWidget(w);
}
