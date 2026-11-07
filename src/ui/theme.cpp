#include "theme.h"
#include <QApplication>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QStyleFactory>
#include <QSvgRenderer>

namespace Theme {

// Original icon set: 24x24 SVG fragments using currentColor-style substitution of %C.
static QString svgBody(const QString &n)
{
    static const QHash<QString, QString> m = {
        {"play", "<path fill='%C' d='M7 4l13 8-13 8z'/>"},
        {"pause", "<path fill='%C' d='M6 4h4v16H6zM14 4h4v16h-4z'/>"},
        {"stepback", "<path fill='%C' d='M5 5h2.5v14H5zM19 5v14L9 12z'/>"},
        {"stepfwd", "<path fill='%C' d='M16.500 5H19v14h-2.500zM5 5v14l10-7z'/>"},
        {"gotoin", "<path fill='%C' d='M4 5h2v14H4zM7 5h4v2H9v10h2v2H7zM21 6v12l-9-6z'/>"},
        {"gotoout", "<path fill='%C' d='M20 5h-2v14h2zM17 5h-4v2h2v10h-2v2h4zM3 6v12l9-6z'/>"},
        {"markin", "<path fill='%C' d='M8 3h9v3h-6v12h6v3H8z'/>"},
        {"markout", "<path fill='%C' d='M16 3H7v3h6v12H7v3h9z'/>"},
        {"marker", "<path fill='%C' d='M6 3h12v12l-6 6-6-6z'/>"},
        {"insert", "<path fill='%C' d='M3 15h7v5H3zM14 15h7v5h-7zM12 3l5 5h-3v5h-4V8H7z'/>"},
        {"overwrite", "<path fill='%C' d='M3 15h18v5H3zM12 13L7 8h3V3h4v5h3z'/>"},
        {"lift", "<path fill='%C' d='M3 15h18v5H3zM12 3l5 5h-3v5h-4V8H7z'/>"},
        {"extract", "<path fill='%C' d='M3 15h7v5H3zM14 15h7v5h-7zM12 13L7 8h3V3h4v5h3z'/>"},
        {"camera", "<path fill='%C' d='M3 7h4l2-3h6l2 3h4v13H3zm9 3a4 4 0 100 8 4 4 0 000-8z' fill-rule='evenodd'/>"},
        {"loop", "<path fill='none' stroke='%C' stroke-width='2' d='M17 7H8a4 4 0 00-4 4v2M7 17h9a4 4 0 004-4v-2'/><path fill='%C' d='M15 3l5 4-5 4zM9 13l-5 4 5 4z'/>"},
        {"safe", "<path fill='none' stroke='%C' stroke-width='1.600' d='M3 5h18v14H3zM6 7.500h12v9H6zM12 10v4M10 12h4'/>"},
        {"plus", "<path fill='%C' d='M11 4h2v7h7v2h-7v7h-2v-7H4v-2h7z'/>"},
        {"wrench", "<path fill='%C' d='M21 7l-4 4-3-1-1-3 4-4a6 6 0 00-7.500 7.500L3 17.500 6.500 21l6.500-6.500A6 6 0 0021 7z'/>"},
        {"menu", "<path fill='%C' d='M3 6h18v2H3zM3 11h18v2H3zM3 16h18v2H3z'/>"},
        {"eye", "<path fill='%C' fill-rule='evenodd' d='M12 5c5 0 9 4 10.500 7-1.500 3-5.500 7-10.500 7S3 15 1.500 12C3 9 7 5 12 5zm0 3a4 4 0 100 8 4 4 0 000-8z'/>"},
        {"eyeoff", "<path fill='%C' d='M3 3l18 18-1.500 1.500-3.700-3.700A11 11 0 0112 19C7 19 3 15 1.500 12c.9-1.800 2.300-3.700 4.300-5.200L1.500 4.500zM12 5c5 0 9 4 10.500 7a15 15 0 01-3.300 4.300L8.100 6A11 11 0 0112 5z'/>"},
        {"lock", "<path fill='%C' fill-rule='evenodd' d='M6 10V8a6 6 0 0112 0v2h2v12H4V10zm3 0h6V8a3 3 0 00-6 0z'/>"},
        {"unlock", "<path fill='%C' fill-rule='evenodd' d='M6 10V8a6 6 0 0111.600-2l-2.800 1A3 3 0 009 8v2h11v12H4V10z'/>"},
        {"mic", "<path fill='%C' d='M12 2a3 3 0 013 3v7a3 3 0 01-6 0V5a3 3 0 013-3zM6 11h2a4 4 0 008 0h2a6 6 0 01-5 5.900V20h3v2H8v-2h3v-3.100A6 6 0 016 11z'/>"},
        {"snap", "<path fill='%C' d='M5 3h5v9a2 2 0 004 0V3h5v9a7 7 0 01-14 0zM5 5v2h5V5zM14 5v2h5V5z' fill-rule='evenodd'/>"},
        {"link", "<path fill='none' stroke='%C' stroke-width='2.400' d='M10 14a4 4 0 005.600 0l3-3a4 4 0 00-5.600-5.600l-1 1M14 10a4 4 0 00-5.600 0l-3 3a4 4 0 005.600 5.600l1-1'/>"},
        {"nest", "<path fill='none' stroke='%C' stroke-width='2' d='M3 4h18v16H3z'/><path fill='%C' d='M8 9h8v6H8z'/>"},
        {"keyframe", "<path fill='%C' d='M12 3l9 9-9 9-9-9z'/>"},
        {"stopwatch", "<path fill='none' stroke='%C' stroke-width='2' d='M12 22a8 8 0 100-16 8 8 0 000 16zM12 10v4'/><path fill='%C' d='M9 2h6v2H9z'/>"},
        {"folder", "<path fill='%C' d='M2 5h7l2 2h11v13H2z'/>"},
        {"newbin", "<path fill='%C' d='M2 5h7l2 2h11v13H2zM11 10v3H8v2h3v3h2v-3h3v-2h-3v-3z' fill-rule='evenodd'/>"},
        {"list", "<path fill='%C' d='M3 5h3v3H3zM8 5h13v3H8zM3 10.500h3v3H3zM8 10.500h13v3H8zM3 16h3v3H3zM8 16h13v3H8z'/>"},
        {"grid", "<path fill='%C' d='M3 4h8v7H3zM13 4h8v7h-8zM3 13h8v7H3zM13 13h8v7h-8z'/>"},
        {"search", "<path fill='none' stroke='%C' stroke-width='2.400' d='M10.500 17a6.500 6.500 0 100-13 6.500 6.500 0 000 13zM15 15l6 6'/>"},
        {"trash", "<path fill='%C' d='M9 2h6l1 2h5v2H3V4h5zM5 8h14l-1 14H6z'/>"},
        {"home", "<path fill='%C' d='M12 3l10 9h-3v9h-5v-6h-4v6H5v-9H2z'/>"},
        {"newitem", "<path fill='%C' d='M5 2h10l5 5v15H5zM11 10v3H8v2h3v3h2v-3h3v-2h-3v-3z' fill-rule='evenodd'/>"},
        {"fx", "<path fill='%C' d='M4 20L8 4h6v2H10L9 10h4v2H8.500L6 20zM13 13h3l1.500 2.500L19 13h3l-3 4 3 4h-3l-1.500-2.500L16 21h-3l3-4z'/>"},
        {"reset", "<path fill='none' stroke='%C' stroke-width='2.200' d='M6 9a7 7 0 11-1 5'/><path fill='%C' d='M3 4v7h7z'/>"},
        {"close", "<path fill='%C' d='M6 4.500L12 10.500l6-6L19.500 6l-6 6 6 6-1.500 1.500-6-6-6 6L4.500 18l6-6-6-6z'/>"},
        {"maximize", "<path fill='none' stroke='%C' stroke-width='2' d='M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5'/>"},
        {"export", "<path fill='%C' d='M12 2l6 6h-4v7h-4V8H6zM3 13h3v6h12v-6h3v9H3z'/>"},
        {"import", "<path fill='%C' d='M12 16L6 10h4V2h4v8h4zM3 13h3v6h12v-6h3v9H3z'/>"},
        {"audio", "<path fill='%C' d='M3 9h4l5-5v16l-5-5H3zM15 8a5 5 0 010 8l-1-1.500a3 3 0 000-5z'/>"},
        // Tools
        {"t_select", "<path fill='%C' d='M6 2l13 12h-6l3.500 7-3 1.500-3.500-7L6 20z'/>"},
        {"t_track", "<path fill='%C' d='M3 4h2v16H3zM7 7h6V4l8 8-8 8v-3H7z'/>"},
        {"t_ripple", "<path fill='%C' d='M3 4h9v3H6v10h6v3H3zM14 7l7 5-7 5z'/>"},
        {"t_roll", "<path fill='%C' d='M11 4h2v16h-2zM9 7v10l-7-5zM15 7v10l7-5z'/>"},
        {"t_rate", "<path fill='%C' d='M2 5h2v14H2zM20 5h2v14h-2zM6 12l4-4v3h4V8l4 4-4 4v-3h-4v3z'/>"},
        {"t_razor", "<path fill='%C' d='M2 15L15 2l7 2-1 2L7 20H2zM14 16a3 3 0 106 0 3 3 0 00-6 0z'/>"},
        {"t_slip", "<path fill='%C' d='M2 4h2v16H2zM20 4h2v16h-2zM6 11h12V8l3 4-3 4v-3H6v3l-3-4 3-4z' transform='translate(0 0)'/>"},
        {"t_slide", "<path fill='%C' d='M8 4h8v16H8zM6 8v8l-4-4zM18 8v8l4-4z'/>"},
        {"t_pen", "<path fill='%C' d='M3 21l2-7L16 3l5 5L10 19zM5.500 18.500l2-.5-1.500-1.500z'/>"},
        {"t_hand", "<path fill='%C' d='M7 12V5a1.500 1.500 0 013 0v5h1V3.500a1.500 1.500 0 013 0V10h1V5a1.500 1.500 0 013 0v6h1V8a1.500 1.500 0 013 0v7a7 7 0 01-7 7h-2a7 7 0 01-6-3L3 14a1.500 1.500 0 012.500-1.500z'/>"},
        {"t_zoom", "<path fill='none' stroke='%C' stroke-width='2.400' d='M10.500 17a6.500 6.500 0 100-13 6.500 6.500 0 000 13zM15 15l6 6M7.500 10.500h6M10.500 7.500v6'/>"},
        {"t_text", "<path fill='%C' d='M4 3h16v5h-2.500V5.500H13.500v13H16V21H8v-2.500h2.500v-13H6.500V8H4z'/>"},
    };
    return m.value(n);
}

QPixmap pixmap(const QString &name, int size, const QColor &c)
{
    // Library icons ship as SVG resources; editor-specific glyphs with no stock equivalent are in svgBody.
    QByteArray svg;
    QFile f(":/icons/" + name + ".svg");
    if (f.open(QIODevice::ReadOnly)) {
        svg = f.readAll();
        svg.replace("currentColor", c.name().toLatin1());
        svg.replace("stroke-width=\"2\"", "stroke-width=\"1.75\"");
    } else {
        QString body = svgBody(name);
        body.replace("%C", c.name());
        svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>" + body.toUtf8() + "</svg>";
    }
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap px(QSize(size, size) * dpr);
    px.setDevicePixelRatio(dpr);
    px.fill(Qt::transparent);
    QSvgRenderer r(svg);
    QPainter p(&px);
    r.render(&p, QRectF(0, 0, size, size));
    return px;
}

QIcon icon(const QString &name, const QColor &c, const QColor &active)
{
    QIcon ic;
    for (int s : {16, 24, 32}) {
        ic.addPixmap(pixmap(name, s, c), QIcon::Normal, QIcon::Off);
        ic.addPixmap(pixmap(name, s, QColor("#555555")), QIcon::Disabled, QIcon::Off);
        ic.addPixmap(pixmap(name, s, active.isValid() ? active : accent), QIcon::Normal, QIcon::On);
    }
    return ic;
}

void apply(QApplication &app)
{
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, bgDark);
    p.setColor(QPalette::AlternateBase, bgLight);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, bgLight);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, bgDark);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, textDim);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor("#606060"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#606060"));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#606060"));
    app.setPalette(p);
    QFont f("Segoe UI");
    f.setPixelSize(12);
    app.setFont(f);
    app.setStyleSheet(R"(
QWidget { color: #c8c8c8; }
QToolTip { background: #1b1b1b; color: #e0e0e0; border: 1px solid #444; padding: 3px; }
QMainWindow, QDialog { background: #0d0d0d; }
QDialog { background: #232323; }
QMenuBar { background: #1b1b1b; padding: 1px 2px; }
QMenuBar::item { padding: 4px 9px; background: transparent; }
QMenuBar::item:selected { background: #3a3a3a; }
QMenu { background: #2a2a2a; border: 1px solid #111; padding: 4px 0; }
QMenu::item { padding: 4px 34px 4px 22px; }
QMenu::item:selected { background: #2d8ceb; color: white; }
QMenu::item:disabled { color: #666; }
QMenu::separator { height: 1px; background: #444; margin: 4px 0; }
QMenu::indicator { width: 12px; height: 12px; left: 6px; }
QSplitter::handle { background: #0d0d0d; }
QScrollBar:vertical { background: #1b1b1b; width: 11px; margin: 0; }
QScrollBar:horizontal { background: #1b1b1b; height: 11px; margin: 0; }
QScrollBar::handle { background: #4a4a4a; border-radius: 4px; min-height: 20px; min-width: 20px; margin: 2px; }
QScrollBar::handle:hover { background: #666; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit { background: #161616; border: 1px solid #3a3a3a; border-radius: 2px; padding: 2px 4px; selection-background-color: #2d8ceb; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus { border-color: #2d8ceb; }
QComboBox { background: #2e2e2e; border: 1px solid #3a3a3a; border-radius: 2px; padding: 2px 6px; min-height: 18px; }
QComboBox:hover { border-color: #5a5a5a; }
QComboBox::drop-down { border: 0; width: 18px; }
QComboBox::down-arrow { image: url(:/icons/ui_down.svg); width: 12px; height: 12px; }
QSpinBox::up-button, QDoubleSpinBox::up-button { border: 0; width: 16px; subcontrol-origin: border; subcontrol-position: top right; }
QSpinBox::down-button, QDoubleSpinBox::down-button { border: 0; width: 16px; subcontrol-origin: border; subcontrol-position: bottom right; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(:/icons/ui_up.svg); width: 10px; height: 10px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(:/icons/ui_down.svg); width: 10px; height: 10px; }
QComboBox QAbstractItemView { background: #2a2a2a; border: 1px solid #111; selection-background-color: #2d8ceb; outline: 0; }
QPushButton { background: #3a3a3a; border: 1px solid #444; border-radius: 3px; padding: 4px 14px; }
QPushButton:hover { background: #474747; }
QPushButton:pressed { background: #2a2a2a; }
QPushButton:disabled { color: #666; background: #2a2a2a; }
QPushButton[primary="true"] { background: #2d8ceb; border-color: #2d8ceb; color: white; font-weight: 600; }
QPushButton[primary="true"]:hover { background: #4aa0f5; }
QToolButton { background: transparent; border: 0; border-radius: 2px; padding: 2px; }
QToolButton:hover { background: #3a3a3a; }
QToolButton:pressed { background: #1b1b1b; }
QToolButton::menu-indicator { image: none; }
QTreeView, QListView, QTableView, QTreeWidget, QTableWidget { background: #1f1f1f; alternate-background-color: #232323; border: 0; outline: 0; selection-background-color: #2d6fb5; }
QTreeView::item, QTableView::item { padding: 2px 2px; }
QHeaderView::section { background: #2a2a2a; color: #aaa; border: 0; border-right: 1px solid #161616; border-bottom: 1px solid #161616; padding: 3px 6px; }
QTabBar::tab { background: #1b1b1b; padding: 4px 12px; border-right: 1px solid #0d0d0d; color: #999; }
QTabBar::tab:selected { background: #2e2e2e; color: #f0f0f0; }
QCheckBox::indicator, QGroupBox::indicator { width: 13px; height: 13px; background: #161616; border: 1px solid #555; border-radius: 2px; }
QCheckBox::indicator:checked, QGroupBox::indicator:checked { background: #2d8ceb; border-color: #2d8ceb; image: url(:/icons/ui_check.svg); }
QGroupBox { border: 1px solid #383838; border-radius: 3px; margin-top: 10px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; color: #aaa; }
QProgressBar { background: #161616; border: 0; border-radius: 2px; height: 6px; text-align: center; }
QProgressBar::chunk { background: #2d8ceb; border-radius: 2px; }
QSlider::groove:horizontal { height: 3px; background: #4a4a4a; }
QSlider::handle:horizontal { width: 10px; margin: -5px 0; background: #d0d0d0; border-radius: 5px; }
QSlider::groove:vertical { width: 3px; background: #4a4a4a; }
QSlider::handle:vertical { height: 10px; margin: 0 -7px; background: #d0d0d0; border-radius: 2px; }
QStatusBar { background: #1b1b1b; color: #999; }
QStatusBar::item { border: 0; }
)");
}
}
