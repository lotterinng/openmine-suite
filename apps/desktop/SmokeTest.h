#pragma once
// SPDX-License-Identifier: Apache-2.0
// End-to-end GUI smoke test: drives survey, CAD, design, geology, geotechnical and scheduling
// workflows through the real widgets, dialogs, console and viewports on generated data, saves,
// reopens and verifies persistence, and writes a log and screenshots.
//   openmine-desktop --smoke-test <output-dir>
#include <QString>

namespace openmine::app {
class MainWindow;
/// Returns the number of failed checks (0 = every check passed).
int runSmokeTest(MainWindow& w, const QString& outDir);
} // namespace openmine::app
