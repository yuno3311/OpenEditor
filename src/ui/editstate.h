// Shared, non-undoable editing state: current tool, source patching and the clip clipboard.
#pragma once
#include "../core/model.h"
#include <QObject>

enum class Tool { Select, TrackFwd, Ripple, Roll, Rate, Razor, Slip, Slide, Pen, Hand, Zoom, Text };

struct ClipboardEntry {
    Clip clip;
    bool video = true;
    int trackIndex = 0;
};

class EditState : public QObject {
    Q_OBJECT
public:
    static EditState *instance() { static EditState e; return &e; }
    Tool tool = Tool::Select;
    int patchV = 0, patchA = 0;     // source patch: destination track index, -1 = disconnected
    QVector<ClipboardEntry> clipboard;
    QString defaultVideoTransition = QStringLiteral("xdissolve");
    QString defaultAudioTransition = QStringLiteral("a_cpower");
    void setTool(Tool t) { if (t != tool) { tool = t; emit toolChanged(t); } }
signals:
    void toolChanged(Tool t);
};
#define EDIT (EditState::instance())

// Mime types for in-app drags
inline constexpr const char *kMimeMedia = "application/x-oe-media";
inline constexpr const char *kMimeEffect = "application/x-oe-effect";
inline constexpr const char *kMimeTransition = "application/x-oe-transition";
