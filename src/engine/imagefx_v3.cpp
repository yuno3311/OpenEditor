// Video effects added in 3.0. Images are ARGB32 premultiplied; preview and export share this code.
#include "imagefx.h"
#include <QPainter>
#include <QtMath>
#include <cmath>

namespace ImageFx {
namespace {
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float luma(float r, float g, float b) { return .2126f * r + .7152f * g + .0722f * b; }
inline float smooth(float e0, float e1, float x)
{
    if (e1 <= e0) return x >= e1 ? 1.f : 0.f;
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
QColor colorOf(const Effect &e, const char *id, double def) { return QColor::fromRgb(QRgb(int(e.val(id, 0, def)))); }

// f(r, g, b, x, y) edits straight colour in 0..1; alpha is kept.
template <class F> void mapRGB(QImage &img, F f)
{
    const int w = img.width();
    parallelRows(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const int A = p[x] >> 24;
                if (!A) continue;
                const float k = 1.f / A;
                float r = ((p[x] >> 16) & 255) * k, g = ((p[x] >> 8) & 255) * k, b = (p[x] & 255) * k;
                f(r, g, b, x, y);
                p[x] = (quint32(A) << 24) | (quint32(clamp01(r) * A + .5f) << 16) | (quint32(clamp01(g) * A + .5f) << 8) | quint32(clamp01(b) * A + .5f);
            }
        }
    });
}
// f(x, y) returns the factor (0..1) every channel, alpha included, is multiplied by.
template <class F> void mulAlpha(QImage &img, F f)
{
    const int w = img.width();
    parallelRows(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                if (!p[x]) continue;
                const int m = int(clamp01(f(x, y)) * 256);
                if (m >= 256) continue;
                p[x] = ((((p[x] >> 8) & 0x00FF00FF) * m) & 0xFF00FF00) | ((((p[x] & 0x00FF00FF) * m) >> 8) & 0x00FF00FF);
            }
        }
    });
}
struct Sampler {
    const QImage &src;
    const quint32 *s;
    int w, h, stride;
    explicit Sampler(const QImage &i) : src(i), s(reinterpret_cast<const quint32 *>(i.constBits())), w(i.width()), h(i.height()), stride(i.bytesPerLine() / 4) {}
    inline quint32 nearest(int x, int y) const { return s[qBound(0, y, h - 1) * stride + qBound(0, x, w - 1)]; }
    // Bilinear, clamped to the edges.
    inline quint32 at(float fx, float fy) const
    {
        fx = qBound(0.f, fx, float(w - 1)); fy = qBound(0.f, fy, float(h - 1));
        const int x0 = qMin(int(fx), qMax(0, w - 2)), y0 = qMin(int(fy), qMax(0, h - 2));
        const int x1 = qMin(x0 + 1, w - 1), y1 = qMin(y0 + 1, h - 1);
        const int ax = qBound(0, int((fx - x0) * 256), 256), ay = qBound(0, int((fy - y0) * 256), 256);
        const quint32 c00 = s[y0 * stride + x0], c10 = s[y0 * stride + x1], c01 = s[y1 * stride + x0], c11 = s[y1 * stride + x1];
        quint32 o = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            const int v0 = ((c00 >> sh) & 255) * (256 - ax) + ((c10 >> sh) & 255) * ax;
            const int v1 = ((c01 >> sh) & 255) * (256 - ax) + ((c11 >> sh) & 255) * ax;
            o |= quint32(((v0 * (256 - ay) + v1 * ay) >> 16) & 255) << sh;
        }
        return o;
    }
};
// f(sx, sy) turns a destination pixel into the source position to read.
template <class F> void remap(QImage &img, F f)
{
    const QImage src = img.copy();
    const Sampler sm(src);
    const int w = img.width();
    parallelRows(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                float sx = x, sy = y;
                f(sx, sy);
                p[x] = sm.at(sx, sy);
            }
        }
    });
}
inline quint32 hash(quint32 x, quint32 y, quint32 z)
{
    quint32 h = x * 374761393u + y * 668265263u + z * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float rnd(quint32 x, quint32 y, quint32 z) { return (hash(x, y, z) & 0xFFFFFF) / float(0x1000000); }
void rgb2hsl(float r, float g, float b, float &h, float &s, float &l)
{
    const float mx = qMax(r, qMax(g, b)), mn = qMin(r, qMin(g, b)), d = mx - mn;
    l = (mx + mn) / 2;
    if (d < 1e-6f) { h = s = 0; return; }
    s = l > .5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
}
inline float hue2(float p, float q, float t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1 / 6.f) return p + (q - p) * 6 * t;
    if (t < .5f) return q;
    if (t < 2 / 3.f) return p + (q - p) * (2 / 3.f - t) * 6;
    return p;
}
void hsl2rgb(float h, float s, float l, float &r, float &g, float &b)
{
    h = std::fmod(h, 360.f);
    if (h < 0) h += 360;
    h /= 360;
    if (s <= 0) { r = g = b = l; return; }
    const float q = l < .5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue2(p, q, h + 1 / 3.f); g = hue2(p, q, h); b = hue2(p, q, h - 1 / 3.f);
}
inline float hueDist(float a, float b)
{
    float d = std::fabs(a - b);
    return d > 180 ? 360 - d : d;
}
// Smooth pseudo-random motion: a few sines that never line up.
inline double wobble(double t, double seed)
{
    return (std::sin(t * 6.2831853 + seed) + .6 * std::sin(t * 6.2831853 * 2.17 + seed * 1.7) + .35 * std::sin(t * 6.2831853 * 3.71 + seed * 2.9)) / 1.95;
}
}

bool applyV3(const Effect &e, QImage &img, const Ctx &c)
{
    const QString &t = e.type;
    auto V = [&](const char *id, double d = 0) { return float(e.val(id, c.kt, d)); };
    const int w = img.width(), h = img.height();
    if (w < 1 || h < 1) return false;
    const float px = float(qMax(.01, c.scale)); // one sequence pixel, in image pixels

    if (t == "unsharp") {
        const float amount = V("amount", 80) / 100, thresh = V("thresh") * 255 / 100;
        QImage blur = img.copy();
        boxBlur(blur, qMax(1, int(std::lround(V("radius", 3) * px))));
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                auto *q = reinterpret_cast<const quint32 *>(blur.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    const int A = p[x] >> 24;
                    if (!A) continue;
                    quint32 o = quint32(A) << 24;
                    for (int sh = 0; sh < 24; sh += 8) {
                        const int v = (p[x] >> sh) & 255, d = v - int((q[x] >> sh) & 255);
                        o |= quint32(std::abs(d) >= thresh ? qBound(0, int(v + d * amount), A) : v) << sh;
                    }
                    p[x] = o;
                }
            }
        });
    } else if (t == "radialblur") {
        const bool spin = V("type") > .5f;
        const float amount = V("amount", 20) / 100, cx = V("cx", 50) / 100 * w, cy = V("cy", 50) / 100 * h;
        if (amount <= 0) return true;
        const QImage src = img.copy();
        const Sampler sm(src);
        const int N = 16;
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    const float dx = x - cx, dy = y - cy;
                    int acc[4] = {0, 0, 0, 0};
                    for (int i = 0; i < N; ++i) {
                        const float u = (i / float(N - 1) - .5f) * amount;
                        float sx, sy;
                        if (spin) { const float a = u * 1.2f, ca = std::cos(a), sa = std::sin(a); sx = cx + dx * ca - dy * sa; sy = cy + dx * sa + dy * ca; }
                        else { const float k = 1 + u; sx = cx + dx * k; sy = cy + dy * k; }
                        const quint32 v = sm.at(sx, sy);
                        acc[0] += v >> 24; acc[1] += (v >> 16) & 255; acc[2] += (v >> 8) & 255; acc[3] += v & 255;
                    }
                    p[x] = (quint32(acc[0] / N) << 24) | (quint32(acc[1] / N) << 16) | (quint32(acc[2] / N) << 8) | quint32(acc[3] / N);
                }
            }
        });
    } else if (t == "tiltshift") {
        const int radius = int(std::lround(V("blur", 12) * px));
        if (radius < 1) return true;
        QImage blur = img.copy();
        boxBlur(blur, radius);
        const float a = qDegreesToRadians(V("angle")), nx = -std::sin(a), ny = std::cos(a);
        const float focus = V("focus", 50) / 100, half = qMax(.001f, V("size", 25) / 200), cxp = w / 2.f;
        const float span = std::fabs(h * ny) + std::fabs(w * nx);
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                auto *q = reinterpret_cast<const quint32 *>(blur.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    // Distance from the focus band as a fraction of the frame, then eased into full blur.
                    const float d = std::fabs(((x - cxp) * nx + (y - h * focus) * ny) / span);
                    const int k = int(smooth(half, half * 2 + .05f, d) * 256);
                    if (!k) continue;
                    quint32 o = 0;
                    for (int sh = 0; sh < 32; sh += 8)
                        o |= quint32((((p[x] >> sh) & 255) * (256 - k) + ((q[x] >> sh) & 255) * k) >> 8) << sh;
                    p[x] = o;
                }
            }
        });
    } else if (t == "grain") {
        const float amount = V("amount", 20) / 100 * .5f, size = qMax(1.f, V("size", 1.5f) * px);
        const bool color = V("color") > .5f;
        const quint32 seed = V("animate", 1) > .5f ? quint32(c.kt) + 1 : 1;
        mapRGB(img, [&](float &r, float &g, float &b, int x, int y) {
            const quint32 gx = quint32(x / size), gy = quint32(y / size);
            // Two uniform samples summed: a triangular spread that looks like film, not TV static.
            const float n = (rnd(gx, gy, seed) + rnd(gx, gy, seed + 7919)) - 1;
            // Grain shows most in the midtones.
            const float l = luma(r, g, b), k = amount * (1 - .6f * std::fabs(l * 2 - 1));
            if (color) {
                r += k * ((rnd(gx, gy, seed + 31) + rnd(gx, gy, seed + 67)) - 1);
                g += k * n;
                b += k * ((rnd(gx, gy, seed + 131) + rnd(gx, gy, seed + 257)) - 1);
            } else { r += k * n; g += k * n; b += k * n; }
        });
    } else if (t == "rgbsplit") {
        const float amount = V("amount", 6) * px, a = qDegreesToRadians(V("angle"));
        const float ox = std::cos(a) * amount, oy = -std::sin(a) * amount;
        const QImage src = img.copy();
        const Sampler sm(src);
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    const quint32 r = sm.at(x + ox, y + oy), b = sm.at(x - ox, y - oy), g = p[x];
                    // Each channel keeps its own coverage, so the fringes stay visible past an edge.
                    const quint32 A = qMax(g >> 24, qMax(r >> 24, b >> 24));
                    p[x] = (A << 24) | (r & 0x00FF0000) | (g & 0x0000FF00) | (b & 0x000000FF);
                }
            }
        });
    } else if (t == "glitch") {
        const float amount = V("amount", 35) / 100, split = V("split", 8) * px;
        const int block = qMax(1, int(V("block", 24) * px));
        const quint32 tick = quint32(qMax(0.0, c.seconds) * qMax(.1f, V("speed", 8)));
        if (amount <= 0) return true;
        const QImage src = img.copy();
        const Sampler sm(src);
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                const quint32 row = quint32(y / block);
                const bool hit = rnd(row, tick, 11) < amount * .6f;
                const int shift = hit ? int((rnd(row, tick, 23) - .5f) * 2 * amount * w * .25f) : 0;
                const int sp = hit ? int(split * (rnd(row, tick, 37) + .5f)) : int(split * .25f * amount);
                for (int x = 0; x < w; ++x) {
                    const quint32 g = sm.nearest(x + shift, y), r = sm.nearest(x + shift + sp, y), b = sm.nearest(x + shift - sp, y);
                    const quint32 A = qMax(g >> 24, qMax(r >> 24, b >> 24));
                    p[x] = (A << 24) | (r & 0x00FF0000) | (g & 0x0000FF00) | (b & 0x000000FF);
                }
            }
        });
    } else if (t == "scanlines") {
        const float spacing = qMax(2.f, V("spacing", 4) * px), dark = V("dark", 40) / 100;
        const float off = float(c.seconds * V("scroll") * px);
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float ph = std::fmod((y + off) / spacing, 1.f);
                if (ph < 0) ph += 1;
                // A soft-edged dark line over the second half of each period.
                const float k = 1 - dark * smooth(.4f, .5f, ph) * (1 - smooth(.9f, 1.f, ph));
                const int m = int(k * 256);
                if (m >= 256) continue;
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    const quint32 A = p[x] & 0xFF000000;
                    p[x] = A | ((((p[x] >> 8) & 0x000000FF) * m) & 0x0000FF00) | ((((p[x] & 0x00FF00FF) * m) >> 8) & 0x00FF00FF);
                }
            }
        });
    } else if (t == "halftone") {
        const float size = qMax(2.f, V("size", 10) * px), a = qDegreesToRadians(V("angle", 45)), ca = std::cos(a), sa = std::sin(a);
        const QColor back = colorOf(e, "back", 0);
        const QImage src = img.copy();
        const Sampler sm(src);
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    const int A = p[x] >> 24;
                    if (!A) continue;
                    // Rotate into screen space, find the cell centre, rotate it back to sample the picture.
                    const float u = x * ca + y * sa, v = -x * sa + y * ca;
                    const float cu = (std::floor(u / size) + .5f) * size, cv = (std::floor(v / size) + .5f) * size;
                    const quint32 s = sm.at(cu * ca - cv * sa, cu * sa + cv * ca);
                    const int SA = qMax(1u, s >> 24);
                    const float r = ((s >> 16) & 255) / float(SA), g = ((s >> 8) & 255) / float(SA), b = (s & 255) / float(SA);
                    const float radius = std::sqrt(clamp01(luma(r, g, b))) * size * .72f;
                    const float cover = clamp01(radius - std::hypot(u - cu, v - cv) + .5f);
                    const float orr = float(back.redF()) + (r - float(back.redF())) * cover, og = float(back.greenF()) + (g - float(back.greenF())) * cover,
                                ob = float(back.blueF()) + (b - float(back.blueF())) * cover;
                    p[x] = (quint32(A) << 24) | (quint32(clamp01(orr) * A) << 16) | (quint32(clamp01(og) * A) << 8) | quint32(clamp01(ob) * A);
                }
            }
        });
    } else if (t == "threshold") {
        const float level = V("level", 50) / 100, soft = V("soft") / 200;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) { r = g = b = smooth(level - soft, level + soft, luma(r, g, b)); });
    } else if (t == "solarize") {
        const float th = V("thresh", 50) / 100;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) {
            if (r > th) r = 1 - r;
            if (g > th) g = 1 - g;
            if (b > th) b = 1 - b;
        });
    } else if (t == "gradientmap") {
        const QColor lo = colorOf(e, "low", 0x10204A), mi = colorOf(e, "mid", 0xC8506E), hi = colorOf(e, "high", 0xFFE6AA);
        const float blend = V("blend") / 100;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) {
            const float l = luma(r, g, b);
            const QColor &c0 = l < .5f ? lo : mi, &c1 = l < .5f ? mi : hi;
            const float u = l < .5f ? l * 2 : l * 2 - 1;
            r = (float(c0.redF()) + float(c1.redF() - c0.redF()) * u) * (1 - blend) + r * blend;
            g = (float(c0.greenF()) + float(c1.greenF() - c0.greenF()) * u) * (1 - blend) + g * blend;
            b = (float(c0.blueF()) + float(c1.blueF() - c0.blueF()) * u) * (1 - blend) + b * blend;
        });
    } else if (t == "hslsecondary" || t == "changecolor") {
        const bool change = t == "changecolor";
        const QColor key = colorOf(e, change ? "from" : "key", 0x2060D0), to = colorOf(e, "to", 0x3060D0);
        float kh, ks, kl, th, ts, tl;
        rgb2hsl(float(key.redF()), float(key.greenF()), float(key.blueF()), kh, ks, kl);
        rgb2hsl(float(to.redF()), float(to.greenF()), float(to.blueF()), th, ts, tl);
        const float range = V(change ? "tol" : "range", 30), soft = V("soft", 15);
        const float minSat = change ? .12f : V("minsat", 15) / 100;
        const float dh = change ? th - kh : V("hue"), ds = change ? (ks > .01f ? ts / ks - 1 : 0) : V("sat") / 100, dl = change ? (tl - kl) * .5f : V("light") / 100;
        const bool show = !change && V("show") > .5f;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) {
            float H, S, L;
            rgb2hsl(r, g, b, H, S, L);
            // Inside the range the pixel is fully selected; the selection fades over the softness.
            const float m = (1 - smooth(range, range + qMax(.01f, soft), hueDist(H, kh))) * smooth(minSat * .5f, minSat + .001f, S);
            if (show) { r = g = b = m; return; }
            if (m <= 0) return;
            float nr, ng, nb;
            hsl2rgb(H + dh, clamp01(S * (1 + ds)), clamp01(L + dl * (dl > 0 ? 1 - L : L)), nr, ng, nb);
            r += (nr - r) * m; g += (ng - g) * m; b += (nb - b) * m;
        });
    } else if (t == "letterbox") {
        static const double ratios[] = {2.39, 2.35, 2.0, 1.85, 16.0 / 9, 4.0 / 3, 1.0, 0.8, 9.0 / 16};
        const double want = ratios[qBound(0, int(V("aspect")), 8)], have = double(w) / h;
        QColor col = colorOf(e, "color", 0);
        col.setAlphaF(clamp01(V("opacity", 100) / 100));
        QPainter p(&img);
        if (want > have) {
            const int bar = int(std::lround((h - w / want) / 2));
            p.fillRect(0, 0, w, bar, col);
            p.fillRect(0, h - bar, w, bar, col);
        } else if (want < have) {
            const int bar = int(std::lround((w - h * want) / 2));
            p.fillRect(0, 0, bar, h, col);
            p.fillRect(w - bar, 0, bar, h, col);
        }
    } else if (t == "shapemask") {
        const bool rect = V("shape") > .5f, inv = V("invert") > .5f;
        const float cx = V("cx", 50) / 100 * w, cy = V("cy", 50) / 100 * h;
        const float rx = qMax(.5f, V("w", 50) / 200 * w), ry = qMax(.5f, V("h", 50) / 200 * h);
        const float a = qDegreesToRadians(V("rot")), ca = std::cos(a), sa = std::sin(a);
        const float feather = V("feather", 10) / 100;
        mulAlpha(img, [&](int x, int y) {
            const float dx = x - cx, dy = y - cy;
            const float u = (dx * ca + dy * sa) / rx, v = (-dx * sa + dy * ca) / ry;
            // d is 1 on the outline, below 1 inside.
            const float d = rect ? qMax(std::fabs(u), std::fabs(v)) : std::sqrt(u * u + v * v);
            const float edgePx = qMin(rx, ry);
            const float m = feather > 0 ? 1 - smooth(1 - feather, 1, d) : clamp01((1 - d) * edgePx + .5f);
            return inv ? 1 - m : m;
        });
    } else if (t == "kaleido") {
        const int n = qBound(2, int(V("segments", 6)), 64);
        const float seg = float(2 * M_PI / n), rot = qDegreesToRadians(V("angle"));
        const float cx = V("cx", 50) / 100 * w, cy = V("cy", 50) / 100 * h;
        remap(img, [&](float &x, float &y) {
            const float dx = x - cx, dy = y - cy, r = std::sqrt(dx * dx + dy * dy);
            float a = std::fmod(std::atan2(dy, dx) - rot, seg);
            if (a < 0) a += seg;
            if (a > seg / 2) a = seg - a;
            float sx = cx + r * std::cos(a + rot), sy = cy + r * std::sin(a + rot);
            // Mirror back into the frame so the pattern has no smeared edge pixels.
            auto fold = [](float v, float m) { v = std::fmod(std::fabs(v), 2 * m); return v > m ? 2 * m - v : v; };
            x = fold(sx, float(w - 1)); y = fold(sy, float(h - 1));
        });
    } else if (t == "shake") {
        const double tm = c.seconds * qMax(.01f, V("speed", 4));
        const float amount = V("amount", 12) * px;
        const float dx = float(wobble(tm, 1.3)) * amount, dy = float(wobble(tm * 1.13, 4.1)) * amount;
        const float a = qDegreesToRadians(V("rot", 1)) * float(wobble(tm * .87, 7.7));
        // Enough zoom that the largest move never shows past an edge.
        const float cover = V("autoscale", 1) > .5f
            ? 1 + 2 * amount / qMin(w, h) + std::fabs(qDegreesToRadians(V("rot", 1))) * float(qMax(w, h)) / qMin(w, h) : 1;
        const float ca = std::cos(a) / cover, sa = std::sin(a) / cover, cx = w / 2.f, cy = h / 2.f;
        const QImage src = img.copy();
        const Sampler sm(src);
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    const float ux = x - cx, uy = y - cy;
                    const float sx = cx + ux * ca - uy * sa - dx / cover, sy = cy + ux * sa + uy * ca - dy / cover;
                    p[x] = (sx < -.5f || sy < -.5f || sx > w - .5f || sy > h - .5f) ? 0 : sm.at(sx, sy);
                }
            }
        });
    } else if (t == "timecode") {
        const double fps = c.fps > 0 ? c.fps : 30;
        const double sec = V("source") > .5f && c.srcSec >= 0 ? c.srcSec : qMax(0.0, c.seconds);
        const QString text = framesToTimecode(Frame(std::floor(sec * fps + 1e-6)), fps, false);
        QFont f("Consolas");
        f.setStyleHint(QFont::Monospace);
        f.setPixelSize(qMax(6, int(h * V("size", 5) / 100)));
        f.setBold(true);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int pad = fm.height() / 4, bw = fm.horizontalAdvance(text) + pad * 4, bh = fm.height() + pad * 2, margin = h / 30;
        const int pos = qBound(0, int(V("pos", 1)), 5), colIdx = pos % 3;
        const int x = colIdx == 0 ? margin : colIdx == 1 ? (w - bw) / 2 : w - bw - margin;
        const int y = pos < 3 ? margin : h - bh - margin;
        p.fillRect(x, y, bw, bh, QColor(0, 0, 0, int(clamp01(V("back", 60) / 100) * 255)));
        p.setPen(colorOf(e, "color", 0xFFFFFF));
        p.drawText(QRect(x, y, bw, bh), Qt::AlignCenter, text);
    } else {
        return false;
    }
    return true;
}
}
