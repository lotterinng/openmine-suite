// SPDX-License-Identifier: Apache-2.0
#include "MainWindow.h"
#include "SmokeTest.h"
#include "theme/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

int main(int argc, char* argv[]) {
    // Required by VTK's Qt integration: set the default OpenGL format before QApplication.
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    QApplication::setApplicationName("OpenMine Suite");
    QApplication::setOrganizationName("OpenMine");
    QApplication::setApplicationVersion("0.1.0");

    QCommandLineParser cli;
    cli.setApplicationDescription("OpenMine Suite — integrated mine survey, design and scheduling");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption smoke("smoke-test", "Run the GUI smoke test, write screenshots/log to <dir>, then exit.", "dir");
    QCommandLineOption light("light", "Start with the light theme.");
    cli.addOption(smoke);
    cli.addOption(light);
    cli.addPositionalArgument("project", "Project file to open (.openmine)", "[project]");
    cli.process(app);

    openmine::app::Theme::apply(app, cli.isSet(light) ? openmine::app::ThemeKind::Light : openmine::app::ThemeKind::Dark);
    openmine::app::MainWindow w;
    w.show();

    if (cli.isSet(smoke)) {
        int rc = 1;
        QTimer::singleShot(200, &w, [&] {
            rc = openmine::app::runSmokeTest(w, cli.value(smoke));
            app.exit(rc);
        });
        app.exec();
        return rc;
    }
    if (!cli.positionalArguments().isEmpty()) w.openProjectFrom(cli.positionalArguments().front());
    else w.context().newProject("Untitled Mine Project", openmine::Date::todayUtc());
    return app.exec();
}
