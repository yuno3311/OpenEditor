#include "compositor.h"
#include "imagefx.h"
#include "../core/mediacache.h"
#include <QImageReader>
#include <QPainterPath>
#include <QtMath>
#include <cmath>

static const QPainter::CompositionMode kBlend[] = {
    QPainter::CompositionMode_SourceOver, QPainter::CompositionMode_Multiply, QPainter::CompositionMode_Screen,
    QPainter::CompositionMode_Overlay, QPainter::CompositionMode_Darken, QPainter::CompositionMode_Lighten,
    QPainter::CompositionMode_ColorDodge, QPainter::CompositionMode_ColorBurn, QPainter::CompositionMode_HardLight,
    QPainter::CompositionMode_SoftLight, QPainter::CompositionMode_Difference, QPainter::CompositionMode_Exclusion,
    QPainter::CompositionMode_Plus};

static int evenDim(double v) { return qMax(2, int(v + .5) & ~1); }

VideoReader *Compositor::reader(const MediaItem &m, int clipId, double scale)
{
    QString proxy = useProxies ? CACHE->proxyFor(m.path) : QString();
    QString key = QString("%1#%2#%3#%4").arg(clipId).arg(m.id).arg(scale).arg(proxy.isEmpty());
    auto it = readers.find(key);
    if (it == readers.end()) {
        Entry e;
        e.r = std::make_shared<VideoReader>(proxy.isEmpty() ? m.path : proxy, evenDim(m.w * scale), evenDim(m.h * scale), m.fps, m.hasAlpha,
                                            m.hdr && proxy.isEmpty() ? VideoReader::hdrToneMapFilter() : QString());
        it = readers.insert(key, e);
    }
    it->used = tick;
    return it->r.get();
}

void Compositor::sweep()
{
    for (auto it = readers.begin(); it != readers.end();) {
        if (tick - it->used > 90) it = readers.erase(it);
        else ++it;
    }
}
void Compositor::releaseAll() { readers.clear(); gpu.reset(); }

QImage Compositor::still(const QString &path, double scale)
{
    QString key = QString("%1@%2").arg(path).arg(scale);
    auto it = stills.constFind(key);
    if (it != stills.constEnd()) return *it;
    QImageReader r(path);
    r.setAutoTransform(true);
    QImage img = r.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (scale < 1 && !img.isNull()) img = img.scaled(evenDim(img.width() * scale), evenDim(img.height() * scale), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (stills.size() > 64) stills.clear();
    stills.insert(key, img);
    return img;
}

QImage Compositor::source(const ProjectState &st, const Sequence &q, const Clip &c, Frame f, double scale, bool exact, int depth, QSizeF &native, QImage *canvas)
{
    const int cw = evenDim(q.w * scale), ch = evenDim(q.h * scale);
    native = QSizeF(q.w, q.h);
    auto gen = [&](const QColor &col) { QImage i(cw, ch, QImage::Format_ARGB32_Premultiplied); i.fill(col); return i; };
    if (c.kind == MediaKind::Text) return ImageFx::renderText(c.text, cw, ch, scale);
    if (c.kind == MediaKind::Adjustment) return canvas ? canvas->copy() : QImage();
    const MediaItem *m = st.mediaById(c.mediaId);
    if (!m) return {};
    switch (c.kind) {
    case MediaKind::Color: return gen(m->color);
    case MediaKind::Black: return gen(Qt::black);
    case MediaKind::Bars: return ImageFx::bars(cw, ch);
    case MediaKind::Image: {
        native = QSizeF(m->w, m->h);
        return still(m->path, scale);
    }
    case MediaKind::Video: {
        if (m->offline || m->w <= 0) return {};
        native = QSizeF(m->w, m->h);
        auto *rd = reader(*m, c.id, scale);
        double t = c.sourceTime(f, q.fps());
        Frame n = Frame(t * m->fps + 1e-4);
        Frame last = Frame(m->duration * m->fps) - 1;
        if (last > 0) n = qMin(n, last);
        return rd->get(n, exact ? 30000 : 2500);
    }
    case MediaKind::Nested: {
        if (depth > 6 || c.mediaId == 0) return {};
        const Sequence *inner = st.seqById(m->seqId);
        if (!inner || inner->id == q.id) return {};
        native = QSizeF(inner->w, inner->h);
        Frame nf = Frame(c.sourceTime(f, q.fps()) * inner->fps() + 1e-4);
        return renderSeq(st, *inner, nf, scale, exact, false, depth + 1, c.cam);
    }
    default: return {};
    }
}

void Compositor::drawClip(QPainter &p, QImage &canvas, const ProjectState &st, const Sequence &q, const Clip &c, Frame f, double scale, bool exact, int depth)
{
    QSizeF native;
    QImage img = source(st, q, c, f, scale, exact, depth, native, c.kind == MediaKind::Adjustment ? &canvas : nullptr);
    if (img.isNull()) return;
    Frame kt = c.kt(f);
    ImageFx::Ctx ctx;
    ctx.kt = kt;
    ctx.scale = scale;
    ctx.seconds = double(f) / q.fps();
    runEffects(c, img, ctx);
    const Effect *mo = c.effect("motion"), *op = c.effect("opacity");
    double opacity = op && op->enabled ? op->val("opacity", kt, 100) / 100.0 : 1.0;
    int blend = op ? qBound(0, int(op->val("blend", 0)), 12) : 0;
    p.save();
    p.setOpacity(opacity);
    p.setCompositionMode(kBlend[blend]);
    if (c.kind == MediaKind::Adjustment) {
        p.drawImage(0, 0, img);
    } else if (mo && mo->enabled) {
        double sx = mo->val("scale", kt, 100) / 100.0;
        double sy = sx;
        if (mo->val("uniform", 0, 1) < .5) sx = mo->val("scaleW", kt, 100) / 100.0;
        double ax = mo->val("ancX", kt) * img.width() / native.width(), ay = mo->val("ancY", kt) * img.height() / native.height();
        QTransform tr;
        tr.scale(scale, scale);
        tr.translate(mo->val("posX", kt), mo->val("posY", kt));
        tr.rotate(mo->val("rot", kt));
        tr.scale(sx * native.width() / img.width(), sy * native.height() / img.height());
        tr.translate(-ax, -ay);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setTransform(tr, false);
        p.drawImage(0, 0, img);
    } else {
        p.drawImage(QRectF(canvas.rect()), img);
    }
    p.restore();
}

// Runs of GPU-capable effects share one upload/readback; everything else stays on the CPU, in order.
void Compositor::runEffects(const Clip &c, QImage &img, const ImageFx::Ctx &ctx)
{
    QVector<const Effect *> batch;
    auto prep = [&] {
        if (img.format() != QImage::Format_ARGB32_Premultiplied) img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        else img.detach();
    };
    auto flush = [&] {
        if (batch.isEmpty()) return;
        prep();
        if (!gpu) gpu = std::make_unique<GpuFx>();
        if (gpu->applyChain(batch, img, ctx)) ++gpuBatches;
        else for (auto *e : batch) ImageFx::apply(*e, img, ctx);
        batch.clear();
    };
    for (auto &e : c.fx) {
        if (!e.enabled || e.type == "motion" || e.type == "opacity" || e.type == "timeremap") continue;
        if (GpuFx::enabled && GpuFx::supports(e)) { batch << &e; continue; }
        flush();
        prep();
        ImageFx::apply(e, img, ctx);
    }
    flush();
}

void Compositor::drawCaption(QPainter &p, const Sequence &q, Frame f, double scale, int w, int h)
{
    for (auto &c : q.captions) {
        if (f < c.start || f >= c.end) continue;
        QString key = QString("%1|%2|%3|%4").arg(c.text).arg(q.captionSize).arg(w).arg(scale);
        if (key != capKey) {
            TextStyle t;
            t.text = ImageFx::wrapText(c.text, t.font, q.captionSize, q.w * .86);
            t.size = q.captionSize; t.bold = true; t.background = true; t.bgColor = QColor(0, 0, 0, 170);
            capImg = ImageFx::renderText(t, w, h, scale);
            capKey = key;
        }
        p.drawImage(QPointF(0, h * .37), capImg);
        return;
    }
}

static void paintDissolve(QPainter &p, const QImage &a, const QImage &b, double u)
{
    p.setOpacity(1);
    p.drawImage(0, 0, a);
    p.setOpacity(u);
    p.drawImage(0, 0, b);
    p.setOpacity(1);
}

QImage Compositor::transitionFrame(const ProjectState &st, const Sequence &q, const Track &t, const Transition &x, Frame f, double scale, bool exact, int depth)
{
    const int w = evenDim(q.w * scale), h = evenDim(q.h * scale);
    const Clip *ca = nullptr, *cb = nullptr;
    for (auto &c : t.clips) {
        if (c.end() == x.cut) ca = &c;
        if (c.start == x.cut) cb = &c;
    }
    auto layer = [&](const Clip *c) {
        QImage l(w, h, QImage::Format_ARGB32_Premultiplied);
        l.fill(0);
        if (c && c->enabled) { QPainter p(&l); drawClip(p, l, st, q, *c, f, scale, exact, depth); }
        return l;
    };
    QImage a = layer(ca), b = layer(cb);
    double u = qBound(0.0, double(f - x.start() + 1) / qMax<Frame>(1, x.dur), 1.0);
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    out.fill(0);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QString &ty = x.type;
    auto clipped = [&](const QPainterPath &path) {
        p.drawImage(0, 0, a);
        p.setClipPath(path);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(0, 0, b);
    };
    if (ty == "xdissolve" || ty == "filmdissolve") {
        paintDissolve(p, a, b, ty == "filmdissolve" ? u * u * (3 - 2 * u) : u);
    } else if (ty == "additive") {
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.setOpacity(qMin(1.0, 2 * (1 - u))); p.drawImage(0, 0, a);
        p.setOpacity(qMin(1.0, 2 * u)); p.drawImage(0, 0, b);
    } else if (ty == "dipblack" || ty == "dipwhite") {
        p.fillRect(out.rect(), ty == "dipblack" ? Qt::black : Qt::white);
        p.setOpacity(u < .5 ? 1 - u * 2 : u * 2 - 1);
        p.drawImage(0, 0, u < .5 ? a : b);
    } else if (ty == "wipeL") {
        QPainterPath path; path.addRect(0, 0, w * u, h); clipped(path);
    } else if (ty == "wipeD") {
        QPainterPath path; path.addRect(0, 0, w, h * u); clipped(path);
    } else if (ty == "barn") {
        QPainterPath path; path.addRect(w / 2.0 * (1 - u), 0, w * u, h); clipped(path);
    } else if (ty == "clock") {
        QPainterPath path; path.moveTo(w / 2.0, h / 2.0);
        double r = std::hypot(w, h);
        path.arcTo(QRectF(w / 2.0 - r, h / 2.0 - r, 2 * r, 2 * r), 90, -360 * u);
        path.closeSubpath(); clipped(path);
    } else if (ty == "irisround") {
        QPainterPath path; double r = std::hypot(w, h) / 2 * u;
        path.addEllipse(QPointF(w / 2.0, h / 2.0), r, r); clipped(path);
    } else if (ty == "irisbox") {
        QPainterPath path; path.addRect(w / 2.0 * (1 - u), h / 2.0 * (1 - u), w * u, h * u); clipped(path);
    } else if (ty == "irisdiamond") {
        double rx = w * u, ry = h * u;
        QPainterPath path; path.moveTo(w / 2.0, h / 2.0 - ry); path.lineTo(w / 2.0 + rx, h / 2.0);
        path.lineTo(w / 2.0, h / 2.0 + ry); path.lineTo(w / 2.0 - rx, h / 2.0); path.closeSubpath(); clipped(path);
    } else if (ty == "push") {
        p.drawImage(QPointF(w * u, 0), a);
        p.drawImage(QPointF(w * u - w, 0), b);
    } else if (ty == "slide") {
        p.drawImage(0, 0, a);
        p.drawImage(QPointF(w * u - w, 0), b);
    } else if (ty == "zoom") {
        double z = 1 + u * 1.5;
        p.save(); p.translate(w / 2.0, h / 2.0); p.scale(z, z); p.translate(-w / 2.0, -h / 2.0); p.setOpacity(1 - u); p.drawImage(0, 0, a); p.restore();
        double z2 = .6 + .4 * u;
        p.save(); p.translate(w / 2.0, h / 2.0); p.scale(z2, z2); p.translate(-w / 2.0, -h / 2.0); p.setOpacity(u); p.drawImage(0, 0, b); p.restore();
    } else {
        paintDissolve(p, a, b, u);
    }
    p.end();
    return out;
}

QImage Compositor::renderSeq(const ProjectState &st, const Sequence &q, Frame f, double scale, bool exact, bool opaque, int depth, int solo)
{
    const int w = evenDim(q.w * scale), h = evenDim(q.h * scale);
    QImage canvas(w, h, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(opaque ? 0xFF000000u : 0u);
    QPainter p(&canvas);
    int trackIndex = -1;
    for (const Track &t : q.video) {
        ++trackIndex;
        if (solo >= 0 && trackIndex != solo) continue;
        if (t.muted) continue;
        const Transition *act = nullptr;
        for (auto &x : t.transitions)
            if (f >= x.start() && f < x.end()) { act = &x; break; }
        if (act) {
            QImage img = transitionFrame(st, q, t, *act, f, scale, exact, depth);
            p.drawImage(0, 0, img);
        } else if (auto *c = t.clipAt(f)) {
            if (c->enabled) drawClip(p, canvas, st, q, *c, f, scale, exact, depth);
        }
    }
    if (q.burnCaptions && !q.captions.isEmpty()) drawCaption(p, q, f, scale, w, h);
    p.end();
    return canvas;
}

QImage Compositor::render(const ProjectState &st, int seqId, Frame f, double scale, bool exact)
{
    ++tick;
    const Sequence *q = st.seqById(seqId);
    if (!q) return {};
    QImage img = renderSeq(st, *q, f, scale, exact, true, 0);
    sweep();
    return img;
}

QImage Compositor::renderAngle(const ProjectState &st, int seqId, Frame f, double scale, int angle)
{
    ++tick;
    const Sequence *q = st.seqById(seqId);
    if (!q) return {};
    QImage img = renderSeq(st, *q, f, scale, false, true, 0, angle);
    sweep();
    return img;
}

void Compositor::preroll(const ProjectState &st, int seqId, Frame f, double scale)
{
    const Sequence *q = st.seqById(seqId);
    if (!q) return;
    Frame horizon = f + Frame(q->fps());
    for (const Track &t : q->video) {
        if (t.muted) continue;
        for (auto &c : t.clips) {
            if (c.start <= f || c.start > horizon || c.kind != MediaKind::Video || !c.enabled) continue;
            if (auto *m = st.mediaById(c.mediaId))
                if (!m->offline && m->w > 0)
                    reader(*m, c.id, scale)->prefetch(Frame(c.sourceTime(c.start, q->fps()) * m->fps + 1e-4));
        }
    }
}
