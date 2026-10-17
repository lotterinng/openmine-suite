// SPDX-License-Identifier: Apache-2.0
// GUI smoke test: drives the Step-4 vertical slice through the real widgets
// (viewport picking, console, properties editor, scheduler), saves, closes,
// reopens and verifies persistence. Writes screenshots and a log.
//   openmine-desktop --smoke-test <output-dir>
#include "SmokeTest.h"

#include "MainWindow.h"
#include "console/CommandConsole.h"
#include "openmine/geometry/Polyline.h"
#include "project_explorer/ProjectExplorer.h"
#include "properties/PropertiesPanel.h"
#include "ribbon/Ribbon.h"
#include "scheduler/SchedulerPanel.h"
#include "viewport/Viewport3D.h"

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QScreen>
#include <QTextStream>
#include <QThread>
#include <QTreeWidget>
#include <QVTKOpenGLNativeWidget.h>

namespace openmine::app {

namespace {
class Smoke {
public:
    Smoke(MainWindow& w, QString dir) : w_(w), dir_(std::move(dir)), log_(dir_ + "/smoke.log") {
        QDir().mkpath(dir_);
        log_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    }

    void pump(int ms = 50) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5);
        }
    }
    void check(bool ok, const QString& what) {
        QTextStream(&log_) << (ok ? "PASS  " : "FAIL  ") << what << "\n";
        log_.flush();
        if (!ok) ++failures_;
    }
    bool run(const QString& line) {
        const bool ok = w_.console()->execute(line);
        pump();
        QTextStream(&log_) << "      > " << line << "  →  " << w_.console()->lastOutput() << "\n";
        return ok;
    }
    void shot(const QString& name) {
        pump(250);
        w_.viewport()->render();
        QPixmap pm = w_.grab();
        const bool ok = pm.save(dir_ + "/" + name);
        check(ok, "screenshot " + name);
    }
    int failures() const { return failures_; }

    MainWindow& w_;
    QString dir_;
    QFile log_;
    int failures_ = 0;
};
} // namespace

int runSmokeTest(MainWindow& w, const QString& outDir) {
    Smoke s(w, outDir);
    auto& ctx = w.context();
    w.setNonInteractive(true);
    s.pump(300);

    // 1. Create a project.
    s.run("NEW Jupiter Smoke Mine 2026-11-02");
    s.check(ctx.hasProject() && ctx.project().model().info.name == "Jupiter Smoke Mine", "project created");
    s.check(ctx.project().model().info.scheduleStart == Date::fromYmd(2026, 11, 2), "schedule start parsed");
    s.check(w.properties()->setProperty("crs.name", "Hartebeesthoek94 / Lo25"), "CRS name edited through Properties");
    s.check(w.properties()->setProperty("crs.code", "EPSG:2050"), "CRS code edited through Properties");

    // 2a. Draw a polyline interactively in the viewport (simulated clicks on the work plane).
    w.viewport()->setView("top");
    s.pump(100);
    w.viewport()->setTool(Viewport3D::Tool::DrawPolyline);
    const QSize vs = w.viewport()->vtkWidget()->size();
    for (QPoint p : {QPoint(vs.width() / 3, vs.height() / 2), QPoint(vs.width() / 2, vs.height() / 3),
                     QPoint(2 * vs.width() / 3, vs.height() / 2)})
        w.viewport()->simulateClick(p);
    w.viewport()->finishDrawing();
    s.pump();
    s.check(ctx.project().model().geometry.size() == 1, "polyline drawn interactively in the viewport");
    if (ctx.project().model().geometry.size() == 1)
        s.check(ctx.project().model().geometry.all()[0].vertices.size() == 3, "drawn polyline has 3 picked vertices");
    w.viewport()->setTool(Viewport3D::Tool::Select);

    // 2b. Create a decline centreline from the console in Lo-style coordinates (1:8).
    s.check(s.run("POLYLINE -25000,2900000,1200 @-90,0,-11.25 @0,-60,-7.5"), "POLYLINE command");
    s.check(ctx.project().model().geometry.size() == 2, "console polyline exists");
    const Uuid geomId = ctx.project().model().geometry.all().back().id;
    s.check(!geomId.isNil(), "geometry has persistent UUID " + qstr(geomId.toString()));
    const double expectedLen = std::hypot(90.0, 11.25) + std::hypot(60.0, 7.5);
    s.check(std::abs(geometry::length3d(ctx.project().model().geometry.find(geomId)->vertices) - expectedLen) < 1e-9,
            "centreline length = " + formatNumber(expectedLen));

    // 3. View it and select it by picking in the viewport.
    ctx.clearSelection();
    w.viewport()->setView("top");
    s.pump(200);
    const QPoint pick = w.viewport()->worldToWidget({-25045.0, 2900000.0, 1194.375});
    w.viewport()->simulateClick(pick);
    s.pump();
    s.check(ctx.selection().id == geomId, QStringLiteral("viewport pick at (%1,%2) selected the centreline").arg(pick.x()).arg(pick.y()));
    if (ctx.selection().id != geomId) s.run("SELECT " + qstr(geomId.toString()));
    w.viewport()->setView("iso");

    // 4. Edit supported properties through the Properties editor.
    s.check(w.properties()->setProperty("name", "LEG2 675 decline CL"), "rename via Properties");
    s.check(ctx.project().model().geometry.find(geomId)->name == "LEG2 675 decline CL", "name stored in model");
    s.check(!w.properties()->setProperty("v0.x", "not-a-number"), "invalid coordinate rejected");
    s.check(ctx.project().model().geometry.find(geomId)->vertices[0].x == -25000.0, "rejected edit left model unchanged");
    s.check(w.properties()->setProperty("layer", "Development Centrelines"), "layer changed via Properties");

    // 5. Domain object + linked schedule activity.
    s.check(s.run("HEADING LEG2_675_D3_DEC 5.5 5.5 675"), "HEADING command");
    s.check(ctx.project().model().developments.size() == 1, "development heading exists");
    const Uuid devId = ctx.project().model().developments.all()[0].id;
    s.check(s.run("ACTIVITY 5"), "ACTIVITY command");
    s.pump(100);
    s.check(ctx.project().model().activities.size() == 1, "linked activity exists");
    const Uuid actId = ctx.project().model().activities.all()[0].id;
    s.check(ctx.project().model().developmentsForActivity(actId) == std::vector<Uuid>{devId}, "activity ↔ heading link stored");
    s.check(ctx.schedule().ok && std::abs(ctx.schedule().timings.at(actId).duration - expectedLen / 5.0) < 1e-9,
            "CPM duration = length / rate = " + formatNumber(expectedLen / 5.0, 3) + " d");

    // 6. Displayed in the Gantt view.
    w.schedulerDock()->raise();
    s.pump(100);
    s.check(w.scheduler()->itemFor(actId) != nullptr, "activity row present in Gantt grid");
    if (auto* it = w.scheduler()->itemFor(actId)) s.check(!it->text(3).isEmpty() && !it->text(4).isEmpty(), "Gantt row shows start " + it->text(3) + " finish " + it->text(4));
    w.ribbon()->setCurrentIndex(4);
    ctx.select(EntityKind::Activity, actId);
    s.shot("01_vertical_slice_gantt.png");
    w.ribbon()->setCurrentIndex(2);
    ctx.select(EntityKind::Development, devId);
    s.shot("02_heading_properties_3d.png");

    // 7. Progress + design change flow (flag, never silently change).
    ctx.select(EntityKind::Activity, actId);
    s.check(s.run("BASELINE Approved Nov-2026"), "baseline captured");
    s.check(s.run("ACTUAL 40"), "actual advance recorded");
    s.check(s.run("STATUSDATE 2026-11-16"), "status date set");
    ctx.select(EntityKind::Geometry, geomId);
    s.pump();
    s.check(w.properties()->setProperty("v2.y", "2899930"), "centreline vertex edited (design change)");
    const auto* act = ctx.project().model().activities.find(actId);
    s.check(act->designImpactFlag, "linked activity flagged: " + qstr(act->designImpactNote));
    s.check(std::abs(act->quantity - expectedLen) < 1e-9, "activity quantity NOT silently changed");
    ctx.select(EntityKind::Activity, actId);
    s.shot("03_design_change_flagged.png");
    s.check(s.run("APPLYDESIGN"), "controlled recalculation applied");
    act = ctx.project().model().activities.find(actId);
    s.check(!act->designImpactFlag && act->quantity > expectedLen, "quantity updated to " + formatNumber(act->quantity));
    s.check(ctx.project().model().baselines.size() == 1 &&
                std::abs(ctx.project().model().baselines[0].activities[0].quantity - expectedLen) < 1e-9,
            "approved baseline preserved");

    // 8. Save, close, reopen, verify.
    const QString file = s.dir_ + "/JupiterSmoke.openmine";
    QFile::remove(file);
    const auto before = ctx.project().model().geometry.find(geomId)->vertices;
    const double qtyBefore = act->quantity;
    const auto auditBefore = ctx.project().model().audit.size();
    s.check(s.run("SAVE " + file), "SAVE command");
    s.check(!ctx.project().isModified(), "project clean after save");
    s.check(s.run("CLOSE"), "CLOSE command");
    s.check(!ctx.hasProject(), "project closed");
    s.check(s.run("OPEN " + file), "OPEN command");
    s.check(ctx.hasProject(), "project reopened");
    if (ctx.hasProject()) {
        const auto& m = ctx.project().model();
        const auto* g = m.geometry.find(geomId);
        s.check(g != nullptr, "geometry UUID persisted");
        s.check(g && g->vertices == before, "vertex coordinates bit-identical after reopen");
        s.check(g && g->name == "LEG2 675 decline CL", "edited name persisted");
        s.check(m.developments.contains(devId) && m.developments.find(devId)->geometryId == geomId, "heading → geometry link persisted");
        s.check(m.activities.contains(actId) && std::abs(m.activities.find(actId)->quantity - qtyBefore) < 1e-12, "activity persisted");
        s.check(m.developmentsForActivity(actId) == std::vector<Uuid>{devId}, "activity → heading link persisted");
        s.check(m.baselines.size() == 1, "baseline persisted");
        s.check(m.info.crs.authorityCode == "EPSG:2050", "CRS persisted");
        s.check(m.audit.size() >= auditBefore, "audit log persisted (" + QString::number(m.audit.size()) + " entries)");
        s.pump(150);
        s.check(w.scheduler()->itemFor(actId) != nullptr, "Gantt shows the reopened activity");
        s.check(s.run("EXPORTCSV " + s.dir_ + "/schedule.csv") && QFile::exists(s.dir_ + "/schedule.csv"), "schedule CSV exported");
        ctx.select(EntityKind::Development, devId);
        s.shot("04_reopened.png");
        s.run("THEME LIGHT");
        s.shot("05_light_theme.png");
        s.run("THEME DARK");
    }
    s.check(!w.console()->execute("TRIM"), "planned command reports unavailable instead of pretending");

    QTextStream(&s.log_) << "\n" << (s.failures() == 0 ? "SMOKE TEST PASSED" : QString("SMOKE TEST FAILED: %1 failure(s)").arg(s.failures())) << "\n";
    return s.failures() == 0 ? 0 : 1;
}

} // namespace openmine::app
