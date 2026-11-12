// Timeline operations added in 3.0: closing gaps, duplicating, replacing, extending edits,
// fades, picture layouts and reframing a sequence for another aspect ratio.
#include "ops.h"
#include "effectdefs.h"
#include <QHash>
#include <algorithm>
#include <cmath>

namespace Ops {

QVector<QPair<Frame, Frame>> findGaps(const Sequence &q)
{
    // Covered stretches across every track that takes part in ripple edits.
    QVector<QPair<Frame, Frame>> spans;
    for (auto *tracks : {&q.video, &q.audio})
        for (auto &t : *tracks)
            for (auto &c : t.clips) spans << qMakePair(c.start, c.end());
    std::sort(spans.begin(), spans.end());
    QVector<QPair<Frame, Frame>> gaps;
    Frame at = 0;
    for (auto &s : spans) {
        if (s.first > at) gaps << qMakePair(at, s.first);
        at = qMax(at, s.second);
    }
    return gaps;
}

int closeGaps(ProjectState &st, Sequence &q)
{
    const auto gaps = findGaps(q);
    if (!gaps.isEmpty()) rippleRemoveRanges(st, q, gaps);
    return gaps.size();
}

QVector<int> duplicateClips(ProjectState &st, Sequence &q, const QSet<int> &ids)
{
    struct Src { int track; Clip c; };
    QVector<Src> src;
    Frame a = -1, b = -1;
    QSet<int> trackIds;
    for (auto *t : q.allTracks())
        for (auto &c : t->clips)
            if (ids.contains(c.id)) {
                src << Src{t->id, c};
                trackIds << t->id;
                a = a < 0 ? c.start : qMin(a, c.start);
                b = qMax(b, c.end());
            }
    QVector<int> made;
    if (src.isEmpty()) return made;
    const Frame len = b - a;
    insertGap(q, st, b, len, trackIds);
    QHash<int, int> links;
    for (auto &s : src) {
        Clip c = s.c;
        c.id = st.newId();
        c.start += len;
        if (c.link) {
            if (!links.contains(c.link)) links[c.link] = st.newId();
            c.link = links[c.link];
        }
        if (Track *t = q.track(s.track)) { placeClip(*t, c); made << c.id; }
    }
    return made;
}

bool replaceClip(ProjectState &st, Sequence &q, int clipId, int mediaId, QString *err)
{
    Clip *c = q.clip(clipId);
    const MediaItem *m = st.mediaById(mediaId);
    auto no = [&](const char *why) { if (err) *err = QString::fromLatin1(why); return false; };
    if (!c) return no("No such clip.");
    if (!m) return no("No such media.");
    const bool audioClip = c->kind == MediaKind::Audio;
    if (audioClip && !m->hasAudio) return no("That media has no audio.");
    if (!audioClip && m->kind == MediaKind::Audio) return no("That media has no picture.");
    if (c->kind == MediaKind::Text || c->kind == MediaKind::Adjustment) return no("Titles and adjustment layers have no source to replace.");
    const double fps = q.fps();
    const double in = m->inSec >= 0 ? m->inSec : 0;
    if (m->duration > 0 && m->kind != MediaKind::Image) {
        const Frame fits = Frame(std::floor((m->duration - in) * fps / qMax(.01, c->speed)));
        if (fits < 1) return no("The new media is too short.");
        c->dur = qMin(c->dur, fits);
    }
    const MediaItem *old = st.mediaById(c->mediaId);
    c->mediaId = m->id;
    c->name = m->name;
    c->srcIn = in;
    if (!audioClip) {
        c->kind = m->kind;
        // Keep the picture the same size on screen when the new source has other dimensions.
        if (Effect *mo = c->effect("motion"); mo && m->w > 0 && m->h > 0) {
            const double ow = old && old->w > 0 ? old->w : mo->find("ancX")->value * 2;
            mo->find("ancX")->value = m->w / 2.0;
            mo->find("ancY")->value = m->h / 2.0;
            if (ow > 0 && !mo->find("scale")->animated) mo->find("scale")->value *= ow / m->w;
        }
    }
    return true;
}

int extendToFrame(const ProjectState &st, Sequence &q, const QSet<int> &ids, Frame f)
{
    int changed = 0;
    const double fps = q.fps();
    for (auto *t : q.allTracks()) {
        if (t->locked) continue;
        for (int i = 0; i < t->clips.size(); ++i) {
            Clip &c = t->clips[i];
            if (!ids.contains(c.id)) continue;
            const MediaItem *m = st.mediaById(c.mediaId);
            const Frame prevEnd = i > 0 ? t->clips[i - 1].end() : 0;
            const Frame nextStart = i + 1 < t->clips.size() ? t->clips[i + 1].start : std::numeric_limits<Frame>::max();
            Frame d = 0;
            if (f < c.start) {
                d = qMin(c.start - qMax(f, prevEnd), maxExtendLeft(c, fps, m));
                if (d > 0) trimIn(c, -d, fps);
            } else if (f > c.end()) {
                d = qMin(qMin(f, nextStart) - c.end(), maxExtendRight(c, fps, m));
                if (d > 0) trimOut(c, d);
            } else if (f > c.start && f < c.end()) {
                // Inside the clip: the nearer edge moves to the frame.
                if (f - c.start < c.end() - f) { d = f - c.start; trimIn(c, d, fps); }
                else { d = c.end() - f; trimOut(c, -d); }
            }
            changed += d > 0;
        }
    }
    return changed;
}

bool fadeClip(Clip &c, Frame fadeIn, Frame fadeOut)
{
    const bool audio = c.kind == MediaKind::Audio;
    Effect *e = c.effect(audio ? "volume" : "opacity");
    if (!e) return false;
    Param *p = e->find(audio ? "level" : "opacity");
    if (!p) return false;
    fadeIn = qBound<Frame>(0, fadeIn, c.dur);
    fadeOut = qBound<Frame>(0, fadeOut, c.dur - fadeIn);
    const double full = p->value, off = audio ? -96 : 0;
    p->keys.clear();
    p->animated = fadeIn > 0 || fadeOut > 0;
    if (!p->animated) return true;
    const Frame k0 = c.kfOffset, k1 = c.kfOffset + c.dur;
    auto key = [&](Frame t, double v) {
        p->setKey(t, v);
        p->keys[p->keyIndexAt(t)].i = audio ? Interp::EaseOut : Interp::Linear;
    };
    if (fadeIn > 0) { key(k0, off); key(k0 + fadeIn, full); }
    else key(k0, full);
    if (fadeOut > 0) {
        key(k1 - fadeOut, full);
        key(k1, off);
        // A fade-out in decibels has to fall slowly first, the mirror of the fade-in.
        if (audio) p->keys[p->keyIndexAt(k1 - fadeOut)].i = Interp::EaseIn;
    }
    return true;
}

QStringList layoutPresets()
{
    return {"Fill Frame", "Fit Frame", "Center", "PiP Top Left", "PiP Top Right", "PiP Bottom Left", "PiP Bottom Right",
            "Split Left", "Split Right", "Split Top", "Split Bottom"};
}

bool applyLayout(const Sequence &q, Clip &c, const QString &preset)
{
    Effect *mo = c.effect("motion");
    if (!mo || !layoutPresets().contains(preset)) return false;
    auto P = [&](const char *id) -> Param & { return *mo->find(id); };
    const double sw = qMax(1.0, P("ancX").value * 2), sh = qMax(1.0, P("ancY").value * 2);
    const double fit = qMin(q.w / sw, q.h / sh) * 100, fill = qMax(q.w / sw, q.h / sh) * 100;
    for (const char *id : {"posX", "posY", "scale", "scaleW", "rot"}) { P(id).animated = false; P(id).keys.clear(); }
    P("uniform").value = 1;
    P("rot").value = 0;
    double x = q.w / 2.0, y = q.h / 2.0, scale = fit;
    double cropL = 0, cropR = 0, cropT = 0, cropB = 0;
    if (preset == "Fill Frame") scale = fill;
    else if (preset == "Center") scale = 100;
    else if (preset.startsWith("PiP")) {
        scale = fit * .3;
        const double w = sw * scale / 100, h = sh * scale / 100, margin = q.w * .03;
        x = preset.endsWith("Left") ? margin + w / 2 : q.w - margin - w / 2;
        y = preset.contains("Top") ? margin + h / 2 : q.h - margin - h / 2;
    } else if (preset.startsWith("Split")) {
        const bool horiz = preset.endsWith("Left") || preset.endsWith("Right");
        // The half-frame cell shows the middle of the picture, scaled to cover the cell.
        const double cw = horiz ? q.w / 2.0 : q.w, ch = horiz ? q.h : q.h / 2.0;
        scale = qMax(cw / sw, ch / sh) * 100;
        const double w = sw * scale / 100, h = sh * scale / 100;
        cropL = cropR = qMax(0.0, (w - cw) / w * 50);
        cropT = cropB = qMax(0.0, (h - ch) / h * 50);
        x = preset.endsWith("Left") ? q.w * .25 : preset.endsWith("Right") ? q.w * .75 : q.w * .5;
        y = preset.endsWith("Top") ? q.h * .25 : preset.endsWith("Bottom") ? q.h * .75 : q.h * .5;
    }
    P("posX").value = x; P("posY").value = y; P("scale").value = scale; P("scaleW").value = scale;
    Effect *crop = c.effect("crop");
    if (preset.startsWith("Split")) {
        if (!crop) { c.fx << EffectRegistry::make("crop"); crop = &c.fx.last(); mo = c.effect("motion"); }
        crop->enabled = true;
        crop->find("left")->value = cropL; crop->find("right")->value = cropR;
        crop->find("top")->value = cropT; crop->find("bottom")->value = cropB;
    } else if (crop) {
        for (const char *id : {"left", "right", "top", "bottom"}) crop->find(id)->value = 0;
    }
    return true;
}

int autoReframe(ProjectState &st, int seqId, int w, int h, const QString &name)
{
    const Sequence *src = st.seqById(seqId);
    if (!src || w < 16 || h < 16) return 0;
    Sequence q = *src;
    const double sx = double(w) / q.w, sy = double(h) / q.h, cover = qMax(sx, sy);
    q.id = st.newId();
    q.name = name.isEmpty() ? QString("%1 %2x%3").arg(src->name).arg(w).arg(h) : name;
    q.w = w; q.h = h;
    QHash<int, int> links;
    for (auto *t : q.allTracks()) {
        t->id = st.newId();
        for (auto &c : t->clips) {
            c.id = st.newId();
            if (c.link) {
                if (!links.contains(c.link)) links[c.link] = st.newId();
                c.link = links[c.link];
            }
            Effect *mo = c.effect("motion");
            if (!mo) continue;
            auto scaleParam = [](Param *p, double k) { if (!p) return; p->value *= k; for (auto &kf : p->keys) kf.v *= k; };
            scaleParam(mo->find("posX"), sx);
            scaleParam(mo->find("posY"), sy);
            const bool generated = c.kind == MediaKind::Text || c.kind == MediaKind::Adjustment || c.kind == MediaKind::Color
                || c.kind == MediaKind::Black || c.kind == MediaKind::Bars;
            if (generated) {
                // Drawn at the sequence size, so only the anchor follows the new frame.
                mo->find("ancX")->value = w / 2.0;
                mo->find("ancY")->value = h / 2.0;
            } else {
                scaleParam(mo->find("scale"), cover);
                scaleParam(mo->find("scaleW"), cover);
            }
        }
    }
    st.seqs << q;
    MediaItem m;
    m.id = st.newId(); m.kind = MediaKind::Nested; m.name = q.name; m.seqId = q.id; m.w = w; m.h = h;
    m.fps = q.fps(); m.hasVideo = m.hasAudio = true; m.duration = q.duration() / q.fps(); m.label = defaultLabel(MediaKind::Nested);
    st.media << m;
    return q.id;
}

int setInterpolation(Clip &c, const QString &effect, const QString &param, Interp i)
{
    int n = 0;
    for (auto &e : c.fx) {
        if (!effect.isEmpty() && e.type != effect) continue;
        for (auto &p : e.params) {
            if (!param.isEmpty() && p.id != param) continue;
            for (auto &k : p.keys) { k.i = i; ++n; }
        }
    }
    return n;
}
}
