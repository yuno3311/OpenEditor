// Declarative registry of every effect and transition the editor offers.
#pragma once
#include "model.h"
#include <QStringList>

enum class ParamKind { Number, Percent, Angle, Toggle, Choice, Color, PointX, PointY, Db };

struct ParamDef {
    QString id, name;
    ParamKind kind = ParamKind::Number;
    double def = 0, min = -1e9, max = 1e9;   // hard limits
    double softMin = 0, softMax = 100;       // slider range
    double step = 1;
    QStringList choices;
    QString unit;
    bool animatable = true;
};

struct EffectDef {
    QString id, name, category;
    bool audio = false;
    bool intrinsic = false;
    QVector<ParamDef> params;
};

struct TransitionDef {
    QString id, name, category;
    bool audio = false;
};

namespace EffectRegistry {
const QVector<EffectDef> &all();
void add(const EffectDef &d);           // plugins, call before threads start
const EffectDef *find(const QString &id);
const QVector<TransitionDef> &transitions();
const TransitionDef *findTransition(const QString &id);
Effect make(const QString &id);
void defaultsFor(Clip &c);     // adds intrinsic motion/opacity or volume/pan
}
