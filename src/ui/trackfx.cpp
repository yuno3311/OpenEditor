// Audio Track Mixer: per-track insert effects.
#include "sidepanels.h"
#include "../core/effectdefs.h"
#include "../core/project.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>

void AudioMixerPanel::trackFxMenu(int trackId, QWidget *anchor)
{
    auto q = PRJ->active();
    auto *t = q ? q->track(trackId) : nullptr;
    if (!t) return;
    QMenu m;
    for (int i = 0; i < t->fx.size(); ++i) {
        auto *d = EffectRegistry::find(t->fx[i].type);
        auto *sub = m.addMenu(d ? d->name : t->fx[i].type);
        sub->addAction(tr("Edit..."), [=, this] { trackFxDialog(trackId, i); });
        auto *on = sub->addAction(tr("Enabled"), [=] {
            PRJ->edit(tr("Toggle Track Effect"), [&](ProjectState &st) { auto &e = st.seqById(st.activeSeq)->track(trackId)->fx[i]; e.enabled = !e.enabled; });
        });
        on->setCheckable(true);
        on->setChecked(t->fx[i].enabled);
        sub->addAction(tr("Remove"), [=] {
            PRJ->edit(tr("Remove Track Effect"), [&](ProjectState &st) { st.seqById(st.activeSeq)->track(trackId)->fx.removeAt(i); });
        });
    }
    if (!t->fx.isEmpty()) m.addSeparator();
    auto *add = m.addMenu(tr("Add Effect"));
    for (auto &d : EffectRegistry::all()) {
        if (!d.audio || d.intrinsic) continue;
        QString id = d.id, name = d.name;
        add->addAction(name, [=, this] {
            PRJ->edit(tr("Add Track Effect: %1").arg(name), [&](ProjectState &st) { st.seqById(st.activeSeq)->track(trackId)->fx << EffectRegistry::make(id); });
            int idx = PRJ->active()->track(trackId)->fx.size() - 1;
            if (!EffectRegistry::find(id)->params.isEmpty()) trackFxDialog(trackId, idx);
        });
    }
    m.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void AudioMixerPanel::trackFxDialog(int trackId, int index)
{
    auto q = PRJ->active();
    auto *t = q ? q->track(trackId) : nullptr;
    if (!t || index >= t->fx.size()) return;
    auto *def = EffectRegistry::find(t->fx[index].type);
    if (!def) return;
    QDialog d(this);
    d.setWindowTitle(QString("%1: %2").arg(t->name, def->name));
    d.setMinimumWidth(300);
    auto *f = new QFormLayout(&d);
    for (auto &pd : def->params) {
        if (pd.kind == ParamKind::Toggle || pd.kind == ParamKind::Choice || pd.kind == ParamKind::Color) continue;
        auto *s = new ScrubValue(&d);
        s->setRange(pd.min < -1e8 ? pd.softMin : pd.min, pd.max > 1e8 ? pd.softMax : pd.max);
        s->setStep(pd.step);
        s->setDecimals(pd.step >= 1 ? 0 : 2);
        s->setSuffix(pd.unit);
        const Param *cur = t->fx[index].find(pd.id);
        s->setValue(cur ? cur->value : pd.def);
        QString id = pd.id;
        connect(s, &ScrubValue::dragBegan, &d, [] { PRJ->liveBegin(); });
        connect(s, &ScrubValue::edited, &d, [=](double v) {
            auto *tr2 = PRJ->active()->track(trackId);
            if (auto *p = tr2->fx[index].find(id)) { p->value = v; PRJ->liveTouch(); }
        });
        connect(s, &ScrubValue::dragEnded, &d, [] { PRJ->liveCommit(tr("Track Effect")); });
        f->addRow(pd.name, s);
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    d.exec();
}
