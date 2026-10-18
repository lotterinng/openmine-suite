#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original OpenMine visual identity: dark engineering workspace (default) and a light theme.

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace openmine::app {

enum class ThemeKind { Dark, Light };

struct ThemeColors {
    QColor window, panel, panelAlt, text, textMuted, accent, accentText, border;
    QColor viewportTop, viewportBottom, grid, selection;
    QColor ganttTask, ganttCritical, ganttSummary, ganttBaseline, ganttProgress, ganttGrid, ganttWeekend, warning;
};

class Theme {
public:
    static void apply(QApplication& app, ThemeKind kind);
    static ThemeKind current();
    static const ThemeColors& colors();
};

/// Original, programmatically drawn tool icons (no third-party artwork).
QIcon toolIcon(const QString& name);
/// True when an icon drawing exists for `name` (otherwise toolIcon draws its initials).
bool hasToolIcon(const QString& name);

} // namespace openmine::app
