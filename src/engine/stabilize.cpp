#include "stabilize.h"
#include <QProcess>
#include <QVariantList>
#include <QVector>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace Stabilize {

static const int kW = 192; // analysis width: enough detail for shake, cheap enough to track every frame

// Mean absolute difference between cur shifted by (dx, dy) and prev over the inner area.
static long long sad(const unsigned char *prev, const unsigned char *cur, int w, int h, int margin, int dx, int dy, int step)
{
    long long s = 0;
    for (int y = margin; y < h - margin; y += step) {
        const unsigned char *p = prev + y * w, *c = cur + (y + dy) * w + dx;
        for (int x = margin; x < w - margin; x += step) s += std::abs(int(p[x]) - int(c[x]));
    }
    return s;
}

static void bestShift(const unsigned char *prev, const unsigned char *cur, int w, int h, int margin, int cx, int cy, int r, int &bx, int &by, long long *costs = nullptr)
{
    long long best = LLONG_MAX;
    bx = cx; by = cy;
    for (int dy = cy - r; dy <= cy + r; ++dy)
        for (int dx = cx - r; dx <= cx + r; ++dx) {
            if (std::abs(dx) >= margin || std::abs(dy) >= margin) continue;
            long long s = sad(prev, cur, w, h, margin, dx, dy, 1);
            if (costs) costs[(dy - cy + r) * (2 * r + 1) + (dx - cx + r)] = s;
            if (s < best) { best = s; bx = dx; by = dy; }
        }
}

void estimateShift(const unsigned char *prev, const unsigned char *cur, int w, int h, double &outX, double &outY)
{
    // Coarse search at half size, then refine at full size with a sub-pixel parabola fit.
    const int hw = w / 2, hh = h / 2;
    QVector<unsigned char> p2(hw * hh), c2(hw * hh);
    for (int y = 0; y < hh; ++y)
        for (int x = 0; x < hw; ++x) {
            auto avg = [&](const unsigned char *s) { return (s[2 * y * w + 2 * x] + s[2 * y * w + 2 * x + 1] + s[(2 * y + 1) * w + 2 * x] + s[(2 * y + 1) * w + 2 * x + 1] + 2) / 4; };
            p2[y * hw + x] = avg(prev);
            c2[y * hw + x] = avg(cur);
        }
    int hx, hy;
    bestShift(p2.constData(), c2.constData(), hw, hh, 10, 0, 0, 8, hx, hy);
    int fx, fy;
    long long cost[25];
    for (auto &c : cost) c = LLONG_MAX;
    bestShift(prev, cur, w, h, 20, hx * 2, hy * 2, 2, fx, fy, cost);
    auto at = [&](int dx, int dy) { return cost[(fy + dy - hy * 2 + 2) * 5 + (fx + dx - hx * 2 + 2)]; };
    auto sub = [](long long a, long long b, long long c) {
        if (a == LLONG_MAX || c == LLONG_MAX) return 0.0;
        double d = double(a) - 2.0 * b + double(c);
        return d > 0 ? qBound(-.5, (double(a) - double(c)) / (2 * d), .5) : 0.0;
    };
    const bool inX = std::abs(fx - hx * 2) < 2, inY = std::abs(fy - hy * 2) < 2;
    outX = fx + (inX ? sub(at(-1, 0), at(0, 0), at(1, 0)) : 0);
    outY = fy + (inY ? sub(at(0, -1), at(0, 0), at(0, 1)) : 0);
}

Result analyze(const QString &ffmpeg, const QString &path, double startSec, double durSec, int srcW, int srcH, double fps, const std::function<bool(double)> &progress)
{
    Result r;
    if (ffmpeg.isEmpty()) { r.error = "FFmpeg was not found."; return r; }
    if (srcW <= 0 || srcH <= 0 || fps <= 0 || durSec <= 0) { r.error = "The clip has no video to analyse."; return r; }
    const int w = kW, h = qMax(32, int(std::lround(double(kW) * srcH / srcW / 2)) * 2);
    QProcess p;
    p.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-nostdin", "-ss", QString::number(startSec, 'f', 4), "-t", QString::number(durSec, 'f', 4), "-i", path,
                     "-an", "-sn", "-fps_mode", "passthrough", "-vf", QString("scale=%1:%2:flags=area,format=gray").arg(w).arg(h), "-f", "rawvideo", "pipe:1"});
    if (!p.waitForStarted(10000)) { r.error = "Could not start FFmpeg."; return r; }
    const qint64 frameBytes = qint64(w) * h;
    const double expected = qMax(1.0, durSec * fps);
    QByteArray buf, prev;
    QVariantList tx, ty;
    double ax = 0, ay = 0;
    bool cancelled = false;
    auto consume = [&] {
        while (buf.size() >= frameBytes) {
            QByteArray cur = buf.left(frameBytes);
            buf.remove(0, frameBytes);
            if (!prev.isEmpty()) {
                double dx, dy;
                estimateShift(reinterpret_cast<const unsigned char *>(prev.constData()), reinterpret_cast<const unsigned char *>(cur.constData()), w, h, dx, dy);
                // cur(x + d) matches prev(x): the picture moved by +d. The path is where the picture has drifted to.
                ax += dx; ay += dy;
            }
            tx << ax / w;
            ty << ay / w;
            prev = cur;
            if (progress && tx.size() % 10 == 0 && !progress(qMin(.99, tx.size() / expected))) { cancelled = true; return; }
        }
    };
    while (!cancelled) {
        bool more = p.waitForReadyRead(60000);
        buf += p.readAllStandardOutput();
        consume();
        if (!more) {
            if (p.state() != QProcess::NotRunning) { r.error = "FFmpeg stopped responding."; p.kill(); p.waitForFinished(3000); return r; }
            break;
        }
    }
    if (cancelled) { p.kill(); p.waitForFinished(3000); r.error = "Cancelled."; return r; }
    p.waitForFinished(10000);
    buf += p.readAllStandardOutput();
    consume();
    if (tx.size() < 2) { r.error = "FFmpeg returned no frames for this clip."; return r; }
    r.extra = {{"fps", fps}, {"t0", startSec}, {"tx", tx}, {"ty", ty}, {"w", w}, {"h", h}};
    if (progress) progress(1);
    return r;
}

} // namespace Stabilize
