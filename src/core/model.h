// Open Editor - core data model.
// All model types are plain value types built on Qt's implicitly shared containers, so a whole
// project state can be snapshotted in O(1) for undo and handed to the render threads safely.
#pragma once
#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

using Frame = qint64;

enum class MediaKind { Video, Audio, Image, Nested, Color, Bars, Black, Text, Adjustment };
enum class TrackType { Video, Audio };
enum class Interp { Linear, Hold, Ease };

struct Keyframe {
    Frame t = 0;
    double v = 0;
    Interp i = Interp::Linear;
};

struct Param {
    QString id;
    double value = 0;
    bool animated = false;
    QVector<Keyframe> keys; // sorted by t
    double at(Frame t) const;
    void setKey(Frame t, double v);
    void removeKeyAt(Frame t);
    int keyIndexAt(Frame t) const;
};

struct Effect {
    QString type;
    bool enabled = true;
    QVector<Param> params;
    QVariantMap extra; // curves, LUT path, ...
    Param *find(const QString &id);
    const Param *find(const QString &id) const;
    double val(const QString &id, Frame t, double fallback = 0) const;
};

struct TextStyle {
    QString text = QStringLiteral("New Title");
    QString font = QStringLiteral("Segoe UI");
    double size = 96;
    bool bold = true, italic = false, underline = false;
    int align = 1; // 0 left, 1 center, 2 right
    QColor fill = Qt::white;
    bool stroke = false;
    QColor strokeColor = Qt::black;
    double strokeWidth = 4;
    bool shadow = false;
    QColor shadowColor = QColor(0, 0, 0, 180);
    double shadowDist = 6, shadowBlur = 8;
    bool background = false;
    QColor bgColor = QColor(0, 0, 0, 160);
    double tracking = 0, leading = 0;

    // Added in 2.1. The defaults draw exactly what earlier versions drew.
    int weight = 0;               // 100 thin .. 900 black; 0 follows bold
    bool strike = false, kerning = true;
    int caps = 0;                 // 0 as typed, 1 ALL CAPS, 2 Small Caps, 3 lowercase, 4 Title Case
    double skew = 0;              // faux italic, degrees
    int valign = 1;               // 0 top, 1 middle, 2 bottom of the title-safe area
    double boxWidth = 0;          // paragraph text: wrap at this width in pixels; 0 = point text
    int fillType = 0;             // 0 solid, 1 linear gradient, 2 radial gradient
    QColor fill2 = QColor(255, 170, 0);
    double gradAngle = 180;       // 180 runs top to bottom, 90 left to right
    int strokePos = 0;            // 0 outside, 1 centre, 2 inside
    double shadowAngle = 135;     // 135 is down and to the right
    bool glow = false;
    QColor glowColor = QColor(255, 255, 255, 200);
    double glowSize = 14, glowStrength = 1;
    double bgPad = -1, bgRadius = 0; // bgPad < 0: automatic padding
    bool bgPerLine = false;
    int animIn = 0, animOut = 0;  // TextAnimation
    double animInSec = .6, animOutSec = .6;

    int effectiveWeight() const { return weight > 0 ? weight : (bold ? 700 : 400); }
};
enum TextAnimation { TextAnimNone, TextAnimFade, TextAnimTypewriter, TextAnimSlideUp, TextAnimSlideDown, TextAnimPop, TextAnimWipe, TextAnimCount };

struct Clip {
    int id = 0;
    int mediaId = 0;
    MediaKind kind = MediaKind::Video;
    QString name;
    Frame start = 0, dur = 0;
    double srcIn = 0; // seconds into the source
    double speed = 1;
    bool reverse = false;
    bool enabled = true;
    int link = 0;
    int label = -1;
    Frame kfOffset = 0; // keyframe time = (frame - start) + kfOffset
    int cam = -1;       // multicam angle when this clip is a nested multicam source
    QVector<Effect> fx;
    TextStyle text;

    Frame end() const { return start + dur; }
    Frame kt(Frame f) const { return f - start + kfOffset; }
    double sourceTime(Frame f, double fps) const;
    double srcOut(double fps) const { return srcIn + dur / fps * speed; }
    Effect *effect(const QString &type);
    const Effect *effect(const QString &type) const;
};

struct Transition {
    QString type;
    Frame cut = 0;     // the edit point the transition belongs to
    Frame dur = 30;
    double align = 0.5; // 0 starts at cut, .5 centred on cut, 1 ends at cut
    Frame start() const { return cut - Frame(dur * align); }
    Frame end() const { return start() + dur; }
};

struct Track {
    int id = 0;
    TrackType type = TrackType::Video;
    QString name;
    bool muted = false; // eye (video) or mute (audio)
    bool solo = false;
    bool locked = false;
    bool sync = true;
    bool target = true;
    int height = 44;
    double volumeDb = 0, pan = 0;
    QVector<Clip> clips; // sorted, non-overlapping
    QVector<Transition> transitions;
    QVector<Effect> fx;  // track insert effects (audio tracks)
    Param volAuto, panAuto; // automation in sequence frames; animated = automation active
    bool writeAuto = false, showAuto = false;
    const Clip *clipAt(Frame f) const;
    Clip *clipById(int id);
};

struct Marker {
    Frame t = 0, dur = 0;
    QString name, comment;
    int color = 0;
};

struct Word {
    Frame start = 0, end = 0;
    QString text;
};

struct Caption {
    Frame start = 0, end = 0;
    QString text;
    QVector<Word> words; // word timing from transcription; empty for imported subtitles
};

struct Sequence {
    int id = 0;
    QString name;
    int w = 1920, h = 1080;
    int fpsNum = 30, fpsDen = 1;
    bool dropFrame = false;
    QVector<Track> video, audio; // index 0 is V1/A1
    Frame playhead = 0, inPt = -1, outPt = -1;
    QVector<Marker> markers;
    double masterDb = 0;
    QVector<Caption> captions; // sorted by start
    bool burnCaptions = true;
    int captionSize = 46;
    bool multicam = false;     // each video track is one camera angle
    double fps() const { return double(fpsNum) / fpsDen; }
    Frame duration() const;
    Track *track(int id);
    const Track *track(int id) const;
    Clip *clip(int id, Track **t = nullptr);
    const Clip *clip(int id) const;
    QVector<Track *> allTracks();
};

struct MediaItem {
    int id = 0;
    int bin = 0;
    MediaKind kind = MediaKind::Video;
    QString path, name;
    int w = 0, h = 0;
    double fps = 0, duration = 0;
    bool hasVideo = false, hasAudio = false, hasAlpha = false;
    int channels = 0, sampleRate = 0;
    QString vcodec, acodec;
    qint64 bitrate = 0, size = 0;
    QColor color = Qt::black;
    int label = -1;
    int seqId = 0;
    double inSec = -1, outSec = -1;
    bool offline = false;
    QString comment;
    bool hdr = false;       // PQ or HLG source; tone mapped to Rec.709 on decode
    QString colorTransfer;
};

struct Bin {
    int id = 0, parent = 0;
    QString name;
};

struct ProjectState {
    QVector<MediaItem> media;
    QVector<Bin> bins;
    QVector<Sequence> seqs;
    int activeSeq = 0;
    int nextId = 1;
    int newId() { return nextId++; }
    MediaItem *mediaById(int id);
    const MediaItem *mediaById(int id) const;
    Sequence *seqById(int id);
    const Sequence *seqById(int id) const;
};

// --- helpers
QString framesToTimecode(Frame f, double fps, bool dropFrame = false);
Frame timecodeToFrames(const QString &tc, double fps, bool dropFrame, bool *ok = nullptr);
QColor labelColor(int idx);
QString labelName(int idx);
int defaultLabel(MediaKind k);
constexpr int kLabelCount = 16;

// --- JSON
QJsonObject stateToJson(const ProjectState &s);
ProjectState stateFromJson(const QJsonObject &o);
QJsonObject clipToJson(const Clip &c);
QJsonObject textStyleToJson(const TextStyle &t);
TextStyle textStyleFromJson(const QJsonObject &o); // missing keys keep their defaults
Clip clipFromJson(const QJsonObject &o);
