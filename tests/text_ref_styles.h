// Title styles using only the options that existed before 2.1. Their renders from the 2.0 renderer
// are stored in tests/data/text_ref, so a change to the renderer cannot quietly alter old projects.
#pragma once
#include "core/model.h"
#include <QList>
#include <QPair>

inline QList<QPair<QString, TextStyle>> textRefStyles()
{
    QList<QPair<QString, TextStyle>> v;
    TextStyle t;
    t.font = "Arial";
    v << qMakePair(QString("default"), t);
    TextStyle s = t; s.text = "Stroked\nTwo Lines"; s.stroke = true; s.strokeWidth = 6; s.strokeColor = QColor(200, 30, 30); s.size = 80;
    v << qMakePair(QString("stroke"), s);
    TextStyle d = t; d.text = "Shadow"; d.shadow = true; d.shadowDist = 10; d.shadowBlur = 12; d.fill = QColor(250, 220, 40);
    v << qMakePair(QString("shadow"), d);
    TextStyle b = t; b.text = "Boxed title"; b.background = true; b.bgColor = QColor(20, 40, 160, 200); b.bold = false; b.size = 60;
    v << qMakePair(QString("background"), b);
    TextStyle l = t; l.text = "Left aligned\nwith a longer line\nend"; l.align = 0; l.italic = true; l.underline = true; l.tracking = 4; l.leading = 12; l.size = 48;
    v << qMakePair(QString("left"), l);
    TextStyle r = l; r.align = 2; r.italic = false; r.underline = false; r.bold = false;
    v << qMakePair(QString("right"), r);
    return v;
}
