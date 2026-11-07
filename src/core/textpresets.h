// Title style presets: built-in looks plus the user's own, saved in the settings. A preset is a
// partial text style in the project JSON keys; applying it keeps the title's words.
#pragma once
#include "model.h"
#include <QJsonObject>
#include <QList>
#include <QPair>

namespace TextPresets {
QList<QPair<QString, QJsonObject>> all(); // built-in first, then saved
bool isBuiltIn(const QString &name);
bool find(const QString &name, QJsonObject *out);
bool save(const QString &name, const TextStyle &style); // false for an empty name or a built-in one
bool remove(const QString &name);                       // saved presets only
TextStyle apply(const TextStyle &t, const QJsonObject &preset);
}
