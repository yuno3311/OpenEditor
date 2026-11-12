// Beat detection and the text formats other tools read: CMX3600 edit decision lists, marker
// lists, YouTube chapters and FFmpeg chapter metadata.
#pragma once
#include "../core/model.h"
#include <QStringList>

namespace Analysis {
struct Beats {
    double bpm = 0;
    QVector<double> times; // seconds from the start of the audio
};
// Tempo and beat positions of 48 kHz stereo audio. bpm is 0 when no steady pulse is found.
Beats detectBeats(const float *stereo, qint64 frames, double minBpm = 70, double maxBpm = 180);
Beats sequenceBeats(const ProjectState &st, int seqId, Frame first = 0, Frame last = -1);
// Adds a marker on every nth beat (1 = all). Existing markers named Beat are replaced.
int addBeatMarkers(Sequence &q, const Beats &b, Frame offset, int every = 1);
}

namespace Interchange {
// Cuts-only CMX3600 list of video track 1 and the first four audio tracks. Dissolves are noted
// in comments, which is what most conform tools expect from a simple EDL.
QString writeEdl(const ProjectState &st, const Sequence &q);
struct EdlEvent { int number = 0; QString reel, track, srcIn, srcOut, recIn, recOut, clipName; };
QVector<EdlEvent> parseEdl(const QString &text, QString *title = nullptr);
QString markersCsv(const Sequence &q);
QString youtubeChapters(const Sequence &q);       // "0:00 Title" lines; empty with no markers
QString ffmetadataChapters(const Sequence &q, Frame first, Frame last); // empty with no markers in range
}
