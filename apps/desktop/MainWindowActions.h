#pragma once
// SPDX-License-Identifier: Apache-2.0
// Internal: the QAction set shared by MainWindow.cpp and console/StandardCommands.cpp.
#include "MainWindow.h"

#include <QAction>

namespace openmine::app {

struct MainWindow::Actions {
    QAction *newProject, *open, *save, *saveAs, *close, *exit;
    QAction *undo, *redo, *del;
    QAction *select, *line, *polyline, *measure, *gridSnap, *endSnap, *workPlane, *newLayer;
    QAction *fit, *top, *front, *side, *iso, *perspective, *dark, *light;
    QAction *heading, *linkedActivity, *impacts;
    QAction *task, *milestone, *summary, *dependency, *calc, *baseline, *applyDesign, *exportCsv;
    QAction *about, *commands;
};

} // namespace openmine::app
