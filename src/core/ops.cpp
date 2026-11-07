#include "ops.h"
#include "effectdefs.h"
#include <algorithm>

namespace Ops {

void sortClips(Track &t)
{
    std::sort(t.clips.begin(), t.clips.end(), [](const Clip &a, const Clip &b) { return a.start < b.start; });
}

void dropTransitionsInRange(Track &t, Frame a, Frame b)
{
    t.transitions.erase(std::remove_if(t.transitions.begin(), t.transitions.end(),
                                       [&](const Transition &x) { return x.cut >= a && x.cut <= b; }),
                        t.transitions.end());
}

// Remove [a,b) from a track: clips inside vanish, straddling clips are trimmed or split.
void clearRange(Sequence &q, Track &t, Frame a, Frame b, ProjectState *st)
{
    if (b <= a) return;
    QVector<Clip> out;
    for (const Clip &c : std::as_const(t.clips)) {
        if (c.end() <= a || c.start >= b) { out << c; continue; }
        if (c.start < a) { Clip l = c; trimOut(l, a - c.end()); out << l; }
        if (c.end() > b) {
            Clip r = c;
            trimIn(r, b - c.start, q.fps());
            if (c.start < a && st) r.id = st->newId();
            out << r;
        }
    }
    t.clips = out;
    sortClips(t);
    t.transitions.erase(std::remove_if(t.transitions.begin(), t.transitions.end(),
                                       [&](const Transition &x) { return x.cut > a && x.cut < b; }),
                        t.transitions.end());
}

void splitAt(Track &t, Frame f, ProjectState &st, double fps)
{
    for (int i = 0; i < t.clips.size(); ++i) {
        Clip &c = t.clips[i];
        if (f > c.start && f < c.end()) {
            Clip r = c;
            r.id = st.newId();
            trimIn(r, f - c.start, fps);
            trimOut(t.clips[i], f - t.clips[i].end());
            t.clips.insert(i + 1, r);
            return;
        }
    }
}

void shiftAfter(Track &t, Frame at, Frame delta)
{
    for (auto &c : t.clips) if (c.start >= at) c.start += delta;
    for (auto &x : t.transitions) if (x.cut >= at) x.cut += delta;
}

void insertGap(Sequence &q, ProjectState &st, Frame at, Frame n, const QSet<int> &extra)
{
    for (auto *t : q.allTracks()) {
        if (t->locked) continue;
        if (!t->sync && !extra.contains(t->id)) continue;
        splitAt(*t, at, st, q.fps());
        shiftAfter(*t, at, n);
    }
}

void placeClip(Track &t, const Clip &c)
{
    t.clips << c;
    sortClips(t);
}

static bool isGenerator(MediaKind k)
{
    return k == MediaKind::Color || k == MediaKind::Bars || k == MediaKind::Black || k == MediaKind::Text || k == MediaKind::Adjustment;
}

QVector<Clip> makeClips(ProjectState &st, const Sequence &q, const MediaItem &m, double in, double out, bool wantV, bool wantA)
{
    QVector<Clip> r;
    double fps = q.fps();
    Frame dur = qMax<Frame>(1, qRound64((out - in) * fps));
    int link = (m.hasVideo && m.hasAudio && wantV && wantA) ? st.newId() : 0;
    auto base = [&](MediaKind k) {
        Clip c;
        c.id = st.newId(); c.mediaId = m.id; c.kind = k; c.name = m.name; c.dur = dur; c.srcIn = in; c.link = link;
        c.label = k == MediaKind::Audio ? defaultLabel(MediaKind::Audio) : m.label;
        if (m.kind == MediaKind::Nested) if (auto *inner = st.seqById(m.seqId); inner && inner->multicam) c.cam = 0;
        EffectRegistry::defaultsFor(c);
        if (k != MediaKind::Audio) {
            Effect &mo = c.fx[0];
            mo.find("posX")->value = q.w / 2.0; mo.find("posY")->value = q.h / 2.0;
            int sw = m.w > 0 ? m.w : q.w, sh = m.h > 0 ? m.h : q.h;
            if (isGenerator(m.kind)) { sw = q.w; sh = q.h; }
            mo.find("ancX")->value = sw / 2.0; mo.find("ancY")->value = sh / 2.0;
            // Stills default to fit inside the frame; moving sources stay at native size.
            if (m.kind == MediaKind::Image && m.w > 0 && m.h > 0)
                mo.find("scale")->value = qMin(100.0, 100.0 * qMin(double(q.w) / m.w, double(q.h) / m.h));
        }
        return c;
    };
    if (m.kind != MediaKind::Audio && wantV && (m.hasVideo || m.kind != MediaKind::Video)) r << base(m.kind);
    if (m.hasAudio && wantA) r << base(MediaKind::Audio);
    return r;
}

static void place(ProjectState &st, Sequence &q, const MediaItem &m, double in, double out, Frame at, Patch p, bool insert)
{
    double mediaEnd = m.duration > 0 ? m.duration : (out > 0 ? out : 5);
    if (out <= in) out = mediaEnd;
    auto clips = makeClips(st, q, m, in, out, p.videoTrack >= 0, p.audioTrack >= 0);
    if (clips.isEmpty()) return;
    Frame dur = clips.first().dur;
    if (insert) {
        QSet<int> extra;
        if (p.videoTrack >= 0 && p.videoTrack < q.video.size()) extra << q.video[p.videoTrack].id;
        if (p.audioTrack >= 0 && p.audioTrack < q.audio.size()) extra << q.audio[p.audioTrack].id;
        insertGap(q, st, at, dur, extra);
    }
    for (Clip c : clips) {
        c.start = at;
        Track *t = nullptr;
        if (c.kind == MediaKind::Audio) { if (p.audioTrack >= 0 && p.audioTrack < q.audio.size()) t = &q.audio[p.audioTrack]; }
        else if (p.videoTrack >= 0 && p.videoTrack < q.video.size()) t = &q.video[p.videoTrack];
        if (!t || t->locked) continue;
        clearRange(q, *t, at, at + dur, &st);
        placeClip(*t, c);
    }
}
void overwriteMedia(ProjectState &st, Sequence &q, const MediaItem &m, double in, double out, Frame at, Patch p) { place(st, q, m, in, out, at, p, false); }
void insertMedia(ProjectState &st, Sequence &q, const MediaItem &m, double in, double out, Frame at, Patch p) { place(st, q, m, in, out, at, p, true); }

void razor(ProjectState &st, Sequence &q, Frame f, const QSet<int> &tracks)
{
    QHash<int, int> newLink;
    for (auto *t : q.allTracks()) {
        if (t->locked || (!tracks.isEmpty() && !tracks.contains(t->id))) continue;
        int before = t->clips.size();
        splitAt(*t, f, st, q.fps());
        if (t->clips.size() == before) continue;
        for (auto &c : t->clips) // right-hand pieces stay A/V linked to each other
            if (c.start == f && c.link) {
                if (!newLink.contains(c.link)) newLink[c.link] = st.newId();
                c.link = newLink[c.link];
            }
    }
}

void deleteClips(Sequence &q, const QSet<int> &ids, bool ripple)
{
    QVector<QPair<Frame, Frame>> ranges;
    for (auto *t : q.allTracks()) {
        for (int i = t->clips.size() - 1; i >= 0; --i)
            if (ids.contains(t->clips[i].id)) {
                const Clip &c = t->clips[i];
                ranges << qMakePair(c.start, c.end());
                dropTransitionsInRange(*t, c.start, c.end());
                t->clips.removeAt(i);
            }
    }
    if (!ripple || ranges.isEmpty()) return;
    std::sort(ranges.begin(), ranges.end());
    QVector<QPair<Frame, Frame>> merged;
    for (auto &r : ranges) {
        if (!merged.isEmpty() && r.first <= merged.last().second) merged.last().second = qMax(merged.last().second, r.second);
        else merged << r;
    }
    for (int i = merged.size() - 1; i >= 0; --i)
        for (auto *t : q.allTracks())
            if (!t->locked && t->sync) shiftAfter(*t, merged[i].second, merged[i].first - merged[i].second);
}

void lift(ProjectState &st, Sequence &q, Frame a, Frame b)
{
    for (auto *t : q.allTracks()) if (t->target && !t->locked) clearRange(q, *t, a, b, &st);
}
void extract(ProjectState &st, Sequence &q, Frame a, Frame b)
{
    lift(st, q, a, b);
    for (auto *t : q.allTracks()) if (!t->locked && (t->target || t->sync)) shiftAfter(*t, b, a - b);
}

Frame maxExtendLeft(const Clip &c, double fps, const MediaItem *m)
{
    if (!m || c.kind == MediaKind::Image || isGenerator(c.kind)) return Frame(1) << 40;
    return Frame(c.srcIn * fps / c.speed + 1e-6);
}
Frame maxExtendRight(const Clip &c, double fps, const MediaItem *m)
{
    if (!m || c.kind == MediaKind::Image || isGenerator(c.kind)) return Frame(1) << 40;
    return qMax<Frame>(0, Frame((m->duration - c.srcOut(fps)) * fps / c.speed + 1e-6));
}
void trimIn(Clip &c, Frame d, double fps)
{
    c.start += d; c.dur -= d;
    if (c.reverse) return;
    c.srcIn += d / fps * c.speed;
    c.kfOffset += d;
}
void trimOut(Clip &c, Frame d) { c.dur += d; }

void addTransition(Track &t, const QString &type, Frame cut, Frame dur, double align)
{
    for (auto &x : t.transitions)
        if (x.cut == cut) { x.type = type; x.dur = dur; x.align = align; return; }
    Transition x; x.type = type; x.cut = cut; x.dur = dur; x.align = align;
    t.transitions << x;
}

Frame nextEdit(const Sequence &q, Frame f, bool forward, bool targetedOnly)
{
    const Frame none = Frame(1) << 60;
    Frame best = forward ? none : Frame(-1);
    auto scan = [&](const QVector<Track> &ts) {
        for (auto &t : ts) {
            if (targetedOnly && !t.target) continue;
            for (auto &c : t.clips)
                for (Frame e : {c.start, c.end()}) {
                    if (forward && e > f && e < best) best = e;
                    if (!forward && e < f && e > best) best = e;
                }
        }
    };
    scan(q.video); scan(q.audio);
    if (forward && best == none) return f;
    if (!forward && best < 0) return 0;
    return best;
}

void addTrack(ProjectState &st, Sequence &q, TrackType ty)
{
    Track t; t.id = st.newId(); t.type = ty;
    auto &list = ty == TrackType::Video ? q.video : q.audio;
    t.name = QString(ty == TrackType::Video ? "Video %1" : "Audio %1").arg(list.size() + 1);
    list << t;
}
void removeTrack(Sequence &q, int id)
{
    for (auto *l : {&q.video, &q.audio})
        for (int i = 0; i < l->size(); ++i)
            if ((*l)[i].id == id) { l->removeAt(i); return; }
}
}
