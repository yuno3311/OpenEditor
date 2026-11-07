#include "textpresets.h"
#include <QJsonDocument>
#include <QSettings>

namespace TextPresets {

static const QList<QPair<QString, QJsonObject>> &builtIns()
{
    static const QList<QPair<QString, QJsonObject>> v = {
        {"Clean White", {{"fill", "#ffffffff"}, {"weight", 600}, {"shadow", true}, {"shadowC", "#99000000"}, {"shadowD", 5}, {"shadowB", 10}}},
        {"Lower Third", {{"size", 54}, {"weight", 600}, {"align", 0}, {"valign", 2}, {"bg", true}, {"bgC", "#d01e1e1e"}, {"bgPad", 18}, {"bgR", 6},
                         {"bgLine", true}, {"fill", "#ffffffff"}}},
        {"Lower Third Slide", {{"size", 54}, {"weight", 600}, {"align", 0}, {"valign", 2}, {"bg", true}, {"bgC", "#d01e1e1e"}, {"bgPad", 18}, {"bgR", 6},
                               {"bgLine", true}, {"fill", "#ffffffff"}, {"animIn", 3}, {"animInS", .5}, {"animOut", 1}, {"animOutS", .4}}},
        {"Bold Outline", {{"weight", 900}, {"caps", 1}, {"stroke", true}, {"strokeW", 8}, {"strokeC", "#ff000000"}, {"strokePos", 0}, {"fill", "#ffffffff"}}},
        {"Neon Glow", {{"weight", 300}, {"fill", "#ffffffff"}, {"glow", true}, {"glowC", "#e600e5ff"}, {"glowS", 20}, {"glowK", 2}, {"track", 4}}},
        {"Sunset Gradient", {{"weight", 800}, {"fillType", 1}, {"fill", "#ffffd86f"}, {"fill2", "#fffc6262"}, {"gradA", 180},
                             {"shadow", true}, {"shadowC", "#80000000"}, {"shadowD", 6}, {"shadowB", 8}}},
        {"Subtitle Box", {{"size", 46}, {"weight", 500}, {"valign", 2}, {"bg", true}, {"bgLine", true}, {"bgC", "#b0000000"}, {"bgPad", 10}, {"bgR", 4}}},
        {"Cinematic", {{"caps", 1}, {"track", 18}, {"weight", 300}, {"size", 72}, {"fill", "#fff2f2f2"}, {"animIn", 1}, {"animInS", 1.2}, {"animOut", 1}, {"animOutS", 1.2}}},
        {"Typewriter", {{"font", "Courier New"}, {"weight", 400}, {"align", 0}, {"animIn", 2}, {"animInS", 1.5}}},
        {"Pop Title", {{"weight", 900}, {"stroke", true}, {"strokeW", 4}, {"strokeC", "#ff101010"}, {"animIn", 5}, {"animInS", .45}, {"animOut", 1}, {"animOutS", .3}}},
    };
    return v;
}

static QJsonObject saved()
{
    return QJsonDocument::fromJson(QSettings().value("text/presets").toByteArray()).object();
}

QList<QPair<QString, QJsonObject>> all()
{
    auto v = builtIns();
    const QJsonObject s = saved();
    for (auto it = s.begin(); it != s.end(); ++it) v << qMakePair(it.key(), it.value().toObject());
    return v;
}

bool isBuiltIn(const QString &name)
{
    for (auto &p : builtIns()) if (p.first.compare(name, Qt::CaseInsensitive) == 0) return true;
    return false;
}

bool find(const QString &name, QJsonObject *out)
{
    for (auto &p : all())
        if (p.first.compare(name, Qt::CaseInsensitive) == 0) { if (out) *out = p.second; return true; }
    return false;
}

bool save(const QString &name, const TextStyle &style)
{
    const QString n = name.trimmed();
    if (n.isEmpty() || isBuiltIn(n)) return false;
    QJsonObject j = textStyleToJson(style);
    j.remove("text");
    QJsonObject s = saved();
    s[n] = j;
    QSettings().setValue("text/presets", QJsonDocument(s).toJson(QJsonDocument::Compact));
    return true;
}

bool remove(const QString &name)
{
    QJsonObject s = saved();
    if (!s.contains(name)) return false;
    s.remove(name);
    QSettings().setValue("text/presets", QJsonDocument(s).toJson(QJsonDocument::Compact));
    return true;
}

TextStyle apply(const TextStyle &t, const QJsonObject &preset)
{
    QJsonObject j = textStyleToJson(t);
    for (auto it = preset.begin(); it != preset.end(); ++it)
        if (it.key() != "text") j[it.key()] = it.value();
    // A preset written with the older "bold" key alone should not be overridden by a stale weight.
    if (preset.contains("b") && !preset.contains("weight")) j["weight"] = 0;
    return textStyleFromJson(j);
}

} // namespace TextPresets
