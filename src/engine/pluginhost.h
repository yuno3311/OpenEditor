// Loads native effect plugins and exposes them through the effect registry.
#pragma once
#include "../core/model.h"
#include <QImage>
#include <memory>

namespace PluginHost {
int loadFrom(const QString &dir);           // returns effects registered; call at startup
QStringList loaded();                       // "file: effect" descriptions, for the About box
QStringList errors();
bool video(const Effect &e, QImage &img, Frame kt, double seconds);
bool audio(const Effect &e, float *stereo, int frames, Frame kt, std::shared_ptr<void> &state);
}
