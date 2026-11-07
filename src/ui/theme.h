#pragma once
#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace Theme {
inline const QColor bg("#232323"), bgDark("#1b1b1b"), bgDeep("#131313"), bgLight("#2e2e2e"), bgHover("#3a3a3a");
inline const QColor border("#0d0d0d"), text("#c8c8c8"), textDim("#8a8a8a"), textBright("#f0f0f0");
inline const QColor accent("#2d8ceb"), accentDim("#1f5f9f"), scrub("#3fa2ff"), playhead("#e8e8e8");
inline const QColor inOut("#4d4d4d"), trackBg("#262626"), trackAlt("#2a2a2a"), selection("#ffffff");
void apply(QApplication &app);
QIcon icon(const QString &name, const QColor &c = QColor("#c8c8c8"), const QColor &active = QColor());
QPixmap pixmap(const QString &name, int size, const QColor &c = QColor("#c8c8c8"));
}
