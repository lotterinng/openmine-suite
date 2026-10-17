#pragma once
// SPDX-License-Identifier: Apache-2.0

#include "AppContext.h"

#include <QMainWindow>

#include <vector>

class QAction;
class QLabel;
class QDockWidget;

namespace openmine::app {

class Ribbon;
class Viewport3D;
class ProjectExplorer;
class PropertiesPanel;
class CommandConsole;
class SchedulerPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    AppContext& context() { return ctx_; }
    Viewport3D* viewport() const { return viewport_; }
    ProjectExplorer* explorer() const { return explorer_; }
    PropertiesPanel* properties() const { return properties_; }
    CommandConsole* console() const { return console_; }
    SchedulerPanel* scheduler() const { return scheduler_; }
    Ribbon* ribbon() const { return ribbon_; }
    QDockWidget* schedulerDock() const { return schedulerDock_; }

    /// When true, no modal "save changes?" prompts are shown (automation / smoke test).
    void setNonInteractive(bool on) { nonInteractive_ = on; }

    bool saveProjectTo(const QString& path);
    bool openProjectFrom(const QString& path);
    bool closeProject();

    /// Turn the selected centreline into a development heading (UI and console share this).
    bool createHeadingFromSelection(const QString& headingId, double width, double height, const QString& level);
    bool createLinkedActivityForSelection(double ratePerDay);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void createActions();
    void createMenus();
    void createRibbon();
    void createDocks();
    void createStatusBar();
    void registerCommands();
    void updateTitle();
    void updateActions();
    bool maybeSave();
    void onDrawingFinished(const std::vector<Vec3>& pts, bool isLine);
    void createHeadingInteractive();
    void createLinkedActivityInteractive();
    void setTheme(bool dark);

    AppContext ctx_;
    Ribbon* ribbon_ = nullptr;
    Viewport3D* viewport_ = nullptr;
    ProjectExplorer* explorer_ = nullptr;
    PropertiesPanel* properties_ = nullptr;
    CommandConsole* console_ = nullptr;
    SchedulerPanel* scheduler_ = nullptr;
    QDockWidget* schedulerDock_ = nullptr;
    QLabel* coordLabel_ = nullptr;
    QLabel* crsLabel_ = nullptr;
    QLabel* snapLabel_ = nullptr;
    QLabel* toolLabel_ = nullptr;
    bool nonInteractive_ = false;
    bool measuring_ = false;

    struct Actions;
    Actions* a_ = nullptr;
};

} // namespace openmine::app
