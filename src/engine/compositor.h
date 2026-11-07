// Layered frame renderer used by preview playback, frame export and final export.
#pragma once
#include "../core/model.h"
#include "imagefx.h"
#include "gpufx.h"
#include "videoreader.h"
#include <QHash>
#include <QImage>
#include <QPainter>
#include <memory>

class Compositor {
public:
    // scale: fraction of sequence resolution. exact: block until each decoded frame is available.
    QImage render(const ProjectState &st, int seqId, Frame f, double scale, bool exact);
    QImage renderAngle(const ProjectState &st, int seqId, Frame f, double scale, int angle);
    void preroll(const ProjectState &st, int seqId, Frame f, double scale);
    void releaseAll();
    bool useProxies = false;
    int gpuBatches = 0;                 // diagnostics: effect chains that ran on the GPU
private:
    QImage renderSeq(const ProjectState &st, const Sequence &q, Frame f, double scale, bool exact, bool opaque, int depth, int solo = -1);
    void runEffects(const Clip &c, QImage &img, const ImageFx::Ctx &ctx);
    void drawCaption(QPainter &p, const Sequence &q, Frame f, double scale, int w, int h);
    void drawClip(QPainter &p, QImage &canvas, const ProjectState &st, const Sequence &q, const Clip &c, Frame f, double scale, bool exact, int depth);
    QImage source(const ProjectState &st, const Sequence &q, const Clip &c, Frame f, double scale, bool exact, int depth, QSizeF &native, QImage *canvas);
    QImage transitionFrame(const ProjectState &st, const Sequence &q, const Track &t, const Transition &x, Frame f, double scale, bool exact, int depth);
    VideoReader *reader(const MediaItem &m, int clipId, double scale);
    QImage still(const QString &path, double scale);
    struct Entry { std::shared_ptr<VideoReader> r; quint64 used = 0; };
    QHash<QString, Entry> readers;
    QHash<QString, QImage> stills;
    quint64 tick = 0;
    std::unique_ptr<GpuFx> gpu;
    QString capKey;
    QImage capImg;
    void sweep();
};
