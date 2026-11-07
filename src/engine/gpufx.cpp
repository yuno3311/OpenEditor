#include "gpufx.h"
#include <QHash>
#include <QMutex>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <cmath>

std::atomic<bool> GpuFx::enabled{true};
static QOffscreenSurface *gSurface = nullptr;
static QString gRenderer;
static QMutex gSurfaceMutex;

static const char *kVert = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Shared prelude: straight-alpha helpers, Rec.709 luma, matching the CPU mapRGB() wrapper.
static const char *kHead = R"(#version 330 core
in vec2 uv;
out vec4 o;
uniform sampler2D tex;
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 rgbToHsl(vec3 c) {
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b)), l = (mx + mn) * 0.5, d = mx - mn;
    if (d < 1e-5) return vec3(0.0, 0.0, l);
    float s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn), h;
    if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    else if (mx == c.g) h = (c.b - c.r) / d + 2.0;
    else h = (c.r - c.g) / d + 4.0;
    return vec3(h / 6.0, s, l);
}
float hue2(float p, float q, float t) {
    if (t < 0.0) t += 1.0; if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}
vec3 hslToRgb(vec3 x) {
    if (x.y < 1e-5) return vec3(x.z);
    float q = x.z < 0.5 ? x.z * (1.0 + x.y) : x.z + x.y - x.z * x.y, p = 2.0 * x.z - q;
    return vec3(hue2(p, q, x.x + 1.0 / 3.0), hue2(p, q, x.x), hue2(p, q, x.x - 1.0 / 3.0));
}
)";
static const char *kColorMain = R"(
void main() {
    vec4 s = texture(tex, uv);
    if (s.a <= 0.0) { o = vec4(0.0); return; }
    vec3 c = s.rgb / s.a;
    c = fx(c);
    o = vec4(clamp(c, 0.0, 1.0) * s.a, s.a);
}
)";

struct Prog { const char *body; bool color; };
static const QHash<QString, Prog> &programs()
{
    static const QHash<QString, Prog> m = {
        {"brightcontrast", {"uniform float b, k; vec3 fx(vec3 c) { return (c - 0.5) * k + 0.5 + b; }", true}},
        {"exposure", {"uniform float k, off, gm; vec3 fx(vec3 c) { return pow(clamp(c * k + off, 0.0, 1.0), vec3(gm)); }", true}},
        {"blackwhite", {"uniform float a; vec3 fx(vec3 c) { return mix(c, vec3(luma(c)), a); }", true}},
        {"sepia", {"uniform float a; vec3 fx(vec3 c) { vec3 r = vec3(dot(c, vec3(.393,.769,.189)), dot(c, vec3(.349,.686,.168)), dot(c, vec3(.272,.534,.131))); return mix(c, r, a); }", true}},
        {"invert", {"uniform float a; vec3 fx(vec3 c) { return c + (1.0 - 2.0 * c) * a; }", true}},
        {"posterize", {"uniform float n; vec3 fx(vec3 c) { return round(c * n) / n; }", true}},
        {"tint", {"uniform vec3 k, w; uniform float a; vec3 fx(vec3 c) { return mix(c, k + (w - k) * luma(c), a); }", true}},
        {"hsl", {"uniform float dh, ds, dl; vec3 fx(vec3 c) { vec3 x = rgbToHsl(clamp(c, 0.0, 1.0)); x.x = fract(x.x + dh); x.y = clamp(x.y * (1.0 + ds), 0.0, 1.0);"
                 " x.z = clamp(x.z + dl * (dl > 0.0 ? 1.0 - x.z : x.z), 0.0, 1.0); return hslToRgb(x); }", true}},
        {"colorbalance", {"uniform vec3 S, M, H; vec3 fx(vec3 c) { float Y = clamp(luma(c), 0.0, 1.0), ws = (1.0-Y)*(1.0-Y), wh = Y*Y, wm = 1.0-ws-wh; return c + S*ws + M*wm + H*wh; }", true}},
        {"vignette", {"uniform float a, md, ft; vec3 fx(vec3 c) { vec2 d = (uv - 0.5) * 2.0; float r = length(d) / 1.414;"
                      " float u = clamp((r - md * (1.0 - ft * 0.5)) / ft, 0.0, 1.0); return c * (1.0 - a * u * u * (3.0 - 2.0 * u)); }", true}},
        {"shadowhigh", {"uniform float s, h; vec3 fx(vec3 c) { float Y = luma(c); return c + s*0.5*(1.0-Y)*(1.0-Y)*Y*4.0 - h*0.5*Y*Y*(1.0-Y)*4.0; }", true}},
        {"levels", {"uniform float ib, iw, gm, ob, ow; vec3 fx(vec3 c) { vec3 t = clamp((clamp(c,0.0,1.0) - ib) / (iw - ib), 0.0, 1.0); return ob + (ow - ob) * pow(t, vec3(gm)); }", true}},
        {"colorgrade", {R"(uniform vec3 tn, lift, gam, gain; uniform float ex, con, hi, sh, wh, bl, sat, vib, fade, vigA, vigM, vigF, asp;
            uniform float W, H;
            vec3 fx(vec3 c) {
                c *= tn * ex;
                c = (c - 0.5) * con + 0.5;
                float Y = clamp(luma(c), 0.0, 1.0), hw = Y*Y, sw = (1.0-Y)*(1.0-Y);
                c += hi*0.35*hw*(1.0-hw)*4.0 + sh*0.35*sw*(1.0-sw)*4.0 + wh*0.3*hw*Y + bl*0.3*sw*(1.0-Y);
                c = clamp(c, 0.0, 1.0);
                c = pow(clamp(c * gain + lift * (1.0 - c), 0.0, 1.0), gam);
                if (fade > 0.0) c = vec3(c.r*(1.0-fade*0.35)+fade*0.12, c.g*(1.0-fade*0.35)+fade*0.11, c.b*(1.0-fade*0.3)+fade*0.13);
                Y = luma(c);
                if (vib != 0.0) { float m = 1.0 + vib * (1.0 - (max(c.r,max(c.g,c.b)) - min(c.r,min(c.g,c.b)))); c = Y + (c - Y) * m; }
                c = Y + (c - Y) * sat;
                if (abs(vigA) > 0.001) {
                    vec2 d = vec2((uv.x - 0.5) * 2.0 * asp, (uv.y - 0.5) * 2.0);
                    float u = clamp((length(d) / 1.414 - vigM * (1.0 - vigF * 0.5)) / vigF, 0.0, 1.0);
                    c *= 1.0 + vigA * 0.35 * u * u * (3.0 - 2.0 * u);
                }
                return c;
            })", true}},
        {"chromakey", {R"(uniform float kcb, kcr, tol, soft, spill, choke; uniform vec3 key;
            void main() {
                vec4 s = texture(tex, uv);
                if (s.a <= 0.0) { o = vec4(0.0); return; }
                vec3 c = s.rgb / s.a;
                float cb = -0.1146*c.r - 0.3854*c.g + 0.5*c.b, cr = 0.5*c.r - 0.4542*c.g - 0.0458*c.b;
                float d = length(vec2(cb - kcb, cr - kcr)) - choke;
                float al = clamp((d - tol) / soft, 0.0, 1.0);
                if (al <= 0.0) { o = vec4(0.0); return; }
                float m = spill * (1.0 - al);
                if (key.g >= key.r && key.g >= key.b) c.g += (min(c.g, (c.r + c.b) * 0.5) - c.g) * m;
                else if (key.b >= key.r) c.b += (min(c.b, (c.r + c.g) * 0.5) - c.b) * m;
                else c.r += (min(c.r, (c.g + c.b) * 0.5) - c.r) * m;
                float A = s.a * al;
                o = vec4(clamp(c, 0.0, 1.0) * A, A);
            })", false}},
        {"blur", {R"(uniform vec2 dir; uniform float sigma; uniform int radius;
            void main() {
                vec4 sum = vec4(0.0); float tot = 0.0;
                for (int i = -radius; i <= radius; ++i) {
                    float t = float(i), w = exp(-0.5 * t * t / (sigma * sigma));
                    sum += texture(tex, uv + dir * t) * w; tot += w;
                }
                o = sum / tot;
            })", false}},
    };
    return m;
}

static bool colorGradeSimple(const Effect &e)
{
    // LUTs, curves and the unsharp pass stay on the CPU path.
    if (e.extra.contains("lut") || e.extra.contains("curveM") || e.extra.contains("curveR") || e.extra.contains("curveG") || e.extra.contains("curveB")) return false;
    auto *s = e.find("sharpen");
    return !s || (!s->animated && s->value <= 0);
}

void GpuFx::prepareOnMainThread()
{
    QMutexLocker l(&gSurfaceMutex);
    if (gSurface) return;
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    gSurface = new QOffscreenSurface;
    gSurface->setFormat(f);
    gSurface->create();
    if (!gSurface->isValid()) { delete gSurface; gSurface = nullptr; }
}
QString GpuFx::rendererName() { QMutexLocker l(&gSurfaceMutex); return gRenderer; }

bool GpuFx::supports(const Effect &e)
{
    if (e.type == "gaussblur") return true;
    if (e.type == "colorgrade") return colorGradeSimple(e);
    return programs().contains(e.type) && e.type != "blur";
}

struct GpuFx::Impl : QOpenGLExtraFunctions {
    QOpenGLContext ctx;
    bool good = false;
    QOpenGLVertexArrayObject vao;
    QHash<QString, QOpenGLShaderProgram *> progs;
    std::unique_ptr<QOpenGLFramebufferObject> a, b;
    GLuint up = 0;
    QSize size;

    bool init()
    {
        QSurfaceFormat f;
        f.setVersion(3, 3);
        f.setProfile(QSurfaceFormat::CoreProfile);
        ctx.setFormat(f);
        if (!gSurface || !ctx.create() || !ctx.makeCurrent(gSurface)) return false;
        if (ctx.format().majorVersion() < 3 || (ctx.format().majorVersion() == 3 && ctx.format().minorVersion() < 3)) return false;
        initializeOpenGLFunctions();
        if (!vao.create()) return false;
        { QMutexLocker l(&gSurfaceMutex); gRenderer = QString::fromLatin1(reinterpret_cast<const char *>(glGetString(GL_RENDERER))); }
        return true;
    }
    QOpenGLShaderProgram *prog(const QString &id)
    {
        auto it = progs.constFind(id);
        if (it != progs.constEnd()) return *it;
        const Prog &p = programs()[id];
        QByteArray src = QByteArray(kHead) + p.body + (p.color ? kColorMain : "");
        auto *pr = new QOpenGLShaderProgram;
        bool ok = pr->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert) &&
                  pr->addShaderFromSourceCode(QOpenGLShader::Fragment, src) && pr->link();
        if (!ok) { qWarning("GpuFx shader %s: %s", qPrintable(id), qPrintable(pr->log())); delete pr; pr = nullptr; }
        progs.insert(id, pr);
        return pr;
    }
    void resize(QSize s)
    {
        if (s == size) return;
        size = s;
        QOpenGLFramebufferObjectFormat f;
        f.setInternalTextureFormat(GL_RGBA8);
        a = std::make_unique<QOpenGLFramebufferObject>(s, f);
        b = std::make_unique<QOpenGLFramebufferObject>(s, f);
        for (auto *fb : {a.get(), b.get()}) {
            glBindTexture(GL_TEXTURE_2D, fb->texture());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
    }
    void pass(QOpenGLShaderProgram *p)
    {
        b->bind();
        glViewport(0, 0, size.width(), size.height());
        glBindTexture(GL_TEXTURE_2D, a->texture());
        p->setUniformValue("tex", 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::swap(a, b);
    }
    bool run(const Effect &e, const ImageFx::Ctx &c, QImage &img)
    {
        auto V = [&](const char *id, double d = 0) { return float(e.val(id, c.kt, d)); };
        if (e.type == "gaussblur") {
            int r = qRound(V("radius", 10) * c.scale * .55);
            if (r < 1) return true;
            r = qMin(r, qMax(img.width(), img.height()) / 2);
            double win = 2.0 * r + 1, sigma = std::sqrt(3 * (win * win - 1) / 12);
            auto *p = prog("blur");
            if (!p) return false;
            p->bind();
            // Large blurs sample every few texels; bilinear filtering keeps the kernel smooth.
            double stride = qMax(1.0, sigma / 12);
            p->setUniformValue("sigma", GLfloat(sigma / stride));
            p->setUniformValue("radius", qMin(48, int(sigma * 3 / stride) + 1));
            p->setUniformValue("dir", QVector2D(float(stride / img.width()), 0));
            pass(p);
            p->setUniformValue("dir", QVector2D(0, float(stride / img.height())));
            pass(p);
            return true;
        }
        auto *p = prog(e.type);
        if (!p) return false;
        p->bind();
        const QString &t = e.type;
        auto col = [&](const char *id, double d = 0) { QColor k = QColor::fromRgb(QRgb(int(V(id, d)))); return QVector3D(k.redF(), k.greenF(), k.blueF()); };
        if (t == "brightcontrast") {
            float k = V("contrast") / 100;
            p->setUniformValue("b", V("bright") / 200);
            p->setUniformValue("k", GLfloat(k >= 0 ? 1 + k * 2 : 1 + k));
        } else if (t == "exposure") {
            p->setUniformValue("k", GLfloat(std::pow(2.f, V("ev"))));
            p->setUniformValue("off", V("offset"));
            p->setUniformValue("gm", 1 / qMax(.01f, V("gamma", 1)));
        } else if (t == "blackwhite" || t == "sepia") p->setUniformValue("a", V("amount", 100) / 100);
        else if (t == "invert") p->setUniformValue("a", 1 - V("amount") / 100);
        else if (t == "posterize") p->setUniformValue("n", qMax(2.f, V("levels", 6)) - 1);
        else if (t == "tint") {
            p->setUniformValue("k", col("black"));
            p->setUniformValue("w", col("white", 0xFFFFFF));
            p->setUniformValue("a", V("amount", 100) / 100);
        } else if (t == "hsl") {
            p->setUniformValue("dh", V("hue") / 360);
            p->setUniformValue("ds", V("sat") / 100);
            p->setUniformValue("dl", V("light") / 100);
        } else if (t == "colorbalance") {
            p->setUniformValue("S", QVector3D(V("sr"), V("sg"), V("sb")) / 200);
            p->setUniformValue("M", QVector3D(V("mr"), V("mg"), V("mb")) / 200);
            p->setUniformValue("H", QVector3D(V("hr"), V("hg"), V("hb")) / 200);
        } else if (t == "vignette") {
            p->setUniformValue("a", V("amount", 50) / 100);
            p->setUniformValue("md", V("mid", 50) / 100);
            p->setUniformValue("ft", qMax(.01f, V("feather", 50) / 100));
        } else if (t == "shadowhigh") {
            p->setUniformValue("s", V("shadow") / 100);
            p->setUniformValue("h", V("high") / 100);
        } else if (t == "levels") {
            float ib = V("inB") / 255;
            p->setUniformValue("ib", ib);
            p->setUniformValue("iw", qMax(ib + .001f, V("inW", 255) / 255));
            p->setUniformValue("gm", 1 / qMax(.01f, V("gamma", 1)));
            p->setUniformValue("ob", V("outB") / 255);
            p->setUniformValue("ow", V("outW", 255) / 255);
        } else if (t == "chromakey") {
            QVector3D k = col("key", 0x00C800);
            p->setUniformValue("key", k);
            p->setUniformValue("kcb", GLfloat(-.1146f * k.x() - .3854f * k.y() + .5f * k.z()));
            p->setUniformValue("kcr", GLfloat(.5f * k.x() - .4542f * k.y() - .0458f * k.z()));
            p->setUniformValue("tol", V("tol", 40) / 100 * .5f);
            p->setUniformValue("soft", qMax(.001f, V("soft", 15) / 100 * .5f));
            p->setUniformValue("spill", V("spill", 60) / 100);
            p->setUniformValue("choke", V("choke") / 100 * .2f);
        } else if (t == "colorgrade") {
            float temp = V("temp") / 100, tint = V("tint") / 100, con = V("contrast");
            p->setUniformValue("tn", QVector3D(1 + temp * .25f - tint * .04f, 1 + tint * .18f, 1 - temp * .25f - tint * .04f));
            p->setUniformValue("ex", GLfloat(std::pow(2.f, V("exposure"))));
            p->setUniformValue("con", 1 + con / 100 * (con > 0 ? 1.2f : .9f));
            p->setUniformValue("hi", V("highlights") / 100); p->setUniformValue("sh", V("shadows") / 100);
            p->setUniformValue("wh", V("whites") / 100); p->setUniformValue("bl", V("blacks") / 100);
            p->setUniformValue("sat", V("sat", 100) / 100); p->setUniformValue("vib", V("vibrance") / 100);
            p->setUniformValue("fade", V("faded") / 100);
            float ly = V("liftY"), gy = V("gammaY"), hy = V("gainY");
            QVector3D li((V("liftR") + ly) * .3f, (V("liftG") + ly) * .3f, (V("liftB") + ly) * .3f);
            auto gv = [&](const char *id) { return 1.f / std::pow(2.f, (V(id) + gy) * 1.2f); };
            auto hv = [&](const char *id) { return 1 + (V(id) + hy) * .6f; };
            p->setUniformValue("lift", li);
            p->setUniformValue("gam", QVector3D(gv("gammaR"), gv("gammaG"), gv("gammaB")));
            p->setUniformValue("gain", QVector3D(hv("gainR"), hv("gainG"), hv("gainB")));
            float W = img.width(), H = img.height();
            p->setUniformValue("vigA", V("vigAmount"));
            p->setUniformValue("vigM", V("vigMid", 50) / 100);
            p->setUniformValue("vigF", qMax(.01f, V("vigFeather", 50) / 100));
            p->setUniformValue("asp", GLfloat(1 + V("vigRound") / 100 * (W / H - 1) * -1));
        }
        pass(p);
        return true;
    }
    ~Impl()
    {
        if (!good || !ctx.makeCurrent(gSurface)) return;
        qDeleteAll(progs);
        a.reset(); b.reset();
        vao.destroy();
        ctx.doneCurrent();
    }
};

GpuFx::GpuFx() : d(new Impl) { d->good = d->init(); }
GpuFx::~GpuFx() = default;
bool GpuFx::ok() const { return d->good; }

bool GpuFx::applyChain(const QVector<const Effect *> &chain, QImage &img, const ImageFx::Ctx &c)
{
    if (!d->good || chain.isEmpty() || !d->ctx.makeCurrent(gSurface)) return false;
    d->resize(img.size());
    if (!d->a || !d->a->isValid()) return false;
    d->vao.bind();
    d->glActiveTexture(GL_TEXTURE0);
    d->glBindTexture(GL_TEXTURE_2D, d->a->texture());
    d->glPixelStorei(GL_UNPACK_ROW_LENGTH, img.bytesPerLine() / 4);
    d->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.width(), img.height(), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, img.constBits());
    d->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    d->glDisable(GL_BLEND);
    for (const Effect *e : chain)
        if (!d->run(*e, c, img)) return false;
    d->a->bind();
    QImage out(img.size(), QImage::Format_ARGB32_Premultiplied);
    d->glPixelStorei(GL_PACK_ROW_LENGTH, out.bytesPerLine() / 4);
    d->glReadPixels(0, 0, out.width(), out.height(), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, out.bits());
    d->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    bool good = d->glGetError() == GL_NO_ERROR;
    QOpenGLFramebufferObject::bindDefault();
    d->ctx.doneCurrent();
    if (good) img = out;
    return good;
}
