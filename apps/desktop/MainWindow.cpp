// SPDX-License-Identifier: Apache-2.0
#include "MainWindow.h"

#include "app/EntityInfo.h"
#include "console/CommandConsole.h"
#include "dialogs/DataDialogs.h"
#include "dialogs/DesignDialogs.h"
#include "dialogs/DialogUtil.h"
#include "dialogs/GeologyDialogs.h"
#include "dialogs/MeshDialogs.h"
#include "dialogs/ModelDialogs.h"
#include "dialogs/SettingsDialogs.h"
#include "dialogs/SurveyDialogs.h"
#include "panels/JobsPanel.h"
#include "panels/LayerManager.h"
#include "panels/PlaybackPanel.h"
#include "panels/ProjectExplorer.h"
#include "panels/PropertiesPanel.h"
#include "panels/PythonConsolePanel.h"
#include "panels/ReportsPanel.h"
#include "panels/StereonetPanel.h"
#include "ribbon/Ribbon.h"
#include "scheduler/GanttChart.h"
#include "scheduler/ScheduleDialogs.h"
#include "scheduler/SchedulerPanel.h"
#include "theme/Theme.h"
#include "viewport/ViewportArea.h"
#include "vtk/QtVtkWidget.h"

#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"

#include "openmine/persistence/EntityCodec.h"
#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"

#ifdef OPENMINE_APP_HAVE_REPOSITORY
#include "openmine/repository/PgRepository.h"
#endif

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>

#include <set>

namespace openmine::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowIcon(toolIcon("iso"));
    resize(1600, 980);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);

    viewports_ = new ViewportArea(ctx_, this);
    setCentralWidget(viewports_);
    createDocks();
    createActions();
    createMenus();
    createRibbon();
    createStatusBar();
    registerCommands();

    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
        updateTitle();
        updateActions();
    });
    connect(&ctx_, &AppContext::undoStackChanged, this, [this] {
        updateTitle();
        updateActions();
    });
    connect(&ctx_, &AppContext::selectionChanged, this, &MainWindow::updateActions);
    connect(&ctx_, &AppContext::message, this, [this](const QString& t, int level) {
        statusBar()->showMessage(t, level == static_cast<int>(MessageLevel::Error) ? 12000 : 6000);
    });
    connect(&ctx_, &AppContext::scenarioChanged, this, [this] {
        const auto* s = ctx_.activeScenario();
        scenarioLabel_->setText(s ? tr("Scenario: %1").arg(qstr(s->name)) : tr("Base schedule"));
    });
    connect(explorer_, &ProjectExplorer::actionRequested, this, &MainWindow::handleEntityAction);
    connect(viewports_, &ViewportArea::contextMenuRequested, this, [this](QPoint global, std::optional<PickHit> hit) {
        QMenu m(this);
        if (hit) {
            if (!ctx_.isSelected(hit->owner.kind, hit->owner.id)) ctx_.select(hit->owner.kind, hit->owner.id);
            m.addSection(describe(ctx_.project().model(), hit->owner));
            m.addAction(toolIcon("fit"), tr("Zoom to"), this, [this] { handleEntityAction("zoom", ctx_.selection()); });
            m.addAction(toolIcon("properties"), tr("Properties"), this, [this] { handleEntityAction("properties", ctx_.selection()); });
            if (hit->owner.kind == EntityKind::Development) {
                m.addAction(toolIcon("section"), tr("Section here"), this, [this, hit] {
                    const auto* d = ctx_.project().model().developments.find(hit->owner.id);
                    if (!d) return;
                    const auto cl = mine_design::centrelinePoints(ctx_.project(), *d);
                    if (auto pr = geometry::projectOntoPolyline(hit->world, cl)) {
                        const double ch = d->chainageFrom + pr->chainage;
                        for (auto* v : viewports_->views())
                            if (v != viewports_->active() && v->mode() == ViewMode::Section) {
                                v->sectionAtChainage(d->id, ch);
                                return;
                            }
                        viewports_->setLayoutKind(viewports_->layoutKind() == ViewportArea::Layout::Single ? ViewportArea::Layout::TwoColumns : viewports_->layoutKind());
                        for (auto* v : viewports_->views())
                            if (v != viewports_->active()) {
                                v->sectionAtChainage(d->id, ch);
                                return;
                            }
                    }
                });
            }
            if (hit->onPointCloud)
                m.addAction(tr("Copy point (project) %1, %2, %3").arg(hit->world.x, 0, 'f', 3).arg(hit->world.y, 0, 'f', 3).arg(hit->world.z, 0, 'f', 3),
                            this, [this, hit] { ctx_.report(tr("Point %1, %2, %3 (stored %4, %5, %6)").arg(hit->world.x, 0, 'f', 4).arg(hit->world.y, 0, 'f', 4).arg(hit->world.z, 0, 'f', 4).arg(hit->stored.x, 0, 'f', 4).arg(hit->stored.y, 0, 'f', 4).arg(hit->stored.z, 0, 'f', 4)); });
            m.addSeparator();
            m.addAction(toolIcon("delete"), tr("Delete"), this, [this] { action("edit.delete")->trigger(); });
        } else {
            m.addAction(toolIcon("fit"), tr("Zoom extents"), this, [this] { activeView()->fitAll(); });
            m.addAction(toolIcon("plan"), tr("Plan view"), this, [this] { activeView()->setNamedView("top"); });
            m.addAction(toolIcon("iso"), tr("3D view"), this, [this] { activeView()->setNamedView("iso"); });
        }
        m.exec(global);
    });
    connect(&viewports_->tools(), &ToolController::polygonDefined, this, [this](std::vector<Vec3> poly, Vec3 n) {
        const auto clouds = ctx_.selected(EntityKind::PointCloud);
        if (clouds.empty()) {
            ctx_.report(tr("Select the point cloud to crop, then draw the polygon"), MessageLevel::Warning);
            return;
        }
        for (const auto& id : clouds) startCropJob(ctx_, id, poly, n, true);
    });

    QSettings& s = ctx_.settings();
    defaultState_ = saveState();
    PreferencesDialog::applyStored(s, *viewports_);
    restoreGeometry(s.value("window/geometry").toByteArray());
    if (!restoreState(s.value("window/state").toByteArray())) {
        // First start: give the viewport most of the window once the layout has its real size.
        QTimer::singleShot(0, this, [this] {
            resizeDocks({dock("explorer"), dock("properties")}, {290, 380}, Qt::Horizontal);
            resizeDocks({dock("scheduler")}, {height() / 4}, Qt::Vertical);
        });
    }
    updateTitle();
    updateActions();
    QTimer::singleShot(0, this, &MainWindow::loadPlugins);
    connect(&ctx_, &AppContext::pluginRescanRequested, this, &MainWindow::loadPlugins);
}

MainWindow::~MainWindow() = default;

Viewport3D* MainWindow::activeView() const { return viewports_->active(); }

QDockWidget* MainWindow::dock(const QString& name) const {
    auto it = docks_.find(name);
    return it == docks_.end() ? nullptr : it->second;
}

QAction* MainWindow::action(const QString& id) const {
    auto it = actions_.find(id);
    return it == actions_.end() ? nullptr : it->second;
}

QStringList MainWindow::actionIds() const {
    QStringList l;
    for (const auto& [k, v] : actions_) l << k;
    return l;
}

void MainWindow::setNonInteractive(bool on) {
    nonInteractive_ = on;
    explorer_->setProperty("nonInteractive", on);
}

// ---- docks -----------------------------------------------------------------------------------------

void MainWindow::createDocks() {
    auto add = [this](const QString& name, const QString& title, QWidget* w, Qt::DockWidgetArea area) {
        auto* d = new QDockWidget(title, this);
        d->setObjectName(name);
        d->setWidget(w);
        addDockWidget(area, d);
        docks_[name] = d;
        return d;
    };
    explorer_ = new ProjectExplorer(ctx_, viewports_->scene(), this);
    layers_ = new LayerManager(ctx_, this);
    properties_ = new PropertiesPanel(ctx_, this);
    jobs_ = new JobsPanel(ctx_, this);
    python_ = new PythonConsolePanel(ctx_, this);
    console_ = new CommandConsole(ctx_, this);
    console_->setToolController(&viewports_->tools());
    scheduler_ = new SchedulerPanel(ctx_, this);
    playback_ = new PlaybackPanel(ctx_, this);
    playback_->setFrameGrabber([this] { return activeView() ? activeView()->grab() : QImage(); });
    connect(playback_, &PlaybackPanel::ghostFutureChanged, this, [this](bool on) {
        viewports_->forEach([on](Viewport3D& v) {
            v.options().ghostFuture = on;
            v.applyOptions();
        });
    });
    stereonet_ = new StereonetPanel(ctx_, this);
    reports_ = new ReportsPanel(ctx_, this);

    auto* ex = add("explorer", tr("Project"), explorer_, Qt::LeftDockWidgetArea);
    auto* ly = add("layers", tr("Layers"), layers_, Qt::LeftDockWidgetArea);
    tabifyDockWidget(ex, ly);
    ex->raise();
    add("properties", tr("Properties"), properties_, Qt::RightDockWidgetArea);
    auto* sc = add("scheduler", tr("Scheduler"), scheduler_, Qt::BottomDockWidgetArea);
    auto* cs = add("console", tr("Command console"), console_, Qt::BottomDockWidgetArea);
    auto* py = add("python", tr("Python"), python_, Qt::BottomDockWidgetArea);
    auto* jb = add("jobs", tr("Jobs"), jobs_, Qt::BottomDockWidgetArea);
    auto* pb = add("playback", tr("4D playback"), playback_, Qt::BottomDockWidgetArea);
    tabifyDockWidget(sc, cs);
    tabifyDockWidget(cs, py);
    tabifyDockWidget(py, jb);
    tabifyDockWidget(jb, pb);
    cs->raise();
    auto* st = add("stereonet", tr("Stereonet"), stereonet_, Qt::RightDockWidgetArea);
    auto* rp = add("reports", tr("Reports"), reports_, Qt::RightDockWidgetArea);
    // Right side: one tabbed column, so the viewport keeps the width.
    tabifyDockWidget(dock("properties"), st);
    tabifyDockWidget(st, rp);
    st->hide();
    rp->hide();
    dock("properties")->raise();
    resizeDocks({ex, dock("properties")}, {290, 380}, Qt::Horizontal);
    resizeDocks({sc}, {270}, Qt::Vertical);
}

// ---- actions ---------------------------------------------------------------------------------------

QAction* MainWindow::addAction(const QString& id, const QString& text, const QString& icon, std::function<void()> slot, const QKeySequence& key,
                               const QString& tip) {
    auto* a = new QAction(icon.isEmpty() ? QIcon() : toolIcon(icon), text, this);
    if (!key.isEmpty()) a->setShortcut(key);
    a->setToolTip(tip.isEmpty() ? text : QStringLiteral("<b>%1</b><br>%2").arg(text, tip));
    a->setStatusTip(tip.isEmpty() ? text : tip);
    a->setObjectName(id);
    a->setProperty("iconName", icon);
    connect(a, &QAction::triggered, this, [this, slot, text] {
        try {
            slot();
        } catch (const std::exception& e) {
            ctx_.report(tr("%1 failed: %2").arg(text, qstr(e.what())), MessageLevel::Error);
        }
    });
    QWidget::addAction(a);
    actions_[id] = a;
    return a;
}

Uuid MainWindow::primaryOf(EntityKind kind) const {
    const auto p = ctx_.primary();
    if (p.kind == kind) return p.id;
    const auto s = ctx_.selected(kind);
    return s.empty() ? Uuid{} : s.back();
}

void MainWindow::requireSelection(EntityKind kind, const QString& what, const std::function<void(const Uuid&)>& f) {
    if (!ctx_.hasProject()) return;
    Uuid id = primaryOf(kind);
    if (id.isNil()) {
        // Offer a choice when exactly one exists.
        const auto& m = ctx_.project().model();
        std::vector<Uuid> all;
#define OPENMINE_COLLECT(T, member, Kind) \
    if (kind == EntityKind::Kind)         \
        for (const auto& e : m.member.all()) all.push_back(e.id);
        OPENMINE_ENTITY_TYPES(OPENMINE_COLLECT)
#undef OPENMINE_COLLECT
        if (all.size() == 1) id = all.front();
    }
    if (id.isNil()) {
        ctx_.report(tr("%1: select a %2 first").arg(what, kindLabel(kind).toLower()), MessageLevel::Warning);
        return;
    }
    f(id);
}

void MainWindow::startTool(int tool) {
    viewports_->tools().setTool(static_cast<Tool>(tool));
    if (auto* v = activeView()) v->vtkWidget()->setFocus();
}

void MainWindow::showLayoutPreview(const std::vector<std::vector<Vec3>>& lines) {
    for (auto* v : viewports_->views()) {
        if (lines.empty()) v->clearOverlay("layout");
        else {
            Overlay o;
            o.lines = lines;
            o.color = 0x00E5FF;
            o.lineWidth = 3;
            v->setOverlay("layout", o);
        }
    }
}

void MainWindow::createActions() {
    auto& tools = viewports_->tools();
    // ---- file
    addAction("file.new", tr("New project"), "new", [this] {
        if (!maybeSave()) return;
        bool ok = nonInteractive_;
        QString name = tr("New mine project");
        if (!nonInteractive_) name = QInputDialog::getText(this, tr("New project"), tr("Project name:"), QLineEdit::Normal, name, &ok);
        if (!ok) return;
        ctx_.newProject(name, Date::todayUtc());
    }, QKeySequence::New);
    addAction("file.open", tr("Open…"), "open", [this] {
        if (!maybeSave()) return;
        const QString f = chooseOpenFile(tr("Open project"), tr("OpenMine projects (*.openmine)"), "project");
        if (!f.isEmpty()) openProjectFrom(f);
    }, QKeySequence::Open);
    addAction("file.save", tr("Save"), "save", [this] {
        if (ctx_.project().filePath().empty()) action("file.saveAs")->trigger();
        else saveProjectTo(QString::fromStdU16String(ctx_.project().filePath().u16string()));
    }, QKeySequence::Save);
    addAction("file.saveAs", tr("Save as…"), "save", [this] {
        const QString f = chooseSaveFile(tr("Save project"), tr("OpenMine projects (*.openmine)"), "project", qstr(ctx_.project().model().info.name) + ".openmine");
        if (!f.isEmpty()) saveProjectTo(f.endsWith(".openmine") ? f : f + ".openmine");
    }, QKeySequence::SaveAs);
    addAction("file.close", tr("Close project"), {}, [this] { closeProject(); });
    addAction("file.import", tr("Import…"), "import", [this] {
        const QString f = chooseOpenFile(tr("Import"),
                                         tr("All supported (*.las *.laz *.e57 *.ply *.xyz *.csv *.txt *.pts *.ptx *.dxf *.omf *.obj *.stl *.step *.stp *.iges *.igs *.shp *.gpkg *.geojson *.kml *.tif *.tiff *.asc);;All files (*)"),
                                         "import");
        if (!f.isEmpty()) importFile(f);
    }, QKeySequence(Qt::CTRL | Qt::Key_I), tr("Point clouds, DXF, OMF, meshes, STEP/IGES, GIS vector and raster data, and formats added by plugins"));
    addAction("file.export", tr("Export…"), "export", [this] {
        const QString f = chooseSaveFile(tr("Export"), tr("DXF (*.dxf);;OMF (*.omf);;Mesh (*.obj *.stl *.ply);;STEP (*.step);;IGES (*.iges);;GIS (*.shp *.gpkg *.geojson *.kml);;DEM (*.tif);;Point cloud (*.las *.laz *.e57 *.xyz *.pts);;Schedule (*.csv *.xlsx *.pdf)"), "export");
        if (!f.isEmpty()) exportFile(f);
    }, QKeySequence(Qt::CTRL | Qt::Key_E), tr("Exports the selection (or everything applicable) by file type"));
    addAction("file.check", tr("Check project integrity"), "info", [this] { projectCheck(); });
    addAction("file.repository", tr("Shared repository…"), "link", [this] {
        RepositoryDialog dlg(ctx_, this);
        dlg.exec();
    }, {}, RepositoryDialog::available() ? QString() : tr("Not available: built without libpq"));
    addAction("file.exit", tr("Exit"), {}, [this] { close(); }, QKeySequence::Quit);
    // ---- edit
    addAction("edit.undo", tr("Undo"), "undo", [this] {
        if (ctx_.project().commands().canUndo()) ctx_.project().commands().undo();
    }, QKeySequence::Undo);
    addAction("edit.redo", tr("Redo"), "redo", [this] {
        if (ctx_.project().commands().canRedo()) ctx_.project().commands().redo();
    }, QKeySequence::Redo);
    addAction("edit.delete", tr("Delete"), "delete", [this] {
        auto sel = ctx_.selection();
        std::erase_if(sel, [](const SelectionItem& s) { return s.kind == EntityKind::ProjectInfo; });
        if (sel.empty()) return;
        if (!nonInteractive_ &&
            QMessageBox::question(this, tr("Delete"), tr("Delete %1 item(s)? This can be undone.").arg(sel.size())) != QMessageBox::Yes)
            return;
        ctx_.tryEdit(tr("Delete"), [&] {
            auto& p = ctx_.project();
            CommandStack::MacroScope macro(p.commands(), "Delete " + std::to_string(sel.size()) + " items");
            for (const auto& s : sel)
                if (findKind(p.model(), s.id)) persistence::removeDocument(p, documentType(s.kind), s.id.toString());
            macro.commit();
        });
    }, QKeySequence::Delete);
    addAction("edit.selectAll", tr("Select all visible objects"), "select", [this] {
        std::vector<SelectionItem> items;
        viewports_->scene().forEachItem([&](const SceneItem& it) {
            if (it.pickable && !it.ghost) items.push_back(it.owner);
        });
        std::sort(items.begin(), items.end());
        items.erase(std::unique(items.begin(), items.end()), items.end());
        ctx_.selectMany(items);
    }, QKeySequence::SelectAll);
    addAction("edit.clearSelection", tr("Clear selection"), {}, [this] { ctx_.clearSelection(); });
    addAction("edit.properties", tr("Properties"), "properties", [this] {
        dock("properties")->show();
        dock("properties")->raise();
    });
    // ---- view
    addAction("view.fit", tr("Zoom extents"), "fit", [this] { activeView()->fitAll(); }, QKeySequence(Qt::Key_Home));
    addAction("view.fitSelection", tr("Zoom to selection"), "zoomin", [this] { activeView()->fitSelection(); });
    addAction("view.top", tr("Plan"), "top", [this] { activeView()->setNamedView("top"); });
    addAction("view.north", tr("Looking south"), "front", [this] { activeView()->setNamedView("north"); });
    addAction("view.east", tr("Looking west"), "side", [this] { activeView()->setNamedView("east"); });
    addAction("view.iso", tr("3D (south-east)"), "iso", [this] { activeView()->setNamedView("iso"); });
    addAction("view.isoNw", tr("3D (north-west)"), "iso", [this] { activeView()->setNamedView("iso-nw"); });
    auto* persp = addAction("view.perspective", tr("Perspective"), "perspective", [this] { activeView()->setPerspective(action("view.perspective")->isChecked()); });
    persp->setCheckable(true);
    persp->setChecked(true);
    addAction("view.section", tr("Section view"), "section", [this] { activeView()->setMode(ViewMode::Section); }, {}, tr("Clip a slab of the scene and look along its normal; PgUp/PgDn step"));
    addAction("view.sectionLine", tr("Section from line"), "section", [this] { startTool(static_cast<int>(Tool::SectionLine)); }, {},
              tr("Draw a line in plan: a vertical section along it opens in another view"));
    addAction("view.sectionWidth", tr("Section width…"), {}, [this] {
        auto s = activeView()->section();
        bool ok = false;
        const double w = QInputDialog::getDouble(this, tr("Section"), tr("Half width (m):"), s.halfWidth, 0.01, 1000, 2, &ok);
        if (!ok) return;
        s.halfWidth = w;
        activeView()->setSection(s);
    });
    addAction("view.layout1", tr("One view"), "view1", [this] { viewports_->setLayoutKind(ViewportArea::Layout::Single); });
    addAction("view.layout2", tr("Two views side by side"), "view2", [this] { viewports_->setLayoutKind(ViewportArea::Layout::TwoColumns); });
    addAction("view.layout2h", tr("Two views stacked"), "view2h", [this] { viewports_->setLayoutKind(ViewportArea::Layout::TwoRows); });
    addAction("view.layout4", tr("Four views"), "view4", [this] { viewports_->setLayoutKind(ViewportArea::Layout::Quad); });
    addAction("view.workplane", tr("Work-plane elevation…"), "grid", [this] {
        bool ok = false;
        const double z = QInputDialog::getDouble(this, tr("Work plane"), tr("Elevation (m):"), activeView()->workPlaneZ(), -1e5, 1e5, 3, &ok);
        if (ok) viewports_->forEach([z](Viewport3D& v) { v.setWorkPlaneZ(z); });
    });
    auto toggle = [this](const QString& id, const QString& text, bool initial, std::function<void(bool)> f) {
        auto* a = addAction(id, text, {}, {});
        a->setCheckable(true);
        a->setChecked(initial);
        disconnect(a, &QAction::triggered, nullptr, nullptr);
        connect(a, &QAction::toggled, this, f);
        return a;
    };
    toggle("view.grid", tr("Grid"), true, [this](bool on) { viewports_->forEach([on](Viewport3D& v) { v.options().grid = on; v.applyOptions(); }); });
    toggle("view.labels", tr("Labels"), true, [this](bool on) { viewports_->forEach([on](Viewport3D& v) { v.options().labels = on; v.applyOptions(); }); });
    toggle("view.clouds", tr("Point clouds"), true, [this](bool on) { viewports_->forEach([on](Viewport3D& v) { v.options().pointClouds = on; v.applyOptions(); }); });
    toggle("view.legend", tr("Colour legend"), true, [this](bool on) { viewports_->forEach([on](Viewport3D& v) { v.options().legend = on; v.applyOptions(); }); });
    toggle("view.envelopes", tr("Excavation envelopes"), true, [this](bool on) {
        auto s = viewports_->scene().settings();
        s.developmentEnvelopes = on;
        viewports_->scene().setSettings(s);
    });
    toggle("view.stations", tr("Chainage stations"), false, [this](bool on) {
        auto s = viewports_->scene().settings();
        s.developmentStations = on;
        viewports_->scene().setSettings(s);
    });
    toggle("view.meshEdges", tr("Mesh edges"), false, [this](bool on) {
        auto s = viewports_->scene().settings();
        s.meshEdges = on;
        viewports_->scene().setSettings(s);
    });
    toggle("view.statusColours", tr("Colour headings by status"), true, [this](bool on) {
        auto s = viewports_->scene().settings();
        s.colorDevelopmentsByStatus = on;
        viewports_->scene().setSettings(s);
    });
    addAction("view.drillholeField", tr("Colour drillholes by…"), "drillhole", [this] {
        const auto fields = viewports_->scene().drillholeFields();
        QStringList l{tr("(hole type)")};
        for (const auto& f : fields) l << qstr(f);
        bool ok = false;
        const QString f = QInputDialog::getItem(this, tr("Drillholes"), tr("Colour traces by:"), l, 0, false, &ok);
        if (!ok) return;
        auto s = viewports_->scene().settings();
        s.drillholeField = f == l.front() ? std::string() : sstr(f);
        viewports_->scene().setSettings(s);
    });
    addAction("view.pointBudget", tr("Point budget…"), "cloud", [this] {
        bool ok = false;
        const int m = QInputDialog::getInt(this, tr("Point clouds"), tr("Points displayed per view (millions):"),
                                           static_cast<int>(activeView()->options().pointBudget / 1000000), 1, 100, 1, &ok);
        if (ok) viewports_->forEach([m](Viewport3D& v) { v.options().pointBudget = static_cast<std::size_t>(m) * 1000000; v.applyOptions(); });
    });
    addAction("view.showAll", tr("Show all hidden"), "eye", [this] { viewports_->scene().showAll(); });
    addAction("view.dark", tr("Dark theme"), "theme", [this] { setTheme(true); });
    addAction("view.light", tr("Light theme"), "theme", [this] { setTheme(false); });
    addAction("view.screenshot", tr("Save view image…"), "export", [this] {
        const QString f = chooseSaveFile(tr("Save view image"), tr("PNG (*.png)"), "image", "view.png");
        if (!f.isEmpty() && activeView()->grab().save(f)) ctx_.report(tr("Saved %1").arg(f));
    });

    // ---- draw / modify (tools)
    struct ToolDef {
        const char* id;
        const char* text;
        const char* icon;
        Tool tool;
        QKeySequence key;
    };
    const ToolDef toolDefs[] = {
        {"draw.select", "Select", "select", Tool::Select, QKeySequence(Qt::Key_Escape)},
        {"draw.point", "Point", "point", Tool::Point, {}},
        {"draw.line", "Line", "line", Tool::Line, QKeySequence(Qt::Key_L)},
        {"draw.polyline", "Polyline", "polyline", Tool::Polyline, QKeySequence(Qt::Key_P)},
        {"draw.polygon", "Polygon", "polygon", Tool::Polygon, {}},
        {"draw.rectangle", "Rectangle", "rectangle", Tool::Rectangle, {}},
        {"draw.circle", "Circle", "circle", Tool::Circle, QKeySequence(Qt::Key_C)},
        {"draw.arc", "Arc (3 points)", "arc", Tool::Arc3, {}},
        {"draw.spline", "Spline", "spline", Tool::Spline, {}},
        {"draw.text", "Text", "text", Tool::Text, {}},
        {"draw.dimension", "Dimension", "dimension", Tool::Dimension, {}},
        {"measure.distance", "Distance", "distance", Tool::Measure, QKeySequence(Qt::Key_D)},
        {"measure.area", "Area", "area", Tool::MeasureArea, {}},
        {"modify.move", "Move", "move", Tool::Move, QKeySequence(Qt::Key_M)},
        {"modify.copy", "Copy", "copy", Tool::Copy, {}},
        {"modify.rotate", "Rotate", "rotate", Tool::Rotate, {}},
        {"modify.mirror", "Mirror", "mirror", Tool::Mirror, {}},
        {"modify.scale", "Scale", "scale", Tool::Scale, {}},
        {"modify.offset", "Offset", "offset", Tool::Offset, QKeySequence(Qt::Key_O)},
        {"modify.trim", "Trim", "trim", Tool::Trim, QKeySequence(Qt::Key_T)},
        {"modify.extend", "Extend", "extend", Tool::Extend, {}},
        {"modify.fillet", "Fillet", "fillet", Tool::Fillet, QKeySequence(Qt::Key_F) },
        {"modify.chamfer", "Chamfer", "chamfer", Tool::Chamfer, {}},
        {"modify.split", "Split", "split", Tool::Split, {}},
        {"survey.crop", "Crop by polygon", "polygon", Tool::CropPolygon, {}},
    };
    auto* toolGroup = new QActionGroup(this);
    for (const auto& t : toolDefs) {
        const int tool = static_cast<int>(t.tool);
        auto* a = addAction(t.id, tr(t.text), t.icon, [this, tool] { startTool(tool); }, t.key);
        a->setCheckable(true);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        toolGroup->addAction(a);
    }
    // Single-key tool shortcuts only act while a viewport has the focus.
    for (const auto& t : toolDefs)
        if (!t.key.isEmpty()) {
            removeAction(action(t.id));
            viewports_->addAction(action(t.id));
        }
    action("modify.fillet")->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F));
    action("draw.select")->setChecked(true);
    connect(&tools, &ToolController::toolChanged, this, [this](Tool t) {
        static const std::map<Tool, const char*> ids = {
            {Tool::Select, "draw.select"},   {Tool::Point, "draw.point"},        {Tool::Line, "draw.line"},        {Tool::Polyline, "draw.polyline"},
            {Tool::Polygon, "draw.polygon"}, {Tool::Rectangle, "draw.rectangle"}, {Tool::Circle, "draw.circle"},   {Tool::Arc3, "draw.arc"},
            {Tool::Spline, "draw.spline"},   {Tool::Text, "draw.text"},          {Tool::Dimension, "draw.dimension"}, {Tool::Measure, "measure.distance"},
            {Tool::MeasureArea, "measure.area"}, {Tool::Move, "modify.move"},   {Tool::Copy, "modify.copy"},      {Tool::Rotate, "modify.rotate"},
            {Tool::Mirror, "modify.mirror"}, {Tool::Scale, "modify.scale"},      {Tool::Offset, "modify.offset"},  {Tool::Trim, "modify.trim"},
            {Tool::Extend, "modify.extend"}, {Tool::Fillet, "modify.fillet"},    {Tool::Chamfer, "modify.chamfer"},    {Tool::Split, "modify.split"},    {Tool::CropPolygon, "survey.crop"}};
        if (auto it = ids.find(t); it != ids.end()) action(it->second)->setChecked(true);
        else action("draw.select")->setChecked(false);
        promptLabel_->setText(viewports_->tools().prompt());
    });
    auto* snap = addAction("draw.snap", tr("Object snap"), "snap", {}, QKeySequence(Qt::Key_F3));
    snap->setCheckable(true);
    snap->setChecked(true);
    connect(snap, &QAction::toggled, &tools, &ToolController::setSnapEnabled);
    auto* surf = addAction("draw.surfaceSnap", tr("Snap to surfaces and points"), "cloud", {});
    surf->setCheckable(true);
    surf->setChecked(true);
    connect(surf, &QAction::toggled, &tools, &ToolController::setSurfaceSnap);
    auto* ortho = addAction("draw.ortho", tr("Ortho"), "ortho", {}, QKeySequence(Qt::Key_F8));
    ortho->setCheckable(true);
    connect(ortho, &QAction::toggled, &tools, &ToolController::setOrtho);
    auto* gsnap = addAction("draw.gridSnap", tr("Grid snap"), "grid", {}, QKeySequence(Qt::Key_F9));
    gsnap->setCheckable(true);
    connect(gsnap, &QAction::toggled, this, [this](bool on) {
        auto& s = viewports_->tools().snapSettings();
        if (on) s.modes |= static_cast<unsigned>(cad::SnapMode::Grid);
        else s.modes &= ~static_cast<unsigned>(cad::SnapMode::Grid);
    });
    addAction("draw.gridSpacing", tr("Grid spacing…"), {}, [this] {
        bool ok = false;
        const double v = QInputDialog::getDouble(this, tr("Grid snap"), tr("Spacing (m):"), viewports_->tools().snapSettings().gridSpacing, 0.001, 10000, 3, &ok);
        if (ok) viewports_->tools().snapSettings().gridSpacing = v;
    });
    addAction("modify.join", tr("Join"), "join", [this] { handleEntityAction("join", ctx_.selection()); });
    addAction("modify.explode", tr("Explode"), "explode", [this] { handleEntityAction("explode", ctx_.selection()); });
    addAction("modify.union", tr("Union solids"), "boolean", [this] { handleEntityAction("union", ctx_.selection()); });
    addAction("modify.subtract", tr("Subtract solids"), "boolean", [this] { handleEntityAction("subtract", ctx_.selection()); });
    addAction("modify.intersect", tr("Intersect solids"), "boolean", [this] { handleEntityAction("intersect", ctx_.selection()); });
    addAction("modify.meshToSolid", tr("Mesh to solid"), "solid", [this] { handleEntityAction("mesh-to-solid", ctx_.selection()); });
    addAction("modify.newLayer", tr("New layer…"), "layer", [this] { handleEntityAction("new-layer", {}); });

    // ---- survey
    addAction("survey.importCloud", tr("Import point cloud…"), "cloud", [this] { handleEntityAction("import-cloud", {}); });
    addAction("survey.importControl", tr("Import control points…"), "control", [this] { handleEntityAction("import-control", {}); });
    const std::pair<const char*, const char*> cloudOps[] = {
        {"survey.subsample", "cloud-subsample"}, {"survey.outliers", "cloud-outliers"}, {"survey.normals", "cloud-normals"},
        {"survey.ground", "cloud-ground"},       {"survey.delaunay", "cloud-delaunay"}, {"survey.poisson", "cloud-poisson"},
        {"survey.distance", "cloud-distance"},   {"survey.stats", "cloud-stats"},       {"survey.planes", "cloud-planes"},
        {"survey.register", "register-pairs"},   {"survey.icp", "register-icp"},        {"survey.asbuilt", "asbuilt"},
        {"survey.conformance", "conformance"},   {"survey.face", "face-advance"},       {"survey.volume", "surface-volume"},
        {"survey.reportCloud", "report-cloud"},
    };
    const char* cloudTexts[] = {"Subsample…", "Remove outliers…", "Estimate normals…", "Classify ground…", "Surface (Delaunay)…", "Surface (Poisson)…",
                                "Distance to reference…", "Statistics", "Extract planes…", "Register (pairs / control)…", "Register (ICP)…", "As-built from scan…",
                                "Design conformance…", "Face position and actuals…", "Volume…", "Point-cloud report"};
    const char* cloudIcons[] = {"filter", "filter", "normals", "ground", "surface", "mesh", "distance", "chart", "structure", "register", "icp", "mesh",
                                "conformance", "face", "volume", "report"};
    for (std::size_t i = 0; i < std::size(cloudOps); ++i) {
        const QString act = cloudOps[i].second;
        addAction(cloudOps[i].first, tr(cloudTexts[i]), cloudIcons[i], [this, act] { handleEntityAction(act, ctx_.selection()); });
    }
    // ---- design
    using LK = LayoutDialog::Kind;
    auto layoutAction = [this](const QString& id, const QString& text, const QString& icon, LK kind) {
        addAction(id, text, icon, [this, kind] {
            auto* d = new LayoutDialog(ctx_, viewports_->tools(), kind, [this](const auto& lines) { showLayoutPreview(lines); }, this);
            d->show();
        });
    };
    layoutAction("design.decline", tr("Decline…"), "decline", LK::Decline);
    layoutAction("design.ramp", tr("Ramp…"), "ramp", LK::Ramp);
    layoutAction("design.alignment", tr("Alignment through PIs…"), "polyline", LK::Alignment);
    layoutAction("design.oredrive", tr("Ore drive…"), "oredrive", LK::OreDrive);
    layoutAction("design.access", tr("Access drive…"), "access", LK::AccessDrive);
    addAction("design.crosscuts", tr("Crosscuts…"), "crosscut", [this] { handleEntityAction("crosscuts", ctx_.selection()); });
    addAction("design.heading", tr("Heading from centreline…"), "heading", [this] { handleEntityAction("heading-from-geometry", ctx_.selection()); },
              QKeySequence(Qt::CTRL | Qt::Key_H));
    addAction("design.linkedActivity", tr("Linked activity…"), "link", [this] { handleEntityAction("linked-activity", ctx_.selection()); });
    addAction("design.stope", tr("Stope…"), "stope", [this] { handleEntityAction("stope-from-geometry", ctx_.selection()); });
    addAction("design.subdivide", tr("Subdivide stope…"), "stope", [this] { handleEntityAction("subdivide-stope", ctx_.selection()); });
    addAction("design.level", tr("New level…"), "level", [this] { handleEntityAction("new-level", {}); });
    addAction("design.revision", tr("Capture revision…"), "revision", [this] { handleEntityAction("capture-revision", ctx_.selection()); });
    addAction("design.gradient", tr("Gradient and design rules"), "chart", [this] { handleEntityAction("gradient", ctx_.selection()); });
    addAction("design.impacts", tr("Design impacts"), "impacts", [this] { handleEntityAction("impacts", {}); });
    addAction("design.applyQuantities", tr("Apply design quantities"), "calc", [this] { scheduler_->applyDesignQuantities(); });
    addAction("design.solid", tr("Excavation solid"), "solid", [this] { handleEntityAction("development-solid", ctx_.selection()); });
    addAction("design.sectionChainage", tr("Section at chainage…"), "section", [this] { handleEntityAction("section-chainage", ctx_.selection()); });
    addAction("design.quantities", tr("Development quantities report"), "report", [this] { reports_->generate("quantities", "pdf"); });
    // ---- geology / geotech
    addAction("geology.drillholes", tr("Import drillholes…"), "drillhole", [this] { handleEntityAction("import-drillholes", {}); });
    addAction("geology.composite", tr("Composite…"), "drillhole", [this] { handleEntityAction("composite", {}); });
    addAction("geology.blockModel", tr("New block model…"), "blocks", [this] {
        BlockModelDialog d(ctx_, this);
        d.exec();
    });
    addAction("geology.code", tr("Code blocks by solid…"), "solid", [this] { handleEntityAction("block-code", ctx_.selection()); });
    addAction("geology.estimate", tr("Estimate grades…"), "estimate", [this] { handleEntityAction("estimate", ctx_.selection()); });
    addAction("geology.display", tr("Block display…"), "blocks", [this] { handleEntityAction("block-display", ctx_.selection()); });
    addAction("geology.gradeTonnage", tr("Grade–tonnage report"), "chart", [this] { handleEntityAction("grade-tonnage", ctx_.selection()); });
    addAction("geotech.structures", tr("Import structures…"), "structure", [this] { handleEntityAction("import-structures", {}); });
    addAction("geotech.stereonet", tr("Stereonet"), "stereonet", [this] { handleEntityAction("stereonet", {}); });
    addAction("geotech.classify", tr("Rock-mass classification…"), "geotech", [this] { handleEntityAction("classify", ctx_.selection()); });
    // ---- schedule
    addAction("schedule.show", tr("Scheduler"), "gantt", [this] {
        dock("scheduler")->show();
        dock("scheduler")->raise();
    });
    addAction("schedule.task", tr("Add task"), "task", [this] { scheduler_->addActivity(ActivityKind::Task); });
    addAction("schedule.milestone", tr("Add milestone"), "milestone", [this] { scheduler_->addActivity(ActivityKind::Milestone); });
    addAction("schedule.summary", tr("Add summary"), "summary", [this] { scheduler_->addActivity(ActivityKind::Summary); });
    addAction("schedule.dependency", tr("Add dependency…"), "dependency", [this] { scheduler_->addDependency(); });
    addAction("schedule.actual", tr("Record actual…"), "actual", [this] { scheduler_->recordActual(); });
    addAction("schedule.calculate", tr("Recalculate"), "calc", [this] { ctx_.recalculateSchedule(); }, QKeySequence(Qt::Key_F5));
    addAction("schedule.baseline", tr("Capture baseline…"), "baseline", [this] { scheduler_->captureBaseline(); });
    addAction("schedule.scenario", tr("New scenario…"), "scenario", [this] { scheduler_->editScenario(true); });
    addAction("schedule.optimise", tr("Optimise sequence…"), "optimise", [this] { scheduler_->optimise(); }, {},
              OptimiserDialog::available() ? tr("OR-Tools CP-SAT") : tr("Not available: built without OR-Tools"));
    addAction("schedule.playback", tr("4D playback"), "4d", [this] {
        dock("playback")->show();
        dock("playback")->raise();
    });
    addAction("schedule.gantt", tr("Gantt chart PDF"), "pdf", [this] { reports_->generate("gantt", "pdf"); });
    addAction("schedule.export", tr("Export schedule…"), "xlsx", [this] {
        const QString f = chooseSaveFile(tr("Export schedule"), tr("Excel (*.xlsx);;CSV (*.csv);;Gantt PDF (*.pdf)"), "schedule", "schedule.xlsx");
        if (!f.isEmpty()) scheduler_->exportFile(f);
    });
    // ---- tools
    addAction("tools.python", tr("Python console"), "python", [this] {
        dock("python")->show();
        dock("python")->raise();
    }, {}, PythonConsolePanel::available() ? QString() : tr("Not available: built without Python"));
    addAction("tools.runScript", tr("Run Python script…"), "python", [this] {
        const QString f = chooseOpenFile(tr("Run Python script"), tr("Python (*.py)"), "python");
        if (!f.isEmpty()) python_->runFile(f);
    });
    addAction("tools.plugins", tr("Plugins…"), "plugin", [this] {
        PluginsDialog d(ctx_, this);
        d.exec();
        rebuildPluginMenu();
    });
    addAction("tools.crs", tr("Coordinate systems…"), "crs", [this] {
        CrsDialog d(ctx_, this);
        d.exec();
    }, {}, CrsDialog::available() ? QString() : tr("Not available: built without PROJ"));
    addAction("tools.localGrid", tr("Local mine grid…"), "grid", [this] {
        LocalGridDialog d(ctx_, this);
        d.exec();
    });
    addAction("tools.transform", tr("Transform objects to another CRS…"), "crs", [this] {
        TransformDialog d(ctx_, this);
        d.exec();
    });
    addAction("tools.jobs", tr("Jobs"), "jobs", [this] {
        dock("jobs")->show();
        dock("jobs")->raise();
    });
    addAction("tools.reports", tr("Reports"), "report", [this] {
        dock("reports")->show();
        dock("reports")->raise();
    });
    addAction("tools.console", tr("Command console"), "console", [this] {
        dock("console")->show();
        dock("console")->raise();
    });
    addAction("help.about", tr("About OpenMine Suite"), "info", [this] { showAbout(); });
    addAction("help.commands", tr("Command reference"), "help", [this] {
        dock("console")->show();
        console_->execute("HELP");
    });

    // ---- 3D modelling
    using SO = SolidModelDialog::Operation;
    auto solidAction = [this](const QString& id, const QString& text, const QString& icon, SO op, const QString& tip) {
        addAction(id, text, icon, [this, op] {
            auto* d = new SolidModelDialog(ctx_, op, this);
            d->setAttribute(Qt::WA_DeleteOnClose);
            d->show();
        }, {}, tip);
    };
    solidAction("solid.extrude", tr("Extrude…"), "extrude", SO::Extrude, tr("Closed curves to solids (inner curves become holes)"));
    solidAction("solid.revolve", tr("Revolve…"), "revolve", SO::Revolve, tr("A closed profile revolved about an axis"));
    solidAction("solid.sweep", tr("Sweep…"), "sweep", SO::Sweep, tr("A tunnel profile swept along any path"));
    solidAction("solid.loft", tr("Loft…"), "loft", SO::Loft, tr("A solid through closed sections, in selection order"));
    addAction("solid.check", tr("Check solids"), "info", [this] { handleEntityAction("solid-check", ctx_.selection()); }, {},
              tr("Validity, closure, volume, area and topology"));
    addAction("solid.heal", tr("Heal solids"), "repair", [this] { handleEntityAction("solid-heal", ctx_.selection()); });
    addAction("solid.fillet", tr("Fillet solid edges…"), "fillet", [this] { handleEntityAction("solid-fillet", ctx_.selection()); }, {},
              tr("Rounds every edge of the selected solids (exact, in place)"));
    addAction("solid.offset", tr("Offset solid…"), "offset", [this] { handleEntityAction("solid-offset", ctx_.selection()); }, {},
              tr("Offsets the faces of the selected solids outwards (or inwards with a negative distance)"));
    addAction("solid.section", tr("Section by plane"), "section", [this] { handleEntityAction("section-objects", ctx_.selection()); }, {},
              tr("Sections the selected solids and meshes by the active view's section plane, or horizontally at the work-plane elevation"));
    // ---- surfaces
    using MO = MeshToolDialog::Operation;
    auto meshAction = [this](const QString& id, const QString& text, const QString& icon, MO op) {
        addAction(id, text, icon, [this, op] {
            Uuid mesh;
            for (const auto& g : ctx_.selectedGeometry())
                if (const auto* o = ctx_.project().model().geometry.find(g); o && o->mesh) mesh = g;
            auto* d = new MeshToolDialog(ctx_, op, mesh, this);
            d->setAttribute(Qt::WA_DeleteOnClose);
            d->open();
        });
    };
    meshAction("mesh.decimate", tr("Decimate surface…"), "decimate", MO::Decimate);
    meshAction("mesh.smooth", tr("Smooth surface…"), "smooth", MO::Smooth);
    meshAction("mesh.repair", tr("Repair surface…"), "repair", MO::Repair);
    meshAction("mesh.compare", tr("Compare surfaces…"), "distance", MO::Compare);
    addAction("survey.segment", tr("Segment into clusters…"), "cluster", [this] { handleEntityAction("cloud-segment", ctx_.selection()); });
    addAction("survey.cropBox", tr("Crop by box…"), "crop", [this] { handleEntityAction("cloud-crop-box", ctx_.selection()); });
    // ---- scheduling libraries
    addAction("schedule.resources", tr("Resources…"), "resource", [this] { handleEntityAction("resources", {}); });
    addAction("schedule.calendars", tr("Calendars…"), "calendar", [this] { handleEntityAction("calendars", {}); });
    addAction("schedule.cycles", tr("Development cycles…"), "calc", [this] { handleEntityAction("cycles", {}); });
    addAction("schedule.targets", tr("Production targets…"), "chart", [this] { handleEntityAction("targets", {}); });
    addAction("schedule.critical", tr("Show critical activities"), "critical", [this] {
        scheduler_->setShow(SchedulerPanel::Show::Critical);
        dock("scheduler")->show();
        dock("scheduler")->raise();
    });
    addAction("schedule.showAll", tr("Show all activities"), "gantt", [this] {
        scheduler_->setFilter({});
        scheduler_->setShow(SchedulerPanel::Show::All);
    });
    addAction("schedule.production", tr("Production report"), "report", [this] { reports_->generate("production", "pdf"); });
    addAction("schedule.scenarios", tr("Scenario comparison report"), "report", [this] { reports_->generate("scenarios", "pdf"); });
    // ---- analysis and reporting
    addAction("analysis.impacts", tr("Design impacts"), "impacts", [this] { handleEntityAction("impacts", {}); });
    addAction("report.quantities", tr("Development quantities (PDF)"), "report", [this] { reports_->generate("quantities", "pdf"); });
    addAction("report.quantitiesXlsx", tr("Development quantities (Excel)"), "xlsx", [this] { reports_->generate("quantities-xlsx", "xlsx"); });
    addAction("report.scheduleXlsx", tr("Schedule workbook (Excel)"), "xlsx", [this] { reports_->generate("schedule-xlsx", "xlsx"); });
    // ---- window and settings
    addAction("window.reset", tr("Reset panel layout"), "view4", [this] { resetLayout(); });
    addAction("window.nextView", tr("Next view"), "view2", [this] {
        const auto views = viewports_->views();
        if (views.size() < 2) return;
        const auto it = std::find(views.begin(), views.end(), activeView());
        viewports_->setActive(views[(static_cast<std::size_t>(it - views.begin()) + 1) % views.size()]);
    }, QKeySequence(Qt::CTRL | Qt::Key_Tab));
    addAction("settings.preferences", tr("Preferences…"), "settings", [this] {
        PreferencesDialog d(ctx_, *viewports_, [this](bool dark) { setTheme(dark); }, this);
        d.exec();
    }, QKeySequence::Preferences);
    addAction("settings.quickAccess", tr("Customise quick-access toolbar…"), "settings", [this] {
        QuickAccessDialog d(actions_, ctx_.settings().value("ui/quickAccess").toStringList(), this);
        if (d.exec() == QDialog::Accepted) setQuickAccess(d.chosen());
    });
    addAction("settings.reset", tr("Restore default settings"), {}, [this] {
        if (!nonInteractive_ &&
            QMessageBox::question(this, tr("Restore defaults"), tr("Forget the preferences, layout, recent files and quick-access toolbar? Projects are not affected.")) !=
                QMessageBox::Yes)
            return;
        const auto recent = ctx_.settings().value("recent/files");
        ctx_.settings().clear();
        ctx_.settings().setValue("recent/files", recent);
        setQuickAccess({});
        resetLayout();
        ctx_.report(tr("Default settings restored (some take effect at the next start)"));
    });

    // Availability of optional features is fixed for the build.
    action("file.repository")->setEnabled(RepositoryDialog::available());
    action("schedule.optimise")->setEnabled(OptimiserDialog::available());
    action("tools.crs")->setEnabled(CrsDialog::available());
    action("tools.localGrid")->setEnabled(CrsDialog::available());
    action("tools.transform")->setEnabled(CrsDialog::available());
    action("tools.python")->setEnabled(PythonConsolePanel::available());
    action("tools.runScript")->setEnabled(PythonConsolePanel::available());
#ifndef OPENMINE_APP_HAVE_OCCT
    for (const char* id : {"modify.union", "modify.subtract", "modify.intersect", "modify.meshToSolid", "design.solid", "solid.extrude", "solid.revolve",
                           "solid.sweep", "solid.loft", "solid.heal", "solid.fillet", "solid.offset"}) {
        action(id)->setEnabled(false);
        action(id)->setToolTip(tr("Not available: built without Open CASCADE"));
    }
#endif
#ifndef OPENMINE_APP_HAVE_PDAL
    for (const char* id : {"survey.ground", "survey.poisson", "survey.asbuilt"}) {
        action(id)->setEnabled(false);
        action(id)->setToolTip(tr("Not available: built without PDAL"));
    }
#endif
}

// ---- menus and ribbon ----------------------------------------------------------------------------

void MainWindow::createMenus() {
    // Our own menu bar: it is placed above the ribbon in the main window's menu widget.
    auto* mb = new QMenuBar(this);
    menuBar_ = mb;
    auto add = [this](QMenu* m, std::initializer_list<const char*> ids) {
        for (const char* id : ids) {
            if (std::string_view(id) == "-") m->addSeparator();
            else if (auto* a = action(id)) m->addAction(a);
        }
    };
    auto* file = mb->addMenu(tr("&File"));
    add(file, {"file.new", "file.open"});
    recentMenu_ = file->addMenu(tr("Recent projects"));
    add(file, {"file.save", "file.saveAs", "file.close", "-", "file.import", "file.export", "-", "file.repository", "file.check", "-", "file.exit"});
    connect(recentMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    auto* edit = mb->addMenu(tr("&Edit"));
    add(edit, {"edit.undo", "edit.redo", "-", "edit.delete", "edit.selectAll", "edit.clearSelection", "-", "edit.properties"});
    auto* view = mb->addMenu(tr("&View"));
    add(view, {"view.fit", "view.fitSelection", "-", "view.top", "view.north", "view.east", "view.iso", "view.isoNw", "view.perspective", "-",
               "view.section", "view.sectionLine", "view.sectionWidth", "view.workplane", "-", "view.grid", "view.labels", "view.legend", "view.clouds",
               "view.envelopes", "view.statusColours", "view.stations", "view.meshEdges", "view.drillholeField", "view.pointBudget", "view.showAll", "-",
               "view.screenshot"});
    auto* draw = mb->addMenu(tr("&Draw"));
    add(draw, {"draw.select", "-", "draw.point", "draw.line", "draw.polyline", "draw.polygon", "draw.rectangle", "draw.circle", "draw.arc", "draw.spline",
               "draw.text", "draw.dimension", "-", "measure.distance", "measure.area", "-", "draw.snap", "draw.surfaceSnap", "draw.ortho", "draw.gridSnap",
               "draw.gridSpacing"});
    auto* modify = mb->addMenu(tr("&Modify"));
    add(modify, {"modify.move", "modify.copy", "modify.rotate", "modify.mirror", "modify.scale", "-", "modify.offset", "modify.trim", "modify.extend",
                 "modify.fillet", "modify.chamfer", "modify.split", "modify.join", "modify.explode", "-", "modify.newLayer"});
    auto* solids = modify->addMenu(toolIcon("solid"), tr("Solids"));
    add(solids, {"solid.extrude", "solid.revolve", "solid.sweep", "solid.loft", "-", "modify.union", "modify.subtract", "modify.intersect", "-",
                 "modify.meshToSolid", "solid.check", "solid.heal", "solid.fillet", "solid.offset", "solid.section"});
    auto* survey = mb->addMenu(tr("&Survey"));
    add(survey, {"survey.importCloud", "survey.importControl", "-", "survey.subsample", "survey.outliers", "survey.normals", "survey.ground", "survey.crop",
                 "survey.cropBox", "survey.segment", "survey.planes", "survey.stats", "-", "survey.register", "survey.icp", "-", "survey.delaunay",
                 "survey.poisson", "survey.distance", "survey.volume", "-", "survey.asbuilt", "survey.conformance", "survey.face", "-", "survey.reportCloud"});
    auto* surfaces = survey->addMenu(toolIcon("mesh"), tr("Surfaces"));
    add(surfaces, {"mesh.decimate", "mesh.smooth", "mesh.repair", "mesh.compare", "solid.section"});
    auto* design = mb->addMenu(tr("Mine D&esign"));
    add(design, {"design.decline", "design.ramp", "design.alignment", "design.oredrive", "design.access", "design.crosscuts", "-", "design.heading",
                 "design.linkedActivity", "design.level", "-", "design.stope", "design.subdivide", "-", "design.revision", "design.gradient",
                 "design.sectionChainage", "design.solid", "-", "design.impacts", "design.applyQuantities", "design.quantities"});
    auto* schedule = mb->addMenu(tr("S&cheduler"));
    add(schedule, {"schedule.show", "-", "schedule.task", "schedule.milestone", "schedule.summary", "schedule.dependency", "schedule.actual", "-",
                   "schedule.resources", "schedule.calendars", "schedule.cycles", "schedule.targets", "-", "schedule.calculate", "schedule.critical",
                   "schedule.showAll", "schedule.scenario", "schedule.optimise", "schedule.baseline", "-", "schedule.playback", "schedule.gantt",
                   "schedule.export"});
    auto* geology = mb->addMenu(tr("&Geology"));
    add(geology, {"geology.drillholes", "geology.composite", "-", "geology.blockModel", "geology.code", "geology.estimate", "geology.display",
                  "geology.gradeTonnage"});
    auto* geotech = mb->addMenu(tr("Geo&technical"));
    add(geotech, {"geotech.structures", "geotech.stereonet", "geotech.classify"});
    auto* tools = mb->addMenu(tr("&Tools"));
    add(tools, {"tools.python", "tools.runScript", "tools.console", "-", "tools.crs", "tools.localGrid", "tools.transform", "-", "tools.jobs", "tools.reports",
                "-", "tools.plugins"});
    pluginMenu_ = tools->addMenu(toolIcon("plugin"), tr("Plugin commands"));
    auto* window = mb->addMenu(tr("&Window"));
    add(window, {"view.layout1", "view.layout2", "view.layout2h", "view.layout4", "window.nextView", "-"});
    auto* panels = window->addMenu(tr("Panels"));
    for (const auto& [name, d] : docks_) panels->addAction(d->toggleViewAction());
    add(window, {"-", "window.reset"});
    auto* settings = mb->addMenu(tr("Se&ttings"));
    add(settings, {"settings.preferences", "settings.quickAccess", "-", "view.dark", "view.light", "-", "settings.reset"});
    auto* help = mb->addMenu(tr("&Help"));
    add(help, {"help.commands", "help.about"});
}

void MainWindow::createRibbon() {
    ribbon_ = new Ribbon(this);
    quickAccess_ = new QToolBar(tr("Quick access"), this);
    quickAccess_->setObjectName("quickAccess");
    quickAccess_->setIconSize(QSize(18, 18));
    quickAccess_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    quickAccess_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(quickAccess_, &QToolBar::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu m(this);
        m.addAction(action("settings.quickAccess"));
        m.exec(quickAccess_->mapToGlobal(pos));
    });
    auto* holder = new QWidget(this);
    auto* lay = new QVBoxLayout(holder);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->addWidget(menuBar_, 1);
    top->addWidget(quickAccess_);
    lay->addLayout(top);
    lay->addWidget(ribbon_);
    setMenuWidget(holder);
    auto big = [this](RibbonGroup* g, std::initializer_list<const char*> ids) {
        for (const char* id : ids)
            if (auto* a = action(id)) g->addAction(a);
    };
    auto small = [this](RibbonGroup* g, std::initializer_list<const char*> ids) {
        QList<QAction*> l;
        for (const char* id : ids)
            if (auto* a = action(id)) l << a;
        g->addSmall(l);
    };
    // 1 Home
    auto* home = ribbon_->addPage(tr("Home"));
    big(home->addGroup(tr("Project")), {"file.new", "file.open", "file.save"});
    small(home->addGroup(tr("Data")), {"file.import", "file.export", "file.check", "file.repository", "tools.crs", "tools.localGrid"});
    big(home->addGroup(tr("Edit")), {"edit.undo", "edit.redo", "edit.delete"});
    big(home->addGroup(tr("View")), {"view.fit", "view.top", "view.iso", "view.section"});
    small(home->addGroup(tr("Layout")), {"view.layout1", "view.layout2", "view.layout4", "view.perspective", "settings.preferences", "window.reset"});
    home->finish();
    // 2 Survey
    auto* survey = ribbon_->addPage(tr("Survey"));
    big(survey->addGroup(tr("Data")), {"survey.importCloud", "survey.importControl"});
    small(survey->addGroup(tr("Process")), {"survey.subsample", "survey.outliers", "survey.normals", "survey.ground", "survey.crop", "survey.cropBox",
                                            "survey.segment", "survey.planes"});
    big(survey->addGroup(tr("Register")), {"survey.register", "survey.icp"});
    small(survey->addGroup(tr("Surfaces")), {"survey.delaunay", "survey.poisson", "mesh.decimate", "mesh.smooth", "mesh.repair", "mesh.compare"});
    big(survey->addGroup(tr("As-built")), {"survey.asbuilt", "survey.conformance", "survey.face"});
    survey->finish();
    // 3 CAD
    auto* cad = ribbon_->addPage(tr("CAD"));
    big(cad->addGroup(tr("Select")), {"draw.select"});
    small(cad->addGroup(tr("Draw")), {"draw.point", "draw.line", "draw.polyline", "draw.polygon", "draw.rectangle", "draw.circle", "draw.arc", "draw.spline", "draw.text",
                                      "draw.dimension"});
    small(cad->addGroup(tr("Modify")), {"modify.move", "modify.copy", "modify.rotate", "modify.mirror", "modify.scale", "modify.offset", "modify.trim", "modify.extend",
                                        "modify.fillet", "modify.chamfer", "modify.split", "modify.join", "modify.explode"});
    small(cad->addGroup(tr("3D")), {"solid.extrude", "solid.revolve", "solid.sweep", "solid.loft", "modify.union", "modify.subtract", "modify.intersect",
                                    "modify.meshToSolid", "solid.heal", "solid.fillet", "solid.offset"});
    small(cad->addGroup(tr("Snaps")), {"draw.snap", "draw.surfaceSnap", "draw.ortho", "draw.gridSnap", "draw.gridSpacing", "view.workplane"});
    big(cad->addGroup(tr("Layers")), {"modify.newLayer"});
    cad->finish();
    // 4 Mine Design
    auto* design = ribbon_->addPage(tr("Mine Design"));
    big(design->addGroup(tr("Layout")), {"design.decline", "design.ramp"});
    small(design->addGroup(tr("Drives")), {"design.alignment", "design.oredrive", "design.access", "design.crosscuts", "design.heading", "design.level"});
    small(design->addGroup(tr("Stopes")), {"design.stope", "design.subdivide", "design.solid"});
    small(design->addGroup(tr("Revisions")), {"design.revision", "design.gradient", "design.sectionChainage", "design.impacts", "design.applyQuantities"});
    big(design->addGroup(tr("Schedule link")), {"design.linkedActivity"});
    design->finish();
    // 5 Scheduler
    auto* schedule = ribbon_->addPage(tr("Scheduler"));
    big(schedule->addGroup(tr("Scheduler")), {"schedule.show", "schedule.calculate"});
    small(schedule->addGroup(tr("Activities")), {"schedule.task", "schedule.milestone", "schedule.summary", "schedule.dependency", "schedule.actual"});
    small(schedule->addGroup(tr("Libraries")), {"schedule.resources", "schedule.calendars", "schedule.cycles", "schedule.targets"});
    small(schedule->addGroup(tr("Scenarios")), {"schedule.scenario", "schedule.optimise", "schedule.baseline", "schedule.critical", "schedule.showAll"});
    big(schedule->addGroup(tr("4D")), {"schedule.playback"});
    schedule->finish();
    // 6 Geology
    auto* geology = ribbon_->addPage(tr("Geology"));
    big(geology->addGroup(tr("Drillholes")), {"geology.drillholes", "geology.composite"});
    small(geology->addGroup(tr("Block models")), {"geology.blockModel", "geology.code", "geology.estimate", "geology.display", "geology.gradeTonnage"});
    geology->finish();
    // 7 Geotechnical
    auto* geotech = ribbon_->addPage(tr("Geotechnical"));
    big(geotech->addGroup(tr("Structures")), {"geotech.structures", "geotech.stereonet", "survey.planes"});
    big(geotech->addGroup(tr("Rock mass")), {"geotech.classify"});
    geotech->finish();
    // 8 Analysis
    auto* analysis = ribbon_->addPage(tr("Analysis"));
    big(analysis->addGroup(tr("Measure")), {"measure.distance", "measure.area"});
    small(analysis->addGroup(tr("Point clouds")), {"survey.stats", "survey.distance", "survey.volume", "survey.segment"});
    small(analysis->addGroup(tr("Surfaces and solids")), {"mesh.compare", "solid.check", "solid.section", "design.gradient", "design.sectionChainage"});
    big(analysis->addGroup(tr("Reconciliation")), {"survey.conformance", "survey.face", "analysis.impacts"});
    analysis->finish();
    // 9 Reporting
    auto* reporting = ribbon_->addPage(tr("Reporting"));
    big(reporting->addGroup(tr("Survey")), {"survey.reportCloud"});
    small(reporting->addGroup(tr("Design")), {"report.quantities", "report.quantitiesXlsx"});
    small(reporting->addGroup(tr("Schedule")), {"schedule.gantt", "report.scheduleXlsx", "schedule.production", "schedule.scenarios", "schedule.export"});
    small(reporting->addGroup(tr("Resources")), {"geology.gradeTonnage"});
    big(reporting->addGroup(tr("Library")), {"tools.reports", "view.screenshot"});
    reporting->finish();
    // 10 Automation
    auto* automation = ribbon_->addPage(tr("Automation"));
    big(automation->addGroup(tr("Scripting")), {"tools.python", "tools.runScript", "tools.console"});
    big(automation->addGroup(tr("Extend")), {"tools.plugins"});
    big(automation->addGroup(tr("Monitor")), {"tools.jobs"});
    small(automation->addGroup(tr("Customise")), {"settings.quickAccess", "settings.preferences", "help.commands"});
    automation->finish();
    connect(ribbon_, &QTabWidget::currentChanged, this, [this](int i) {
        const QString t = ribbon_->tabText(i);
        if (t == tr("Scheduler")) {
            dock("scheduler")->show();
            dock("scheduler")->raise();
        } else if (t == tr("Survey")) {
            dock("jobs")->show();
        } else if (t == tr("Geotechnical")) {
            dock("stereonet")->show();
            dock("stereonet")->raise();
        } else if (t == tr("Reporting")) {
            dock("reports")->show();
            dock("reports")->raise();
        } else if (t == tr("Automation")) {
            dock("python")->show();
            dock("python")->raise();
        }
    });
    QStringList quick = ctx_.settings().value("ui/quickAccess").toStringList();
    setQuickAccess(quick);
}

void MainWindow::setQuickAccess(const QStringList& idsIn) {
    QStringList ids = idsIn;
    if (ids.isEmpty()) ids = {"file.new", "file.open", "file.save", "edit.undo", "edit.redo", "view.fit"};
    quickAccess_->clear();
    for (const auto& id : ids)
        if (auto* a = action(id)) quickAccess_->addAction(a);
    ctx_.settings().setValue("ui/quickAccess", idsIn.isEmpty() ? QStringList() : ids);
}

void MainWindow::rebuildQuickAccess() { setQuickAccess(ctx_.settings().value("ui/quickAccess").toStringList()); }

void MainWindow::resetLayout() {
    if (defaultState_.isEmpty()) return;
    restoreState(defaultState_);
    for (const char* n : {"stereonet", "reports"})
        if (auto* d = dock(n)) d->hide();
    dock("properties")->raise();
    dock("explorer")->raise();
    resizeDocks({dock("explorer"), dock("properties")}, {290, 380}, Qt::Horizontal);
    resizeDocks({dock("scheduler")}, {height() / 4}, Qt::Vertical);
    ctx_.settings().remove("window/state");
}

void MainWindow::createStatusBar() {
    auto mk = [this](int minWidth) {
        auto* l = new QLabel(this);
        l->setMinimumWidth(minWidth);
        statusBar()->addPermanentWidget(l);
        return l;
    };
    promptLabel_ = new QLabel(this);
    statusBar()->addWidget(promptLabel_, 1);
    snapLabel_ = mk(90);
    coordLabel_ = mk(330);
    pointsLabel_ = mk(120);
    jobsLabel_ = mk(80);
    scenarioLabel_ = mk(140);
    crsLabel_ = mk(160);
    connect(viewports_, &ViewportArea::cursorMoved, this, [this](Vec3 w, bool valid) {
        if (!valid || !ctx_.hasProject()) {
            coordLabel_->clear();
            return;
        }
        const auto& info = ctx_.project().model().info;
        const int d = info.coordinateDisplayDecimals;
        if (info.southOrientatedAxes)
            coordLabel_->setText(tr("Y %1  X %2  Z %3").arg(-w.x, 0, 'f', d).arg(-w.y, 0, 'f', d).arg(w.z, 0, 'f', d));
        else
            coordLabel_->setText(tr("E %1  N %2  Z %3").arg(w.x, 0, 'f', d).arg(w.y, 0, 'f', d).arg(w.z, 0, 'f', d));
        pointsLabel_->setText(activeView() && activeView()->displayedPoints() ? tr("%L1 pts").arg(static_cast<qulonglong>(activeView()->displayedPoints())) : QString());
    });
    connect(&viewports_->tools(), &ToolController::snapped, this, [this](const QString& label, Vec3) { snapLabel_->setText(label); });
    connect(&viewports_->tools(), &ToolController::promptChanged, promptLabel_, &QLabel::setText);
    connect(&ctx_.jobs(), &JobManager::jobsChanged, this, [this] {
        const int n = ctx_.jobs().runningCount();
        jobsLabel_->setText(n ? tr("%1 job(s)").arg(n) : QString());
        updateActions();
    });
    connect(&ctx_.jobs(), &JobManager::jobFinished, this, [this](int, bool ok, const QString& msg) {
        if (!ok) ctx_.report(msg, MessageLevel::Error);
    });
    auto updateCrs = [this] {
        if (!ctx_.hasProject()) return;
        const auto* c = ctx_.project().model().projectCrs();
        crsLabel_->setText(c ? (c->authorityCode.empty() ? qstr(c->name) : qstr(c->authorityCode)) : tr("CRS not declared"));
        crsLabel_->setToolTip(c ? qstr(c->name + "\n" + c->axisDescription) : QString());
    };
    connect(&ctx_, &AppContext::modelChanged, this, updateCrs);
    connect(&ctx_, &AppContext::projectReplaced, this, updateCrs);
    scenarioLabel_->setText(tr("Base schedule"));
}

// ---- state --------------------------------------------------------------------------------------

void MainWindow::updateTitle() {
    if (!ctx_.hasProject()) {
        setWindowTitle(tr("OpenMine Suite"));
        return;
    }
    const auto& p = ctx_.project();
    const QString file = p.filePath().empty() ? tr("unsaved") : QFileInfo(QString::fromStdU16String(p.filePath().u16string())).fileName();
    setWindowTitle(QStringLiteral("%1[*] — %2 — OpenMine Suite").arg(qstr(p.model().info.name), file));
    setWindowModified(p.isModified());
}

void MainWindow::updateActions() {
    const bool has = ctx_.hasProject();
    for (auto& [id, a] : actions_) {
        if (id == "file.new" || id == "file.open" || id == "file.exit" || id.startsWith("help.") || id.startsWith("view.") || id.startsWith("settings.") ||
            id.startsWith("window.") || id == "tools.plugins")
            continue;
        if (a->toolTip().contains(tr("Not available"))) continue;
        a->setEnabled(has);
    }
    if (!has) return;
    auto& cmd = ctx_.project().commands();
    action("edit.undo")->setEnabled(cmd.canUndo());
    action("edit.redo")->setEnabled(cmd.canRedo());
    action("edit.undo")->setText(cmd.canUndo() ? tr("Undo %1").arg(qstr(cmd.undoText())) : tr("Undo"));
    action("edit.redo")->setText(cmd.canRedo() ? tr("Redo %1").arg(qstr(cmd.redoText())) : tr("Redo"));
    action("edit.delete")->setEnabled(!ctx_.selection().empty());
    // Saving while a job writes assets could copy a half-written file.
    action("file.save")->setEnabled(!ctx_.jobs().assetJobsRunning());
    action("file.saveAs")->setEnabled(!ctx_.jobs().assetJobsRunning());
}

void MainWindow::setTheme(bool dark) {
    Theme::apply(*qApp, dark ? ThemeKind::Dark : ThemeKind::Light);
    viewports_->forEach([](Viewport3D& v) { v.refreshTheme(); });
    // Icons are drawn in the theme's colours: redraw them for the new theme.
    for (auto& [id, a] : actions_) {
        const QString icon = a->property("iconName").toString();
        if (!icon.isEmpty()) a->setIcon(toolIcon(icon));
    }
    properties_->refresh();
    ctx_.settings().setValue("ui/theme", dark ? "dark" : "light");
}

void MainWindow::rebuildRecentMenu() {
    recentMenu_->clear();
    for (const auto& f : ctx_.settings().value("recent/files").toStringList()) {
        auto* a = recentMenu_->addAction(QFileInfo(f).fileName(), this, [this, f] {
            if (maybeSave()) openProjectFrom(f);
        });
        a->setToolTip(f);
        a->setEnabled(QFileInfo::exists(f));
    }
    if (recentMenu_->isEmpty()) recentMenu_->addAction(tr("(none)"))->setEnabled(false);
}

void MainWindow::loadPlugins() {
    std::vector<std::string> disabled;
    for (const auto& id : ctx_.settings().value("plugins/disabled").toStringList()) disabled.push_back(sstr(id));
    const int n = ctx_.plugins().loadAll(ctx_.pluginDirectories(), disabled);
    for (const auto& s : ctx_.plugins().plugins())
        if (!s.loaded && std::find(disabled.begin(), disabled.end(), s.manifest.id) == disabled.end())
            ctx_.report(tr("Plugin %1 not loaded: %2").arg(qstr(s.manifest.id), qstr(s.error)), MessageLevel::Warning);
    if (n) ctx_.report(tr("%1 plugin(s) loaded").arg(n));
    // Python plugins: installed distributions and the "python" subfolder of each plugin folder.
    if (PythonConsolePanel::available() && ctx_.settings().value("plugins/python", true).toBool()) {
        QStringList problems;
        const int k = python_->discoverPlugins(ctx_.pluginDirectories(), &problems);
        for (const auto& p : problems) ctx_.report(p, MessageLevel::Warning);
        ctx_.setScriptPlugins(python_);
        std::set<std::string> native;
        for (const auto& c : ctx_.plugins().commands()) native.insert(c.name);
        for (const auto& c : python_->scriptCommands())
            if (native.count(c.name))
                ctx_.report(tr("Python plugin command %1 (%2) is hidden by a native plugin command of the same name").arg(qstr(c.name), qstr(c.plugin)),
                            MessageLevel::Warning);
        if (k) ctx_.report(tr("%1 Python plugin command(s) available").arg(k));
    }
    rebuildPluginMenu();
}

void MainWindow::rebuildPluginMenu() {
    pluginMenu_->clear();
    const auto cmds = ctx_.pluginCommands();
    bool pythonSection = false;
    for (const auto& c : cmds) {
        if (c.python && !pythonSection) {
            pluginMenu_->addSection(tr("Python plugins"));
            pythonSection = true;
        }
        const QString name = qstr(c.name);
        auto* a = pluginMenu_->addAction(name, this, [this, name] {
            QString args = "{}";
            if (!nonInteractive_) {
                bool ok = false;
                args = QInputDialog::getText(this, name, tr("Arguments (JSON):"), QLineEdit::Normal, "{}", &ok);
                if (!ok) return;
            }
            if (!ctx_.hasProject()) {
                ctx_.report(tr("Open or create a project first"), MessageLevel::Warning);
                return;
            }
            const auto r = ctx_.runPluginCommand(sstr(name), sstr(args));
            for (const auto& l : r.log) ctx_.report(qstr(l));
            if (r.ok) ctx_.report(tr("%1: %2").arg(name, qstr(r.text)));
            else ctx_.report(tr("%1 failed: %2%3").arg(name, qstr(r.error), r.rolledBack ? tr(" (its changes were rolled back)") : QString()), MessageLevel::Error);
        });
        a->setToolTip(qstr(c.description + " [" + c.plugin + (c.python ? ", Python" : "") + "]"));
    }
    if (cmds.empty()) pluginMenu_->addAction(tr("(no plugin commands)"))->setEnabled(false);
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About OpenMine Suite"),
                       tr("<h3>OpenMine Suite %1</h3><p>Open-source integrated mine survey, design and scheduling.</p>"
                          "<p>Licensed under the Apache License 2.0. Third-party components and their licences are listed in THIRD_PARTY_NOTICES.</p>"
                          "<p>Qt %2 · VTK · Open CASCADE · PDAL · PROJ · GDAL · OR-Tools · SQLite · PostgreSQL/PostGIS · Python</p>")
                           .arg(qstr(persistence::kAppVersion), qVersion()));
}

void MainWindow::projectCheck() {
    QStringList lines;
    if (!ctx_.project().filePath().empty()) {
        const auto r = persistence::checkProjectFile(ctx_.project().filePath(), true);
        lines << tr("File: %1 (schema v%2)").arg(r.ok ? tr("OK") : tr("PROBLEMS")).arg(r.schemaVersion);
        for (const auto& p : r.problems) lines << "  " + qstr(p);
    } else {
        lines << tr("The project has not been saved yet (file checks skipped).");
    }
    const auto problems = persistence::checkModel(ctx_.project());
    lines << tr("Model: %1 referential problem(s)").arg(problems.size());
    for (const auto& p : problems) lines << "  " + qstr(p);
    int assetsBad = 0;
    for (const auto& a : ctx_.project().model().assets.all()) {
        const auto v = ctx_.project().assets().verify(a, true);
        if (!v.ok) {
            ++assetsBad;
            for (const auto& p : v.problems) lines << "  " + qstr(p);
        }
    }
    lines << tr("Assets: %1 checked, %2 with problems").arg(ctx_.project().model().assets.size()).arg(assetsBad);
    ctx_.report(lines.front());
    if (nonInteractive_) {
        for (const auto& l : lines) ctx_.report(l);
        return;
    }
    QMessageBox box(this);
    box.setWindowTitle(tr("Project integrity"));
    box.setText(lines.join("\n"));
    box.exec();
}

// ---- project lifecycle ----------------------------------------------------------------------------

QString MainWindow::chooseOpenFile(const QString& title, const QString& filter, const QString& key) {
    if (nonInteractive_) return {};
    const QString dir = ctx_.settings().value("dirs/" + key).toString();
    const QString f = QFileDialog::getOpenFileName(this, title, dir, filter);
    if (!f.isEmpty()) ctx_.settings().setValue("dirs/" + key, QFileInfo(f).absolutePath());
    return f;
}

QString MainWindow::chooseSaveFile(const QString& title, const QString& filter, const QString& key, const QString& suggestion) {
    if (nonInteractive_) return {};
    const QString dir = ctx_.settings().value("dirs/" + key).toString();
    const QString f = QFileDialog::getSaveFileName(this, title, QDir(dir).filePath(suggestion), filter);
    if (!f.isEmpty()) ctx_.settings().setValue("dirs/" + key, QFileInfo(f).absolutePath());
    return f;
}

bool MainWindow::saveProjectTo(const QString& path) {
    if (ctx_.jobs().assetJobsRunning()) {
        ctx_.report(tr("Wait for the running jobs to finish (or cancel them) before saving"), MessageLevel::Warning);
        return false;
    }
    try {
        ctx_.saveProject(path);
#ifdef OPENMINE_APP_HAVE_REPOSITORY
        if (!ctx_.pendingWorkingCopy().empty()) {
            repository::WorkingCopy::fromJson(ctx_.pendingWorkingCopy()).save(std::filesystem::path(path.toStdU16String()));
            ctx_.setPendingWorkingCopy({});
        }
#endif
        updateTitle();
        return true;
    } catch (const std::exception& e) {
        ctx_.report(tr("Save failed: %1").arg(qstr(e.what())), MessageLevel::Error);
        if (!nonInteractive_) QMessageBox::critical(this, tr("Save failed"), qstr(e.what()));
        return false;
    }
}

bool MainWindow::openProjectFrom(const QString& path) {
    try {
        ctx_.openProject(path);
        return true;
    } catch (const std::exception& e) {
        ctx_.report(tr("Open failed: %1").arg(qstr(e.what())), MessageLevel::Error);
        if (!nonInteractive_) QMessageBox::critical(this, tr("Open failed"), qstr(e.what()));
        return false;
    }
}

bool MainWindow::closeProject() {
    if (!maybeSave()) return false;
    ctx_.closeProject();
    return true;
}

bool MainWindow::maybeSave() {
    if (!ctx_.hasProject() || !ctx_.project().isModified() || nonInteractive_) return true;
    const auto r = QMessageBox::question(this, tr("Unsaved changes"), tr("Save the changes to '%1'?").arg(qstr(ctx_.project().model().info.name)),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) {
        action("file.save")->trigger();
        return !ctx_.project().isModified();
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (ctx_.jobs().runningCount() > 0 && !nonInteractive_) {
        if (QMessageBox::question(this, tr("Jobs running"), tr("%1 background job(s) are running. Cancel them and exit?").arg(ctx_.jobs().runningCount())) !=
            QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    ctx_.jobs().cancelAll();
    ctx_.jobs().waitAll(15000);
    ctx_.settings().setValue("window/geometry", saveGeometry());
    ctx_.settings().setValue("window/state", saveState());
    e->accept();
}

} // namespace openmine::app
