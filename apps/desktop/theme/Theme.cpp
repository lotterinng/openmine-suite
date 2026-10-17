// SPDX-License-Identifier: Apache-2.0
#include "Theme.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>

#include <array>
#include <cmath>

namespace openmine::app {

namespace {
ThemeKind g_kind = ThemeKind::Dark;

ThemeColors darkColors() {
    ThemeColors c;
    c.window = QColor(0x1e, 0x21, 0x26);
    c.panel = QColor(0x26, 0x2a, 0x30);
    c.panelAlt = QColor(0x2d, 0x32, 0x39);
    c.text = QColor(0xe3, 0xe6, 0xea);
    c.textMuted = QColor(0x8c, 0x94, 0x9e);
    c.accent = QColor(0xf2, 0xa9, 0x00); // safety amber
    c.accentText = QColor(0x15, 0x17, 0x1a);
    c.border = QColor(0x3a, 0x40, 0x48);
    c.viewportTop = QColor(0x2b, 0x31, 0x3a);
    c.viewportBottom = QColor(0x10, 0x12, 0x15);
    c.grid = QColor(0x4a, 0x52, 0x5c);
    c.selection = QColor(0xff, 0xe0, 0x4a);
    c.ganttTask = QColor(0x3f, 0xa9, 0xf5);
    c.ganttCritical = QColor(0xe5, 0x48, 0x4d);
    c.ganttSummary = QColor(0xc8, 0xcc, 0xd2);
    c.ganttBaseline = QColor(0x7a, 0x82, 0x8c);
    c.ganttProgress = QColor(0x2e, 0xc2, 0x7e);
    c.ganttGrid = QColor(0x33, 0x38, 0x3f);
    c.ganttWeekend = QColor(0x22, 0x25, 0x2a);
    c.warning = QColor(0xff, 0x9f, 0x1a);
    return c;
}

ThemeColors lightColors() {
    ThemeColors c;
    c.window = QColor(0xf3, 0xf4, 0xf6);
    c.panel = QColor(0xff, 0xff, 0xff);
    c.panelAlt = QColor(0xf7, 0xf8, 0xfa);
    c.text = QColor(0x1d, 0x22, 0x28);
    c.textMuted = QColor(0x6b, 0x73, 0x7c);
    c.accent = QColor(0xc9, 0x7a, 0x00);
    c.accentText = QColor(0xff, 0xff, 0xff);
    c.border = QColor(0xcf, 0xd4, 0xda);
    c.viewportTop = QColor(0xe9, 0xed, 0xf2);
    c.viewportBottom = QColor(0xc4, 0xcb, 0xd4);
    c.grid = QColor(0x9a, 0xa3, 0xad);
    c.selection = QColor(0xd9, 0x6b, 0x00);
    c.ganttTask = QColor(0x1f, 0x7a, 0xc7);
    c.ganttCritical = QColor(0xcc, 0x2f, 0x35);
    c.ganttSummary = QColor(0x3a, 0x40, 0x48);
    c.ganttBaseline = QColor(0xa0, 0xa7, 0xb0);
    c.ganttProgress = QColor(0x1d, 0x9a, 0x5f);
    c.ganttGrid = QColor(0xe3, 0xe6, 0xea);
    c.ganttWeekend = QColor(0xf0, 0xf1, 0xf4);
    c.warning = QColor(0xd9, 0x7a, 0x00);
    return c;
}

ThemeColors g_colors = darkColors();
} // namespace

ThemeKind Theme::current() { return g_kind; }
const ThemeColors& Theme::colors() { return g_colors; }

void Theme::apply(QApplication& app, ThemeKind kind) {
    g_kind = kind;
    g_colors = kind == ThemeKind::Dark ? darkColors() : lightColors();
    const auto& c = g_colors;
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.panel);
    p.setColor(QPalette::AlternateBase, c.panelAlt);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.panelAlt);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::ToolTipBase, c.panelAlt);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, c.accentText);
    p.setColor(QPalette::PlaceholderText, c.textMuted);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::Disabled, QPalette::Text, c.textMuted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.textMuted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.textMuted);
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow::separator { background: %1; width: 3px; height: 3px; }
        QDockWidget { titlebar-close-icon: none; }
        QDockWidget::title { background: %2; padding: 5px 8px; border-bottom: 1px solid %1; }
        QTabWidget#Ribbon::pane { border: none; border-bottom: 1px solid %1; background: %2; }
        QTabWidget#Ribbon > QTabBar::tab { padding: 6px 14px; background: %3; color: %4; border: none; }
        QTabWidget#Ribbon > QTabBar::tab:selected { background: %2; color: %5; border-top: 2px solid %6; }
        QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 2px; }
        QToolButton:hover:enabled { border: 1px solid %1; background: %3; }
        QToolButton:checked { background: %6; color: %7; }
        QFrame#RibbonGroup { border-right: 1px solid %1; }
        QLabel#RibbonGroupTitle { color: %4; font-size: 8pt; }
        QStatusBar { background: %2; border-top: 1px solid %1; }
        QHeaderView::section { background: %3; color: %5; border: none; border-right: 1px solid %1; border-bottom: 1px solid %1; padding: 3px 6px; }
        QTreeView, QTableView, QPlainTextEdit, QLineEdit { border: 1px solid %1; }
        QLineEdit#ConsoleInput { font-family: monospace; }
        QPlainTextEdit#ConsoleOutput { font-family: monospace; }
    )")
                          .arg(c.border.name(), c.panel.name(), c.window.name(), c.textMuted.name(), c.text.name(),
                               c.accent.name(), c.accentText.name()));
}

QIcon toolIcon(const QString& name) {
    const auto& c = Theme::colors();
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QPen ink(c.text, 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    const QPen accent(c.accent, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    auto dot = [&](QPointF pt, QColor col) {
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawEllipse(pt, 5.5, 5.5);
    };
    p.setBrush(Qt::NoBrush);

    if (name == "line") {
        p.setPen(ink);
        p.drawLine(12, 52, 52, 12);
        dot({12, 52}, c.accent);
        dot({52, 12}, c.accent);
    } else if (name == "polyline") {
        p.setPen(ink);
        QPolygonF poly{{8, 50}, {22, 20}, {40, 40}, {56, 12}};
        p.drawPolyline(poly);
        for (auto pt : poly) dot(pt, c.accent);
    } else if (name == "select") {
        QPainterPath path;
        path.moveTo(16, 8);
        path.lineTo(16, 52);
        path.lineTo(27, 41);
        path.lineTo(36, 58);
        path.lineTo(43, 55);
        path.lineTo(34, 38);
        path.lineTo(49, 38);
        path.closeSubpath();
        p.setPen(QPen(c.text, 3));
        p.setBrush(c.panelAlt);
        p.drawPath(path);
    } else if (name == "heading") {
        p.setPen(QPen(c.text, 4));
        QPainterPath arch;
        arch.moveTo(12, 56);
        arch.lineTo(12, 30);
        arch.arcTo(12, 10, 40, 40, 180, -180);
        arch.lineTo(52, 56);
        p.drawPath(arch);
        p.setPen(QPen(c.accent, 4, Qt::DashLine));
        p.drawLine(32, 56, 32, 24);
    } else if (name == "activity" || name == "gantt") {
        p.setPen(Qt::NoPen);
        p.setBrush(c.ganttTask);
        p.drawRoundedRect(QRectF(8, 12, 30, 10), 2, 2);
        p.setBrush(c.ganttCritical);
        p.drawRoundedRect(QRectF(24, 28, 30, 10), 2, 2);
        p.setBrush(c.ganttTask);
        p.drawRoundedRect(QRectF(16, 44, 22, 10), 2, 2);
        if (name == "activity") {
            p.setPen(accent);
            p.drawLine(50, 46, 50, 60);
            p.drawLine(43, 53, 57, 53);
        }
    } else if (name == "link") {
        p.setPen(QPen(c.text, 5));
        p.drawRoundedRect(QRectF(6, 22, 28, 18), 9, 9);
        p.setPen(QPen(c.accent, 5));
        p.drawRoundedRect(QRectF(30, 24, 28, 18), 9, 9);
    } else if (name == "calc") {
        p.setPen(ink);
        p.drawEllipse(QPointF(32, 32), 22, 22);
        p.setPen(accent);
        QPolygonF play{{26, 20}, {46, 32}, {26, 44}};
        p.setBrush(c.accent);
        p.drawPolygon(play);
    } else if (name == "baseline") {
        p.setPen(Qt::NoPen);
        p.setBrush(c.ganttTask);
        p.drawRect(QRectF(8, 18, 44, 12));
        p.setBrush(c.ganttBaseline);
        p.drawRect(QRectF(8, 36, 36, 6));
        p.setPen(accent);
        p.drawLine(52, 40, 58, 52);
    } else if (name == "fit") {
        p.setPen(ink);
        for (auto [x, y, dx, dy] : {std::array<int, 4>{8, 8, 1, 1}, {56, 8, -1, 1}, {8, 56, 1, -1}, {56, 56, -1, -1}}) {
            p.drawLine(x, y, x + dx * 14, y);
            p.drawLine(x, y, x, y + dy * 14);
        }
        p.setPen(accent);
        p.drawRect(QRectF(22, 22, 20, 20));
    } else if (name == "top" || name == "front" || name == "side" || name == "iso") {
        p.setPen(QPen(c.text, 3.5));
        QPolygonF top{{32, 8}, {56, 20}, {32, 32}, {8, 20}};
        QPolygonF left{{8, 20}, {32, 32}, {32, 58}, {8, 46}};
        QPolygonF right{{32, 32}, {56, 20}, {56, 46}, {32, 58}};
        auto face = [&](const QPolygonF& f, bool hi) {
            p.setBrush(hi ? c.accent : c.panelAlt);
            p.drawPolygon(f);
        };
        face(top, name == "top" || name == "iso");
        face(left, name == "front" || name == "iso");
        face(right, name == "side" || name == "iso");
    } else if (name == "distance") {
        p.setPen(ink);
        p.drawLine(8, 40, 56, 40);
        p.drawLine(8, 30, 8, 50);
        p.drawLine(56, 30, 56, 50);
        p.setPen(accent);
        p.drawLine(14, 22, 50, 22);
        p.drawLine(14, 22, 20, 16);
        p.drawLine(14, 22, 20, 28);
        p.drawLine(50, 22, 44, 16);
        p.drawLine(50, 22, 44, 28);
    } else if (name == "delete") {
        p.setPen(QPen(c.ganttCritical, 6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(16, 16, 48, 48);
        p.drawLine(48, 16, 16, 48);
    } else if (name == "undo" || name == "redo") {
        p.save();
        if (name == "redo") {
            p.translate(64, 0);
            p.scale(-1, 1);
        }
        p.setPen(ink);
        QPainterPath path;
        path.moveTo(16, 26);
        path.cubicTo(30, 12, 56, 16, 54, 40);
        path.cubicTo(52, 52, 40, 54, 32, 54);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        p.drawPolygon(QPolygonF{{6, 18}, {26, 14}, {18, 34}});
        p.restore();
    } else if (name == "new" || name == "open" || name == "save") {
        p.setPen(QPen(c.text, 3.5));
        p.setBrush(c.panelAlt);
        if (name == "open") {
            p.drawPolygon(QPolygonF{{6, 18}, {24, 18}, {28, 24}, {58, 24}, {58, 54}, {6, 54}});
            p.setBrush(c.accent);
            p.drawPolygon(QPolygonF{{14, 32}, {62, 32}, {54, 54}, {6, 54}});
        } else if (name == "save") {
            p.drawRoundedRect(QRectF(8, 8, 48, 48), 4, 4);
            p.setBrush(c.accent);
            p.drawRect(QRectF(18, 8, 26, 16));
            p.setBrush(c.panel);
            p.drawRect(QRectF(16, 34, 32, 22));
        } else {
            p.drawPolygon(QPolygonF{{12, 6}, {40, 6}, {52, 18}, {52, 58}, {12, 58}});
            p.setPen(accent);
            p.drawLine(32, 26, 32, 46);
            p.drawLine(22, 36, 42, 36);
        }
    } else if (name == "theme") {
        p.setPen(Qt::NoPen);
        p.setBrush(c.text);
        p.drawEllipse(QPointF(32, 32), 22, 22);
        p.setBrush(c.accent);
        p.drawPie(QRectF(10, 10, 44, 44), 90 * 16, 180 * 16);
    } else if (name == "move") {
        p.setPen(ink);
        p.drawLine(32, 8, 32, 56);
        p.drawLine(8, 32, 56, 32);
        p.setPen(accent);
        for (auto [a, b, cc, d] : {std::array<int, 4>{32, 8, 26, 14}, {32, 8, 38, 14}, {56, 32, 50, 26}, {56, 32, 50, 38}})
            p.drawLine(a, b, cc, d);
    } else {
        // Generic placeholder glyph for tools that are planned but not yet available.
        p.setPen(QPen(c.textMuted, 3, Qt::DashLine));
        p.drawRoundedRect(QRectF(10, 10, 44, 44), 8, 8);
        QFont f = p.font();
        f.setPixelSize(22);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.textMuted);
        p.drawText(QRectF(10, 10, 44, 44), Qt::AlignCenter, name.left(2).toUpper());
    }
    p.end();
    return QIcon(pm);
}

} // namespace openmine::app
