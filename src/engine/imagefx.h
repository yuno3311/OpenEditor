// CPU image effects. Both preview and export run this exact code, so what you see is what renders.
#pragma once
#include "../core/model.h"
#include <QFont>
#include <QImage>
#include <QRectF>
#include <functional>

namespace ImageFx {
struct Ctx {
    Frame kt = 0;        // keyframe time
    double scale = 1;    // image pixels per sequence pixel, so radii look the same at any res
    double seconds = 0;  // sequence time, drives animated effects
    double srcSec = -1;  // time in the clip's source media, for effects analysed against it
    double fps = 30;     // sequence frame rate, for timecode
};
void apply(const Effect &e, QImage &img, const Ctx &c); // img: ARGB32_Premultiplied
void boxBlur(QImage &img, int radius, int passes = 3);
bool applyV3(const Effect &e, QImage &img, const Ctx &c); // the 3.0 effects; false if e is not one of them
void parallelRows(int h, const std::function<void(int, int)> &fn);
QImage bars(int w, int h);
// Titles. The animation state comes from textAnimAt for the time within the clip.
struct TextAnim { double reveal = 1, opacity = 1, dx = 0, dy = 0, scale = 1, wipe = 1; };
struct TextLine { QString text; double x = 0, baseline = 0, width = 0; bool paraEnd = true; };
struct TextLayout { QFont font; QVector<TextLine> lines; QRectF block; double ascent = 0, lineHeight = 0, lineStep = 0; };
QFont titleFont(const TextStyle &t, double scale);
TextLayout layoutTitle(const TextStyle &t, int w, int h, double scale);
QImage renderText(const TextStyle &t, int w, int h, double scale, const TextAnim &anim = {});
QRectF textBounds(const TextStyle &t, int w, int h); // the text block in sequence pixels
TextAnim textAnimAt(const TextStyle &t, double secondsIntoClip, double clipSeconds);
QString wrapText(const QString &text, const QString &family, double pixelSize, double maxWidth);
QVector<float> curveLut(const QVariantList &pts); // 256 entries, 0..1
}
