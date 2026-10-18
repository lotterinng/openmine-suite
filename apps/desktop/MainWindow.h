#pragma once
// SPDX-License-Identifier: Apache-2.0
// The OpenMine Suite main window: ribbon workspaces (Home, Survey, CAD, Design, Geology,
// Schedule, Tools), menus, the viewport area, dockable panels (explorer, layers, properties,
// jobs, Python, command console, scheduler, 4D playback, stereonet, reports) and the status bar.
// Every action runs a real operation of the modules through validated, undoable edits or
// background jobs; actions that need something the build or the selection lacks are disabled and
// their tooltip says why.

#include "app/AppContext.h"

#include <QMainWindow>

#include <map>

class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QMenuBar;
class QToolBar;

namespace openmine::app {

class CommandConsole;
class JobsPanel;
class LayerManager;
class PlaybackPanel;
class ProjectExplorer;
class PropertiesPanel;
class PythonConsolePanel;
class ReportsPanel;
class Ribbon;
class SchedulerPanel;
class StereonetPanel;
class ViewportArea;
class Viewport3D;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    AppContext& context() { return ctx_; }
    ViewportArea* viewports() const { return viewports_; }
    Viewport3D* activeView() const;
    ProjectExplorer* explorer() const { return explorer_; }
    LayerManager* layers() const { return layers_; }
    PropertiesPanel* properties() const { return properties_; }
    CommandConsole* console() const { return console_; }
    PythonConsolePanel* python() const { return python_; }
    SchedulerPanel* scheduler() const { return scheduler_; }
    PlaybackPanel* playback() const { return playback_; }
    StereonetPanel* stereonet() const { return stereonet_; }
    ReportsPanel* reports() const { return reports_; }
    JobsPanel* jobsPanel() const { return jobs_; }
    Ribbon* ribbon() const { return ribbon_; }
    QDockWidget* dock(const QString& name) const;
    QToolBar* quickAccessBar() const { return quickAccess_; }
    /// Quick-access toolbar contents (action ids), persisted per user.
    void setQuickAccess(const QStringList& ids);
    /// Action by id ("file.save", "draw.polyline", "survey.icp", …).
    QAction* action(const QString& id) const;
    QStringList actionIds() const;

    /// No modal questions (automation, smoke test); file dialogs are never needed then.
    void setNonInteractive(bool on);
    bool nonInteractive() const { return nonInteractive_; }

    bool saveProjectTo(const QString& path);
    bool openProjectFrom(const QString& path);
    bool closeProject();
    /// Imports any supported file by extension (point clouds, DXF, OMF, meshes, STEP/IGES, GIS,
    /// DEM, CSV control points, plugin formats). Returns false when it cannot be imported.
    bool importFile(const QString& path);
    /// Exports by extension (DXF, OMF, OBJ/STL/PLY, STEP/IGES, SHP/GPKG/GeoJSON, TIF, LAS/LAZ/E57/
    /// PLY/XYZ/PTS point clouds, CSV/XLSX/PDF schedule).
    bool exportFile(const QString& path);
    /// Context-menu and explorer actions on entities ("zoom", "report-cloud", "register-icp", …).
    void handleEntityAction(const QString& action, const std::vector<SelectionItem>& items);

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
    void setTheme(bool dark);
    void showAbout();
    void projectCheck();
    void loadPlugins();
    void rebuildPluginMenu();
    void rebuildRecentMenu();
    void rebuildQuickAccess();
    void resetLayout();
    QAction* addAction(const QString& id, const QString& text, const QString& icon, std::function<void()> slot, const QKeySequence& key = {},
                       const QString& tip = {});
    QString chooseOpenFile(const QString& title, const QString& filter, const QString& key);
    QString chooseSaveFile(const QString& title, const QString& filter, const QString& key, const QString& suggestion = {});
    Uuid primaryOf(EntityKind kind) const;
    void requireSelection(EntityKind kind, const QString& what, const std::function<void(const Uuid&)>& f);
    void startTool(int tool);
    void showLayoutPreview(const std::vector<std::vector<Vec3>>& lines);

    AppContext ctx_;
    ViewportArea* viewports_ = nullptr;
    ProjectExplorer* explorer_ = nullptr;
    LayerManager* layers_ = nullptr;
    PropertiesPanel* properties_ = nullptr;
    JobsPanel* jobs_ = nullptr;
    PythonConsolePanel* python_ = nullptr;
    CommandConsole* console_ = nullptr;
    SchedulerPanel* scheduler_ = nullptr;
    PlaybackPanel* playback_ = nullptr;
    StereonetPanel* stereonet_ = nullptr;
    ReportsPanel* reports_ = nullptr;
    Ribbon* ribbon_ = nullptr;
    std::map<QString, QDockWidget*> docks_;
    std::map<QString, QAction*> actions_;
    QMenuBar* menuBar_ = nullptr;
    QToolBar* quickAccess_ = nullptr;
    QByteArray defaultState_;
    QMenu* pluginMenu_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QLabel* coordLabel_ = nullptr;
    QLabel* snapLabel_ = nullptr;
    QLabel* promptLabel_ = nullptr;
    QLabel* crsLabel_ = nullptr;
    QLabel* scenarioLabel_ = nullptr;
    QLabel* pointsLabel_ = nullptr;
    QLabel* jobsLabel_ = nullptr;
    bool nonInteractive_ = false;
};

} // namespace openmine::app
