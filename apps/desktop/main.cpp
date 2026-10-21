// SPDX-License-Identifier: Apache-2.0
#include "MainWindow.h"
#include "SmokeTest.h"
#include "app/AppContext.h"
#include "console/CommandConsole.h"
#include "theme/Theme.h"
#include "vtk/QtVtkWidget.h"

#include "openmine/persistence/ProjectFile.h"

#ifdef OPENMINE_APP_HAVE_PDAL
#include "openmine/pdalio/Pdal.h"
#endif
#ifdef OPENMINE_APP_HAVE_OPEN3D
#include "openmine/o3d/Open3d.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>

#include <cstdio>

int main(int argc, char* argv[]) {
    // The VTK views need a desktop OpenGL 3.2 core context: set it before QApplication exists.
    QSurfaceFormat::setDefaultFormat(openmine::app::QtVtkWidget::defaultFormat());
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    QApplication::setApplicationName("OpenMine Suite");
    QApplication::setOrganizationName("OpenMine");
    QApplication::setApplicationVersion(QString::fromLatin1(openmine::persistence::kAppVersion).section(' ', -1));

    QCommandLineParser cli;
    cli.setApplicationDescription("OpenMine Suite — integrated mine survey, design and scheduling");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption smoke("smoke-test", "Run the end-to-end GUI smoke test, write the log and screenshots to <dir>, exit with the number of failures.", "dir");
    QCommandLineOption light("light", "Start with the light theme.");
    QCommandLineOption fresh("reset-layout", "Ignore the saved window layout.");
    QCommandLineOption reference("command-reference", "Write the console command reference (Markdown) to <file> and exit.", "file");
    cli.addOption(smoke);
    cli.addOption(reference);
    cli.addOption(light);
    cli.addOption(fresh);
    cli.addPositionalArgument("file", "Project (.openmine) to open, or data files to import", "[file…]");
    cli.process(app);

#ifdef OPENMINE_APP_HAVE_PDAL
    // LAS/LAZ/E57/PLY point-cloud readers and writers.
    openmine::pdalio::registerFormats();
#endif
#ifdef OPENMINE_APP_HAVE_OPEN3D
    // Open3D ICP as an alternative registration engine (the other Open3D engines are chosen in the dialogs).
    openmine::o3d::registerEngines();
#endif

    if (cli.isSet(smoke) || cli.isSet(fresh)) {
        // Deterministic layout and settings for the test (and on request).
        QSettings s;
        s.remove("window");
        if (cli.isSet(smoke)) s.clear();
    }
    const QString saved = QSettings().value("ui/theme").toString();
    const bool useLight = cli.isSet(light) || (!cli.isSet(smoke) && saved == "light");
    openmine::app::Theme::apply(app, useLight ? openmine::app::ThemeKind::Light : openmine::app::ThemeKind::Dark);

    openmine::app::MainWindow w;
    if (cli.isSet(reference)) {
        QFile f(cli.value(reference));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(cli.value(reference)));
            return 1;
        }
        f.write(w.console()->referenceMarkdown().toUtf8());
        return 0;
    }
    w.show();

    if (cli.isSet(smoke)) {
        int failures = 1;
        QTimer::singleShot(300, &w, [&] {
            failures = openmine::app::runSmokeTest(w, cli.value(smoke));
            std::fprintf(stderr, "smoke test: %d failure(s); log in %s/smoke.log\n", failures, qPrintable(cli.value(smoke)));
            w.setNonInteractive(true);
            w.context().jobs().cancelAll();
            w.close();
            app.exit(failures);
        });
        app.exec();
        return failures > 125 ? 125 : failures;
    }

    const QStringList files = cli.positionalArguments();
    bool opened = false;
    for (const auto& f : files) {
        if (f.endsWith(".openmine", Qt::CaseInsensitive) && !opened) opened = w.openProjectFrom(f);
        else w.importFile(f);
    }
    if (!w.context().hasProject()) w.context().newProject(QObject::tr("Untitled mine project"), openmine::Date::todayUtc());
    return app.exec();
}
