// Range removal for assisted edits, and multicam construction.
#include "effectdefs.h"
#include "ops.h"
#include "../engine/analysis.h"
#include <QHash>
#include <algorithm>

namespace Ops {

// Removes ranges from every unlocked track, closing each gap. Captions and markers move with the picture.
void rippleRemoveRanges(ProjectState &st, Sequence &q, QVector<QPair<Frame, Frame>> ranges)
{
    // Overlapping or touching ranges must become one, or the overlap would be removed twice.
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    QVector<QPair<Frame, Frame>> merged;
    for (auto &r : ranges) {
        if (r.second <= r.first) continue;
        if (!merged.isEmpty() && r.first <= merged.last().second) merged.last().second = qMax(merged.last().second, r.second);
        else merged << r;
    }
    for (int ri = merged.size() - 1; ri >= 0; --ri) {
        const Frame a = merged[ri].first, b = merged[ri].second, len = b - a;
        for (auto *t : q.allTracks()) {
            if (t->locked) continue;
            clearRange(q, *t, a, b, &st);
            shiftAfter(*t, b, -len);
        }
        // The piece after the cut is a separate clip now; it must not stay linked to the piece before it.
        QHash<int, int> relink;
        for (auto *t : q.allTracks())
            for (auto &c : t->clips) {
                if (c.start != a || !c.link) continue;
                bool split = false;
                for (auto &o : t->clips) split |= o.id != c.id && o.link == c.link && o.start < a;
                if (!split) continue;
                if (!relink.contains(c.link)) relink[c.link] = st.newId();
                c.link = relink[c.link];
            }
        QVector<Caption> kept;
        for (Caption c : std::as_const(q.captions)) {
            if (c.end <= a) { kept << c; continue; }
            if (c.start >= b) {
                c.start -= len; c.end -= len;
                for (auto &w : c.words) { w.start -= len; w.end -= len; }
                kept << c;
                continue;
            }
            if (!c.words.isEmpty()) {
                // Text editing: keep exactly the words that survive, at their new times.
                QVector<Word> left;
                for (Word w : std::as_const(c.words)) {
                    const Frame mid = (w.start + w.end) / 2;
                    if (mid >= a && mid < b) continue;
                    if (mid >= b) { w.start -= len; w.end -= len; }
                    left << w;
                }
                if (left.isEmpty()) continue;
                c.words = left;
                c.text = Analysis::joinWords(left);
                c.start = qMin(left.first().start, c.start < a ? c.start : a);
                c.end = c.end >= b ? c.end - len : qMin(c.end, a);
                kept << c;
            } else if (c.start < a && c.end > b) { c.end -= len; kept << c; }
            else if (c.start < a) { c.end = a; kept << c; }
            else if (c.end > b) { c.start = a; c.end -= len; kept << c; }
        }
        q.captions = kept;
        QVector<Marker> mk;
        for (Marker m : std::as_const(q.markers)) {
            if (m.t >= a && m.t < b) continue;
            if (m.t >= b) m.t -= b - a;
            mk << m;
        }
        q.markers = mk;
    }
}

// A multicam source is a sequence with one video and one audio track per camera, aligned by offset.
int makeMulticam(ProjectState &st, const QString &name, const QVector<int> &ids, const QVector<double> &offsets, int w, int h, int num, int den)
{
    Sequence q;
    q.id = st.newId(); q.name = name; q.w = w; q.h = h; q.fpsNum = num; q.fpsDen = den; q.multicam = true;
    double earliest = 0;
    for (double o : offsets) earliest = qMin(earliest, o);
    for (int i = 0; i < ids.size(); ++i) {
        addTrack(st, q, TrackType::Video);
        addTrack(st, q, TrackType::Audio);
        q.video.last().name = QString("Camera %1").arg(i + 1);
        q.audio.last().name = QString("Camera %1").arg(i + 1);
        const MediaItem *m = st.mediaById(ids[i]);
        if (!m) continue;
        const Frame at = qRound64((offsets.value(i) - earliest) * q.fps());
        const auto clips = makeClips(st, q, *m, 0, m->duration, true, true);
        for (Clip c : clips) {
            c.start = at; c.link = 0;
            if (c.kind != MediaKind::Audio) {
                if (auto *mo = c.effect("motion"); mo && m->w > 0)
                    mo->find("scale")->value = 100.0 * qMin(double(w) / m->w, double(h) / m->h);
                placeClip(q.video.last(), c);
            } else placeClip(q.audio.last(), c);
        }
    }
    st.seqs << q;
    MediaItem mi;
    mi.id = st.newId(); mi.kind = MediaKind::Nested; mi.name = name; mi.seqId = q.id; mi.w = w; mi.h = h; mi.fps = q.fps();
    mi.hasVideo = mi.hasAudio = true; mi.duration = q.duration() / q.fps(); mi.label = defaultLabel(MediaKind::Nested);
    st.media << mi;
    return mi.id;
}

// Live multicam cut: split at f, then the piece under f shows the chosen angle.
void cutToAngle(ProjectState &st, Sequence &q, Frame f, int angle)
{
    razor(st, q, f, {});
    for (auto *t : q.allTracks())
        for (auto &c : t->clips)
            if (c.cam >= 0 && f >= c.start && f < c.end()) c.cam = angle;
}
}
