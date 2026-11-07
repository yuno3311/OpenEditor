// Warp Stabilizer analysis: tracks the frame-to-frame shift of a clip's source and stores the camera
// path. The effect smooths that path when it renders, so Smoothness can change without re-analysing.
// Translation only: rotation and rolling-shutter wobble are not corrected.
#pragma once
#include <QString>
#include <QVariantMap>
#include <functional>

namespace Stabilize {
struct Result {
    QVariantMap extra; // fps, t0, tx, ty (fractions of the frame width)
    QString error;
};
// progress receives 0..1 and returns false to cancel.
Result analyze(const QString &ffmpeg, const QString &path, double startSec, double durSec, int srcW, int srcH, double fps,
               const std::function<bool(double)> &progress = {});
// Shift that moves cur onto prev, in analysis pixels. Exposed for the tests.
void estimateShift(const unsigned char *prev, const unsigned char *cur, int w, int h, double &dx, double &dy);
}
