#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <QString>

namespace openmine::app {
class MainWindow;
/// Returns 0 when every check passed.
int runSmokeTest(MainWindow& w, const QString& outDir);
} // namespace openmine::app
