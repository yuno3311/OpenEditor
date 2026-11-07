#include "imagefx.h"
#include "pluginhost.h"
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QPainter>
#include <QPainterPath>
#include <QTextStream>
#include <QThread>
#include <QtConcurrent>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>

namespace ImageFx {

void parallelRows(int h, const std::function<void(int, int)> &fn)
{
    int n = qBound(1, QThread::idealThreadCount(), 16);
    if (h < 64 || n == 1) { fn(0, h); return; }
    QVector<QPair<int, int>> jobs;
    for (int i = 0; i < n; ++i) jobs << qMakePair(h * i / n, h * (i + 1) / n);
    QtConcurrent::blockingMap(jobs, [&](const QPair<int, int> &r) { fn(r.first, r.second); });
}

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float luma(float r, float g, float b) { return .2126f * r + .7152f * g + .0722f * b; }

// Run a per-pixel colour function in straight alpha on a premultiplied image.
template <class F> static void mapRGB(QImage &img, F f)
{
    const int w = img.width();
    parallelRows(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                quint32 c = p[x];
                int a = c >> 24;
                if (!a) continue;
                float k = a == 255 ? 1.f / 255 : 1.f / a;
                float r = ((c >> 16) & 255) * k, g = ((c >> 8) & 255) * k, b = (c & 255) * k;
                f(r, g, b, x, y);
                float m = float(a);
                p[x] = (quint32(a) << 24) | (quint32(clamp01(r) * m + .5f) << 16) | (quint32(clamp01(g) * m + .5f) << 8) | quint32(clamp01(b) * m + .5f);
            }
        }
    });
}

void boxBlur(QImage &img, int radius, int passes)
{
    if (radius < 1) return;
    const int w = img.width(), h = img.height();
    const int bpl = img.bytesPerLine() / 4;
    radius = qMin(radius, qMax(w, h) / 2);
    QImage tmpI(w, h, img.format());
    auto *src = reinterpret_cast<quint32 *>(img.bits());
    auto *tmp = reinterpret_cast<quint32 *>(tmpI.bits());
    const int tbpl = tmpI.bytesPerLine() / 4;
    const int win = radius * 2 + 1;
    auto blur1d = [&](const quint32 *in, quint32 *out, int n, int inStep, int outStep) {
        int sum[4] = {0, 0, 0, 0};
        auto add = [&](quint32 c, int s) {
            sum[0] += s * int(c >> 24); sum[1] += s * int((c >> 16) & 255); sum[2] += s * int((c >> 8) & 255); sum[3] += s * int(c & 255);
        };
        for (int i = -radius; i <= radius; ++i) add(in[qBound(0, i, n - 1) * inStep], 1);
        for (int i = 0; i < n; ++i) {
            out[i * outStep] = (quint32(sum[0] / win) << 24) | (quint32(sum[1] / win) << 16) | (quint32(sum[2] / win) << 8) | quint32(sum[3] / win);
            add(in[qMin(i + radius + 1, n - 1) * inStep], 1);
            add(in[qMax(i - radius, 0) * inStep], -1);
        }
    };
    for (int p = 0; p < passes; ++p) {
        parallelRows(h, [&](int a, int b) { for (int y = a; y < b; ++y) blur1d(src + y * bpl, tmp + y * tbpl, w, 1, 1); });
        parallelRows(w, [&](int a, int b) { for (int x = a; x < b; ++x) blur1d(tmp + x, src + x, h, tbpl, bpl); });
    }
}

QVector<float> curveLut(const QVariantList &pts)
{
    QVector<QPointF> p;
    for (int i = 0; i + 1 < pts.size(); i += 2) p << QPointF(pts[i].toDouble(), pts[i + 1].toDouble());
    QVector<float> lut(256);
    if (p.size() < 2) { for (int i = 0; i < 256; ++i) lut[i] = i / 255.f; return lut; }
    std::sort(p.begin(), p.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    // Catmull-Rom through the control points gives the smooth curve graders expect.
    auto seg = [&](double x) {
        int i = 0;
        while (i + 2 < p.size() && x > p[i + 1].x()) ++i;
        QPointF p1 = p[i], p2 = p[i + 1];
        QPointF p0 = i > 0 ? p[i - 1] : p1, p3 = i + 2 < p.size() ? p[i + 2] : p2;
        double dx = qMax(1e-6, p2.x() - p1.x());
        double u = qBound(0.0, (x - p1.x()) / dx, 1.0);
        double m1 = (p2.y() - p0.y()) / qMax(1e-6, p2.x() - p0.x()) * dx;
        double m2 = (p3.y() - p1.y()) / qMax(1e-6, p3.x() - p1.x()) * dx;
        double u2 = u * u, u3 = u2 * u;
        return (2 * u3 - 3 * u2 + 1) * p1.y() + (u3 - 2 * u2 + u) * m1 + (-2 * u3 + 3 * u2) * p2.y() + (u3 - u2) * m2;
    };
    for (int i = 0; i < 256; ++i) lut[i] = clamp01(float(seg(i / 255.0)));
    return lut;
}

// --- .cube 3D LUT support
struct Lut3D { int n = 0; QVector<float> d; };
static std::shared_ptr<Lut3D> loadCube(const QString &path)
{
    static QHash<QString, std::shared_ptr<Lut3D>> cacheMap;
    static QMutex mx;
    QMutexLocker l(&mx);
    if (cacheMap.contains(path)) return cacheMap[path];
    auto lut = std::make_shared<Lut3D>();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream s(&f);
        while (!s.atEnd()) {
            QString ln = s.readLine().trimmed();
            if (ln.isEmpty() || ln.startsWith('#')) continue;
            if (ln.startsWith("LUT_3D_SIZE")) { lut->n = ln.section(' ', -1).toInt(); lut->d.reserve(lut->n * lut->n * lut->n * 3); continue; }
            if (ln[0].isLetter()) continue;
            for (auto &v : ln.split(' ', Qt::SkipEmptyParts)) lut->d << v.toFloat();
        }
    }
    if (lut->n < 2 || lut->d.size() < lut->n * lut->n * lut->n * 3) lut->n = 0;
    cacheMap[path] = lut;
    return lut;
}
static inline void lutSample(const Lut3D &l, float &r, float &g, float &b)
{
    const int n = l.n;
    float fx = clamp01(r) * (n - 1), fy = clamp01(g) * (n - 1), fz = clamp01(b) * (n - 1);
    int x0 = int(fx), y0 = int(fy), z0 = int(fz);
    int x1 = qMin(x0 + 1, n - 1), y1 = qMin(y0 + 1, n - 1), z1 = qMin(z0 + 1, n - 1);
    float dx = fx - x0, dy = fy - y0, dz = fz - z0;
    auto at = [&](int x, int y, int z, int c) { return l.d[(x + y * n + z * n * n) * 3 + c]; };
    float o[3];
    for (int c = 0; c < 3; ++c) {
        float c00 = at(x0, y0, z0, c) * (1 - dx) + at(x1, y0, z0, c) * dx;
        float c10 = at(x0, y1, z0, c) * (1 - dx) + at(x1, y1, z0, c) * dx;
        float c01 = at(x0, y0, z1, c) * (1 - dx) + at(x1, y0, z1, c) * dx;
        float c11 = at(x0, y1, z1, c) * (1 - dx) + at(x1, y1, z1, c) * dx;
        o[c] = (c00 * (1 - dy) + c10 * dy) * (1 - dz) + (c01 * (1 - dy) + c11 * dy) * dz;
    }
    r = o[0]; g = o[1]; b = o[2];
}

static void rgb2hsl(float r, float g, float b, float &h, float &s, float &l)
{
    float mx = qMax(r, qMax(g, b)), mn = qMin(r, qMin(g, b));
    l = (mx + mn) / 2; float d = mx - mn;
    if (d < 1e-5f) { h = s = 0; return; }
    s = l > .5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
}
static inline float hue2(float p, float q, float t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < .5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}
static void hsl2rgb(float h, float s, float l, float &r, float &g, float &b)
{
    if (s < 1e-5f) { r = g = b = l; return; }
    float q = l < .5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue2(p, q, h + 1.f / 3); g = hue2(p, q, h); b = hue2(p, q, h - 1.f / 3);
}

static void colorGrade(const Effect &e, QImage &img, const Ctx &c)
{
    auto V = [&](const char *id) { return float(e.val(id, c.kt)); };
    float temp = V("temp") / 100, tint = V("tint") / 100, ex = std::pow(2.f, V("exposure"));
    float con = 1 + V("contrast") / 100 * (V("contrast") > 0 ? 1.2f : .9f);
    float hi = V("highlights") / 100, sh = V("shadows") / 100, wh = V("whites") / 100, bl = V("blacks") / 100;
    float sat = V("sat") / 100, vib = V("vibrance") / 100, fade = V("faded") / 100;
    float lift[3], gam[3], gain[3];
    const char *ln[] = {"liftR", "liftG", "liftB"}, *gn[] = {"gammaR", "gammaG", "gammaB"}, *hn[] = {"gainR", "gainG", "gainB"};
    float ly = V("liftY"), gy = V("gammaY"), hy = V("gainY");
    for (int i = 0; i < 3; ++i) {
        lift[i] = (V(ln[i]) + ly) * .3f;
        gam[i] = 1.f / std::pow(2.f, (V(gn[i]) + gy) * 1.2f);
        gain[i] = 1 + (V(hn[i]) + hy) * .6f;
    }
    float tr = 1 + temp * .25f - tint * .04f, tg = 1 + tint * .18f, tb = 1 - temp * .25f - tint * .04f;
    QVector<float> cm, cr, cg, cb;
    bool hasCurve = e.extra.contains("curveM") || e.extra.contains("curveR") || e.extra.contains("curveG") || e.extra.contains("curveB");
    if (hasCurve) {
        cm = curveLut(e.extra.value("curveM").toList()); cr = curveLut(e.extra.value("curveR").toList());
        cg = curveLut(e.extra.value("curveG").toList()); cb = curveLut(e.extra.value("curveB").toList());
    }
    std::shared_ptr<Lut3D> lut;
    float lutAmt = float(e.extra.value("lutAmount", 100).toDouble() / 100);
    if (e.extra.contains("lut") && !e.extra.value("lut").toString().isEmpty()) {
        lut = loadCube(e.extra.value("lut").toString());
        if (lut && !lut->n) lut.reset();
    }
    const float W = img.width(), H = img.height();
    float vigA = V("vigAmount"), vigM = V("vigMid") / 100, vigR = V("vigRound") / 100, vigF = qMax(.01f, V("vigFeather") / 100);
    bool useVig = std::fabs(vigA) > 1e-3f;
    float asp = 1 + vigR * (W / H - 1) * -1;
    mapRGB(img, [&](float &r, float &g, float &b, int x, int y) {
        if (lut) {
            float a = r, bb = g, d = b; lutSample(*lut, a, bb, d);
            r += (a - r) * lutAmt; g += (bb - g) * lutAmt; b += (d - b) * lutAmt;
        }
        r *= tr * ex; g *= tg * ex; b *= tb * ex;
        r = (r - .5f) * con + .5f; g = (g - .5f) * con + .5f; b = (b - .5f) * con + .5f;
        float Y = clamp01(luma(r, g, b));
        float hw = Y * Y, sw = (1 - Y) * (1 - Y);
        float d = hi * .35f * hw * (1 - hw) * 4 + sh * .35f * sw * (1 - sw) * 4 + wh * .3f * hw * Y + bl * .3f * sw * (1 - Y);
        r += d; g += d; b += d;
        r = clamp01(r); g = clamp01(g); b = clamp01(b);
        r = std::pow(r * gain[0] + lift[0] * (1 - r), gam[0]);
        g = std::pow(clamp01(g * gain[1] + lift[1] * (1 - g)), gam[1]);
        b = std::pow(clamp01(b * gain[2] + lift[2] * (1 - b)), gam[2]);
        if (std::isnan(r)) r = 0;
        if (fade > 0) { r = r * (1 - fade * .35f) + fade * .12f; g = g * (1 - fade * .35f) + fade * .11f; b = b * (1 - fade * .3f) + fade * .13f; }
        Y = luma(r, g, b);
        if (vib != 0) {
            float mx = qMax(r, qMax(g, b)), mn = qMin(r, qMin(g, b));
            float s = 1 + vib * (1 - (mx - mn));
            r = Y + (r - Y) * s; g = Y + (g - Y) * s; b = Y + (b - Y) * s;
        }
        if (sat != 1) { r = Y + (r - Y) * sat; g = Y + (g - Y) * sat; b = Y + (b - Y) * sat; }
        if (hasCurve) {
            int ri = int(clamp01(r) * 255), gi = int(clamp01(g) * 255), bi = int(clamp01(b) * 255);
            r = cm[int(cr[ri] * 255)]; g = cm[int(cg[gi] * 255)]; b = cm[int(cb[bi] * 255)];
        }
        if (useVig) {
            float dx = (x / W - .5f) * 2 * asp, dy = (y / H - .5f) * 2;
            float dist = std::sqrt(dx * dx + dy * dy) / 1.414f;
            float u = clamp01((dist - vigM * (1 - vigF * .5f)) / vigF);
            u = u * u * (3 - 2 * u);
            float m = 1 + vigA * .35f * u;
            r *= m; g *= m; b *= m;
        }
    });
    float sharp = V("sharpen");
    if (sharp > 0) {
        QImage bl2 = img.copy();
        boxBlur(bl2, 1, 1);
        float k = sharp / 50.f;
        const int w = img.width();
        parallelRows(img.height(), [&](int a, int b) {
            for (int y = a; y < b; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                auto *q = reinterpret_cast<const quint32 *>(bl2.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    int A = p[x] >> 24;
                    int R = (p[x] >> 16) & 255, G = (p[x] >> 8) & 255, B = p[x] & 255;
                    R = qBound(0, int(R + (R - int((q[x] >> 16) & 255)) * k), A);
                    G = qBound(0, int(G + (G - int((q[x] >> 8) & 255)) * k), A);
                    B = qBound(0, int(B + (B - int(q[x] & 255)) * k), A);
                    p[x] = (quint32(A) << 24) | (R << 16) | (G << 8) | B;
                }
            }
        });
    }
}

// Inverse-mapped warp with bilinear sampling.
template <class F> static void warp(QImage &img, F f)
{
    QImage src = img.copy();
    const int w = img.width(), h = img.height();
    const int sb = src.bytesPerLine() / 4;
    auto *s = reinterpret_cast<const quint32 *>(src.constBits());
    parallelRows(h, [&](int a, int b) {
        for (int y = a; y < b; ++y) {
            auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                float sx = x, sy = y;
                f(sx, sy);
                if (sx < 0 || sy < 0 || sx >= w - 1 || sy >= h - 1) { p[x] = 0; continue; }
                int x0 = int(sx), y0 = int(sy);
                int fx = int((sx - x0) * 256), fy = int((sy - y0) * 256);
                quint32 c00 = s[y0 * sb + x0], c10 = s[y0 * sb + x0 + 1], c01 = s[(y0 + 1) * sb + x0], c11 = s[(y0 + 1) * sb + x0 + 1];
                quint32 o = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    int v0 = ((c00 >> sh) & 255) * (256 - fx) + ((c10 >> sh) & 255) * fx;
                    int v1 = ((c01 >> sh) & 255) * (256 - fx) + ((c11 >> sh) & 255) * fx;
                    o |= quint32(((v0 * (256 - fy) + v1 * fy) >> 16) & 255) << sh;
                }
                p[x] = o;
            }
        }
    });
}

void apply(const Effect &e, QImage &img, const Ctx &c)
{
    img.detach(); // scanLine() below runs on worker threads and must not detach concurrently
    const QString &t = e.type;
    auto V = [&](const char *id, double d = 0) { return float(e.val(id, c.kt, d)); };
    if (t.startsWith(QLatin1String("plugin."))) { PluginHost::video(e, img, c.kt, c.seconds); return; }
    if (t == "colorgrade") { colorGrade(e, img, c); return; }
    if (t == "brightcontrast") {
        float b = V("bright") / 200, k = V("contrast") / 100;
        k = k >= 0 ? 1 + k * 2 : 1 + k;
        mapRGB(img, [&](float &r, float &g, float &bl, int, int) { r = (r - .5f) * k + .5f + b; g = (g - .5f) * k + .5f + b; bl = (bl - .5f) * k + .5f + b; });
    } else if (t == "levels") {
        float ib = V("inB") / 255, iw = qMax(ib + .001f, V("inW", 255) / 255), gm = 1 / qMax(.01f, V("gamma", 1)), ob = V("outB") / 255, ow = V("outW", 255) / 255;
        float lut[256];
        for (int i = 0; i < 256; ++i) lut[i] = ob + (ow - ob) * std::pow(clamp01((i / 255.f - ib) / (iw - ib)), gm);
        mapRGB(img, [&](float &r, float &g, float &b, int, int) { r = lut[int(clamp01(r) * 255)]; g = lut[int(clamp01(g) * 255)]; b = lut[int(clamp01(b) * 255)]; });
    } else if (t == "shadowhigh") {
        float s = V("shadow") / 100, h = V("high") / 100;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) {
            float Y = luma(r, g, b), d = s * .5f * (1 - Y) * (1 - Y) * Y * 4 - h * .5f * Y * Y * (1 - Y) * 4;
            r += d; g += d; b += d;
        });
    } else if (t == "exposure") {
        float k = std::pow(2.f, V("ev")), o = V("offset"), gm = 1 / qMax(.01f, V("gamma", 1));
        mapRGB(img, [&](float &r, float &g, float &b, int, int) {
            r = std::pow(clamp01(r * k + o), gm); g = std::pow(clamp01(g * k + o), gm); b = std::pow(clamp01(b * k + o), gm);
        });
    } else if (t == "tint") {
        QColor k = QColor::fromRgb(QRgb(int(V("black")))), w = QColor::fromRgb(QRgb(int(V("white", 0xFFFFFF))));
        float a = V("amount", 100) / 100;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) {
            float Y = luma(r, g, b);
            r += (k.redF() + (w.redF() - k.redF()) * Y - r) * a;
            g += (k.greenF() + (w.greenF() - k.greenF()) * Y - g) * a;
            b += (k.blueF() + (w.blueF() - k.blueF()) * Y - b) * a;
        });
    } else if (t == "hsl") {
        float dh = V("hue") / 360, ds = V("sat") / 100, dl = V("light") / 100;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) {
            float h, s, l;
            rgb2hsl(clamp01(r), clamp01(g), clamp01(b), h, s, l);
            h = h + dh; h -= std::floor(h);
            s = clamp01(s * (1 + ds));
            l = clamp01(l + dl * (dl > 0 ? 1 - l : l));
            hsl2rgb(h, s, l, r, g, b);
        });
    } else if (t == "colorbalance") {
        float S[3] = {V("sr") / 200, V("sg") / 200, V("sb") / 200}, M[3] = {V("mr") / 200, V("mg") / 200, V("mb") / 200}, H[3] = {V("hr") / 200, V("hg") / 200, V("hb") / 200};
        mapRGB(img, [&](float &r, float &g, float &b, int, int) {
            float Y = clamp01(luma(r, g, b)), ws = (1 - Y) * (1 - Y), wh = Y * Y, wm = 1 - ws - wh;
            r += S[0] * ws + M[0] * wm + H[0] * wh; g += S[1] * ws + M[1] * wm + H[1] * wh; b += S[2] * ws + M[2] * wm + H[2] * wh;
        });
    } else if (t == "blackwhite") {
        float a = V("amount", 100) / 100;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) { float Y = luma(r, g, b); r += (Y - r) * a; g += (Y - g) * a; b += (Y - b) * a; });
    } else if (t == "sepia") {
        float a = V("amount", 100) / 100;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) {
            float R = .393f * r + .769f * g + .189f * b, G = .349f * r + .686f * g + .168f * b, B = .272f * r + .534f * g + .131f * b;
            r += (R - r) * a; g += (G - g) * a; b += (B - b) * a;
        });
    } else if (t == "invert") {
        float a = 1 - V("amount") / 100;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) { r += (1 - 2 * r) * a; g += (1 - 2 * g) * a; b += (1 - 2 * b) * a; });
    } else if (t == "posterize") {
        float n = qMax(2.f, V("levels", 6)) - 1;
        mapRGB(img, [&](float &r, float &g, float &b, int, int) { r = std::round(r * n) / n; g = std::round(g * n) / n; b = std::round(b * n) / n; });
    } else if (t == "vignette") {
        float a = V("amount", 50) / 100, md = V("mid", 50) / 100, ft = qMax(.01f, V("feather", 50) / 100), W = img.width(), H = img.height();
        mapRGB(img, [&](float &r, float &g, float &b, int x, int y) {
            float dx = (x / W - .5f) * 2, dy = (y / H - .5f) * 2, d = std::sqrt(dx * dx + dy * dy) / 1.414f;
            float u = clamp01((d - md * (1 - ft * .5f)) / ft);
            float m = 1 - a * u * u * (3 - 2 * u);
            r *= m; g *= m; b *= m;
        });
    } else if (t == "noise") {
        float a = V("amount", 15) / 100 * .5f;
        bool colored = V("color") > .5f;
        quint32 seed = quint32(c.seconds * 977) * 2654435761u;
        mapRGB(img, [&](float &r, float &g, float &b, int x, int y) {
            quint32 h = (quint32(x) * 73856093u) ^ (quint32(y) * 19349663u) ^ seed;
            auto rnd = [&]() { h ^= h << 13; h ^= h >> 17; h ^= h << 5; return (h & 0xFFFF) / 65535.f - .5f; };
            float n = rnd() * a * 2;
            if (colored) { r += n; g += rnd() * a * 2; b += rnd() * a * 2; } else { r += n; g += n; b += n; }
        });
    } else if (t == "gaussblur") {
        boxBlur(img, qRound(V("radius", 10) * c.scale * .55));
    } else if (t == "dirblur") {
        int n = qBound(2, qRound(V("len", 20) * c.scale), 96);
        double a = qDegreesToRadians(V("angle"));
        double dx = std::sin(a), dy = -std::cos(a);
        QImage src = img.copy();
        img.fill(0);
        QPainter pa(&img);
        pa.setCompositionMode(QPainter::CompositionMode_Plus);
        pa.setRenderHint(QPainter::SmoothPixmapTransform);
        int steps = qMin(n, 16);
        pa.setOpacity(1.0 / steps);
        for (int i = 0; i < steps; ++i) {
            double u = (double(i) / (steps - 1) - .5) * n;
            pa.drawImage(QPointF(dx * u, dy * u), src);
        }
    } else if (t == "sharpen") {
        Effect g = e; g.type = "colorgrade";
        QImage bl2 = img.copy();
        boxBlur(bl2, 1, 1);
        float k = V("amount", 50) / 40.f;
        const int w = img.width();
        parallelRows(img.height(), [&](int a, int b) {
            for (int y = a; y < b; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                auto *q = reinterpret_cast<const quint32 *>(bl2.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    int A = p[x] >> 24, R = (p[x] >> 16) & 255, G = (p[x] >> 8) & 255, B = p[x] & 255;
                    R = qBound(0, int(R + (R - int((q[x] >> 16) & 255)) * k), A);
                    G = qBound(0, int(G + (G - int((q[x] >> 8) & 255)) * k), A);
                    B = qBound(0, int(B + (B - int(q[x] & 255)) * k), A);
                    p[x] = (quint32(A) << 24) | (R << 16) | (G << 8) | B;
                }
            }
        });
    } else if (t == "glow") {
        float th = V("thresh", 70) / 100;
        QImage hi = img.copy();
        mapRGB(hi, [&](float &r, float &g, float &b, int, int) {
            float Y = luma(r, g, b), m = clamp01((Y - th) / qMax(.01f, 1 - th));
            r *= m; g *= m; b *= m;
        });
        boxBlur(hi, qRound(V("radius", 20) * c.scale * .55));
        QPainter pa(&img);
        pa.setCompositionMode(QPainter::CompositionMode_Screen);
        pa.setOpacity(qMin(1.f, V("amount", 60) / 100));
        pa.drawImage(0, 0, hi);
    } else if (t == "mosaic") {
        int nx = qMax(1, int(V("h", 40))), ny = qMax(1, int(V("v", 24)));
        QImage small = img.scaled(nx, ny, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        img = small.scaled(img.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    } else if (t == "dropshadow") {
        QColor col = QColor::fromRgb(QRgb(int(V("color"))));
        double a = qDegreesToRadians(V("dir", 135)), d = V("dist", 8) * c.scale;
        QImage sh(img.size(), QImage::Format_ARGB32_Premultiplied);
        sh.fill(0);
        { QPainter pa(&sh); pa.drawImage(0, 0, img); pa.setCompositionMode(QPainter::CompositionMode_SourceIn); pa.fillRect(sh.rect(), col); }
        boxBlur(sh, qRound(V("soft", 8) * c.scale * .55));
        QImage out(img.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(0);
        QPainter pa(&out);
        pa.setOpacity(V("opacity", 50) / 100);
        pa.drawImage(QPointF(std::sin(a) * d, -std::cos(a) * d), sh);
        pa.setOpacity(1);
        pa.drawImage(0, 0, img);
        pa.end();
        img = out;
    } else if (t == "crop") {
        const int w = img.width(), h = img.height();
        float l = V("left") / 100 * w, tp = V("top") / 100 * h, r = w - V("right") / 100 * w, b = h - V("bottom") / 100 * h;
        float ft = V("feather") / 100 * qMin(w, h) * .25f + .5f;
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                float ey = qMin(y - tp, b - y) / ft;
                for (int x = 0; x < w; ++x) {
                    float k = qMin(qMin(x - l, r - x) / ft, ey);
                    if (k >= 1) continue;
                    if (k <= 0) { p[x] = 0; continue; }
                    int m = int(k * 256), o = 0;
                    quint32 c2 = p[x], out2 = 0;
                    for (int sh = 0; sh < 32; sh += 8) { o = ((c2 >> sh) & 255) * m >> 8; out2 |= quint32(o) << sh; }
                    p[x] = out2;
                }
            }
        });
    } else if (t == "hflip") {
        img = img.flipped(Qt::Horizontal);
    } else if (t == "vflip") {
        img = img.flipped(Qt::Vertical);
    } else if (t == "mirror") {
        int ax = int(V("axis"));
        QImage fl = img.flipped(ax < 2 ? Qt::Horizontal : Qt::Vertical);
        QPainter pa(&img);
        pa.setCompositionMode(QPainter::CompositionMode_Source);
        QRect r = img.rect();
        if (ax == 0) r = QRect(img.width() / 2, 0, img.width() - img.width() / 2, img.height());
        else if (ax == 1) r = QRect(0, 0, img.width() / 2, img.height());
        else if (ax == 2) r = QRect(0, img.height() / 2, img.width(), img.height() - img.height() / 2);
        else r = QRect(0, 0, img.width(), img.height() / 2);
        pa.drawImage(r, fl, r);
    } else if (t == "wave") {
        float h = V("height", 20) * c.scale, wl = qMax(1.f, V("width", 80) * float(c.scale)), ph = float(c.seconds) * V("speed", 1) * 6.283f;
        float a = qDegreesToRadians(V("dir")), ca = std::cos(a), sa = std::sin(a);
        warp(img, [&](float &x, float &y) {
            float o = h * std::sin((x * ca + y * sa) / wl * 6.283f + ph);
            x -= sa * o; y += ca * o;
        });
    } else if (t == "lens") {
        float k = V("curv", -30) / 100;
        float cx = img.width() / 2.f, cy = img.height() / 2.f, nr = qMax(cx, cy);
        warp(img, [&](float &x, float &y) {
            float dx = (x - cx) / nr, dy = (y - cy) / nr, rr = dx * dx + dy * dy, m = 1 + k * rr;
            x = cx + dx * m * nr; y = cy + dy * m * nr;
        });
    } else if (t == "chromakey") {
        QColor kc = QColor::fromRgb(QRgb(int(V("key", 0x00C800))));
        float kr = kc.redF(), kg = kc.greenF(), kb = kc.blueF();
        float kcb = -.1146f * kr - .3854f * kg + .5f * kb, kcr = .5f * kr - .4542f * kg - .0458f * kb;
        float tol = V("tol", 40) / 100 * .5f, soft = qMax(.001f, V("soft", 15) / 100 * .5f), spill = V("spill", 60) / 100, choke = V("choke") / 100 * .2f;
        const int w = img.width();
        parallelRows(img.height(), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    int A = p[x] >> 24;
                    if (!A) continue;
                    float k = 1.f / A;
                    float r = ((p[x] >> 16) & 255) * k, g = ((p[x] >> 8) & 255) * k, b = (p[x] & 255) * k;
                    float cb = -.1146f * r - .3854f * g + .5f * b, cr = .5f * r - .4542f * g - .0458f * b;
                    float dcb = cb - kcb, dcr = cr - kcr;
                    float d = std::sqrt(dcb * dcb + dcr * dcr) - choke;
                    float al = clamp01((d - tol) / soft);
                    if (al <= 0) { p[x] = 0; continue; }
                    float Y = luma(r, g, b);
                    float s = spill * (1 - al);
                    if (kg >= kr && kg >= kb) g += (qMin(g, (r + b) / 2) - g) * s;
                    else if (kb >= kr) b += (qMin(b, (r + g) / 2) - b) * s;
                    else r += (qMin(r, (g + b) / 2) - r) * s;
                    (void)Y;
                    float m = A * al;
                    p[x] = (quint32(m) << 24) | (quint32(clamp01(r) * m) << 16) | (quint32(clamp01(g) * m) << 8) | quint32(clamp01(b) * m);
                }
            }
        });
    } else if (t == "lumakey") {
        float th = V("thresh", 20) / 100, sf = qMax(.001f, V("cutoff", 10) / 100);
        bool inv = V("invert") > .5f;
        const int w = img.width();
        parallelRows(img.height(), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto *p = reinterpret_cast<quint32 *>(img.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    int A = p[x] >> 24;
                    if (!A) continue;
                    int R = (p[x] >> 16) & 255, G = (p[x] >> 8) & 255, B = p[x] & 255;
                    float Y = luma(R, G, B) / A;
                    float al = clamp01((Y - th) / sf);
                    if (inv) al = 1 - al;
                    p[x] = (quint32(A * al) << 24) | (quint32(R * al) << 16) | (quint32(G * al) << 8) | quint32(B * al);
                }
            }
        });
    }
}

QImage bars(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&img);
    static const QRgb top[7] = {0xC0C0C0, 0xC0C000, 0x00C0C0, 0x00C000, 0xC000C0, 0xC00000, 0x0000C0};
    static const QRgb mid[7] = {0x0000C0, 0x101010, 0xC000C0, 0x101010, 0x00C0C0, 0x101010, 0xC0C0C0};
    int h1 = h * 2 / 3, h2 = h / 12;
    for (int i = 0; i < 7; ++i) {
        int x0 = w * i / 7, x1 = w * (i + 1) / 7;
        p.fillRect(x0, 0, x1 - x0, h1, QColor(top[i]));
        p.fillRect(x0, h1, x1 - x0, h2, QColor(mid[i]));
    }
    int y = h1 + h2;
    p.fillRect(0, y, w * 5 / 28, h - y, QColor(0x00214C));
    p.fillRect(w * 5 / 28, y, w * 5 / 28, h - y, Qt::white);
    p.fillRect(w * 10 / 28, y, w * 5 / 28, h - y, QColor(0x32006A));
    p.fillRect(w * 15 / 28, y, w, h - y, QColor(0x101010));
    return img;
}

QString wrapText(const QString &text, const QString &family, double pixelSize, double maxWidth)
{
    QFont f(family);
    f.setPixelSize(qMax(1, int(pixelSize)));
    f.setBold(true);
    QFontMetricsF fm(f);
    QStringList lines;
    for (const QString &para : text.split('\n')) {
        QString cur;
        for (const QString &w : para.split(' ', Qt::SkipEmptyParts)) {
            QString next = cur.isEmpty() ? w : cur + ' ' + w;
            if (!cur.isEmpty() && fm.horizontalAdvance(next) > maxWidth) { lines << cur; cur = w; }
            else cur = next;
        }
        lines << cur;
    }
    return lines.join('\n');
}

QImage renderText(const TextStyle &t, int w, int h, double scale)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    QFont f(t.font);
    f.setPixelSize(qMax(1, qRound(t.size * scale)));
    f.setBold(t.bold); f.setItalic(t.italic); f.setUnderline(t.underline);
    if (t.tracking != 0) f.setLetterSpacing(QFont::AbsoluteSpacing, t.tracking * scale);
    QFontMetricsF fm(f);
    QStringList lines = t.text.split('\n');
    double lineH = fm.height() + t.leading * scale;
    double blockH = lineH * lines.size(), blockW = 0;
    for (auto &l : lines) blockW = qMax(blockW, fm.horizontalAdvance(l));
    double y0 = (h - blockH) / 2 + fm.ascent();
    QPainterPath path;
    for (int i = 0; i < lines.size(); ++i) {
        double lw = fm.horizontalAdvance(lines[i]);
        double x = t.align == 0 ? (w - blockW) / 2 : t.align == 1 ? (w - lw) / 2 : (w + blockW) / 2 - lw;
        path.addText(x, y0 + i * lineH, f, lines[i]);
    }
    if (t.background) {
        double m = t.size * scale * .25;
        p.fillRect(QRectF((w - blockW) / 2 - m, (h - blockH) / 2 - m * .5, blockW + 2 * m, blockH + m), t.bgColor);
    }
    if (t.shadow) {
        QImage sh(w, h, QImage::Format_ARGB32_Premultiplied);
        sh.fill(0);
        { QPainter s(&sh); s.setRenderHint(QPainter::Antialiasing); s.setPen(Qt::NoPen); s.setBrush(t.shadowColor);
          s.translate(t.shadowDist * scale, t.shadowDist * scale); s.drawPath(path); }
        boxBlur(sh, qRound(t.shadowBlur * scale * .55));
        p.drawImage(0, 0, sh);
    }
    if (t.stroke && t.strokeWidth > 0) {
        QPen pen(t.strokeColor, t.strokeWidth * 2 * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(path, pen);
    }
    p.fillPath(path, t.fill);
    return img;
}
}
