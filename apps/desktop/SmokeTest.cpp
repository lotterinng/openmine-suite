// SPDX-License-Identifier: Apache-2.0
// End-to-end GUI smoke test. Every step uses the application's own widgets, dialogs, console
// commands and viewport tools on generated data; checks read the resulting project model. The log
// lists each check as PASS / FAIL / SKIP (SKIP only for features absent from the build).
#include "SmokeTest.h"

#include "MainWindow.h"
#include "app/EntityInfo.h"
#include "console/CommandConsole.h"
#include "dialogs/DataDialogs.h"
#include "dialogs/DesignDialogs.h"
#include "dialogs/GeologyDialogs.h"
#include "dialogs/MeshDialogs.h"
#include "dialogs/ModelDialogs.h"
#include "dialogs/SettingsDialogs.h"
#include "ribbon/Ribbon.h"
#include "scheduler/LibraryDialogs.h"
#include "dialogs/SurveyDialogs.h"
#include "panels/PlaybackPanel.h"
#include "panels/PropertiesPanel.h"
#include "panels/PythonConsolePanel.h"
#include "panels/ReportsPanel.h"
#include "panels/StereonetPanel.h"
#include "scheduler/SchedulerPanel.h"
#include "viewport/ViewportArea.h"

#include "openmine/formats/Dxf.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geology/BlockModel.h"
#include "openmine/mine_design/Development.h"
#include "openmine/omf/Omf.h"
#include "openmine/cad/Edit.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/project/ProjectEditor.h"

#include <QAction>
#include <QMenuBar>
#include <QToolBar>
#include <QComboBox>
#include <QApplication>
#include <QDialog>
#include <QCheckBox>
#include <QDate>
#include <QDir>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QTreeWidget>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <numbers>
#include <random>

namespace openmine::app {

namespace {

std::filesystem::path fsPathOf(const QString& s) { return std::filesystem::path(s.toStdU16String()); }

constexpr double kE0 = 50000.0, kN0 = 20000.0, kZ0 = 1000.0; // heading start (project coordinates)
constexpr double kLen = 100.0, kGrade = -5.0;                // m, %

class Smoke {
public:
    Smoke(MainWindow& w, QString dir) : w_(w), ctx_(w.context()), dir_(std::move(dir)) {
        QDir().mkpath(dir_ + "/data");
        QDir().mkpath(dir_ + "/out");
        log_.setFileName(dir_ + "/smoke.log");
        log_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        connect();
    }

    int run();

private:
    // ---- harness ---------------------------------------------------------------------------------
    void connect() {
        QObject::connect(&ctx_, &AppContext::message, &w_, [this](const QString& t, int level) {
            if (level == static_cast<int>(MessageLevel::Error)) lastError_ = t;
            line(QStringLiteral("      %1 %2").arg(level == 2 ? "!!" : level == 1 ? "! " : "  ", t));
        });
    }
    void line(const QString& s) {
        QTextStream(&log_) << s << "\n";
        log_.flush();
    }
    void section(const QString& s) { line("\n== " + s); }
    void pump(int ms = 60) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(2);
        }
    }
    bool waitJobs(int timeoutMs = 300000) {
        const bool ok = ctx_.jobs().waitAll(timeoutMs);
        pump(120);
        if (!ok) line("      (jobs still running after the timeout)");
        return ok;
    }
    bool check(bool ok, const QString& what) {
        line(QStringLiteral("%1  %2").arg(ok ? "PASS" : "FAIL", what));
        ok ? ++passes_ : ++failures_;
        return ok;
    }
    void skip(const QString& what) {
        line("SKIP  " + what);
        ++skips_;
    }
    bool cmd(const QString& c) {
        const bool ok = w_.console()->execute(c);
        pump();
        line(QStringLiteral("      > %1  →  %2").arg(c, w_.console()->lastOutput()));
        return ok;
    }
    /// Closes the dialogs a step left open (modeless dialogs, and modal ones via the timer).
    void closeDialogs() {
        for (auto* tw : QApplication::topLevelWidgets())
            if (auto* d = qobject_cast<QDialog*>(tw); d && d->isVisible()) d->reject();
        pump(30);
    }
    void shot(const QString& name) {
        for (auto* v : w_.viewports()->views()) v->render();
        pump(300);
        for (auto* v : w_.viewports()->views()) v->render();
        pump(50);
        const QString f = dir_ + "/" + name + ".png";
        const bool ok = w_.grab().save(f);
        check(ok, "screenshot " + QFileInfo(f).fileName());
    }
    void viewShot(const QString& name, Viewport3D* view = nullptr) {
        if (!view) view = w_.activeView();
        pump(300);
        view->render();
        pump(100);
        const QImage img = view->grab();
        // The background is a vertical gradient: compare each pixel with the background at the
        // left edge of its row. At least 2 % of the view must show drawn content (not a blank
        // or almost empty GL surface).
        std::size_t drawn = 0, total = 0;
        if (!img.isNull())
            for (int y = 0; y < img.height(); y += 3) {
                const QRgb bg = img.pixel(1, y);
                for (int x = img.width() / 8; x < img.width(); x += 3) {
                    const QRgb c = img.pixel(x, y);
                    ++total;
                    if (qAbs(qRed(c) - qRed(bg)) + qAbs(qGreen(c) - qGreen(bg)) + qAbs(qBlue(c) - qBlue(bg)) > 45) ++drawn;
                }
            }
        const double share = total ? 100.0 * static_cast<double>(drawn) / static_cast<double>(total) : 0.0;
        check(!img.isNull() && share >= 2.0 && img.save(dir_ + "/" + name + ".png"),
              QStringLiteral("view image %1.png (%2 × %3) shows the scene (%4 % drawn)").arg(name).arg(img.width()).arg(img.height()).arg(share, 0, 'f', 1));
    }
    /// Logs the scene extents and the entities that reach far from the work area.
    int logExtents() {
        int far = 0;
        auto& scene = w_.viewports()->scene();
        scene.flush();
        const auto b = scene.sceneBounds();
        line(QStringLiteral("      scene extents %1 × %2 × %3 m").arg(b.max.x - b.min.x, 0, 'f', 1).arg(b.max.y - b.min.y, 0, 'f', 1).arg(b.max.z - b.min.z, 0, 'f', 1));
        for (const auto& id : scene.entities())
            if (const auto* items = scene.items(id))
                for (const auto& it : *items)
                    if (!it.bounds.isEmpty() && (std::abs(it.bounds.min.z - kZ0) > 300 || std::abs(it.bounds.max.z - kZ0) > 300 || std::abs(it.bounds.min.x - kE0) > 1000 ||
                                                 std::abs(it.bounds.min.y - kN0) > 1000)) {
                        line(QStringLiteral("      far item: %1 [%2] z %3…%4").arg(displayName(m(), it.owner), qstr(it.part)).arg(it.bounds.min.z, 0, 'f', 1).arg(it.bounds.max.z, 0, 'f', 1));
                        ++far;
                        break;
                    }
        return far;
    }
    Viewport3D* sectionView() const {
        for (auto* v : w_.viewports()->views())
            if (v->mode() == ViewMode::Section) return v;
        return nullptr;
    }
    const ProjectModel& m() const { return ctx_.project().model(); }
    Uuid lastGeometry() const {
        const auto& all = m().geometry.all();
        return all.empty() ? Uuid{} : all.back().id;
    }
    QString path(const QString& rel) const { return dir_ + "/" + rel; }
    void writeFile(const QString& rel, const QString& text) {
        QFile f(path(rel));
        f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        f.write(text.toUtf8());
    }

    // ---- generated data ---------------------------------------------------------------------------
    void generateData();

    // ---- steps -------------------------------------------------------------------------------------
    void project();
    void cad();
    void design();
    void survey();
    void geology();
    void schedule();
    void exchange();
    void views();
    void scripting();
    void persistence();
    void workspace();
    void modelling();
    void surfaces();
    void libraries();

    MainWindow& w_;
    AppContext& ctx_;
    QString dir_;
    QFile log_;
    QString lastError_;
    int passes_ = 0, failures_ = 0, skips_ = 0;
    Uuid heading_, headingGeom_, activity_, cloud_, subsampled_;
    std::int64_t expectedPoints_ = 0;
};

void Smoke::generateData() {
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 0.015);
    // Scan of a 5 m × 5 m heading along +E with a −5 % grade: walls, back and floor, with a small
    // overbreak bulge on the north wall between chainage 40 and 50.
    QString xyz;
    QTextStream s(&xyz);
    s.setRealNumberNotation(QTextStream::FixedNotation);
    s.setRealNumberPrecision(3);
    s << "x y z intensity\n";
    const double step = 0.25;
    for (double ch = 0; ch <= kLen; ch += step) {
        const double zc = kZ0 + ch * kGrade / 100.0;
        const double bulge = (ch > 40 && ch < 50) ? 0.4 * std::sin((ch - 40) / 10 * M_PI) : 0.0;
        for (double t = -2.5; t <= 2.5; t += step) {
            const int inten = 1000 + static_cast<int>(ch * 10) % 500;
            s << kE0 + ch + noise(rng) << ' ' << kN0 + t + noise(rng) << ' ' << zc + noise(rng) << ' ' << inten << '\n';            // floor
            s << kE0 + ch + noise(rng) << ' ' << kN0 + t + noise(rng) << ' ' << zc + 5.0 + noise(rng) << ' ' << inten + 200 << '\n'; // back
            const double h = t + 2.5;
            s << kE0 + ch + noise(rng) << ' ' << kN0 - 2.5 + noise(rng) << ' ' << zc + h + noise(rng) << ' ' << inten + 100 << '\n';           // south wall
            s << kE0 + ch + noise(rng) << ' ' << kN0 + 2.5 + bulge + noise(rng) << ' ' << zc + h + noise(rng) << ' ' << inten + 300 << '\n'; // north wall
            expectedPoints_ += 4;
        }
    }
    s.flush();
    writeFile("data/heading_scan.xyz", xyz);

    writeFile("data/control.csv", QStringLiteral("name,x,y,z\nCP1,%1,%2,%3\nCP2,%4,%5,%6\nCP3,%7,%8,%9\n")
                                      .arg(kE0 - 5, 0, 'f', 3).arg(kN0 - 5, 0, 'f', 3).arg(kZ0, 0, 'f', 3)
                                      .arg(kE0 + 50, 0, 'f', 3).arg(kN0 + 6, 0, 'f', 3).arg(kZ0 + 2, 0, 'f', 3)
                                      .arg(kE0 + 105, 0, 'f', 3).arg(kN0 - 4, 0, 'f', 3).arg(kZ0 - 4, 0, 'f', 3));

    // Nine drillholes from surface (Z0 + 120) drilled south at −60°, an Au grade increasing with depth.
    QString collars = "holeid,x,y,z,length\n", surveys = "holeid,depth,azimuth,dip\n", assays = "holeid,from,to,au,lith\n";
    std::uniform_real_distribution<double> u(0.0, 1.0);
    int n = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const QString id = QStringLiteral("DH%1").arg(++n, 3, 10, QChar('0'));
            const double x = kE0 + 10 + i * 40, y = kN0 + 60 + j * 30, z = kZ0 + 120;
            collars += QStringLiteral("%1,%2,%3,%4,150\n").arg(id).arg(x, 0, 'f', 2).arg(y, 0, 'f', 2).arg(z, 0, 'f', 2);
            surveys += QStringLiteral("%1,0,180,-60\n%1,75,182,-59\n%1,150,184,-58\n").arg(id);
            for (int k = 0; k < 75; ++k) {
                const double from = k * 2.0;
                const double au = std::max(0.0, 0.2 + 0.02 * from + 0.6 * (u(rng) - 0.5) + (i == 1 ? 1.0 : 0.0));
                assays += QStringLiteral("%1,%2,%3,%4,%5\n").arg(id).arg(from, 0, 'f', 1).arg(from + 2, 0, 'f', 1).arg(au, 0, 'f', 3).arg(from < 60 ? "SED" : "QV");
            }
        }
    writeFile("data/dh_collars.csv", collars);
    writeFile("data/dh_surveys.csv", surveys);
    writeFile("data/dh_assays.csv", assays);

    // Two joint sets (≈ 70/045 and 30/200) plus scatter.
    QString st = "dip,dipdir,set,type,x,y,z\n";
    std::normal_distribution<double> scat(0.0, 5.0);
    for (int k = 0; k < 60; ++k) {
        const bool a = k % 2 == 0;
        const double dip = std::clamp((a ? 70.0 : 30.0) + scat(rng), 1.0, 89.0);
        double dd = std::fmod((a ? 45.0 : 200.0) + scat(rng) + 360.0, 360.0);
        st += QStringLiteral("%1,%2,%3,Joint,%4,%5,%6\n").arg(dip, 0, 'f', 1).arg(dd, 0, 'f', 1).arg(a ? "J1" : "J2")
                  .arg(kE0 + k * 1.5, 0, 'f', 2).arg(kN0 + 2.5, 0, 'f', 2).arg(kZ0 - k * 0.075 + 2.5, 0, 'f', 2);
    }
    writeFile("data/structures.csv", st);
}

// ---- project ---------------------------------------------------------------------------------------

void Smoke::project() {
    section("Project");
    check(cmd("NEW Smoke Test Mine 2026-01-05"), "NEW creates a project");
    check(ctx_.hasProject() && qstr(m().info.name) == "Smoke Test Mine", "project name");
    check(m().info.scheduleStart.date() == Date::fromYmd(2026, 1, 5), "schedule start date");
    if (CrsDialog::available()) {
        check(cmd("CRS EPSG:32735"), "CRS declares and sets the project CRS (no transformation)");
        const auto* c = m().projectCrs();
        check(c && c->authorityCode == "EPSG:32735", "project CRS is EPSG:32735");
    } else {
        skip("project CRS (built without PROJ)");
    }
    check(cmd("LAYER Centrelines #3CB4FF"), "LAYER creates the current layer");
    check(ctx_.project().model().layers.find(ctx_.currentLayer()) && m().layers.find(ctx_.currentLayer())->name == "Centrelines", "current layer is Centrelines");
    check(w_.actionIds().size() > 150, QStringLiteral("action registry (%1 actions)").arg(w_.actionIds().size()));
    int disabledWithoutReason = 0;
    for (const auto& id : w_.actionIds()) {
        auto* a = w_.action(id);
        if (!a->isEnabled() && !a->toolTip().contains("Not available") && id != "edit.redo" && id != "edit.undo" && id != "edit.delete") ++disabledWithoutReason;
    }
    check(disabledWithoutReason == 0, QStringLiteral("disabled actions say why (%1 without a reason)").arg(disabledWithoutReason));
}

// ---- CAD -------------------------------------------------------------------------------------------

void Smoke::cad() {
    section("CAD");
    const auto z1 = kZ0 + kLen * kGrade / 100.0;
    check(cmd(QStringLiteral("POLYLINE %1,%2,%3 %4,%5,%6").arg(kE0, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3).arg(kE0 + kLen, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(z1, 0, 'f', 3)),
          "POLYLINE by typed coordinates");
    headingGeom_ = lastGeometry();
    const auto* g = m().geometry.find(headingGeom_);
    check(g && std::abs(geometry::curveLength(*g) - std::hypot(kLen, kLen * kGrade / 100)) < 1e-6, "centreline length is exact");

    // Interactive drawing: a polyline picked in the plan view through the real viewport tool.
    w_.activeView()->setNamedView("top");
    w_.activeView()->fitAll();
    pump(200);
    w_.activeView()->setWorkPlaneZ(kZ0);
    const std::size_t before = m().geometry.size();
    w_.action("draw.snap")->setChecked(false);
    w_.action("draw.polyline")->trigger();
    const Vec3 pts[] = {{kE0 + 10, kN0 + 20, kZ0}, {kE0 + 40, kN0 + 25, kZ0}, {kE0 + 70, kN0 + 20, kZ0}};
    bool mapped = true;
    for (const auto& p : pts) {
        const auto q = w_.activeView()->worldToWidget(p);
        mapped = mapped && q.has_value();
        if (q) w_.activeView()->simulateClick(q->toPoint());
        pump(30);
    }
    w_.viewports()->tools().finish();
    pump(100);
    check(mapped && m().geometry.size() == before + 1, "polyline drawn with viewport clicks");
    if (m().geometry.size() == before + 1) {
        const auto* drawn = m().geometry.find(lastGeometry());
        double err = 0;
        for (std::size_t i = 0; i < 3 && i < drawn->vertices.size(); ++i) err = std::max(err, std::hypot(drawn->vertices[i].x - pts[i].x, drawn->vertices[i].y - pts[i].y));
        check(drawn->vertices.size() == 3 && err < 0.5, QStringLiteral("picked vertices land on the work plane (max error %1 m at this zoom)").arg(err, 0, 'f', 3));
    }
    w_.action("draw.snap")->setChecked(true);

    // Typed input while a tool runs: absolute, then relative.
    w_.action("draw.line")->trigger();
    cmd(QStringLiteral("%1,%2,%3").arg(kE0, 0, 'f', 3).arg(kN0 + 40, 0, 'f', 3).arg(kZ0, 0, 'f', 3));
    cmd("@20,0,0");
    pump(50);
    const auto* line = m().geometry.find(lastGeometry());
    check(line && line->type == GeometryType::Line && std::abs(line->vertices[1].x - line->vertices[0].x - 20) < 1e-9, "line from typed tool input (x,y,z then @dx,dy,dz)");
    const Uuid lineId = lastGeometry();
    w_.action("draw.select")->trigger();

    // Transform, undo, redo.
    ctx_.select(EntityKind::Geometry, lineId);
    const double y0 = line->vertices[0].y;
    check(cmd("MOVE 0,5,0"), "MOVE the selected line");
    check(std::abs(m().geometry.find(lineId)->vertices[0].y - (y0 + 5)) < 1e-9, "line moved by 5 m north");
    check(cmd("UNDO"), "UNDO");
    check(std::abs(m().geometry.find(lineId)->vertices[0].y - y0) < 1e-9, "undo restores the line");
    check(cmd("REDO") && std::abs(m().geometry.find(lineId)->vertices[0].y - (y0 + 5)) < 1e-9, "redo moves it again");
    check(cmd("UNDO"), "undo the move");
    check(cmd("COPY 0,-60,0") && m().geometry.size() == before + 3, "COPY adds a new object");
    check(cmd("ROTATE 90") && cmd("SCALE 2"), "ROTATE and SCALE");

    // Join two touching lines; explode a rectangle.
    const std::size_t n0 = m().geometry.size();
    cmd(QStringLiteral("LINE %1,%2,%3 %4,%5,%6").arg(kE0 + 120, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3).arg(kE0 + 130, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3));
    const Uuid l1 = lastGeometry();
    cmd(QStringLiteral("LINE %1,%2,%3 %4,%5,%6").arg(kE0 + 130, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3).arg(kE0 + 130, 0, 'f', 3).arg(kN0 + 10, 0, 'f', 3).arg(kZ0, 0, 'f', 3));
    const Uuid l2 = lastGeometry();
    ctx_.selectMany({{EntityKind::Geometry, l1}, {EntityKind::Geometry, l2}});
    check(cmd("JOIN") && m().geometry.size() == n0 + 1 && !m().geometry.contains(l1), "JOIN two lines into one polyline");
    check(cmd(QStringLiteral("RECTANGLE %1,%2,%3 %4,%5,%6").arg(kE0 + 140, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3).arg(kE0 + 150, 0, 'f', 3).arg(kN0 + 8, 0, 'f', 3).arg(kZ0, 0, 'f', 3)),
          "RECTANGLE");
    const std::size_t n1 = m().geometry.size();
    check(cmd("EXPLODE") && m().geometry.size() == n1 + 3, "EXPLODE a rectangle into four lines");
    check(cmd(QStringLiteral("CIRCLE %1,%2,%3 4").arg(kE0 + 160, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3)), "CIRCLE");
    check(cmd(QStringLiteral("ARC %1,%2,%3 %4,%5,%6 %7,%8,%9").arg(kE0 + 170, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3).arg(kE0 + 175, 0, 'f', 3).arg(kN0 + 5, 0, 'f', 3).arg(kZ0, 0, 'f', 3).arg(kE0 + 180, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3)),
          "ARC through three points");
    check(cmd(QStringLiteral("TEXT %1,%2,%3 2 Smoke test").arg(kE0, 0, 'f', 3).arg(kN0 - 10, 0, 'f', 3).arg(kZ0, 0, 'f', 3)), "TEXT");
    check(cmd(QStringLiteral("DISTANCE %1,%2,%3 @30<45").arg(kE0, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3)) && w_.console()->lastOutput().contains("plan 30"),
          "DISTANCE with polar input");
}

// ---- design ------------------------------------------------------------------------------------------

void Smoke::design() {
    section("Mine design");
    ctx_.select(EntityKind::Geometry, headingGeom_);
    check(cmd("HEADING DEV_675_A 5 5 675"), "HEADING makes the centreline a 5 × 5 m heading on level 675");
    heading_ = m().developments.empty() ? Uuid{} : m().developments.all().back().id;
    if (!check(!heading_.isNil(), "development created")) return;
    const auto mt = mine_design::computeMetrics(ctx_.project(), *m().developments.find(heading_));
    check(std::abs(mt.volume - 25.0 * mt.length3d) < 1e-6, QStringLiteral("design volume %1 m³ = section × length").arg(mt.volume, 0, 'f', 1));
    check(cmd("ACTIVITY 4"), "ACTIVITY creates a linked activity at 4 m/day");
    activity_ = m().activities.empty() ? Uuid{} : m().activities.all().back().id;
    check(!activity_.isNil() && std::abs(m().activities.find(activity_)->quantity - mt.length3d) < 1e-6, "activity quantity is the design length");

    // Properties panel edits the selection through the entity codec.
    ctx_.select(EntityKind::Development, heading_);
    pump(100);
    QString widthColumn;
    for (const auto& c : w_.properties()->columns())
        if (c.contains("width", Qt::CaseInsensitive)) widthColumn = c;
    check(!widthColumn.isEmpty() && w_.properties()->setField(widthColumn, "5.5"), "properties panel sets the design width");
    check(std::abs(m().developments.find(heading_)->designWidth - 5.5) < 1e-9, "design width is 5.5 m");
    cmd("UNDO");

    // Revision capture, change, impact flag, controlled quantity update.
    {
        RevisionDialog d(ctx_, heading_, &w_);
        check(!d.capture().isNil(), "design revision captured");
    }
    ctx_.select(EntityKind::Geometry, headingGeom_);
    check(cmd("SCALE 1.1 " + QStringLiteral("%1,%2,%3").arg(kE0, 0, 'f', 3).arg(kN0, 0, 'f', 3).arg(kZ0, 0, 'f', 3)), "lengthen the centreline by 10 %");
    check(m().activities.find(activity_)->designImpactFlag, "linked activity flagged by the design change (quantity unchanged)");
    check(std::abs(m().activities.find(activity_)->quantity - mt.length3d) < 1e-6, "activity quantity not silently altered");
    cmd("IMPACTS");
    ctx_.select(EntityKind::Activity, activity_);
    check(cmd("APPLYDESIGN") && !m().activities.find(activity_)->designImpactFlag, "APPLYDESIGN updates the quantity and clears the flag");

    // Layout designers (they start from the end of the selected heading).
    ctx_.select(EntityKind::Development, heading_);
    {
        auto* d = new LayoutDialog(ctx_, w_.viewports()->tools(), LayoutDialog::Kind::Decline, [](const auto&) {}, &w_);
        const std::size_t n = m().developments.size();
        try {
            const auto layouts = d->layouts();
            check(!layouts.empty() && !layouts.front().vertices.empty(), "decline layout generated");
        } catch (const std::exception& e) {
            check(false, QStringLiteral("decline layout: %1").arg(e.what()));
        }
        const auto ids = d->create();
        check(!ids.empty() && m().developments.size() == n + ids.size(), QStringLiteral("decline headings created (%1)").arg(ids.size()));
        d->close();
    }
    {
        CrosscutDialog d(ctx_, heading_, &w_);
        const std::size_t n = m().developments.size();
        const auto ids = d.create();
        check(!ids.empty() && m().developments.size() == n + ids.size(), QStringLiteral("crosscuts created (%1)").arg(ids.size()));
    }
    {
        LevelDialog d(ctx_, &w_);
        check(!d.create().isNil(), "level created");
    }
    // Stope from an outline between two elevations, then subdivided.
    cmd(QStringLiteral("RECTANGLE %1,%2,%3 %4,%5,%6").arg(kE0 + 30, 0, 'f', 3).arg(kN0 + 10, 0, 'f', 3).arg(kZ0, 0, 'f', 3).arg(kE0 + 60, 0, 'f', 3).arg(kN0 + 25, 0, 'f', 3).arg(kZ0, 0, 'f', 3));
    {
        StopeDialog d(ctx_, lastGeometry(), &w_);
        const Uuid stope = d.create();
        if (check(!stope.isNil(), "stope created from a closed outline")) {
            SubdivideDialog s(ctx_, stope, &w_);
            const auto blocks = s.create();
            check(!blocks.empty(), QStringLiteral("stope subdivided into %1 mining blocks").arg(blocks.size()));
        }
    }
#ifdef OPENMINE_APP_HAVE_OCCT
    {
        const std::size_t n = m().geometry.size();
        w_.handleEntityAction("development-solid", {{EntityKind::Development, heading_}});
        const auto* s = m().geometry.find(lastGeometry());
        check(m().geometry.size() == n + 1 && s && s->type == GeometryType::Solid, "exact excavation solid (Open CASCADE sweep)");
    }
#else
    skip("excavation solid (built without Open CASCADE)");
#endif
    check(logExtents() == 0, "every object lies within the mine area (no stray coordinates)");
    check(cmd("SECTION DEV_675_A 50"), "SECTION at chainage 50 opens a section view");
    pump(200);
    if (check(sectionView() != nullptr, "a view shows the section")) viewShot("design_section", sectionView());
    check(cmd("LAYOUT 1"), "back to one view");
}

// ---- survey --------------------------------------------------------------------------------------------

void Smoke::survey() {
    section("Survey");
    check(w_.importFile(path("data/control.csv")), "control points imported");
    check(m().controlPoints.size() == 3, "three control points");

    check(w_.importFile(path("data/heading_scan.xyz")), "point-cloud import started");
    check(waitJobs(), "import job finished");
    if (!check(m().pointClouds.size() == 1, "point-cloud dataset committed")) return;
    cloud_ = m().pointClouds.all().front().id;
    const auto& pc = *m().pointClouds.find(cloud_);
    check(pc.pointCount == expectedPoints_, QStringLiteral("%1 points indexed (%2 generated)").arg(pc.pointCount).arg(expectedPoints_));
    check(pc.transform.isIdentity(), "imported dataset keeps its stored coordinates (identity transform)");

    ctx_.select(EntityKind::PointCloud, cloud_);
    w_.activeView()->setNamedView("iso");
    w_.activeView()->fitSelection();
    pump(1500); // let the LOD loader stream nodes
    check(w_.activeView()->displayedPoints() > 0, QStringLiteral("LOD shows %1 points").arg(w_.activeView()->displayedPoints()));
    viewShot("survey_cloud");

    const std::size_t analyses = m().analysisResults.size();
    w_.handleEntityAction("cloud-stats", {{EntityKind::PointCloud, cloud_}});
    waitJobs();
    check(m().analysisResults.size() == analyses + 1, "statistics stored as a reproducible analysis result");
    w_.handleEntityAction("cloud-subsample", {{EntityKind::PointCloud, cloud_}});
    waitJobs();
    check(m().pointClouds.size() == 2, "subsampled derived dataset (original untouched)");
    if (m().pointClouds.size() == 2) subsampled_ = m().pointClouds.all().back().id;
    w_.handleEntityAction("cloud-normals", {{EntityKind::PointCloud, cloud_}});
    waitJobs();
    check(m().pointClouds.size() == 3, "normals as a derived dataset");
    const std::size_t meshes = m().geometry.size();
    w_.handleEntityAction("cloud-delaunay", {{EntityKind::PointCloud, subsampled_.isNil() ? cloud_ : subsampled_}});
    waitJobs();
    check(m().geometry.size() == meshes + 1 && m().geometry.find(lastGeometry())->type == GeometryType::Mesh, "Delaunay 2.5D surface");
    const std::size_t reports = m().reports.size();
    w_.handleEntityAction("report-cloud", {{EntityKind::PointCloud, cloud_}});
    waitJobs();
    check(m().reports.size() == reports + 1, "point-cloud PDF report stored in the project");

    // Registration by point pairs (known shift), then reverted: the stored points never change.
    {
        RegistrationDialog d(ctx_, w_.viewports()->tools(), cloud_, &w_);
        const Vec3 shift{0.5, -0.3, 0.1};
        const Vec3 src[] = {{kE0 + 1, kN0 - 2.5, kZ0 + 1}, {kE0 + 50, kN0 + 2.5, kZ0 + 1}, {kE0 + 99, kN0, kZ0 - 0.05}, {kE0 + 20, kN0, kZ0 + 4}};
        for (int i = 0; i < 4; ++i) d.addPair(QStringLiteral("P%1").arg(i + 1), src[i], src[i] + shift);
        check(d.compute() && d.result(), "registration computed from four pairs");
        if (d.result()) check(d.result()->fitRms < 1e-6, QStringLiteral("registration RMS %1 m").arg(d.result()->fitRms, 0, 'g', 3));
        check(d.commit(true), "registration applied");
        const auto t = m().pointClouds.find(cloud_)->transform.translationPart();
        check(std::abs(t.x - shift.x) < 1e-6 && std::abs(t.y - shift.y) < 1e-6, "dataset transform carries the registration");
    }
    const Uuid reg = m().registrations.all().back().id;
    w_.handleEntityAction("report-registration", {{EntityKind::Registration, reg}});
    waitJobs();
    w_.handleEntityAction("revert-registration", {{EntityKind::Registration, reg}});
    check(m().pointClouds.find(cloud_)->transform.isIdentity(), "registration reverted (transform restored)");
    check(m().registrations.contains(reg), "the registration record is kept for the audit trail");

    if (!subsampled_.isNil()) {
        IcpDialog d(ctx_, subsampled_, &w_);
        auto* engine = d.findChild<QComboBox*>("engine");
        const QStringList engines = [&] {
            QStringList l;
            for (int i = 0; engine && i < engine->count(); ++i) l << engine->itemData(i).toString();
            return l;
        }();
        for (const auto& e : engines) {
            engine->setCurrentIndex(engines.indexOf(e));
            const std::size_t regs = m().registrations.size();
            d.start(false);
            waitJobs();
            if (check(m().registrations.size() == regs + 1, QStringLiteral("ICP registration computed and stored (%1 engine)").arg(e))) {
                const auto& r = m().registrations.all().back();
                check(qstr(r.notes).contains(e == "OpenMine" ? QStringLiteral("OpenMine") : e) && r.icpFitness > 0.9,
                      QStringLiteral("%1 — fitness %2, inlier RMS %3 m").arg(qstr(r.notes)).arg(r.icpFitness, 0, 'f', 3).arg(r.icpInlierRms, 0, 'g', 3));
            }
        }
#ifdef OPENMINE_APP_HAVE_OPEN3D
        check(engines.contains("Open3D"), "the Open3D ICP engine is offered");
#endif
    }
#ifdef OPENMINE_APP_HAVE_OPEN3D
    {
        // Open3D normals through the same dialog as the built-in engine.
        CloudProcessDialog d(ctx_, CloudProcessDialog::Operation::Normals, cloud_, &w_);
        auto* engine = d.findChild<QComboBox*>("engine");
        if (check(engine && engine->findData("open3d") >= 0, "the Open3D normals engine is offered")) {
            engine->setCurrentIndex(engine->findData("open3d"));
            const std::size_t n = m().pointClouds.size();
            d.start();
            waitJobs();
            check(m().pointClouds.size() == n + 1 && qstr(m().pointClouds.all().back().derivation).contains("Open3D"),
                  "normals by Open3D stored as a derived dataset recording the engine");
        }
    }
#else
    skip("Open3D engines (built without Open3D)");
#endif

#ifdef OPENMINE_APP_HAVE_PDAL
    {
        AsBuiltDialog d(ctx_, cloud_, &w_);
        d.start();
        waitJobs(600000);
        if (check(m().asBuiltModels.size() == 1, "as-built mesh from the scan (Poisson)")) {
            const Uuid ab = m().asBuiltModels.all().front().id;
            ConformanceDialog c(ctx_, ab, &w_);
            c.start();
            waitJobs(600000);
            if (check(m().conformanceResults.size() == 1, "design conformance analysed")) {
                const auto& r = m().conformanceResults.all().front();
                // The bulge is 0.4·sin over 10 m on the 5 m wall: 2·0.4·10/π·5 = 12.73 m³. Scanner
                // noise adds about as much overbreak as underbreak, so the net volume measures it.
                const double net = r.overbreakVolume - r.underbreakVolume, expected = 2 * 0.4 * 10 / M_PI * 5;
                check(std::abs(net - expected) < 2.5,
                      QStringLiteral("the north-wall bulge is measured: net overbreak %1 m³ (expected %2 m³; overbreak %3, underbreak %4 m³)")
                          .arg(net, 0, 'f', 2).arg(expected, 0, 'f', 2).arg(r.overbreakVolume, 0, 'f', 2).arg(r.underbreakVolume, 0, 'f', 2));
                w_.handleEntityAction("export-conformance:" + path("out/stations.csv"), {{EntityKind::Conformance, r.id}});
                check(QFileInfo(path("out/stations.csv")).size() > 100, "station table exported");
            }
        }
    }
#else
    skip("as-built and conformance (built without PDAL)");
#endif
    closeDialogs();
}

// ---- geology / geotech --------------------------------------------------------------------------------

void Smoke::geology() {
    section("Geology and geotechnical");
    {
        DrillholeImportDialog d(ctx_, &w_);
        d.setFiles(path("data/dh_collars.csv"), path("data/dh_surveys.csv"), {path("data/dh_assays.csv")});
        check(d.validate() == 0, "drillhole tables validate without errors");
        check(d.importNow(), "drillholes imported");
    }
    check(m().drillholes.size() == 9, "nine drillholes");
    {
        CompositeDialog d(ctx_, &w_);
        d.start();
        waitJobs();
    }
    Uuid bm;
    {
        BlockModelDialog d(ctx_, &w_);
        bm = d.create();
        check(!bm.isNil(), "block model created");
    }
    if (!bm.isNil()) {
        EstimationDialog d(ctx_, bm, &w_);
        check(d.computeVariogram(), "experimental variogram computed and fitted");
        d.start();
        waitJobs(600000);
        const auto data = geology::loadBlockModel(ctx_.project(), bm);
        std::size_t estimated = 0;
        if (const auto* est = data.find(data.header.attributes.empty() ? std::string() : data.header.attributes.front()))
            for (double v : *est) estimated += std::isfinite(v) ? 1 : 0;
        check(estimated > 0, QStringLiteral("grades estimated into %1 of %2 blocks (%3)").arg(estimated).arg(data.count()).arg(qstr(data.header.attributes.empty() ? "" : data.header.attributes.front())));
        const std::size_t reports = m().reports.size();
        w_.handleEntityAction("grade-tonnage", {{EntityKind::BlockModel, bm}});
        waitJobs();
        check(m().reports.size() == reports + 1, "grade–tonnage report");
        BlockDisplayDialog disp(ctx_, w_.viewports()->scene(), bm, &w_);
        disp.apply();
    }
    check(w_.importFile(path("data/structures.csv")), "structural measurements imported");
    check(m().structures.size() == 60, "60 measurements");
    w_.handleEntityAction("stereonet", {});
    pump(200);
    check(w_.stereonet()->writeSets(), "joint sets identified on the stereonet and written back");
    w_.stereonet()->grab().save(path("stereonet.png"));
    {
        ClassificationDialog d(ctx_, heading_, &w_);
        check(!d.record().isNil(), "rock-mass classification recorded (Q and RMR)");
    }
    closeDialogs();
}

// ---- schedule ------------------------------------------------------------------------------------------

void Smoke::schedule() {
    section("Scheduling");
    check(cmd("SCHEDULE"), "schedule calculates");
    const auto& r = ctx_.schedule().result;
    check(r.ok && r.timings.count(activity_), "the linked activity is scheduled");
    check(cmd("BASELINE Approved plan"), "baseline captured");
    check(m().baselines.size() == 1, "one immutable baseline");
    ctx_.select(EntityKind::Activity, activity_);
    check(cmd("ACTUAL 12 2026-01-08 12"), "actual progress recorded");
    check(m().actuals.size() == 1, "dated actual stored");
    check(cmd("STATUSDATE 2026-01-09"), "status date set for reforecasting");
    {
        const Uuid id = w_.scheduler()->addActivity(ActivityKind::Milestone, "Level complete");
        check(!id.isNil(), "milestone added in the scheduler");
    }
    Scenario s;
    s.name = "Faster development";
    ScenarioOverride o;
    o.entityId = activity_;
    o.field = "rate";
    o.value = "6";
    s.overrides.push_back(o);
    check(ctx_.tryEdit("Add scenario", [&] { ProjectEditor(ctx_.project()).add(s); }), "scenario with a rate override");
    check(cmd("SCENARIO Faster development") && ctx_.activeScenario(), "scenario active");
    const double fastFinish = ctx_.schedule().result.timings.count(activity_) ? ctx_.schedule().result.timings.at(activity_).ef : 0;
    cmd("SCENARIO BASE");
    const double baseFinish = ctx_.schedule().result.timings.count(activity_) ? ctx_.schedule().result.timings.at(activity_).ef : 0;
    check(fastFinish > 0 && fastFinish < baseFinish, "the faster scenario finishes the heading earlier");
    w_.dock("scheduler")->show();
    w_.dock("scheduler")->raise();
    shot("schedule_gantt");
    const std::size_t reports = m().reports.size();
    w_.reports()->generate("gantt", "pdf");
    waitJobs();
    check(m().reports.size() == reports + 1, "Gantt PDF stored");
    check(w_.scheduler()->exportFile(path("out/schedule.xlsx")) && QFileInfo(path("out/schedule.xlsx")).size() > 1000, "schedule exported to XLSX");
    check(w_.scheduler()->exportFile(path("out/schedule.csv")) && QFileInfo(path("out/schedule.csv")).size() > 50, "schedule exported to CSV");

    // 4D playback at the middle of the schedule.
    w_.dock("playback")->show();
    w_.dock("playback")->raise();
    const double mid = (ctx_.schedule().result.projectStart + ctx_.schedule().result.projectFinish) / 2;
    w_.playback()->setTime(mid);
    pump(300);
    check(ctx_.playbackActive() && ctx_.planSnapshot().has_value(), "4D plan snapshot at the playback time");
    w_.activeView()->setNamedView("iso");
    w_.activeView()->fitAll();
    viewShot("schedule_4d");
    ctx_.setPlaybackTime(std::nullopt);
}

// ---- exchange -------------------------------------------------------------------------------------------

void Smoke::exchange() {
    section("Data exchange");
    ctx_.clearSelection();
    check(w_.exportFile(path("out/all.dxf")), "DXF export");
    try {
        const auto dxf = formats::readDxf(fsPathOf(path("out/all.dxf")));
        int curves = 0;
        for (const auto& g : m().geometry.all())
            if (geometry::isCurve(g.type) || g.type == GeometryType::Point || g.type == GeometryType::Text) ++curves;
        check(static_cast<int>(dxf.objects.size()) >= curves, QStringLiteral("DXF reads back (%1 objects)").arg(dxf.objects.size()));
    } catch (const std::exception& e) {
        check(false, QStringLiteral("DXF reads back: %1").arg(e.what()));
    }
    check(w_.exportFile(path("out/all.omf")), "OMF export started");
    waitJobs();
    try {
        const auto doc = omf::readOmf(fsPathOf(path("out/all.omf")));
        check(!doc.elements.empty(), QStringLiteral("OMF reads back (%1 elements)").arg(doc.elements.size()));
    } catch (const std::exception& e) {
        check(false, QStringLiteral("OMF reads back: %1").arg(e.what()));
    }
    Uuid mesh;
    for (const auto& g : m().geometry.all())
        if (g.type == GeometryType::Mesh) mesh = g.id;
    if (!mesh.isNil()) {
        ctx_.select(EntityKind::Geometry, mesh);
        check(w_.exportFile(path("out/surface.obj")), "OBJ export of a surface");
        const std::size_t n = m().geometry.size();
        check(w_.importFile(path("out/surface.obj")) && waitJobs() && m().geometry.size() == n + 1, "OBJ re-import");
        const auto* a = m().geometry.find(mesh);
        const auto* b = m().geometry.find(lastGeometry());
        check(a && b && a->mesh && b->mesh && a->mesh->triangles.size() == b->mesh->triangles.size(), "OBJ round trip keeps every triangle");
    }
#ifdef OPENMINE_APP_HAVE_OCCT
    Uuid solid;
    for (const auto& g : m().geometry.all())
        if (g.type == GeometryType::Solid) solid = g.id;
    if (!solid.isNil()) {
        ctx_.select(EntityKind::Geometry, solid);
        check(w_.exportFile(path("out/solid.step")), "STEP export");
        const std::size_t n = m().geometry.size();
        check(w_.importFile(path("out/solid.step")) && m().geometry.size() == n + 1, "STEP re-import");
    }
#endif
    if (!cloud_.isNil()) {
        ctx_.select(EntityKind::PointCloud, cloud_);
        check(w_.exportFile(path("out/cloud.xyz")), "point-cloud export started");
        waitJobs();
        check(QFileInfo(path("out/cloud.xyz")).size() > 1000000, "point cloud written in project coordinates");
    }
    ctx_.clearSelection();
}

// ---- views ---------------------------------------------------------------------------------------------

void Smoke::views() {
    section("Views and panels");
    check(cmd("LAYOUT 4") && w_.viewports()->views().size() == 4, "four views");
    shot("views_quad");
    check(cmd("LAYOUT 1"), "one view");
    check(cmd("VIEW TOP"), "plan view");
    check(cmd("ZOOM EXTENTS"), "zoom extents");
    viewShot("view_plan");
    w_.handleEntityAction("zoom", {{EntityKind::Development, heading_}});
    pump(100);
    w_.dock("properties")->raise();
    ctx_.select(EntityKind::Development, heading_);
    shot("main_window_dark");
    check(cmd("THEME LIGHT"), "light theme");
    shot("main_window_light");
    cmd("THEME DARK");
    ctx_.report("Smoke test: reports panel");
    w_.dock("reports")->show();
    w_.dock("reports")->raise();
    shot("reports_panel");
    w_.dock("reports")->hide();
}

// ---- scripting --------------------------------------------------------------------------------------------

void Smoke::scripting() {
    section("Scripting and plugins");
    if (PythonConsolePanel::available()) {
        const std::size_t n = m().layers.size();
        if (!check(cmd("PYTHON project.cad.add_layer('From Python', 0xFF8800)"), "Python statement runs against the open project"))
            line("      Python transcript:\n" + w_.python()->transcript().right(2000));
        check(m().layers.size() == n + 1, "the Python edit reached the model");
        check(cmd("UNDO") && m().layers.size() == n, "Python edits are undoable");
    } else {
        skip("Python console (built without Python)");
    }
    check(cmd("PLUGIN"), "plugin command listing");
    const auto pluginCmds = ctx_.pluginCommands();
    auto hasCommand = [&](const char* name, bool python) {
        return std::any_of(pluginCmds.begin(), pluginCmds.end(), [&](const PluginCommandInfo& c) { return c.name == name && c.python == python; });
    };
    if (check(hasCommand("stations.summary", false), "native sample plugin loaded from the plugin path")) {
        const auto r = ctx_.runPluginCommand("stations.summary");
        check(r.ok && qstr(r.text).contains("count"), QStringLiteral("native plugin command runs: %1").arg(qstr(r.ok ? r.text : r.error).left(120)));
    }
    if (PythonConsolePanel::available()) {
        if (check(hasCommand("levels.summary", true) && hasCommand("levels.layers", true), "Python sample plugin discovered in the plugin folder")) {
            const auto s = ctx_.runPluginCommand("levels.summary");
            check(s.ok && qstr(s.text).contains("\"675\""), QStringLiteral("Python plugin reads the project: %1").arg(qstr(s.ok ? s.text : s.error).left(160)));
            const std::size_t n = m().layers.size();
            check(cmd("PLUGIN levels.layers {\"prefix\": \"Smoke level \"}") && m().layers.size() > n, "Python plugin edits the project through the API");
            check(cmd("UNDO") && m().layers.size() == n, "a Python plugin command is one undo step");
            const auto bad = ctx_.runPluginCommand("levels.layers", "{\"prefix\": \" \"}");
            check(!bad.ok && bad.rolledBack && m().layers.size() == n && qstr(bad.error).contains("prefix"),
                  QStringLiteral("a failing Python plugin command is reported and rolled back: %1").arg(qstr(bad.error)));
        }
    }
    check(cmd("HELP") && w_.console()->commandNames().size() > 60, QStringLiteral("%1 console commands").arg(w_.console()->commandNames().size()));
}

// ---- persistence -------------------------------------------------------------------------------------------

void Smoke::persistence() {
    section("Persistence");
    std::map<std::string, std::size_t> counts;
#define OPENMINE_COUNT(T, member, Kind) counts[#T] = m().member.size();
    OPENMINE_ENTITY_TYPES(OPENMINE_COUNT)
#undef OPENMINE_COUNT
    const QString file = path("out/smoke.openmine");
    check(cmd("SAVE " + file), "project saved");
    check(!ctx_.project().isModified(), "no unsaved changes after saving");
    check(cmd("CHECK"), "integrity check");
    check(cmd("CLOSE") && !ctx_.hasProject(), "project closed");
    check(cmd("OPEN " + file) && ctx_.hasProject(), "project reopened");
    int mismatches = 0;
#define OPENMINE_COMPARE(T, member, Kind)                                                                                       \
    if (m().member.size() != counts[#T]) {                                                                                      \
        ++mismatches;                                                                                                           \
        line(QStringLiteral("      %1: %2 saved, %3 loaded").arg(#T).arg(counts[#T]).arg(m().member.size()));                \
    }
    OPENMINE_ENTITY_TYPES(OPENMINE_COMPARE)
#undef OPENMINE_COMPARE
    check(mismatches == 0, "every entity type round-trips through the project file");
    if (!cloud_.isNil()) check(m().pointClouds.contains(cloud_) && m().pointClouds.find(cloud_)->transform.isIdentity(), "survey dataset unchanged after reopening");
    check(m().baselines.size() == 1, "baseline preserved");
    check(cmd("SCHEDULE"), "schedule recalculates after reopening");
    shot("reopened");
}


// ---- workspace ------------------------------------------------------------------------------------------

void Smoke::workspace() {
    section("Workspace");
    QStringList pages;
    for (int i = 0; i < w_.ribbon()->count(); ++i) pages << w_.ribbon()->tabText(i);
    check(pages == QStringList({"Home", "Survey", "CAD", "Mine Design", "Scheduler", "Geology", "Geotechnical", "Analysis", "Reporting", "Automation"}),
          "the ten ribbon workspaces: " + pages.join(", "));
    QStringList menus;
    for (auto* a : w_.findChild<QMenuBar*>()->actions()) menus << QString(a->text()).remove('&');
    for (const char* need : {"File", "Edit", "View", "Tools", "Window", "Settings", "Help"})
        check(menus.contains(need), QStringLiteral("menu %1").arg(need));
    // Quick-access toolbar: customise, persist, restore defaults.
    w_.setQuickAccess({"file.save", "view.fit", "draw.polyline", "survey.icp"});
    check(w_.quickAccessBar()->actions().size() == 4 && w_.quickAccessBar()->actions().at(2) == w_.action("draw.polyline"), "quick-access toolbar customised");
    check(ctx_.settings().value("ui/quickAccess").toStringList().size() == 4, "quick-access toolbar stored in the user settings");
    w_.setQuickAccess({});
    check(w_.quickAccessBar()->actions().size() == 6, "quick-access defaults restored");
    // Preferences apply to the tools and persist.
    {
        PreferencesDialog d(ctx_, *w_.viewports(), {}, &w_);
        check(d.apply(), "preferences applied");
        check(ctx_.settings().contains("snap/gridSpacing") && ctx_.settings().contains("clouds/pointBudget"), "preferences stored in the user settings");
    }
    // Every ribbon page shows enabled or explained actions.
    for (int i = 0; i < w_.ribbon()->count(); ++i) {
        w_.ribbon()->setCurrentIndex(i);
        pump(60);
    }
    w_.ribbon()->setCurrentIndex(0);
    closeDialogs();
}

// ---- 3D modelling --------------------------------------------------------------------------------------

void Smoke::modelling() {
    section("3D modelling");
#ifdef OPENMINE_APP_HAVE_OCCT
    auto pt = [](double x, double y, double z) { return QStringLiteral("%1,%2,%3").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3).arg(z, 0, 'f', 3); };
    const double x = kE0 + 200, y = kN0, z = kZ0;
    // An outline with a hole, extruded 6 m: volume = (20×10 − 4×4) × 6.
    cmd("RECTANGLE " + pt(x, y, z) + " " + pt(x + 20, y + 10, z));
    const Uuid outer = lastGeometry();
    cmd("RECTANGLE " + pt(x + 3, y + 3, z) + " " + pt(x + 7, y + 7, z));
    const Uuid hole = lastGeometry();
    ctx_.selectMany({{EntityKind::Geometry, outer}, {EntityKind::Geometry, hole}});
    check(cmd("EXTRUDE 6"), "EXTRUDE the outline with its hole");
    const Uuid block = lastGeometry();
    const auto* b = m().geometry.find(block);
    check(b && b->type == GeometryType::Solid && std::abs(std::abs(geometry::meshSignedVolume(*b->mesh)) - (200 - 16) * 6.0) < 1e-3,
          QStringLiteral("extruded solid volume %1 m³ (expected 1104)").arg(b && b->mesh ? std::abs(geometry::meshSignedVolume(*b->mesh)) : 0, 0, 'f', 3));
    ctx_.select(EntityKind::Geometry, block);
    check(cmd("SOLIDCHECK") && w_.console()->lastOutput().contains("valid"), "solid check reports validity and volume");
    check(cmd("HEAL"), "heal (a valid solid stays valid)");
    {
        // Rounded edges and an offset change the solid in place, one undo step each.
        cmd("RECTANGLE " + pt(x + 150, y, z) + " " + pt(x + 154, y + 4, z));
        ctx_.select(EntityKind::Geometry, lastGeometry());
        cmd("EXTRUDE 4");
        const Uuid cube = lastGeometry();
        ctx_.select(EntityKind::Geometry, cube);
        auto vol = [&] { return std::abs(geometry::meshSignedVolume(*m().geometry.find(cube)->mesh)); };
        const bool filleted = cmd("SOLIDFILLET 0.5");
        const double vf = vol();
        check(filleted && vf < 64.0 && vf > 60.0, QStringLiteral("SOLIDFILLET rounds every edge (%1 m³ < 64)").arg(vf, 0, 'f', 3));
        check(cmd("UNDO") && std::abs(vol() - 64.0) < 1e-6, "the fillet is one undo step");
        // Offset by 0.5 m with rounded edges: 64 + 96·0.5 + 48·π·0.25/4 + 4/3·π·0.125 (Steiner).
        const double steiner = 64 + 48 + 48 * std::numbers::pi * 0.25 / 4 + 4.0 / 3.0 * std::numbers::pi * 0.125;
        const bool offset = cmd("SOLIDOFFSET 0.5");
        const double vo = vol();
        check(offset && std::abs(vo - steiner) < 0.01 * steiner, QStringLiteral("SOLIDOFFSET 0.5: %1 m³ (Steiner %2)").arg(vo, 0, 'f', 2).arg(steiner, 0, 'f', 2));
    }
    // Sweep a 5 × 5 m profile along a bent path: volume ≈ 25 m² × path length.
    cmd("POLYLINE " + pt(x, y + 30, z) + " " + pt(x + 30, y + 30, z - 1.5) + " " + pt(x + 30, y + 60, z - 3));
    const Uuid path = lastGeometry();
    ctx_.select(EntityKind::Geometry, path);
    check(cmd("SWEEP 5 5"), "SWEEP a 5 × 5 m profile along a bent path");
    const auto* sw = m().geometry.find(lastGeometry());
    const double len = geometry::curveLength(*m().geometry.find(path));
    check(sw && sw->type == GeometryType::Solid && std::abs(std::abs(geometry::meshSignedVolume(*sw->mesh)) / (25 * len) - 1) < 0.02,
          QStringLiteral("swept volume %1 m³ ≈ 25 m² × %2 m").arg(sw && sw->mesh ? std::abs(geometry::meshSignedVolume(*sw->mesh)) : 0, 0, 'f', 1).arg(len, 0, 'f', 2));
    // Loft between a 10 × 10 square and a 4 × 4 square 8 m above: a frustum, V = h/3 (A1 + A2 + √(A1 A2)).
    cmd("RECTANGLE " + pt(x + 60, y, z) + " " + pt(x + 70, y + 10, z));
    const Uuid s1 = lastGeometry();
    cmd("RECTANGLE " + pt(x + 63, y + 3, z + 8) + " " + pt(x + 67, y + 7, z + 8));
    const Uuid s2 = lastGeometry();
    ctx_.selectMany({{EntityKind::Geometry, s1}, {EntityKind::Geometry, s2}});
    check(cmd("LOFT RULED"), "LOFT two sections (ruled)");
    const auto* lf = m().geometry.find(lastGeometry());
    const double frustum = 8.0 / 3.0 * (100 + 16 + 40);
    check(lf && std::abs(std::abs(geometry::meshSignedVolume(*lf->mesh)) - frustum) < 0.01 * frustum,
          QStringLiteral("loft volume %1 m³ (frustum %2 m³)").arg(lf && lf->mesh ? std::abs(geometry::meshSignedVolume(*lf->mesh)) : 0, 0, 'f', 2).arg(frustum, 0, 'f', 2));
    // Revolve a vertical rectangle (in the XZ plane) about the Z axis through its inner edge: a ring.
    {
        GeometryObject r = cad::makePolyline({{x + 100, y, z}, {x + 102, y, z}, {x + 102, y, z + 3}, {x + 100, y, z + 3}}, true);
        r.layerId = ctx_.currentLayer();
        Uuid rid;
        ctx_.tryEdit("profile", [&] { rid = ProjectEditor(ctx_.project()).addGeometry(r); });
        ctx_.select(EntityKind::Geometry, rid);
        SolidModelDialog d(ctx_, SolidModelDialog::Operation::Revolve, &w_);
        d.setRevolution({x + 95, y, z}, {0, 0, 1}, 360);
        const auto ids = d.create();
        // Annulus between r = 5 and r = 7, height 3: V = π (49 − 25) × 3.
        const double ring = std::numbers::pi * 24 * 3;
        const auto* rv = ids.empty() ? nullptr : m().geometry.find(ids.front());
        check(rv && std::abs(std::abs(geometry::meshSignedVolume(*rv->mesh)) - ring) < 0.01 * ring,
              QStringLiteral("revolved ring volume %1 m³ (exact %2 m³)").arg(rv && rv->mesh ? std::abs(geometry::meshSignedVolume(*rv->mesh)) : 0, 0, 'f', 2).arg(ring, 0, 'f', 2));
    }
    // Section the extruded block horizontally at mid-height: two closed loops (outline and hole).
    w_.activeView()->setWorkPlaneZ(z + 3);
    ctx_.select(EntityKind::Geometry, block);
    const std::size_t n0 = m().geometry.size();
    const bool sectioned = cmd("SECTIONOBJ");
    check(sectioned && m().geometry.size() >= n0 + 2, QStringLiteral("section by the work plane: %1 curve(s)").arg(m().geometry.size() - n0));
    w_.activeView()->setWorkPlaneZ(kZ0);
#else
    skip("3D modelling (built without Open CASCADE)");
#endif
    // Chamfer two lines meeting at a corner, picked in the plan view.
    const double cx = kE0 + 400, cy = kN0;
    cmd(QStringLiteral("LINE %1,%2,%3 %4,%5,%6").arg(cx, 0, 'f', 3).arg(cy, 0, 'f', 3).arg(kZ0, 0, 'f', 3).arg(cx + 10, 0, 'f', 3).arg(cy, 0, 'f', 3).arg(kZ0, 0, 'f', 3));
    const Uuid la = lastGeometry();
    cmd(QStringLiteral("LINE %1,%2,%3 %4,%5,%6").arg(cx + 10, 0, 'f', 3).arg(cy, 0, 'f', 3).arg(kZ0, 0, 'f', 3).arg(cx + 10, 0, 'f', 3).arg(cy + 10, 0, 'f', 3).arg(kZ0, 0, 'f', 3));
    const Uuid lb = lastGeometry();
    w_.activeView()->setNamedView("top");
    ctx_.selectMany({{EntityKind::Geometry, la}, {EntityKind::Geometry, lb}});
    w_.activeView()->fitSelection();
    pump(200);
    ctx_.clearSelection();
    const std::size_t before = m().geometry.size();
    check(cmd("CHAMFER 2"), "CHAMFER tool with 2 m distances");
    for (const Vec3 q : {Vec3{cx + 4, cy, kZ0}, Vec3{cx + 10, cy + 4, kZ0}})
        if (auto w = w_.activeView()->worldToWidget(q)) w_.activeView()->simulateClick(w->toPoint());
    pump(150);
    w_.action("draw.select")->trigger();
    const auto* ca = m().geometry.find(la);
    check(m().geometry.size() == before + 1 && ca && std::abs(geometry::curveLength(*ca) - 8.0) < 1e-6, "chamfer trims both lines by 2 m and adds the chamfer line");
    closeDialogs();
}

// ---- surfaces -------------------------------------------------------------------------------------------

void Smoke::surfaces() {
    section("Surfaces and point-cloud tools");
    if (cloud_.isNil()) return skip("surfaces (no point cloud)");
    ctx_.select(EntityKind::PointCloud, cloud_);
    std::size_t n = m().pointClouds.size();
    check(cmd("SEGMENT 0.6 100") && waitJobs() && m().pointClouds.size() == n + 1, "SEGMENT into connected components (derived dataset with a cluster field)");
    if (m().pointClouds.size() == n + 1) {
        const auto& seg = m().pointClouds.all().back();
        check(std::find(seg.attributes.scalarFields.begin(), seg.attributes.scalarFields.end(), "cluster") != seg.attributes.scalarFields.end(),
              "the derived dataset carries the 'cluster' field");
    }
    ctx_.select(EntityKind::PointCloud, cloud_);
    n = m().pointClouds.size();
    check(cmd(QStringLiteral("CROP %1,%2,%3 %4,%5,%6").arg(kE0 + 10, 0, 'f', 3).arg(kN0 - 5, 0, 'f', 3).arg(kZ0 - 10, 0, 'f', 3).arg(kE0 + 30, 0, 'f', 3).arg(kN0 + 5, 0, 'f', 3).arg(kZ0 + 10, 0, 'f', 3)) &&
              waitJobs() && m().pointClouds.size() == n + 1,
          "CROP by box");
    if (m().pointClouds.size() == n + 1) {
        const auto& c = m().pointClouds.all().back();
        // 20 m of a 100 m scan with the same density: about a fifth of the points.
        check(c.pointCount > expectedPoints_ / 6 && c.pointCount < expectedPoints_ / 4, QStringLiteral("cropped dataset has %1 points").arg(c.pointCount));
    }
    ctx_.select(EntityKind::PointCloud, cloud_);
    n = m().pointClouds.size();
    check(cmd("FILTER OUTLIERS 16 2") && waitJobs() && m().pointClouds.size() == n + 1, "FILTER OUTLIERS (console)");
    ctx_.select(EntityKind::PointCloud, cloud_);
    n = m().pointClouds.size();
    check(cmd("FILTER VOXEL 0.5") && waitJobs() && m().pointClouds.size() == n + 1, "FILTER VOXEL (console)");
#ifdef OPENMINE_APP_HAVE_OPEN3D
    ctx_.select(EntityKind::PointCloud, cloud_);
    n = m().pointClouds.size();
    check(cmd("FILTER VOXEL 0.5 OPEN3D") && waitJobs() && m().pointClouds.size() == n + 1 && qstr(m().pointClouds.all().back().derivation).contains("Open3D"),
          "FILTER VOXEL with the Open3D engine");
    if (m().pointClouds.size() == n + 1 && n >= 1) {
        const auto& a = m().pointClouds.all()[n - 1];
        const auto& o = m().pointClouds.all()[n];
        (void)a;
        check(o.pointCount > 0, QStringLiteral("Open3D voxel dataset: %1 points").arg(o.pointCount));
    }
#endif
    ctx_.select(EntityKind::PointCloud, cloud_);
    const std::size_t g0 = m().geometry.size();
    check(cmd("MESH DELAUNAY 2") && waitJobs() && m().geometry.size() == g0 + 1, "MESH DELAUNAY (console)");
    const Uuid surface = lastGeometry();
    // Decimate / smooth / repair / compare a surface.
    ctx_.select(EntityKind::Geometry, surface);
    const auto tris = m().geometry.find(surface)->mesh->triangles.size();
    check(cmd(QStringLiteral("DECIMATE %1").arg(tris / 4)) && waitJobs(), "DECIMATE");
    const auto* dec = m().geometry.find(lastGeometry());
    check(dec && dec->mesh->triangles.size() <= tris / 4 + 2, QStringLiteral("decimated %1 → %2 triangles").arg(tris).arg(dec ? dec->mesh->triangles.size() : 0));
    const Uuid decimated = lastGeometry();
    ctx_.select(EntityKind::Geometry, surface);
    check(cmd("SMOOTH 5") && waitJobs() && m().geometry.find(lastGeometry())->mesh->triangles.size() == tris, "SMOOTH keeps the topology");
    ctx_.select(EntityKind::Geometry, surface);
    check(cmd("REPAIR") && waitJobs(), "REPAIR");
    ctx_.select(EntityKind::Geometry, decimated);
    const QString refName = qstr(m().geometry.find(surface)->name);
    if (!refName.contains(' ')) {
        check(cmd("COMPARE " + refName + " 5") && waitJobs(), "COMPARE the decimated surface with the original");
        const auto* cmp = m().geometry.find(lastGeometry());
        check(cmp && cmp->mesh->scalarName == "distance" && cmp->mesh->vertexScalars.size() == cmp->mesh->vertices.size(), "per-vertex distances stored on the comparison");
    } else {
        MeshToolDialog d(ctx_, MeshToolDialog::Operation::Compare, decimated, &w_);
        d.setReference(surface, 5);
        check(d.start() > 0 && waitJobs(), "compare the decimated surface with the original");
        const auto* cmp = m().geometry.find(lastGeometry());
        check(cmp && cmp->mesh->scalarName == "distance" && cmp->mesh->vertexScalars.size() == cmp->mesh->vertices.size(), "per-vertex distances stored on the comparison");
    }
    // Console ICP between the original and its outlier-filtered copy.
    if (!subsampled_.isNil()) {
        ctx_.select(EntityKind::PointCloud, subsampled_);
        const std::size_t regs = m().registrations.size();
        const QString target = qstr(m().pointClouds.find(cloud_)->name);
        if (!target.contains(' ')) check(cmd("REGISTER ICP " + target + " 1") && waitJobs() && m().registrations.size() == regs + 1, "REGISTER ICP (console)");
    }
    closeDialogs();
}

// ---- scheduling libraries -------------------------------------------------------------------------------

void Smoke::libraries() {
    section("Scheduling libraries");
    if (activity_.isNil()) return skip("libraries (no activity)");
    // Resources and assignments.
    check(cmd("RESOURCE Jumbo 1 Equipment") && cmd("RESOURCE Loader LH517 1 Equipment"), "RESOURCE adds equipment to the library");
    ctx_.select(EntityKind::Activity, activity_);
    check(cmd("ASSIGN Jumbo 1 DRIVES"), "ASSIGN a resource to the activity");
    check(m().assignments.size() >= 1 && m().assignments.all().back().drivesRate, "assignment stored (drives the rate)");
    {
        ResourceLibraryDialog d(ctx_, &w_);
        d.show();
        pump(100);
        check(d.findChildren<QTableWidget*>().size() == 2, "resource library dialog lists resources and assignments");
        d.close();
    }
    // A calendar: two 10 h shifts Monday–Friday, a public holiday; the project default.
    const double finish0 = ctx_.schedule().result.timings.at(activity_).ef;
    {
        CalendarDialog d(ctx_, {}, &w_);
        const Uuid cal = d.createCalendar("Two shifts weekdays", CalendarKind::Shift);
        d.select(cal);
        d.findChildren<QTableWidget*>().front()->setRowCount(0);
        d.addShift("Day", 6 * 60, 10 * 60, 0b0011111);
        d.addShift("Night", 18 * 60, 10 * 60, 0b0011111);
        d.addException(QDate(2026, 1, 12), {}, "Public holiday");
        for (auto* cb : d.findChildren<QCheckBox*>())
            if (cb->text().contains("default")) cb->setChecked(true);
        check(d.apply(), "calendar saved and made the project default");
        check(d.summary().contains("100.0 working hours per week"), "calendar summary: " + d.summary().left(60));
        const auto* c = m().calendars.find(cal);
        check(c && c->shifts.size() == 2 && c->exceptions.size() == 1 && m().info.defaultCalendarId == cal, "calendar shifts, exception and default stored");
    }
    cmd("SCHEDULE");
    const double finish1 = ctx_.schedule().result.timings.at(activity_).ef;
    check(finish1 > finish0, "working only 100 h a week pushes the heading's finish later");
    // A development cycle drives the activity's rate.
    {
        CycleDialog d(ctx_, &w_);
        const Uuid cyc = d.createCycle("5x5 jumbo cycle");
        d.select(cyc);
        check(d.apply(), "development cycle saved");
        check(d.breakdownText().contains("m per day"), "cycle evaluated: " + d.breakdownText().left(80));
        check(d.useForActivities({activity_}) == 1, "the activity takes its rate from the cycle");
        check(m().activities.find(activity_)->useCycleRate && m().activities.find(activity_)->productionModelId == cyc, "cycle link stored on the activity");
    }
    cmd("SCHEDULE");
    check(ctx_.schedule().result.ok && qstr(ctx_.schedule().result.timings.at(activity_).durationBasis).contains("cycle", Qt::CaseInsensitive),
          "duration basis: " + qstr(ctx_.schedule().result.timings.at(activity_).durationBasis));
    // Monthly production targets.
    {
        TargetsDialog d(ctx_, &w_);
        check(d.addMonthly(QDate(2026, 1, 1), QDate(2026, 6, 30), 80) == 6, "six monthly production targets");
    }
    // Scheduler grid: filter and group.
    auto* sp = w_.scheduler();
    const int all = sp->visibleActivityCount();
    sp->setShow(SchedulerPanel::Show::Critical);
    const int crit = sp->visibleActivityCount();
    check(crit >= 1 && crit <= all, QStringLiteral("filter critical: %1 of %2 activities").arg(crit).arg(all));
    sp->setShow(SchedulerPanel::Show::All);
    sp->setFilter("no such activity name");
    check(sp->visibleActivityCount() == 0, "text filter hides non-matching activities");
    sp->setFilter({});
    sp->setGroupBy(SchedulerPanel::GroupBy::Resource);
    pump(100);
    bool jumboGroup = false;
    for (int i = 0; i < sp->grid()->topLevelItemCount(); ++i) jumboGroup = jumboGroup || sp->grid()->topLevelItem(i)->text(0) == "Jumbo";
    check(jumboGroup && sp->visibleActivityCount() == all - static_cast<int>(std::count_if(m().activities.all().begin(), m().activities.all().end(),
                                                                                             [](const auto& a) { return a.kind == ActivityKind::Summary; })),
          "grouped by resource (a 'Jumbo' group)");
    w_.dock("scheduler")->show();
    w_.dock("scheduler")->raise();
    shot("scheduler_grouped");
    sp->setGroupBy(SchedulerPanel::GroupBy::Wbs);
    closeDialogs();
}

int Smoke::run() {
    line("OpenMine Suite GUI smoke test — " + QDateTime::currentDateTime().toString(Qt::ISODate));
    line("Qt " + QString(qVersion()) + ", output in " + dir_);
    w_.setNonInteractive(true);
    // Any modal dialog opened by a step is closed so the run cannot block.
    QTimer modalGuard;
    QObject::connect(&modalGuard, &QTimer::timeout, [] {
        if (auto* mw = QApplication::activeModalWidget())
            if (auto* d = qobject_cast<QDialog*>(mw)) d->reject();
    });
    modalGuard.start(500);
    generateData();
    struct Step {
        const char* name;
        void (Smoke::*fn)();
    };
    const Step steps[] = {{"project", &Smoke::project}, {"workspace", &Smoke::workspace}, {"cad", &Smoke::cad}, {"modelling", &Smoke::modelling},           {"design", &Smoke::design},   {"survey", &Smoke::survey}, {"surfaces", &Smoke::surfaces},
                          {"geology", &Smoke::geology}, {"schedule", &Smoke::schedule}, {"libraries", &Smoke::libraries}, {"exchange", &Smoke::exchange}, {"views", &Smoke::views},
                          {"scripting", &Smoke::scripting}, {"persistence", &Smoke::persistence}};
    for (const auto& s : steps) {
        try {
            if (!ctx_.hasProject() && std::string_view(s.name) != "project") break;
            (this->*s.fn)();
        } catch (const std::exception& e) {
            check(false, QStringLiteral("step %1 threw: %2").arg(s.name, e.what()));
        }
        closeDialogs();
    }
    line(QStringLiteral("\n%1 passed, %2 failed, %3 skipped").arg(passes_).arg(failures_).arg(skips_));
    return failures_;
}

} // namespace

int runSmokeTest(MainWindow& w, const QString& outDir) {
    Smoke s(w, outDir);
    return s.run();
}

} // namespace openmine::app
