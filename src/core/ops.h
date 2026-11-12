// Pure timeline edit operations. Every function mutates model values, so the same code runs for
// real edits and undo snapshots. Nothing here touches UI.
#pragma once
#include "model.h"
#include <QSet>
#include <QStringList>

namespace Ops {
struct Patch { // which destination track indexes a source edit goes to (-1 = none)
    int videoTrack = -1, audioTrack = -1;
};
void sortClips(Track &t);
void clearRange(Sequence &q, Track &t, Frame a, Frame b, ProjectState *st);
void splitAt(Track &t, Frame f, ProjectState &st, double fps);
void insertGap(Sequence &q, ProjectState &st, Frame at, Frame n, const QSet<int> &extraTracks = {});
void shiftAfter(Track &t, Frame at, Frame delta);
void placeClip(Track &t, const Clip &c);
QVector<Clip> makeClips(ProjectState &st, const Sequence &q, const MediaItem &m, double in, double out, bool wantV, bool wantA);
void overwriteMedia(ProjectState &st, Sequence &q, const MediaItem &m, double in, double out, Frame at, Patch p);
void insertMedia(ProjectState &st, Sequence &q, const MediaItem &m, double in, double out, Frame at, Patch p);
void razor(ProjectState &st, Sequence &q, Frame f, const QSet<int> &tracks);
void deleteClips(Sequence &q, const QSet<int> &ids, bool ripple);
void lift(ProjectState &st, Sequence &q, Frame a, Frame b);
void extract(ProjectState &st, Sequence &q, Frame a, Frame b);
Frame maxExtendLeft(const Clip &c, double fps, const MediaItem *m);
Frame maxExtendRight(const Clip &c, double fps, const MediaItem *m);
void trimIn(Clip &c, Frame delta, double fps);
void trimOut(Clip &c, Frame delta);
void addTransition(Track &t, const QString &type, Frame cut, Frame dur, double align);
Frame nextEdit(const Sequence &q, Frame f, bool forward, bool targetedOnly);
void addTrack(ProjectState &st, Sequence &q, TrackType ty);
void removeTrack(Sequence &q, int trackId);
void dropTransitionsInRange(Track &t, Frame a, Frame b);
void rippleRemoveRanges(ProjectState &st, Sequence &q, QVector<QPair<Frame, Frame>> ranges);
int makeMulticam(ProjectState &st, const QString &name, const QVector<int> &mediaIds, const QVector<double> &offsets, int w, int h, int num, int den);
void cutToAngle(ProjectState &st, Sequence &q, Frame f, int angle);
// --- 3.0
QVector<QPair<Frame, Frame>> findGaps(const Sequence &q);           // stretches with nothing on any track
int closeGaps(ProjectState &st, Sequence &q);                        // ripple-removes them; returns how many
QVector<int> duplicateClips(ProjectState &st, Sequence &q, const QSet<int> &ids); // copies go in right after
bool replaceClip(ProjectState &st, Sequence &q, int clipId, int mediaId, QString *err = nullptr);
int extendToFrame(const ProjectState &st, Sequence &q, const QSet<int> &ids, Frame f);
bool fadeClip(Clip &c, Frame fadeIn, Frame fadeOut);                 // opacity or volume keyframes
QStringList layoutPresets();
bool applyLayout(const Sequence &q, Clip &c, const QString &preset); // fill, fit, picture-in-picture, splits
int autoReframe(ProjectState &st, int seqId, int w, int h, const QString &name); // new sequence id
int setInterpolation(Clip &c, const QString &effect, const QString &param, Interp i);
}
