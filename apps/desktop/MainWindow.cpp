// SPDX-License-Identifier: Apache-2.0
#include "MainWindow.h"
#include "MainWindowActions.h"

#include "console/CommandConsole.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"
#include "project_explorer/ProjectExplorer.h"
#include "properties/PropertiesPanel.h"
#include "ribbon/Ribbon.h"
#include "scheduler/SchedulerPanel.h"
#include "theme/Theme.h"
#include "viewport/Viewport3D.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QVBoxLayout>

namespace openmine::app {

// (MainWindow::Actions is defined in MainWindowActions.h)

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), a_(new Actions) {
    setObjectName("OpenMineMainWindow");
    resize(1600, 980);
    setDockNestingEnabled(true);
    viewport_ = new Viewport3D(ctx_, this);
    setCentralWidget(nullptr);
    createActions();
    createDocks();
    createRibbon();
    createMenus();
    createStatusBar();
    registerCommands();

    connect(viewport_, &Viewport3D::drawingFinished, this, &MainWindow::onDrawingFinished);
    connect(&ctx_, &AppContext::undoStackChanged, this, [this] {
        updateTitle();
        updateActions();
    });
    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
        updateTitle();
        updateActions();
    });
    connect(&ctx_, &AppContext::modelChanged, this, &MainWindow::updateTitle);
    connect(&ctx_, &AppContext::selectionChanged, this, &MainWindow::updateActions);
    connect(&ctx_, &AppContext::message, this, [this](const QString& t, bool err) {
        statusBar()->showMessage(t, err ? 8000 : 4000);
    });
    updateTitle();
    updateActions();
}

MainWindow::~MainWindow() { delete a_; }

// ---------------------------------------------------------------------------
void MainWindow::createActions() {
    auto mk = [this](const QString& text, const QString& icon, auto slot, const QKeySequence& ks = {}) {
        auto* a = new QAction(icon.isEmpty() ? QIcon() : toolIcon(icon), text, this);
        if (!ks.isEmpty()) a->setShortcut(ks);
        a->setProperty("iconName", icon);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    auto& A = *a_;
    A.newProject = mk("New", "new", [this] {
        if (!maybeSave()) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "New project", "Project name:", QLineEdit::Normal, "New Mine Project", &ok);
        if (ok && !name.isEmpty()) ctx_.newProject(name, Date::todayUtc());
    }, QKeySequence::New);
    A.open = mk("Open", "open", [this] {
        if (!maybeSave()) return;
        const QString p = QFileDialog::getOpenFileName(this, "Open project", {}, "OpenMine project (*.openmine)");
        if (!p.isEmpty()) openProjectFrom(p);
    }, QKeySequence::Open);
    A.save = mk("Save", "save", [this] {
        if (!ctx_.hasProject()) return;
        if (ctx_.project().filePath().empty()) a_->saveAs->trigger();
        else saveProjectTo(QString::fromStdU16String(ctx_.project().filePath().u16string()));
    }, QKeySequence::Save);
    A.saveAs = mk("Save As…", "", [this] {
        if (!ctx_.hasProject()) return;
        QString p = QFileDialog::getSaveFileName(this, "Save project", qstr(ctx_.project().model().info.name) + ".openmine",
                                                 "OpenMine project (*.openmine)");
        if (p.isEmpty()) return;
        if (!p.endsWith(".openmine")) p += ".openmine";
        saveProjectTo(p);
    }, QKeySequence::SaveAs);
    A.close = mk("Close", "", [this] { closeProject(); });
    A.exit = mk("Exit", "", [this] { close(); }, QKeySequence::Quit);

    A.undo = mk("Undo", "undo", [this] {
        if (ctx_.hasProject()) ctx_.project().commands().undo();
    }, QKeySequence::Undo);
    A.redo = mk("Redo", "redo", [this] {
        if (ctx_.hasProject()) ctx_.project().commands().redo();
    }, QKeySequence::Redo);
    A.del = mk("Delete", "delete", [this] { console_->execute("DELETE"); }, QKeySequence::Delete);

    A.select = mk("Select", "select", [this] { viewport_->setTool(Viewport3D::Tool::Select); }, QKeySequence(Qt::Key_Escape));
    A.line = mk("Line", "line", [this] {
        measuring_ = false;
        viewport_->setTool(Viewport3D::Tool::DrawLine);
    }, QKeySequence(Qt::Key_L));
    A.polyline = mk("Polyline", "polyline", [this] {
        measuring_ = false;
        viewport_->setTool(Viewport3D::Tool::DrawPolyline);
    }, QKeySequence(Qt::Key_P));
    A.measure = mk("Distance", "distance", [this] {
        viewport_->setTool(Viewport3D::Tool::DrawLine);
        measuring_ = true;
        ctx_.report("DISTANCE: click two points (snaps apply).");
    }, QKeySequence(Qt::Key_D));
    for (QAction* x : {A.select, A.line, A.polyline, A.measure}) x->setCheckable(true);
    auto* toolGroup = new QActionGroup(this);
    for (QAction* x : {A.select, A.line, A.polyline, A.measure}) toolGroup->addAction(x);
    A.select->setChecked(true);
    connect(viewport_, &Viewport3D::toolChanged, this, [this] {
        const auto t = viewport_->tool();
        if (t == Viewport3D::Tool::Select) {
            a_->select->setChecked(true);
            measuring_ = false;
        }
        toolLabel_->setText(t == Viewport3D::Tool::Select ? "Select" : t == Viewport3D::Tool::DrawLine ? (measuring_ ? "Distance" : "Line") : "Polyline");
    });

    A.gridSnap = mk("Grid Snap", "fit", [this](bool on) {
        viewport_->setGridSnap(on ? 1.0 : 0.0);
        snapLabel_->setText(QStringLiteral("Grid %1 · Endpoint %2").arg(on ? "1 m" : "off", viewport_->endpointSnap() ? "on" : "off"));
    });
    A.gridSnap->setCheckable(true);
    A.gridSnap->setIconText("Grid");
    A.gridSnap->setChecked(true);
    A.endSnap = mk("Endpoint Snap", "polyline", [this](bool on) {
        viewport_->setEndpointSnap(on);
        snapLabel_->setText(QStringLiteral("Grid %1 · Endpoint %2").arg(viewport_->gridSnap() > 0 ? "1 m" : "off", on ? "on" : "off"));
    });
    A.endSnap->setCheckable(true);
    A.endSnap->setIconText("Endpoint");
    A.endSnap->setChecked(true);
    A.workPlane = mk("Work Plane Z…", "top", [this] {
        bool ok = false;
        const double z = QInputDialog::getDouble(this, "Work plane", "Drawing elevation Z (m):", viewport_->workPlaneZ(), -1e7, 1e7, 3, &ok);
        if (ok) console_->execute(QStringLiteral("ELEV %1").arg(z, 0, 'f', 3));
    });
    A.workPlane->setIconText("Plane Z");
    A.newLayer = mk("New Layer…", "new", [this] {
        bool ok = false;
        const QString n = QInputDialog::getText(this, "New layer", "Layer name:", QLineEdit::Normal, {}, &ok);
        if (ok && !n.isEmpty()) console_->execute("LAYER " + n);
    });

    A.newLayer->setIconText("Layer");
    A.fit = mk("Fit", "fit", [this] { viewport_->fitAll(); }, QKeySequence(Qt::Key_F));
    A.top = mk("Top", "top", [this] { viewport_->setView("top"); });
    A.front = mk("Front", "front", [this] { viewport_->setView("front"); });
    A.side = mk("Side", "side", [this] { viewport_->setView("side"); });
    A.iso = mk("Isometric", "iso", [this] { viewport_->setView("iso"); });
    A.perspective = mk("Perspective", "iso", [this](bool on) { viewport_->setPerspective(on); });
    A.perspective->setCheckable(true);
    A.perspective->setChecked(true);
    A.dark = mk("Dark Theme", "theme", [this] { setTheme(true); });
    A.light = mk("Light Theme", "theme", [this] { setTheme(false); });

    A.heading = mk("Create Heading", "heading", [this] { createHeadingInteractive(); });
    A.linkedActivity = mk("Linked Activity", "activity", [this] { createLinkedActivityInteractive(); });
    A.impacts = mk("Design Impacts", "link", [this] { console_->execute("IMPACTS"); });

    A.task = mk("Task", "activity", [this] { scheduler_->addTask(); });
    A.milestone = mk("Milestone", "baseline", [this] { scheduler_->addMilestone(); });
    A.summary = mk("WBS Group", "gantt", [this] { scheduler_->addSummary(); });
    A.dependency = mk("Dependency", "link", [this] { scheduler_->addDependencyDialog(); });
    A.calc = mk("Calculate", "calc", [this] {
        schedulerDock_->raise();
        scheduler_->calculate();
    }, QKeySequence(Qt::Key_F9));
    A.baseline = mk("Baseline", "baseline", [this] { scheduler_->captureBaseline(); });
    A.applyDesign = mk("Apply Design Qty", "calc", [this] { scheduler_->applyDesignQuantities(); });
    A.exportCsv = mk("Export CSV", "save", [this] { scheduler_->exportCsv(); });

    A.about = mk("About OpenMine Suite", "", [this] {
        QMessageBox::about(this, "About OpenMine Suite",
                           "<b>OpenMine Suite 0.1.0</b><br>Open-source integrated mine survey, CAD/design and scheduling.<br>"
                           "Licensed under Apache-2.0. Built with Qt, VTK, Open CASCADE and SQLite (see THIRD_PARTY_NOTICES.md).");
    });
    A.commands = mk("Command Reference", "", [this] { console_->execute("HELP"); });
}

void MainWindow::createMenus() {
    auto& A = *a_;
    auto* file = menuBar()->addMenu("&File");
    file->addActions({A.newProject, A.open, A.save, A.saveAs});
    file->addSeparator();
    file->addAction(A.close);
    file->addSeparator();
    file->addAction(A.exit);
    auto* edit = menuBar()->addMenu("&Edit");
    edit->addActions({A.undo, A.redo});
    edit->addSeparator();
    edit->addAction(A.del);
    auto* view = menuBar()->addMenu("&View");
    view->addActions({A.fit, A.top, A.front, A.side, A.iso});
    view->addSeparator();
    view->addAction(A.perspective);
    view->addSeparator();
    view->addActions({A.dark, A.light});
    auto* tools = menuBar()->addMenu("&Tools");
    tools->addActions({A.select, A.line, A.polyline, A.measure});
    tools->addSeparator();
    tools->addActions({A.heading, A.linkedActivity, A.impacts});
    tools->addSeparator();
    tools->addActions({A.calc, A.baseline, A.applyDesign});
    auto* win = menuBar()->addMenu("&Window");
    for (auto* d : findChildren<QDockWidget*>()) win->addAction(d->toggleViewAction());
    auto* settings = menuBar()->addMenu("&Settings");
    settings->addActions({A.gridSnap, A.endSnap, A.workPlane});
    auto* help = menuBar()->addMenu("&Help");
    help->addActions({A.commands, A.about});
}

void MainWindow::createRibbon() {
    auto& A = *a_;
    ribbon_ = new Ribbon(this);
    auto* holder = new QWidget(this);
    auto* lay = new QVBoxLayout(holder);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(ribbon_);
    lay->addWidget(viewport_, 1);
    setCentralWidget(holder);

    const QString p2 = "Phase 2 — Initial CAD", p3 = "Phase 3 — Initial Survey", p4 = "Phase 4 — Scheduler",
                  p6 = "Phase 6 — Advanced";
    {
        auto* pg = ribbon_->addPage("Home");
        auto* g = pg->addGroup("Project");
        for (auto* x : {A.newProject, A.open, A.save}) g->addAction(x);
        g = pg->addGroup("Edit");
        for (auto* x : {A.undo, A.redo, A.del}) g->addAction(x);
        g = pg->addGroup("View");
        for (auto* x : {A.fit, A.top, A.front, A.side, A.iso}) g->addAction(x);
        g = pg->addGroup("Appearance");
        g->addAction(A.dark);
        g->addAction(A.light);
        pg->finish();
    }
    {
        auto* pg = ribbon_->addPage("Survey");
        auto* g = pg->addGroup("Import");
        g->addPlanned("LAS/LAZ", p3);
        g->addPlanned("E57", p3);
        g->addPlanned("CSV Points", p3);
        g = pg->addGroup("Point Cloud");
        g->addPlanned("Crop", p3);
        g->addPlanned("Filter", p3);
        g->addPlanned("Register", p3);
        g->addPlanned("Mesh", p3);
        g = pg->addGroup("Measure");
        g->addAction(A.measure);
        g = pg->addGroup("Conformance");
        g->addPlanned("Overbreak", p6);
        g->addPlanned("Sections", p6);
        pg->finish();
    }
    {
        auto* pg = ribbon_->addPage("CAD");
        auto* g = pg->addGroup("Draw");
        for (auto* x : {A.select, A.line, A.polyline}) g->addAction(x);
        g->addPlanned("Arc", p2);
        g->addPlanned("Circle", p2);
        g->addPlanned("Text", p2);
        g = pg->addGroup("Modify");
        g->addAction(A.del);
        g->addPlanned("Offset", p2);
        g->addPlanned("Trim", p2);
        g->addPlanned("Extend", p2);
        g->addPlanned("Fillet", p2);
        g = pg->addGroup("Snap");
        g->addAction(A.gridSnap);
        g->addAction(A.endSnap);
        g->addAction(A.workPlane);
        g = pg->addGroup("Layers");
        g->addAction(A.newLayer);
        g = pg->addGroup("Exchange");
        g->addPlanned("DXF", p2);
        pg->finish();
    }
    {
        auto* pg = ribbon_->addPage("Mine Design");
        auto* g = pg->addGroup("Development");
        g->addAction(A.heading);
        g->addAction(A.linkedActivity);
        g->addPlanned("Decline", p6);
        g->addPlanned("Profile", p6);
        g = pg->addGroup("Stopes");
        g->addPlanned("Stope", p6);
        g->addPlanned("Block", p6);
        g = pg->addGroup("Revisions");
        g->addAction(A.impacts);
        pg->finish();
    }
    {
        auto* pg = ribbon_->addPage("Scheduler");
        auto* g = pg->addGroup("Activities");
        for (auto* x : {A.task, A.milestone, A.summary, A.dependency}) g->addAction(x);
        g = pg->addGroup("Calculate");
        for (auto* x : {A.calc, A.baseline, A.applyDesign}) g->addAction(x);
        g = pg->addGroup("Resources");
        g->addPlanned("Resources", p6);
        g->addPlanned("Levelling", p6);
        g->addPlanned("Optimise", p6 + " (OR-Tools)");
        g = pg->addGroup("4D");
        g->addPlanned("Playback", p6);
        g = pg->addGroup("Export");
        g->addAction(A.exportCsv);
        g->addPlanned("PDF", p4);
        pg->finish();
    }
    for (const auto& [name, tools] : std::vector<std::pair<QString, QStringList>>{
             {"Geology", {"Drillholes", "Surfaces", "Block Model"}},
             {"Geotechnical", {"Support", "Stability", "Joints"}},
             {"Analysis", {"Volumes", "Compare", "Clearance"}},
             {"Reporting", {"Survey Rpt", "Schedule Rpt", "Templates"}},
             {"Automation", {"Python", "Macros", "Plugins"}}}) {
        auto* pg = ribbon_->addPage(name);
        auto* g = pg->addGroup(name);
        for (const auto& t : tools) g->addPlanned(t, p6);
        if (name == "Reporting") pg->addGroup("Available")->addAction(A.exportCsv);
        pg->finish();
    }
    ribbon_->setCurrentIndex(2); // CAD
}

void MainWindow::createDocks() {
    auto dock = [this](const QString& title, QWidget* w, Qt::DockWidgetArea area) {
        auto* d = new QDockWidget(title, this);
        d->setObjectName(title);
        d->setWidget(w);
        addDockWidget(area, d);
        return d;
    };
    explorer_ = new ProjectExplorer(ctx_, this);
    properties_ = new PropertiesPanel(ctx_, this);
    console_ = new CommandConsole(ctx_, this);
    scheduler_ = new SchedulerPanel(ctx_, this);
    auto* ex = dock("Project Explorer", explorer_, Qt::LeftDockWidgetArea);
    auto* pr = dock("Properties", properties_, Qt::RightDockWidgetArea);
    schedulerDock_ = dock("Scheduler — Gantt", scheduler_, Qt::BottomDockWidgetArea);
    auto* co = dock("Command Console", console_, Qt::BottomDockWidgetArea);
    tabifyDockWidget(co, schedulerDock_);
    schedulerDock_->raise();
    resizeDocks({ex, pr}, {300, 360}, Qt::Horizontal);
    resizeDocks({schedulerDock_}, {320}, Qt::Vertical);
}

void MainWindow::createStatusBar() {
    coordLabel_ = new QLabel("X — Y — Z —", this);
    coordLabel_->setMinimumWidth(330);
    crsLabel_ = new QLabel(this);
    snapLabel_ = new QLabel("Grid 1 m · Endpoint on", this);
    toolLabel_ = new QLabel("Select", this);
    statusBar()->addPermanentWidget(toolLabel_);
    statusBar()->addPermanentWidget(snapLabel_);
    statusBar()->addPermanentWidget(crsLabel_);
    statusBar()->addPermanentWidget(coordLabel_);
    connect(viewport_, &Viewport3D::cursorMoved, this, [this](Vec3 w, bool ok) {
        coordLabel_->setText(ok ? QStringLiteral("X %1   Y %2   Z %3").arg(formatNumber(w.x), formatNumber(w.y), formatNumber(w.z))
                                : QStringLiteral("cursor not on work plane"));
    });
}

void MainWindow::updateTitle() {
    if (!ctx_.hasProject()) {
        setWindowTitle("OpenMine Suite");
        crsLabel_->setText("No project");
        return;
    }
    const auto& p = ctx_.project();
    const QString file = p.filePath().empty() ? "unsaved" : QString::fromStdU16String(p.filePath().filename().u16string());
    setWindowTitle(QStringLiteral("%1 — %2%3 — OpenMine Suite").arg(qstr(p.model().info.name), file, p.isModified() ? " •" : ""));
    crsLabel_->setText("CRS: " + qstr(p.model().info.crs.name));
}

void MainWindow::updateActions() {
    const bool has = ctx_.hasProject();
    auto& A = *a_;
    A.undo->setEnabled(has && ctx_.project().commands().canUndo());
    A.redo->setEnabled(has && ctx_.project().commands().canRedo());
    A.undo->setToolTip(has && A.undo->isEnabled() ? "Undo: " + qstr(ctx_.project().commands().undoText()) : "Undo");
    A.redo->setToolTip(has && A.redo->isEnabled() ? "Redo: " + qstr(ctx_.project().commands().redoText()) : "Redo");
    for (auto* x : {A.save, A.saveAs, A.close, A.line, A.polyline, A.measure, A.task, A.milestone, A.summary, A.dependency,
                    A.calc, A.baseline, A.exportCsv, A.impacts, A.newLayer})
        x->setEnabled(has);
    const auto sel = ctx_.selection();
    A.heading->setEnabled(has && sel.kind == EntityKind::Geometry);
    A.linkedActivity->setEnabled(has && sel.kind == EntityKind::Development);
    A.applyDesign->setEnabled(has && sel.kind == EntityKind::Activity);
    A.del->setEnabled(has && !sel.isEmpty() && sel.kind != EntityKind::ProjectInfo);
}

void MainWindow::setTheme(bool dark) {
    Theme::apply(*qApp, dark ? ThemeKind::Dark : ThemeKind::Light);
    viewport_->refreshTheme();
    properties_->refresh();
    scheduler_->update();
    // Icons are drawn from theme colours; re-create them.
    for (auto* act : findChildren<QAction*>())
        if (const QString n = act->property("iconName").toString(); !n.isEmpty()) act->setIcon(toolIcon(n));
    ctx_.report(dark ? "Dark theme" : "Light theme");
}

// ---------------------------------------------------------------------------
bool MainWindow::saveProjectTo(const QString& path) {
    try {
        ctx_.saveProject(path);
        updateTitle();
        return true;
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
        if (!nonInteractive_) QMessageBox::critical(this, "Save failed", e.what());
        return false;
    }
}

bool MainWindow::openProjectFrom(const QString& path) {
    try {
        ctx_.openProject(path);
        return true;
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
        if (!nonInteractive_) QMessageBox::critical(this, "Open failed", e.what());
        return false;
    }
}

bool MainWindow::closeProject() {
    if (!maybeSave()) return false;
    ctx_.closeProject();
    return true;
}

bool MainWindow::maybeSave() {
    if (nonInteractive_ || !ctx_.hasProject() || !ctx_.project().isModified()) return true;
    const auto r = QMessageBox::warning(this, "Unsaved changes", "The project has unsaved changes. Save them?",
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) {
        a_->save->trigger();
        return !ctx_.project().isModified();
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (maybeSave()) e->accept();
    else e->ignore();
}

void MainWindow::onDrawingFinished(const std::vector<Vec3>& pts, bool isLine) {
    if (!ctx_.hasProject()) return;
    if (measuring_) {
        measuring_ = false;
        const Vec3 d = pts.back() - pts.front();
        ctx_.report(QStringLiteral("Distance: 3D %1 m · plan %2 m · ΔX %3 · ΔY %4 · ΔZ %5")
                        .arg(formatNumber(d.length()), formatNumber(d.horizontalLength()), formatNumber(d.x), formatNumber(d.y),
                             formatNumber(d.z)));
        return;
    }
    try {
        GeometryObject g;
        g.type = isLine ? GeometryType::Line : GeometryType::Polyline;
        g.vertices = pts;
        // Draw on the selected layer, else the first unlocked layer.
        const auto& m = ctx_.project().model();
        for (const auto& l : m.layers.all())
            if (!l.locked) {
                g.layerId = l.id;
                break;
            }
        if (ctx_.selection().kind == EntityKind::Layer) g.layerId = ctx_.selection().id;
        const Uuid id = ProjectEditor(ctx_.project()).addGeometry(g);
        ctx_.select(EntityKind::Geometry, id);
        ctx_.report(QStringLiteral("%1 created: %2 vertices, length %3 m")
                        .arg(isLine ? "Line" : "Polyline")
                        .arg(pts.size())
                        .arg(formatNumber(geometry::length3d(pts))));
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
    }
}

bool MainWindow::createHeadingFromSelection(const QString& headingId, double width, double height, const QString& level) {
    if (!ctx_.hasProject() || ctx_.selection().kind != EntityKind::Geometry) {
        ctx_.report("Select a line or polyline centreline first", true);
        return false;
    }
    try {
        mine_design::NewDevelopment spec;
        spec.headingId = sstr(headingId);
        spec.width = width;
        spec.height = height;
        spec.level = sstr(level);
        const Uuid id = mine_design::createDevelopment(ctx_.project(), ctx_.selection().id, spec);
        ctx_.select(EntityKind::Development, id);
        const auto mt = mine_design::computeMetrics(ctx_.project(), *ctx_.project().model().developments.find(id));
        ctx_.report(QStringLiteral("Heading %1 created: %2 m, %3 × %4 m").arg(headingId, formatNumber(mt.length3d)).arg(width).arg(height));
        return true;
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
        return false;
    }
}

bool MainWindow::createLinkedActivityForSelection(double ratePerDay) {
    if (!ctx_.hasProject() || ctx_.selection().kind != EntityKind::Development) {
        ctx_.report("Select a development heading first", true);
        return false;
    }
    try {
        const Uuid id = mine_design::createLinkedActivity(ctx_.project(), ctx_.selection().id, ratePerDay);
        ctx_.recalculateSchedule();
        ctx_.select(EntityKind::Activity, id);
        const auto& t = ctx_.schedule().timings.at(id);
        ctx_.report(QStringLiteral("Linked activity created: %1 working days at %2 m/day, %3 → %4")
                        .arg(formatNumber(t.duration, 1), formatNumber(ratePerDay, 2), qstr(t.startDate.toString()),
                             qstr(t.finishDate.toString())));
        schedulerDock_->raise();
        return true;
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
        return false;
    }
}

void MainWindow::createHeadingInteractive() {
    QDialog dlg(this);
    dlg.setWindowTitle("Create development heading");
    auto* form = new QFormLayout(&dlg);
    auto* id = new QLineEdit(&dlg);
    id->setPlaceholderText("e.g. LEG2_675_D3_DEC");
    auto* level = new QLineEdit(&dlg);
    auto* w = new QDoubleSpinBox(&dlg);
    auto* h = new QDoubleSpinBox(&dlg);
    for (auto* s : {w, h}) {
        s->setRange(0.5, 50);
        s->setDecimals(2);
        s->setValue(5.5);
        s->setSuffix(" m");
    }
    form->addRow("Heading ID", id);
    form->addRow("Level", level);
    form->addRow("Design width", w);
    form->addRow("Design height", h);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) createHeadingFromSelection(id->text().trimmed(), w->value(), h->value(), level->text().trimmed());
}

void MainWindow::createLinkedActivityInteractive() {
    bool ok = false;
    const double rate = QInputDialog::getDouble(this, "Linked schedule activity", "Development rate (m per working day):", 5.0, 0.01, 1000, 2, &ok);
    if (ok) createLinkedActivityForSelection(rate);
}

} // namespace openmine::app
