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

} // namespace openmine::app
