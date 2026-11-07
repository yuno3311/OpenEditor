// Imported fonts. A font file the user imports is copied into the app data folder and loaded at
// every start, so titles keep their font even if the original file is moved or deleted.
#pragma once
#include "model.h"
#include <QStringList>

namespace Fonts {
QString folder();                                            // where imported fonts are kept
bool isFontFile(const QString &path);                        // .ttf .otf .ttc .otc .woff .woff2
QStringList loadImported();                                  // at startup; returns the families now available
QStringList import(const QString &path, QString *error = nullptr); // returns the font's families
QStringList importedFamilies();
bool available(const QString &family);
QStringList missingFamilies(const ProjectState &st);         // used by titles but not installed or imported
}
