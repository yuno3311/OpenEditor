// CPU image effects. Both preview and export run this exact code, so what you see is what renders.
#pragma once
#include "../core/model.h"
#include <QImage>
#include <functional>

namespace ImageFx {
struct Ctx {
    Frame kt = 0;        // keyframe time
    double scale = 1;    // image pixels per sequence pixel, so radii look the same at any res
    double seconds = 0;  // sequence time, drives animated effects
};
void apply(const Effect &e, QImage &img, const Ctx &c); // img: ARGB32_Premultiplied
void boxBlur(QImage &img, int radius, int passes = 3);
void parallelRows(int h, const std::function<void(int, int)> &fn);
QImage bars(int w, int h);
QImage renderText(const TextStyle &t, int w, int h, double scale);
QString wrapText(const QString &text, const QString &family, double pixelSize, double maxWidth);
QVector<float> curveLut(const QVariantList &pts); // 256 entries, 0..1
}
