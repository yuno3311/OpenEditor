// Titles: old projects render exactly as before, each text option visibly does its job, fonts
// import, and presets are well-formed. Usage: oe_text_test [font file to import]
#include "core/fonts.h"
#include "core/textpresets.h"
#include "engine/imagefx.h"
#include "text_ref_styles.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } else printf("ok   %s\n", #c); fflush(stdout); } while (0)
#define SAY(...) do { printf("     " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static const int W = 640, H = 360;
static QImage render(const TextStyle &t, const ImageFx::TextAnim &a = {}) { return ImageFx::renderText(t, W, H, .5, a); }
static double ink(const QImage &i)
{
    double s = 0;
    for (int y = 0; y < i.height(); ++y) for (int x = 0; x < i.width(); ++x) s += qAlpha(i.pixel(x, y));
    return s / 255;
}
static QRect box(const QImage &i, int th = 16)
{
    int x0 = i.width(), y0 = i.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < i.height(); ++y) for (int x = 0; x < i.width(); ++x)
        if (qAlpha(i.pixel(x, y)) > th) { x0 = qMin(x0, x); y0 = qMin(y0, y); x1 = qMax(x1, x); y1 = qMax(y1, y); }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}
static QPointF centroid(const QImage &i)
{
    double sx = 0, sy = 0, n = 0;
    for (int y = 0; y < i.height(); ++y) for (int x = 0; x < i.width(); ++x) { double a = qAlpha(i.pixel(x, y)); sx += a * x; sy += a * y; n += a; }
    return n ? QPointF(sx / n, sy / n) : QPointF();
}
static int maxDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()) return 999;
    int d = 0;
    for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) {
        QRgb p = a.pixel(x, y), q = b.pixel(x, y);
        d = qMax(d, qMax(qMax(std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q))), qMax(std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q)))));
    }
    return d;
}
static TextStyle base(const QString &text = "Open Editor")
{
    TextStyle t;
    t.font = "Arial";
    t.text = text;
    t.size = 96;
    return t;
}
// Mean straight colour of the ink in a band of rows.
static QColor bandColor(const QImage &i, int y0, int y1)
{
    double r = 0, g = 0, b = 0, n = 0;
    for (int y = y0; y < y1; ++y) for (int x = 0; x < i.width(); ++x) {
        QRgb p = i.pixel(x, y);
        if (qAlpha(p) < 200) continue;
        r += qRed(p); g += qGreen(p); b += qBlue(p); ++n;
    }
    return n ? QColor(int(r / n), int(g / n), int(b / n)) : QColor();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("Open Editor Test");
    QCoreApplication::setOrganizationName("Open Editor");

    printf("--- old titles render exactly as before\n");
    for (auto &s : textRefStyles()) {
        // Through the project file, without the 2.1 keys, as an older project loads.
        Clip c;
        c.kind = MediaKind::Text;
        c.text = s.second;
        QJsonObject o = clipToJson(c), tj = o["text"].toObject();
        const QJsonObject fresh = textStyleToJson(TextStyle()), old{{"text", 1}, {"font", 1}, {"size", 1}, {"b", 1}, {"i", 1}, {"u", 1}, {"align", 1}, {"fill", 1}, {"stroke", 1},
            {"strokeC", 1}, {"strokeW", 1}, {"shadow", 1}, {"shadowC", 1}, {"shadowD", 1}, {"shadowB", 1}, {"bg", 1}, {"bgC", 1}, {"track", 1}, {"lead", 1}};
        for (auto it = fresh.begin(); it != fresh.end(); ++it) if (!old.contains(it.key())) tj.remove(it.key());
        o["text"] = tj;
        const TextStyle loaded = clipFromJson(o).text;
        for (int sc : {50, 100}) {
            QImage ref(QString(OE_SOURCE_DIR "/tests/data/text_ref/%1_%2.png").arg(s.first).arg(sc));
            QImage now = ImageFx::renderText(loaded, 1280 * sc / 100, 720 * sc / 100, sc / 100.0).convertToFormat(ref.format());
            int d = maxDiff(ref, now);
            printf("%s %s at %d%%: max difference %d\n", d <= 1 && !ref.isNull() ? "ok  " : "FAIL", qPrintable(s.first), sc, d);
            fails += d > 1 || ref.isNull();
        }
    }

    printf("--- every option survives the project file\n");
    {
        TextStyle t = base();
        t.weight = 300; t.strike = true; t.kerning = false; t.caps = 4; t.skew = 12; t.valign = 2; t.boxWidth = 500; t.fillType = 2;
        t.fill2 = QColor(1, 2, 3, 4); t.gradAngle = 33; t.strokePos = 2; t.shadowAngle = 77; t.glow = true; t.glowColor = QColor(9, 8, 7, 6);
        t.glowSize = 21; t.glowStrength = 2.5; t.bgPad = 11; t.bgRadius = 7; t.bgPerLine = true; t.animIn = 5; t.animOut = 6; t.animInSec = 1.25; t.animOutSec = .75;
        Clip c;
        c.kind = MediaKind::Text;
        c.text = t;
        {
            const QJsonObject a = textStyleToJson(clipFromJson(clipToJson(c)).text), b = textStyleToJson(t);
            for (auto it = b.begin(); it != b.end(); ++it)
                if (a[it.key()] != it.value())
                    SAY("round trip changed %s: %s -> %s", qPrintable(it.key()), qPrintable(QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact)),
                        qPrintable(QJsonDocument(QJsonArray{a[it.key()]}).toJson(QJsonDocument::Compact)));
        }
        CHECK(textStyleToJson(clipFromJson(clipToJson(c)).text) == textStyleToJson(t));
        CHECK(textStyleToJson(textStyleFromJson(textStyleToJson(t))) == textStyleToJson(t));
        CHECK(textStyleFromJson(QJsonObject{{"size", 40}}).shadowDist == TextStyle().shadowDist); // partial styles keep defaults
    }

    printf("--- each option does its job\n");
    {
        const QImage plain = render(base());
        TextStyle t = base(); t.weight = 900;
        TextStyle th = base(); th.weight = 100;
        SAY("ink: thin %.0f, regular %.0f, black %.0f", ink(render(th)), ink(plain), ink(render(t)));
        CHECK(ink(render(t)) > ink(render(th)) * 1.4);
        t = base(); t.strike = true;
        CHECK(ink(render(t)) > ink(plain) * 1.03);
        t = base("Hello"); TextStyle up = t; up.caps = 1; TextStyle sm = t; sm.caps = 2;
        CHECK(box(render(up)).width() > box(render(t)).width());
        CHECK(maxDiff(render(sm), render(t)) > 50 && box(render(sm)).height() < box(render(up)).height() + 1);
        t = base("HELLO"); TextStyle low = t; low.caps = 3;
        CHECK(box(render(low)).width() < box(render(t)).width());
        t = base("hello world"); TextStyle tc = t; tc.caps = 4; TextStyle typed = base("Hello World");
        CHECK(maxDiff(render(tc), render(typed)) == 0);
        t = base("AVAWAVA"); TextStyle nk = t; nk.kerning = false;
        SAY("AVAWAVA width with kerning %d, without %d", box(render(t)).width(), box(render(nk)).width());
        CHECK(box(render(nk)).width() > box(render(t)).width());
        t = base(); t.skew = 20;
        CHECK(maxDiff(render(t), plain) > 50 && box(render(t)).width() > box(plain).width());
        t = base(); t.valign = 0;
        CHECK(box(render(t)).top() < H * .2);
        t.valign = 2;
        CHECK(box(render(t)).bottom() > H * .8);
        t = base("The quick brown fox jumps over the lazy dog again and again"); t.boxWidth = 700;
        TextStyle one = t; one.boxWidth = 0;
        SAY("wrapped height %d, unwrapped %d", box(render(t)).height(), box(render(one)).height());
        CHECK(box(render(t)).height() > box(render(one)).height() * 2.5);
        CHECK(box(render(t)).width() <= 700 / 2 + 2);
        TextStyle just = t; just.align = 3; t.align = 0;
        auto rightOfFirstLine = [](const QImage &i) { QRect b = box(i); int r = 0; for (int y = b.top(); y < b.top() + 30; ++y) for (int x = 0; x < i.width(); ++x) if (qAlpha(i.pixel(x, y)) > 16) r = qMax(r, x); return r; };
        const QRectF blk = ImageFx::layoutTitle(just, W, H, .5).block;
        SAY("first line ends at x %d justified, %d left aligned; block ends at %.0f", rightOfFirstLine(render(just)), rightOfFirstLine(render(t)), blk.right());
        CHECK(std::abs(rightOfFirstLine(render(just)) - blk.right()) < 6 && rightOfFirstLine(render(t)) < blk.right() - 10);

        t = base("Gradient\nFill"); t.fill = QColor(255, 0, 0); t.fill2 = QColor(0, 0, 255); t.fillType = 1; t.gradAngle = 180; t.weight = 900;
        QImage g = render(t);
        QRect gb = box(g);
        QColor top = bandColor(g, gb.top(), gb.top() + gb.height() / 5), bot = bandColor(g, gb.bottom() - gb.height() / 5, gb.bottom());
        SAY("linear gradient: top %s, bottom %s", qPrintable(top.name()), qPrintable(bot.name()));
        CHECK(top.red() > top.blue() + 100 && bot.blue() > bot.red() + 100);
        t.fillType = 2;
        CHECK(maxDiff(render(t), g) > 60);

        t = base(); t.weight = 900; t.stroke = true; t.strokeWidth = 6; t.strokeColor = QColor(255, 255, 0);
        double none = ink(render(base())), out, cen, in;
        TextStyle s0 = t; s0.strokePos = 0; TextStyle s1 = t; s1.strokePos = 1; TextStyle s2 = t; s2.strokePos = 2;
        TextStyle bw = base(); bw.weight = 900; none = ink(render(bw));
        out = ink(render(s0)); cen = ink(render(s1)); in = ink(render(s2));
        SAY("ink: no stroke %.0f, inside %.0f, centre %.0f, outside %.0f", none, in, cen, out);
        CHECK(out > cen && cen > in && box(render(s2)) == box(render(bw)));
        CHECK(maxDiff(render(s2), render(bw)) > 100);

        t = base(); t.fill = QColor(0, 0, 0, 0); t.shadow = true; t.shadowDist = 40; t.shadowBlur = 0; t.shadowColor = Qt::black;
        const QPointF textC = centroid(plain);
        t.shadowAngle = 90;
        QPointF r90 = centroid(render(t));
        t.shadowAngle = 180;
        QPointF r180 = centroid(render(t));
        SAY("shadow centre moves (%.1f, %.1f) at 90 degrees and (%.1f, %.1f) at 180", r90.x() - textC.x(), r90.y() - textC.y(), r180.x() - textC.x(), r180.y() - textC.y());
        CHECK(std::abs(r90.x() - textC.x() - 20) < 2 && std::abs(r90.y() - textC.y()) < 2);
        CHECK(std::abs(r180.y() - textC.y() - 20) < 2 && std::abs(r180.x() - textC.x()) < 2);

        t = base(); t.glow = true; t.glowSize = 30; t.glowColor = QColor(0, 255, 255);
        CHECK(box(render(t), 4).width() > box(plain, 4).width() + 10);
        TextStyle g2 = t; g2.glowStrength = 3;
        CHECK(ink(render(g2)) > ink(render(t)));

        t = base(); t.background = true; t.bgColor = QColor(0, 0, 200); t.bgPad = 20;
        const QRectF blk2 = ImageFx::layoutTitle(t, W, H, .5).block;
        const QPoint corner(int(blk2.left() - 10 + 1), int(blk2.top() - 10 + 1));
        CHECK(qAlpha(render(t).pixel(corner)) == 255);
        t.bgRadius = 30;
        CHECK(qAlpha(render(t).pixel(corner)) < 40);
        t = base("A much longer first line\nshort"); t.background = true; t.bgColor = QColor(0, 0, 200); t.bgPad = 4;
        const ImageFx::TextLayout lay = ImageFx::layoutTitle(t, W, H, .5);
        const QPoint beside(int(lay.block.left() + 6), int(lay.lines[1].baseline - lay.ascent / 2));
        CHECK(qAlpha(render(t).pixel(beside)) == 255);
        t.bgPerLine = true;
        CHECK(qAlpha(render(t).pixel(beside)) == 0);

        TextStyle bnd = base("Bounds\nCheck"); bnd.skew = 15;
        const QRectF tb = ImageFx::textBounds(bnd, 1280, 720);
        QRect ib = box(ImageFx::renderText(bnd, 1280, 720, 1), 8);
        CHECK(tb.adjusted(-4, -4, 4, 4).contains(QRectF(ib)));
    }

    printf("--- animation\n");
    {
        TextStyle t = base(); t.animIn = TextAnimFade; t.animInSec = 1; t.animOut = TextAnimFade; t.animOutSec = 1;
        const double full = ink(render(base()));
        CHECK(ink(render(t, ImageFx::textAnimAt(t, 0, 5))) == 0);
        double mid = ink(render(t, ImageFx::textAnimAt(t, .3, 5)));
        CHECK(mid > full * .3 && mid < full * .9);
        CHECK(maxDiff(render(t, ImageFx::textAnimAt(t, 2.5, 5)), render(base())) == 0);
        CHECK(ink(render(t, ImageFx::textAnimAt(t, 4.995, 5))) < full * .05);
        t = base("Typewriter"); t.animIn = TextAnimTypewriter; t.animInSec = 2;
        const QImage tw = render(t, ImageFx::textAnimAt(t, 1, 5)), whole = render(base("Typewriter"));
        SAY("typewriter at half time: %.0f%% of the ink", 100 * ink(tw) / ink(whole));
        CHECK(ink(tw) > ink(whole) * .3 && ink(tw) < ink(whole) * .7);
        CHECK(box(tw).left() == box(whole).left()); // letters already typed do not move
        t = base(); t.animIn = TextAnimSlideUp; t.animInSec = 1;
        CHECK(centroid(render(t, ImageFx::textAnimAt(t, .3, 5))).y() > centroid(render(base())).y() + 3);
        t.animIn = TextAnimSlideDown;
        CHECK(centroid(render(t, ImageFx::textAnimAt(t, .3, 5))).y() < centroid(render(base())).y() - 3);
        t.animIn = TextAnimPop;
        CHECK(box(render(t, ImageFx::textAnimAt(t, .15, 5))).width() < box(render(base())).width() * .9);
        t.animIn = TextAnimWipe;
        CHECK(box(render(t, ImageFx::textAnimAt(t, .3, 5))).right() < box(render(base())).right() - 20);
        t.animOut = TextAnimTypewriter; t.animOutSec = 1;
        CHECK(ink(render(t, ImageFx::textAnimAt(t, 4.5, 5))) < full * .7);
    }

    printf("--- presets\n");
    {
        const QJsonObject known = textStyleToJson(TextStyle());
        for (auto &p : TextPresets::all()) {
            QStringList bad;
            for (auto it = p.second.begin(); it != p.second.end(); ++it) if (!known.contains(it.key())) bad << it.key();
            TextStyle s = TextPresets::apply(base("Preset"), p.second);
            const bool ok = bad.isEmpty() && s.text == "Preset" && ink(render(s)) > 100;
            printf("%s preset %-18s %s\n", ok ? "ok  " : "FAIL", qPrintable(p.first), bad.isEmpty() ? "" : qPrintable("unknown keys: " + bad.join(", ")));
            fails += !ok;
        }
        QSettings().remove("text/presets");
        TextStyle mine = base(); mine.fill = QColor(10, 200, 30); mine.glow = true;
        CHECK(!TextPresets::save("Neon Glow", mine) && !TextPresets::save("  ", mine));
        CHECK(TextPresets::save("My Green", mine));
        QJsonObject j;
        CHECK(TextPresets::find("my green", &j));
        const TextStyle applied = TextPresets::apply(base("Other words"), j);
        CHECK(applied.text == "Other words" && applied.fill == mine.fill && applied.glow);
        CHECK(TextPresets::remove("My Green") && !TextPresets::find("My Green", &j) && !TextPresets::remove("Neon Glow"));
    }

    printf("--- font import\n");
    {
        QDir(Fonts::folder()).removeRecursively();
        QTemporaryDir tmp;
        const QString given = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString("C:/Qt/Examples/Qt-6.11.1/canvaspainter/gallery/fonts/Pacifico.ttf");
        if (!QFileInfo::exists(given)) {
            printf("SKIP font import: %s not found\n", qPrintable(given));
        } else {
            const QString src = tmp.filePath("Pacifico.ttf");
            CHECK(QFile::copy(given, src));
            CHECK(!Fonts::available("Pacifico"));
            TextStyle t = base("Imported"); t.font = "Pacifico";
            const QImage before = render(t);
            QString err;
            const QStringList fams = Fonts::import(src, &err);
            SAY("imported families: %s %s", qPrintable(fams.join(", ")), qPrintable(err));
            CHECK(fams.contains("Pacifico") && Fonts::available("Pacifico"));
            CHECK(QFileInfo::exists(Fonts::folder() + "/Pacifico.ttf"));
            CHECK(maxDiff(render(t), before) > 100); // no longer the substitute
            CHECK(Fonts::import(src, &err).contains("Pacifico"));
            CHECK(QDir(Fonts::folder()).entryList(QDir::Files).size() == 1); // the same file is not copied twice
            QFile::remove(src);
            CHECK(Fonts::available("Pacifico")); // the copy keeps working without the original
            CHECK(Fonts::importedFamilies().contains("Pacifico"));
            ProjectState st;
            Sequence q;
            Track tr;
            Clip a; a.kind = MediaKind::Text; a.text = base(); a.text.font = "Pacifico";
            Clip b; b.kind = MediaKind::Text; b.text = base(); b.text.font = "No Such Font 123";
            tr.clips << a << b;
            q.video << tr;
            st.seqs << q;
            CHECK(Fonts::missingFamilies(st) == QStringList{"No Such Font 123"});
        }
        const QString txt = tmp.filePath("notes.txt"), fake = tmp.filePath("broken.ttf");
        { QFile f(txt); if (f.open(QIODevice::WriteOnly)) f.write("hello"); }
        { QFile f(fake); if (f.open(QIODevice::WriteOnly)) f.write(QByteArray(4000, 'x')); }
        QString err;
        CHECK(Fonts::import(txt, &err).isEmpty() && err.contains("not a font"));
        CHECK(Fonts::import(fake, &err).isEmpty() && err.contains("could not be read"));
        CHECK(!QFileInfo::exists(Fonts::folder() + "/broken.ttf"));
        CHECK(Fonts::import(tmp.filePath("missing.otf"), &err).isEmpty() && err.contains("does not exist"));
        QDir(Fonts::folder()).removeRecursively();
    }

    printf(fails ? "\n%d FAILURES\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
