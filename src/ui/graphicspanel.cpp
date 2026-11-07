// Graphics panel: every title option, style presets, and font import.
#include "colorpanel.h"
#include "../core/fonts.h"
#include "../core/project.h"
#include "../core/textpresets.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

static const int kWeights[] = {100, 200, 300, 400, 500, 600, 700, 800, 900};
static const char *const kWeightNames[] = {"Thin", "Extra Light", "Light", "Regular", "Medium", "Semibold", "Bold", "Extra Bold", "Black"};

QStringList GraphicsPanel::animationNames()
{
    return {tr("None"), tr("Fade"), tr("Typewriter"), tr("Slide Up"), tr("Slide Down"), tr("Pop"), tr("Wipe")};
}

GraphicsPanel::GraphicsPanel(QWidget *p) : Panel("graphics", tr("Graphics"), p)
{
    auto *o = new QVBoxLayout(this);
    o->setContentsMargins(0, 0, 0, 0);
    empty = new QLabel(tr("Select a title in the timeline, or press T and click to add one."), this);
    empty->setWordWrap(true);
    empty->setAlignment(Qt::AlignCenter);
    empty->setStyleSheet("color:#777;");
    o->addWidget(empty);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    form = new QWidget(scroll);
    scroll->setWidget(form);
    o->addWidget(scroll);
    scrollArea = scroll;

    auto *v = new QVBoxLayout(form);
    v->setContentsMargins(8, 6, 8, 8);
    auto section = [&](const QString &t) {
        auto *l = new QLabel(t, form);
        l->setStyleSheet("color:#cfcfcf;font-weight:600;margin-top:8px;");
        v->addWidget(l);
        auto *f = new QFormLayout;
        f->setLabelAlignment(Qt::AlignLeft);
        f->setHorizontalSpacing(10);
        f->setVerticalSpacing(5);
        v->addLayout(f);
        return f;
    };
    auto scrub = [&](double lo, double hi, int dec = 0, const QString &suffix = {}) {
        auto *s = new ScrubValue(form);
        s->setRange(lo, hi);
        s->setDecimals(dec);
        if (!suffix.isEmpty()) s->setSuffix(suffix);
        return s;
    };
    auto combo = [&](const QStringList &items) {
        auto *c = new QComboBox(form);
        c->addItems(items);
        c->setFocusPolicy(Qt::StrongFocus);
        return c;
    };
    auto swatch = [&] { auto *s = new ColorSwatch(form); s->withAlpha = true; return s; };
    auto rowOf = [&](std::initializer_list<QWidget *> ws) {
        auto *h = new QHBoxLayout;
        h->setSpacing(6);
        for (auto *w : ws) h->addWidget(w);
        h->addStretch(1);
        return h;
    };

    // Presets
    auto *fp = section(tr("Style"));
    preset = combo({});
    preset->setToolTip(tr("Apply a look to this title. The words stay as they are."));
    auto *savePre = new QPushButton(tr("Save..."), form);
    savePre->setToolTip(tr("Save this title's look as a preset"));
    delPre = new QPushButton(tr("Delete"), form);
    delPre->setToolTip(tr("Delete the selected saved preset"));
    fp->addRow(rowOf({preset, savePre, delPre}));

    // Text
    auto *ft = section(tr("Text"));
    text = new QPlainTextEdit(form);
    text->setMaximumHeight(80);
    ft->addRow(text);

    // Font
    auto *ff = section(tr("Font"));
    font = new QFontComboBox(form);
    auto *imp = new QPushButton(tr("Import Font..."), form);
    imp->setToolTip(tr("Add a .ttf, .otf, .ttc or .woff font. It is kept with Open Editor's settings."));
    ff->addRow(rowOf({font, imp}));
    fontNote = new QLabel(form);
    fontNote->setWordWrap(true);
    fontNote->setStyleSheet("color:#e5a33a;");
    fontNote->hide();
    ff->addRow(fontNote);
    weight = combo({});
    for (int i = 0; i < 9; ++i) weight->addItem(QString("%1 %2").arg(tr(kWeightNames[i])).arg(kWeights[i]), kWeights[i]);
    size = scrub(4, 800, 0, " px");
    ff->addRow(tr("Weight"), rowOf({weight, new QLabel(tr("Size"), form), size}));
    auto tog = [&](const QString &t, const QString &tip) {
        auto *b = new QToolButton(form);
        b->setText(t); b->setCheckable(true); b->setToolTip(tip); b->setFixedSize(26, 22); b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet("QToolButton:checked{background:#2d8ceb;color:white;}");
        return b;
    };
    ital = tog("I", tr("Italic")); under = tog("U", tr("Underline")); strike = tog("S", tr("Strikethrough"));
    ital->setStyleSheet(ital->styleSheet() + "QToolButton{font-style:italic;}");
    under->setStyleSheet(under->styleSheet() + "QToolButton{text-decoration:underline;}");
    strike->setStyleSheet(strike->styleSheet() + "QToolButton{text-decoration:line-through;}");
    caps = combo({tr("As Typed"), tr("ALL CAPS"), tr("Small Caps"), tr("lowercase"), tr("Title Case")});
    ff->addRow(tr("Style"), rowOf({ital, under, strike, caps}));
    tracking = scrub(-50, 400);
    kern = new QCheckBox(tr("Kerning"), form);
    ff->addRow(tr("Tracking"), rowOf({tracking, kern}));
    skew = scrub(-45, 45, 0, QString::fromUtf8("\xc2\xb0"));
    skew->setToolTip(tr("Slants the letters, for fonts without an italic"));
    ff->addRow(tr("Skew"), skew);

    // Paragraph
    auto *fa = section(tr("Paragraph"));
    const char *alNames[] = {"L", "C", "R", "J"};
    const QString alTips[] = {tr("Align Left"), tr("Center"), tr("Align Right"), tr("Justify")};
    for (int i = 0; i < 4; ++i) al[i] = tog(alNames[i], alTips[i]);
    valign = combo({tr("Top"), tr("Middle"), tr("Bottom")});
    valign->setToolTip(tr("Where the text sits in the frame, inside the title-safe area"));
    fa->addRow(tr("Align"), rowOf({al[0], al[1], al[2], al[3], valign}));
    leading = scrub(-200, 400);
    fa->addRow(tr("Leading"), leading);
    boxW = scrub(0, 7680, 0, " px");
    boxW->setToolTip(tr("Wrap text at this width. 0 keeps every line as typed."));
    fa->addRow(tr("Wrap Width"), boxW);

    // Fill
    auto *fl = section(tr("Fill"));
    fillType = combo({tr("Solid"), tr("Linear Gradient"), tr("Radial Gradient")});
    fill = swatch(); fill2 = swatch();
    gradA = scrub(-360, 360, 0, QString::fromUtf8("\xc2\xb0"));
    fl->addRow(tr("Type"), rowOf({fillType, fill, fill2}));
    fl->addRow(tr("Gradient Angle"), gradA);

    // Stroke
    auto *fs = section(tr("Stroke"));
    stroke = new QCheckBox(tr("Stroke"), form);
    strokeC = swatch();
    strokeW = scrub(0, 100, 1, " px");
    strokePos = combo({tr("Outside"), tr("Center"), tr("Inside")});
    fs->addRow(rowOf({stroke, strokeC, strokeW, strokePos}));

    // Shadow
    auto *fsh = section(tr("Shadow"));
    shadow = new QCheckBox(tr("Shadow"), form);
    shadowC = swatch();
    fsh->addRow(rowOf({shadow, shadowC}));
    shadowD = scrub(0, 300, 1, " px");
    shadowA = scrub(-360, 360, 0, QString::fromUtf8("\xc2\xb0"));
    shadowB = scrub(0, 200, 0, " px");
    fsh->addRow(tr("Distance"), rowOf({shadowD, new QLabel(tr("Angle"), form), shadowA, new QLabel(tr("Blur"), form), shadowB}));

    // Glow
    auto *fg = section(tr("Glow"));
    glow = new QCheckBox(tr("Glow"), form);
    glowC = swatch();
    glowS = scrub(1, 200, 0, " px");
    glowK = scrub(0, 3, 1);
    glowK->setStep(.05);
    fg->addRow(rowOf({glow, glowC}));
    fg->addRow(tr("Size"), rowOf({glowS, new QLabel(tr("Strength"), form), glowK}));

    // Background
    auto *fb = section(tr("Background"));
    bg = new QCheckBox(tr("Background"), form);
    bgC = swatch();
    bgLine = new QCheckBox(tr("Per Line"), form);
    bgLine->setToolTip(tr("A box behind each line instead of one box behind the block"));
    fb->addRow(rowOf({bg, bgC, bgLine}));
    bgPad = scrub(-1, 400, 0, " px");
    bgPad->setToolTip(tr("Space around the text. -1 sizes it from the font."));
    bgR = scrub(0, 400, 0, " px");
    fb->addRow(tr("Padding"), rowOf({bgPad, new QLabel(tr("Corner Radius"), form), bgR}));

    // Animation
    auto *fan = section(tr("Animation"));
    animIn = combo(animationNames());
    animOut = combo(animationNames());
    animInS = scrub(0, 30, 2, " s");
    animOutS = scrub(0, 30, 2, " s");
    animInS->setStep(.05); animOutS->setStep(.05);
    fan->addRow(tr("In"), rowOf({animIn, animInS}));
    fan->addRow(tr("Out"), rowOf({animOut, animOutS}));
    v->addStretch(1);

    // ---------------------------------------------------------------- wiring
    auto num = [this](ScrubValue *s, std::function<void(TextStyle &, double)> fn) {
        connect(s, &ScrubValue::edited, this, [this, fn](double x) {
            auto q = PRJ->active();
            if (auto *c = q ? q->clip(clipId) : nullptr) { fn(c->text, x); PRJ->liveTouch(); }
        });
        connect(s, &ScrubValue::dragBegan, this, [] { PRJ->liveBegin(); });
        connect(s, &ScrubValue::dragEnded, this, [] { PRJ->liveCommit(tr("Edit Title")); });
    };
    num(size, [](TextStyle &t, double x) { t.size = x; });
    num(tracking, [](TextStyle &t, double x) { t.tracking = x; });
    num(leading, [](TextStyle &t, double x) { t.leading = x; });
    num(skew, [](TextStyle &t, double x) { t.skew = x; });
    num(boxW, [](TextStyle &t, double x) { t.boxWidth = x; });
    num(gradA, [](TextStyle &t, double x) { t.gradAngle = x; });
    num(strokeW, [](TextStyle &t, double x) { t.strokeWidth = x; });
    num(shadowD, [](TextStyle &t, double x) { t.shadowDist = x; });
    num(shadowA, [](TextStyle &t, double x) { t.shadowAngle = x; });
    num(shadowB, [](TextStyle &t, double x) { t.shadowBlur = x; });
    num(glowS, [](TextStyle &t, double x) { t.glowSize = x; });
    num(glowK, [](TextStyle &t, double x) { t.glowStrength = x; });
    num(bgPad, [](TextStyle &t, double x) { t.bgPad = x; });
    num(bgR, [](TextStyle &t, double x) { t.bgRadius = x; });
    num(animInS, [](TextStyle &t, double x) { t.animInSec = x; });
    num(animOutS, [](TextStyle &t, double x) { t.animOutSec = x; });
    connect(text, &QPlainTextEdit::textChanged, this, [this] {
        if (loading) return;
        QString s = text->toPlainText();
        store([s](TextStyle &t) { t.text = s; }, tr("Edit Title Text"));
    });
    connect(font, &QFontComboBox::currentFontChanged, this, [this](const QFont &f) { if (!loading) store([f](TextStyle &t) { t.font = f.family(); }, tr("Change Font")); });
    auto pick = [this](QComboBox *c, std::function<void(TextStyle &, int)> fn, const QString &label) {
        connect(c, &QComboBox::currentIndexChanged, this, [this, c, fn, label](int i) { if (!loading && i >= 0) store([fn, i](TextStyle &t) { fn(t, i); }, label); (void)c; });
    };
    pick(weight, [](TextStyle &t, int i) { t.weight = kWeights[i]; t.bold = t.weight >= 600; }, tr("Font Weight"));
    pick(caps, [](TextStyle &t, int i) { t.caps = i; }, tr("Capitalization"));
    pick(valign, [](TextStyle &t, int i) { t.valign = i; }, tr("Vertical Align"));
    pick(fillType, [](TextStyle &t, int i) { t.fillType = i; }, tr("Fill Type"));
    pick(strokePos, [](TextStyle &t, int i) { t.strokePos = i; }, tr("Stroke Position"));
    pick(animIn, [](TextStyle &t, int i) { t.animIn = i; }, tr("In Animation"));
    pick(animOut, [](TextStyle &t, int i) { t.animOut = i; }, tr("Out Animation"));
    auto flag = [this](QAbstractButton *b, std::function<void(TextStyle &, bool)> fn, const QString &label) {
        connect(b, &QAbstractButton::clicked, this, [this, fn, label](bool on) { store([fn, on](TextStyle &t) { fn(t, on); }, label); });
    };
    flag(ital, [](TextStyle &t, bool on) { t.italic = on; }, tr("Italic"));
    flag(under, [](TextStyle &t, bool on) { t.underline = on; }, tr("Underline"));
    flag(strike, [](TextStyle &t, bool on) { t.strike = on; }, tr("Strikethrough"));
    flag(kern, [](TextStyle &t, bool on) { t.kerning = on; }, tr("Kerning"));
    flag(stroke, [](TextStyle &t, bool on) { t.stroke = on; }, tr("Stroke"));
    flag(shadow, [](TextStyle &t, bool on) { t.shadow = on; }, tr("Shadow"));
    flag(glow, [](TextStyle &t, bool on) { t.glow = on; }, tr("Glow"));
    flag(bg, [](TextStyle &t, bool on) { t.background = on; }, tr("Background"));
    flag(bgLine, [](TextStyle &t, bool on) { t.bgPerLine = on; }, tr("Background Per Line"));
    for (int i = 0; i < 4; ++i) connect(al[i], &QToolButton::clicked, this, [this, i] { store([i](TextStyle &t) { t.align = i; }, tr("Align")); });
    auto color = [this](ColorSwatch *s, std::function<void(TextStyle &, const QColor &)> fn, const QString &label) {
        connect(s, &ColorSwatch::changed, this, [this, fn, label](const QColor &c) { store([fn, c](TextStyle &t) { fn(t, c); }, label); });
    };
    color(fill, [](TextStyle &t, const QColor &c) { t.fill = c; }, tr("Fill"));
    color(fill2, [](TextStyle &t, const QColor &c) { t.fill2 = c; }, tr("Gradient Color"));
    color(strokeC, [](TextStyle &t, const QColor &c) { t.strokeColor = c; }, tr("Stroke"));
    color(shadowC, [](TextStyle &t, const QColor &c) { t.shadowColor = c; }, tr("Shadow"));
    color(glowC, [](TextStyle &t, const QColor &c) { t.glowColor = c; }, tr("Glow"));
    color(bgC, [](TextStyle &t, const QColor &c) { t.bgColor = c; }, tr("Background"));
    connect(imp, &QPushButton::clicked, this, &GraphicsPanel::importFont);
    connect(preset, &QComboBox::activated, this, [this](int i) { applyPreset(i); });
    connect(savePre, &QPushButton::clicked, this, &GraphicsPanel::savePreset);
    connect(delPre, &QPushButton::clicked, this, [this] {
        const QString n = preset->currentData().toString();
        if (n.isEmpty() || TextPresets::isBuiltIn(n)) return;
        if (QMessageBox::question(this, tr("Delete Preset"), tr("Delete the preset \"%1\"?").arg(n)) == QMessageBox::Yes) { TextPresets::remove(n); reloadPresets(); }
    });
    connect(preset, &QComboBox::currentIndexChanged, this, [this] { delPre->setEnabled(!TextPresets::isBuiltIn(preset->currentData().toString()) && !preset->currentData().toString().isEmpty()); });
    connect(PRJ, &Project::selectionChanged, this, &GraphicsPanel::load);
    connect(PRJ, &Project::changed, this, &GraphicsPanel::load);
    reloadPresets();
    load();
}

void GraphicsPanel::reloadPresets()
{
    QSignalBlocker b(preset);
    preset->clear();
    preset->addItem(tr("Choose a preset..."), QString());
    for (auto &p : TextPresets::all()) preset->addItem(TextPresets::isBuiltIn(p.first) ? p.first : p.first + tr(" (saved)"), p.first);
    preset->setCurrentIndex(0);
    delPre->setEnabled(false);
}

void GraphicsPanel::applyPreset(int i)
{
    const QString name = preset->itemData(i).toString();
    QJsonObject j;
    if (name.isEmpty() || !TextPresets::find(name, &j)) return;
    store([j](TextStyle &t) { t = TextPresets::apply(t, j); }, tr("Apply Style: %1").arg(name));
}

void GraphicsPanel::savePreset()
{
    auto q = PRJ->active();
    const Clip *c = q ? q->clip(clipId) : nullptr;
    if (!c) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save Style"), tr("Preset name:"), QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (TextPresets::isBuiltIn(name)) { QMessageBox::information(this, tr("Save Style"), tr("\"%1\" is a built-in preset. Choose another name.").arg(name)); return; }
    TextPresets::save(name, c->text);
    reloadPresets();
    preset->setCurrentIndex(preset->findData(name));
}

void GraphicsPanel::importFont()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Font"), {}, tr("Fonts (*.ttf *.otf *.ttc *.otc *.woff *.woff2)"));
    if (files.isEmpty()) return;
    QStringList fams, errs;
    for (auto &f : files) {
        QString e;
        QStringList got = Fonts::import(f, &e);
        if (got.isEmpty()) errs << e; else fams << got;
    }
    fams.removeDuplicates();
    if (!fams.isEmpty() && clipId) {
        const QString fam = fams.first();
        store([fam](TextStyle &t) { t.font = fam; }, tr("Change Font"));
    }
    if (!errs.isEmpty()) QMessageBox::warning(this, tr("Import Font"), errs.join('\n'));
    else if (!fams.isEmpty() && !clipId) QMessageBox::information(this, tr("Import Font"), tr("Imported: %1").arg(fams.join(", ")));
    load();
}

void GraphicsPanel::store(const std::function<void(TextStyle &)> &f, const QString &label)
{
    int id = clipId;
    if (!id) return;
    PRJ->edit(label, [&](ProjectState &st) { if (auto *c = st.seqById(st.activeSeq)->clip(id)) f(c->text); });
}

void GraphicsPanel::load()
{
    if (PRJ->inLive() || text->hasFocus()) return;
    auto q = PRJ->active();
    const Clip *c = nullptr;
    if (q) for (int id : std::as_const(PRJ->selClips)) if (auto *k = q->clip(id)) if (k->kind == MediaKind::Text) c = k;
    clipId = c ? c->id : 0;
    empty->setVisible(!c);
    scrollArea->setVisible(c);
    if (!c) return;
    loading = true;
    const TextStyle &t = c->text;
    if (text->toPlainText() != t.text) text->setPlainText(t.text);
    font->setCurrentFont(QFont(t.font));
    const bool have = Fonts::available(t.font);
    fontNote->setVisible(!have);
    if (!have) fontNote->setText(tr("\"%1\" is not installed, so a substitute is shown. Import the font file to use it.").arg(t.font));
    int wi = 3;
    for (int i = 0; i < 9; ++i) if (std::abs(kWeights[i] - t.effectiveWeight()) < std::abs(kWeights[wi] - t.effectiveWeight())) wi = i;
    weight->setCurrentIndex(wi);
    size->setValue(t.size); tracking->setValue(t.tracking); leading->setValue(t.leading); skew->setValue(t.skew); boxW->setValue(t.boxWidth);
    ital->setChecked(t.italic); under->setChecked(t.underline); strike->setChecked(t.strike); kern->setChecked(t.kerning);
    caps->setCurrentIndex(t.caps);
    for (int i = 0; i < 4; ++i) al[i]->setChecked(t.align == i);
    valign->setCurrentIndex(t.valign);
    fillType->setCurrentIndex(t.fillType);
    fill->setColor(t.fill); fill2->setColor(t.fill2); gradA->setValue(t.gradAngle);
    fill2->setEnabled(t.fillType != 0); gradA->setEnabled(t.fillType == 1);
    stroke->setChecked(t.stroke); strokeC->setColor(t.strokeColor); strokeW->setValue(t.strokeWidth); strokePos->setCurrentIndex(t.strokePos);
    shadow->setChecked(t.shadow); shadowC->setColor(t.shadowColor); shadowD->setValue(t.shadowDist); shadowA->setValue(t.shadowAngle); shadowB->setValue(t.shadowBlur);
    glow->setChecked(t.glow); glowC->setColor(t.glowColor); glowS->setValue(t.glowSize); glowK->setValue(t.glowStrength);
    bg->setChecked(t.background); bgC->setColor(t.bgColor); bgLine->setChecked(t.bgPerLine); bgPad->setValue(t.bgPad); bgR->setValue(t.bgRadius);
    animIn->setCurrentIndex(t.animIn); animOut->setCurrentIndex(t.animOut); animInS->setValue(t.animInSec); animOutS->setValue(t.animOutSec);
    loading = false;
}
