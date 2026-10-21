// SPDX-License-Identifier: Apache-2.0
// The command console's standard command set. Every command calls the same validated, undoable
// operations as the menus and tools; with no arguments, drawing and editing commands start the
// interactive viewport tool so the remaining input can be picked or typed.
#include "../MainWindow.h"

#include "../app/EntityInfo.h"
#include "../dialogs/MeshDialogs.h"
#include "../dialogs/ModelDialogs.h"
#include "../dialogs/SurveyDialogs.h"
#include "../scheduler/LibraryDialogs.h"
#include "../panels/PropertiesPanel.h"
#include "../panels/PythonConsolePanel.h"
#include "../panels/ReportsPanel.h"
#include "../scheduler/SchedulerPanel.h"
#include "../viewport/ViewportArea.h"
#include "CommandConsole.h"

#include "openmine/cad/Edit.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/mine_design/GeometryEdits.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Cpm.h"
#include "openmine/survey/Registration.h"

#ifdef OPENMINE_APP_HAVE_OCCT
#include "openmine/occt/SolidObjects.h"
#endif
#ifdef OPENMINE_APP_HAVE_CRS
#include "openmine/crs/Crs.h"
#endif

#include <QAction>
#include <QDockWidget>
#include <QRegularExpression>

#include <cmath>
#include <numbers>

namespace openmine::app {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

/// "x,y,z" | "x,y" (z = work plane) | "@dx,dy[,dz]" relative to the previous point |
/// "@d<bearing" polar (bearing clockwise from north, plan).
std::optional<Vec3> parsePoint(const QString& tok, const std::optional<Vec3>& prev, double workZ) {
    QString t = tok.trimmed();
    const bool rel = t.startsWith('@');
    if (rel) t = t.mid(1);
    if (rel && t.contains('<')) {
        if (!prev) return std::nullopt;
        const auto parts = t.split('<');
        bool ok1 = false, ok2 = false;
        const double d = parts.value(0).toDouble(&ok1), brg = parts.value(1).toDouble(&ok2);
        if (!ok1 || !ok2) return std::nullopt;
        return *prev + Vec3{d * std::sin(brg * kDeg), d * std::cos(brg * kDeg), 0};
    }
    const QStringList c = t.split(',');
    if (c.size() < 2 || c.size() > 3) return std::nullopt;
    double v[3] = {0, 0, rel ? 0.0 : workZ};
    for (int i = 0; i < c.size(); ++i) {
        bool ok = false;
        v[i] = c[i].toDouble(&ok);
        if (!ok || !std::isfinite(v[i])) return std::nullopt;
    }
    Vec3 p{v[0], v[1], v[2]};
    if (rel) {
        if (!prev) return std::nullopt;
        p = *prev + p;
    }
    return p;
}

std::vector<Vec3> parsePoints(const QStringList& args, double workZ, bool* closed = nullptr) {
    std::vector<Vec3> pts;
    if (closed) *closed = false;
    std::optional<Vec3> prev;
    for (const auto& a : args) {
        if (closed && (a.compare("CLOSE", Qt::CaseInsensitive) == 0 || a.compare("C", Qt::CaseInsensitive) == 0)) {
            *closed = true;
            continue;
        }
        auto p = parsePoint(a, prev, workZ);
        if (!p) throw ValidationError("Cannot read point '" + a.toStdString() + "' (x,y[,z] | @dx,dy[,dz] | @distance<bearing)");
        pts.push_back(*p);
        prev = p;
    }
    return pts;
}

double number(const QString& s, const char* what) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (!ok || !std::isfinite(v)) throw ValidationError(std::string(what) + " must be a number");
    return v;
}

Vec3 vectorArg(const QString& s, const char* what) {
    auto d = parsePoint("@" + QString(s).remove('@'), Vec3{}, 0);
    if (!d) throw ValidationError(std::string(what) + " must be dx,dy[,dz]");
    return *d;
}

} // namespace

void MainWindow::registerCommands() {
    auto& con = *console_;
    auto needProject = [this] {
        if (!ctx_.hasProject()) throw ValidationError("No project is open (NEW or OPEN first)");
    };
    auto workZ = [this] { return activeView() ? activeView()->workPlaneZ() : 0.0; };
    auto selectedObjects = [this]() {
        std::vector<GeometryObject> out;
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* g = ctx_.project().model().geometry.find(id)) out.push_back(*g);
        if (out.empty()) throw ValidationError("Select one or more objects first (SELECT, or pick in the viewport)");
        return out;
    };
    // Creates an object on the current layer (one undo step) and selects it.
    auto addObject = [this](GeometryObject g, const QString& what) {
        ctx_.tryEdit(what, [&] {
            g.layerId = ctx_.currentLayer();
            if (g.layerId.isNil()) throw ValidationError("No unlocked layer to draw on (LAYER <name> creates one)");
            const Uuid id = ProjectEditor(ctx_.project()).addGeometry(std::move(g), sstr(what));
            ctx_.select(EntityKind::Geometry, id);
            const auto* o = ctx_.project().model().geometry.find(id);
            ctx_.report(QStringLiteral("%1 created%2").arg(what, geometry::isCurve(o->type) ? QStringLiteral(": length %1 m").arg(formatNumber(geometry::curveLength(*o))) : QString()));
        });
    };
    // Applies a transform to the selection (solids exactly through the kernel; centrelines flag
    // linked schedule quantities).
    auto transformSelection = [this, selectedObjects](const Transform3& t, bool copy, const QString& what) {
        const auto objs = selectedObjects();
        ctx_.tryEdit(what, [&] {
            auto& p = ctx_.project();
            CommandStack::MacroScope macro(p.commands(), sstr(what));
            std::vector<SelectionItem> created;
            std::vector<Uuid> others;
            int flagged = 0;
            for (const auto& g : objs) {
                if (g.type == GeometryType::Solid) {
#ifdef OPENMINE_APP_HAVE_OCCT
                    const Uuid id = occt::transformSolidObject(p, g.id, t, copy);
                    if (copy) created.push_back({EntityKind::Geometry, id});
#else
                    throw ValidationError("Solids can only be transformed in builds with Open CASCADE");
#endif
                    continue;
                }
                others.push_back(g.id);
            }
            if (!others.empty()) {
                const auto r = mine_design::transformObjects(p, others, t, copy, sstr(what));
                flagged = r.flaggedActivities;
                for (const auto& id : r.created) created.push_back({EntityKind::Geometry, id});
            }
            macro.commit();
            if (copy) ctx_.selectMany(created);
            ctx_.report(QStringLiteral("%1: %2 object(s)%3").arg(what).arg(objs.size()).arg(flagged ? QStringLiteral(" — %1 schedule activities flagged for review").arg(flagged) : QString()));
        });
    };
    auto tool = [this](Tool t) { startTool(static_cast<int>(t)); };
    auto trigger = [this](const char* id) {
        if (auto* a = action(id)) a->trigger();
    };

    // ---- project ---------------------------------------------------------------------------------
    con.registerCommand("NEW", "NEW <name> [YYYY-MM-DD]", "Create a new project (schedule starts today unless a date is given)", [this](const QStringList& a) {
        if (!maybeSave()) return;
        QStringList words = a;
        std::optional<Date> start;
        if (!words.isEmpty()) start = Date::parse(sstr(words.last()));
        if (start) words.removeLast();
        ctx_.newProject(words.isEmpty() ? tr("New mine project") : words.join(' '), start.value_or(Date::todayUtc()));
    });
    con.registerCommand("OPEN", "OPEN <path>", "Open an .openmine project", [this](const QStringList& a) {
        if (a.isEmpty()) throw ValidationError("OPEN needs a file path");
        if (!maybeSave()) return;
        if (!openProjectFrom(a.join(' '))) throw ValidationError("The project could not be opened");
    });
    con.registerCommand("SAVE", "SAVE [path]", "Save the project (atomic, verified)", [this, needProject](const QStringList& a) {
        needProject();
        QString path = a.join(' ');
        if (path.isEmpty()) path = QString::fromStdU16String(ctx_.project().filePath().u16string());
        if (path.isEmpty()) throw ValidationError("The first save needs a path: SAVE /data/mine.openmine");
        if (!path.endsWith(".openmine")) path += ".openmine";
        if (!saveProjectTo(path)) throw ValidationError("The project was not saved");
        ctx_.report("Saved " + path);
    }, {"SAVEAS"});
    con.registerCommand("CLOSE", "CLOSE", "Close the project", [this](const QStringList&) { closeProject(); });
    con.registerCommand("CHECK", "CHECK", "Check the project file, model references and assets", [this, needProject](const QStringList&) {
        needProject();
        projectCheck();
    });
    con.registerCommand("IMPORT", "IMPORT <path>", "Import a file by type (point clouds, DXF, OMF, meshes, STEP/IGES, GIS, DEM, CSV tables, plugin formats)",
                        [this](const QStringList& a) {
                            if (a.isEmpty()) throw ValidationError("IMPORT needs a file path");
                            if (!importFile(a.join(' '))) throw ValidationError("Import failed");
                        });
    con.registerCommand("EXPORT", "EXPORT <path>", "Export the selection (or everything applicable) by file type", [this, needProject](const QStringList& a) {
        needProject();
        if (a.isEmpty()) throw ValidationError("EXPORT needs a file path");
        if (!exportFile(a.join(' '))) throw ValidationError("Export failed");
    });
    con.registerCommand("UNDO", "UNDO [n]", "Undo the last change(s)", [this, needProject](const QStringList& a) {
        needProject();
        const int n = a.isEmpty() ? 1 : static_cast<int>(number(a[0], "Count"));
        auto& cmd = ctx_.project().commands();
        for (int i = 0; i < n; ++i) {
            if (!cmd.canUndo()) throw ValidationError("Nothing to undo");
            const auto t = cmd.undoText();
            cmd.undo();
            ctx_.report("Undone: " + qstr(t));
        }
    }, {"U"});
    con.registerCommand("REDO", "REDO [n]", "Redo undone change(s)", [this, needProject](const QStringList& a) {
        needProject();
        const int n = a.isEmpty() ? 1 : static_cast<int>(number(a[0], "Count"));
        auto& cmd = ctx_.project().commands();
        for (int i = 0; i < n; ++i) {
            if (!cmd.canRedo()) throw ValidationError("Nothing to redo");
            const auto t = cmd.redoText();
            cmd.redo();
            ctx_.report("Redone: " + qstr(t));
        }
    });
    con.registerCommand("LIST", "LIST", "Summarise the project contents", [this, needProject](const QStringList&) {
        needProject();
        const auto& m = ctx_.project().model();
        QStringList parts;
#define OPENMINE_COUNT(T, member, Kind) \
    if (m.member.size()) parts << QStringLiteral("%1 %2").arg(m.member.size()).arg(kindLabel(EntityKind::Kind, m.member.size() != 1).toLower());
        OPENMINE_ENTITY_TYPES(OPENMINE_COUNT)
#undef OPENMINE_COUNT
        ctx_.report(qstr(m.info.name) + ": " + (parts.isEmpty() ? tr("empty") : parts.join(", ")));
    });

    // ---- drawing ---------------------------------------------------------------------------------
    con.registerCommand("POINT", "POINT [p …]", "Create points (no args: pick in the viewport)", [=, this](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Point);
        for (const auto& p : parsePoints(a, workZ())) {
            GeometryObject g;
            g.type = GeometryType::Point;
            g.vertices = {p};
            addObject(std::move(g), tr("Point"));
        }
    }, {"PO"});
    con.registerCommand("LINE", "LINE [p1 p2]", "Create a line (no args: draw in the viewport)", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Line);
        const auto pts = parsePoints(a, workZ());
        if (pts.size() != 2) throw ValidationError("LINE needs exactly two points");
        addObject(cad::makeLine(pts[0], pts[1]), QStringLiteral("Line"));
    }, {"L"});
    con.registerCommand("POLYLINE", "POLYLINE [p1 p2 … [CLOSE]]", "Create a polyline (no args: draw in the viewport)", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Polyline);
        bool closed = false;
        const auto pts = parsePoints(a, workZ(), &closed);
        if (pts.size() < 2) throw ValidationError("A polyline needs at least two points");
        addObject(cad::makePolyline(pts, closed), QStringLiteral("Polyline"));
    }, {"PLINE", "PL"});
    con.registerCommand("RECTANGLE", "RECTANGLE [corner1 corner2]", "Create a rectangle on the work plane", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Rectangle);
        const auto pts = parsePoints(a, workZ());
        if (pts.size() != 2) throw ValidationError("RECTANGLE needs two corners");
        addObject(cad::makeRectangle(pts[0], pts[1]), QStringLiteral("Rectangle"));
    }, {"REC", "RECT"});
    con.registerCommand("POLYGON", "POLYGON [centre radius sides [rotation°]]", "Regular polygon (inscribed); no args: draw a free polygon", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Polygon);
        if (a.size() < 3) throw ValidationError("POLYGON needs centre, radius and number of sides");
        const auto c = parsePoints({a[0]}, workZ()).front();
        const int sides = static_cast<int>(number(a[2], "Sides"));
        if (sides < 3 || sides > 1000) throw ValidationError("Sides must be 3–1000");
        addObject(cad::makePolygon(c, number(a[1], "Radius"), sides, a.size() > 3 ? number(a[3], "Rotation") * kDeg : 0.0), QStringLiteral("Polygon"));
    }, {"POL"});
    con.registerCommand("CIRCLE", "CIRCLE [centre radius]", "Create a circle (no args: draw in the viewport)", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Circle);
        if (a.size() != 2) throw ValidationError("CIRCLE needs a centre and a radius");
        const double r = number(a[1], "Radius");
        if (r <= 0) throw ValidationError("The radius must be positive");
        addObject(cad::makeCircle(parsePoints({a[0]}, workZ()).front(), r), QStringLiteral("Circle"));
    }, {"CI"});
    con.registerCommand("ARC", "ARC [p1 p2 p3]", "Arc through three points (no args: draw in the viewport)", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Arc3);
        const auto pts = parsePoints(a, workZ());
        if (pts.size() != 3) throw ValidationError("ARC needs three points");
        addObject(cad::makeArc3(pts[0], pts[1], pts[2]), QStringLiteral("Arc"));
    }, {"A"});
    con.registerCommand("SPLINE", "SPLINE [fit points … [CLOSE]]", "Spline through fit points", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Spline);
        bool closed = false;
        const auto pts = parsePoints(a, workZ(), &closed);
        if (pts.size() < 2) throw ValidationError("A spline needs at least two fit points");
        addObject(cad::makeSpline(pts, closed), QStringLiteral("Spline"));
    }, {"SPL"});
    con.registerCommand("TEXT", "TEXT [insertion height text…]", "Create a text annotation", [=, this](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Text);
        if (a.size() < 3) throw ValidationError("TEXT needs an insertion point, a height and the text");
        const double h = number(a[1], "Height");
        viewports_->tools().setTextHeight(h);
        addObject(cad::makeText(parsePoints({a[0]}, workZ()).front(), sstr(a.mid(2).join(' ')), h), QStringLiteral("Text"));
    }, {"DT"});
    con.registerCommand("DIMENSION", "DIMENSION [p1 p2 [p3]]", "Aligned dimension between two points (offset through p3)", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Dimension);
        auto pts = parsePoints(a, workZ());
        if (pts.size() < 2 || pts.size() > 3) throw ValidationError("DIMENSION needs two or three points");
        if (pts.size() == 2) pts.push_back(pts[1]);
        addObject(cad::makeDimension(DimensionKind::Aligned, pts), QStringLiteral("Dimension"));
    }, {"DIM"});

    // ---- measuring -------------------------------------------------------------------------------
    con.registerCommand("DISTANCE", "DISTANCE [p1 p2]", "Measure between two points (no args: pick in the viewport)", [=, this](const QStringList& a) {
        if (a.isEmpty()) return tool(Tool::Measure);
        const auto pts = parsePoints(a, workZ());
        if (pts.size() != 2) throw ValidationError("DISTANCE needs two points");
        const Vec3 d = pts[1] - pts[0];
        const double plan = std::hypot(d.x, d.y);
        double brg = std::atan2(d.x, d.y) / kDeg;
        if (brg < 0) brg += 360;
        ctx_.report(QStringLiteral("Distance: slope %1 m · plan %2 m · ΔZ %3 m · bearing %4° · grade %5")
                        .arg(formatNumber(d.length()), formatNumber(plan), formatNumber(d.z), formatNumber(brg, 4),
                             plan > 1e-9 ? formatNumber(100.0 * d.z / plan, 2) + " %" : QStringLiteral("vertical")));
    }, {"DI", "DIST"});
    con.registerCommand("LENGTH", "LENGTH", "Length and gradient of the selected curves", [=, this](const QStringList&) {
        needProject();
        double total = 0;
        for (const auto& g : selectedObjects()) {
            if (!geometry::isCurve(g.type)) continue;
            const double len = geometry::curveLength(g), plan = geometry::planLength(g);
            total += len;
            const auto s = geometry::curveStart(g), e = geometry::curveEnd(g);
            const QString grade = s && e && plan > 1e-9 && !g.closed ? formatNumber(100.0 * (e->z - s->z) / plan, 2) + " %" : QStringLiteral("n/a");
            ctx_.report(QStringLiteral("%1: slope %2 m · plan %3 m · overall grade %4").arg(qstr(g.name.empty() ? "(unnamed)" : g.name), formatNumber(len), formatNumber(plan), grade));
        }
        ctx_.report(QStringLiteral("Total slope length %1 m").arg(formatNumber(total)));
    }, {"LEN"});
    con.registerCommand("AREA", "AREA", "Plan area and perimeter of the selected closed curves (no selection: pick in the viewport)", [=, this](const QStringList&) {
        needProject();
        if (ctx_.selectedGeometry().empty()) return tool(Tool::MeasureArea);
        for (const auto& g : selectedObjects()) {
            if (g.type == GeometryType::Mesh || g.type == GeometryType::Solid) {
                if (!g.mesh) continue;
                const auto topo = geometry::topology(*g.mesh);
                ctx_.report(QStringLiteral("%1: surface area %2 m²%3").arg(qstr(g.name), formatNumber(geometry::meshArea(*g.mesh), 2))
                                .arg(topo.closed() ? QStringLiteral(" · volume %1 m³").arg(formatNumber(std::abs(geometry::meshSignedVolume(*g.mesh)), 2)) : QString()));
                continue;
            }
            if (!geometry::isCurve(g.type) || !(g.closed || g.type == GeometryType::Circle)) continue;
            const auto pts = geometry::curvePolyline(g, 1e-4);
            double s = 0;
            const Vec3 o = pts.front();
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const Vec3 p = pts[i] - o, q = pts[(i + 1) % pts.size()] - o;
                s += p.x * q.y - q.x * p.y;
            }
            ctx_.report(QStringLiteral("%1: plan area %2 m² · perimeter %3 m").arg(qstr(g.name), formatNumber(std::abs(s) / 2.0, 3), formatNumber(geometry::curveLength(g))));
        }
    }, {"AA"});
    con.registerCommand("ID", "ID", "Report the coordinates of a picked point", [this](const QStringList&) {
        viewports_->tools().requestPoint(tr("Pick a point"), [this](Vec3 p) {
            ctx_.report(QStringLiteral("X %1  Y %2  Z %3").arg(p.x, 0, 'f', 4).arg(p.y, 0, 'f', 4).arg(p.z, 0, 'f', 4));
        });
    });

    // ---- editing ---------------------------------------------------------------------------------
    con.registerCommand("MOVE", "MOVE [dx,dy[,dz]]", "Move the selection (no args: base and target points in the viewport)", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Move);
        transformSelection(Transform3::translation(vectorArg(a[0], "Displacement")), false, QStringLiteral("Move"));
    }, {"M"});
    con.registerCommand("COPY", "COPY [dx,dy[,dz]]", "Copy the selection with an offset", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Copy);
        transformSelection(Transform3::translation(vectorArg(a[0], "Displacement")), true, QStringLiteral("Copy"));
    }, {"CO", "CP"});
    con.registerCommand("ROTATE", "ROTATE [degrees [base]]", "Rotate the selection about a vertical axis (counter-clockwise positive)", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Rotate);
        const double deg = number(a[0], "Angle");
        Vec3 base;
        if (a.size() > 1) base = parsePoints({a[1]}, workZ()).front();
        else if (auto s = geometry::curveStart(selectedObjects().front())) base = *s;
        else base = selectedObjects().front().vertices.empty() ? Vec3{} : selectedObjects().front().vertices.front();
        transformSelection(Transform3::rotation({0, 0, 1}, deg * kDeg, base), false, QStringLiteral("Rotate"));
    }, {"RO"});
    con.registerCommand("SCALE", "SCALE [factor [base]]", "Scale the selection uniformly about a base point (default: the start of the first selected object)", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Scale);
        const double f = number(a[0], "Factor");
        if (f <= 0) throw ValidationError("The scale factor must be positive");
        // Base point: given, else the start of the first selected object (never the origin,
        // which would throw mine coordinates kilometres away).
        Vec3 base;
        if (a.size() > 1) base = parsePoints({a[1]}, workZ()).front();
        else {
            const auto first = selectedObjects().front();
            if (auto st = geometry::curveStart(first)) base = *st;
            else if (const auto bb = geometry::objectBounds(first)) base = bb->center();
        }
        transformSelection(Transform3::scaling(f, base), false, QStringLiteral("Scale"));
    }, {"SC"});
    con.registerCommand("MIRROR", "MIRROR [p1 p2]", "Mirror the selection about the vertical plane through two points (keeps the original)", [=](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return tool(Tool::Mirror);
        const auto pts = parsePoints(a, workZ());
        if (pts.size() != 2) throw ValidationError("MIRROR needs two points on the mirror line");
        const Vec3 d = pts[1] - pts[0];
        const Vec3 n = Vec3{-d.y, d.x, 0}.normalized();
        transformSelection(Transform3::mirror(pts[0], n), true, QStringLiteral("Mirror"));
    }, {"MI"});
    con.registerCommand("OFFSET", "OFFSET [distance]", "Offset a curve (pick the object and the side)", [=, this](const QStringList& a) {
        needProject();
        if (!a.isEmpty()) viewports_->tools().setOffsetDistance(number(a[0], "Distance"));
        tool(Tool::Offset);
    }, {"O"});
    con.registerCommand("FILLET", "FILLET [radius]", "Fillet two curves (radius 0 = sharp corner)", [=, this](const QStringList& a) {
        needProject();
        if (!a.isEmpty()) viewports_->tools().setFilletRadius(number(a[0], "Radius"));
        tool(Tool::Fillet);
    }, {"F"});
    con.registerCommand("TRIM", "TRIM", "Trim curves at cutting edges", [=](const QStringList&) { needProject(); tool(Tool::Trim); }, {"TR"});
    con.registerCommand("EXTEND", "EXTEND", "Extend curves to boundaries", [=](const QStringList&) { needProject(); tool(Tool::Extend); }, {"EX"});
    con.registerCommand("SPLIT", "SPLIT", "Split a curve at a point", [=](const QStringList&) { needProject(); tool(Tool::Split); }, {"BREAK", "BR"});
    con.registerCommand("JOIN", "JOIN", "Join the selected curves end to end", [this](const QStringList&) { handleEntityAction("join", ctx_.selection()); }, {"J"});
    con.registerCommand("EXPLODE", "EXPLODE", "Explode the selected objects into their parts", [this](const QStringList&) { handleEntityAction("explode", ctx_.selection()); }, {"X"});
    con.registerCommand("DELETE", "DELETE", "Delete the selection (UNDO restores it)", [=, this](const QStringList&) {
        needProject();
        if (ctx_.selection().empty()) throw ValidationError("Nothing is selected");
        trigger("edit.delete");
    }, {"ERASE", "E", "DEL"});
    con.registerCommand("UNION", "UNION", "Boolean union of the selected solids", [this](const QStringList&) { handleEntityAction("union", ctx_.selection()); }, {"UNI"});
    con.registerCommand("SUBTRACT", "SUBTRACT", "Subtract the later selected solids from the first", [this](const QStringList&) { handleEntityAction("subtract", ctx_.selection()); }, {"SU"});
    con.registerCommand("INTERSECT", "INTERSECT", "Boolean intersection of the selected solids", [this](const QStringList&) { handleEntityAction("intersect", ctx_.selection()); }, {"IN"});
    con.registerCommand("ELEV", "ELEV [z]", "Show or set the work-plane elevation used for drawing", [this](const QStringList& a) {
        if (!a.isEmpty()) {
            const double z = number(a[0], "Elevation");
            viewports_->forEach([z](Viewport3D& v) { v.setWorkPlaneZ(z); });
        }
        ctx_.report("Work plane Z = " + formatNumber(activeView()->workPlaneZ()));
    }, {"WORKPLANE"});
    con.registerCommand("SNAP", "SNAP [ON|OFF]", "Object snap on/off", [this](const QStringList& a) {
        const bool on = a.isEmpty() ? !action("draw.snap")->isChecked() : a[0].compare("OFF", Qt::CaseInsensitive) != 0;
        action("draw.snap")->setChecked(on);
        ctx_.report(on ? tr("Object snap on") : tr("Object snap off"));
    });
    con.registerCommand("ORTHO", "ORTHO [ON|OFF]", "Ortho mode on/off", [this](const QStringList& a) {
        const bool on = a.isEmpty() ? !action("draw.ortho")->isChecked() : a[0].compare("OFF", Qt::CaseInsensitive) != 0;
        action("draw.ortho")->setChecked(on);
        ctx_.report(on ? tr("Ortho on") : tr("Ortho off"));
    });
    con.registerCommand("GRIDSNAP", "GRIDSNAP [spacing | OFF]", "Grid snap with a spacing, or off", [this](const QStringList& a) {
        if (!a.isEmpty() && a[0].compare("OFF", Qt::CaseInsensitive) == 0) {
            action("draw.gridSnap")->setChecked(false);
            return ctx_.report(tr("Grid snap off"));
        }
        if (!a.isEmpty()) viewports_->tools().snapSettings().gridSpacing = number(a[0], "Spacing");
        action("draw.gridSnap")->setChecked(true);
        ctx_.report(tr("Grid snap on, spacing %1 m").arg(formatNumber(viewports_->tools().snapSettings().gridSpacing)));
    });
    con.registerCommand("LAYER", "LAYER <name> [#RRGGBB]", "Make a layer current (created when it does not exist)", [=, this](const QStringList& a) {
        needProject();
        if (a.isEmpty()) {
            const auto* l = ctx_.project().model().layers.find(ctx_.currentLayer());
            return ctx_.report(tr("Current layer: %1").arg(l ? qstr(l->name) : tr("(none)")));
        }
        QStringList words = a;
        std::uint32_t rgb = 0x9AD06B;
        if (words.size() > 1 && words.last().startsWith('#')) {
            bool ok = false;
            rgb = words.takeLast().mid(1).toUInt(&ok, 16);
            if (!ok) throw ValidationError("The colour must be #RRGGBB");
        }
        const std::string name = sstr(words.join(' '));
        Uuid id;
        ctx_.tryEdit(tr("Layer"), [&] {
            ProjectEditor ed(ctx_.project());
            const auto* existing = ed.findLayerByName(name);
            id = existing ? existing->id : ed.addLayer(name, rgb);
        });
        if (id.isNil()) return;
        ctx_.setCurrentLayer(id);
        ctx_.report(tr("Current layer: %1").arg(qstr(name)));
    }, {"LA"});

    // ---- selection and properties ----------------------------------------------------------------
    con.registerCommand("SELECT", "SELECT <name | heading id | uuid | ALL | NONE>…", "Select entities by name, heading ID or id (+name adds)", [=, this](const QStringList& a) {
        needProject();
        if (a.isEmpty()) throw ValidationError("SELECT needs a name, heading ID or id");
        if (a[0].compare("NONE", Qt::CaseInsensitive) == 0) return ctx_.clearSelection();
        if (a[0].compare("ALL", Qt::CaseInsensitive) == 0) return trigger("edit.selectAll");
        const auto& m = ctx_.project().model();
        std::vector<SelectionItem> found;
        SelectMode mode = SelectMode::Replace;
        for (QString key : a) {
            if (key.startsWith('+')) {
                mode = SelectMode::Add;
                key = key.mid(1);
            }
            const auto id = Uuid::parse(sstr(key));
            bool hit = false;
#define OPENMINE_FIND(T, member, Kind)                                                            \
    for (const auto& e : m.member.all()) {                                                        \
        const SelectionItem s{EntityKind::Kind, e.id};                                            \
        if ((id && e.id == *id) || displayName(m, s).compare(key, Qt::CaseInsensitive) == 0) {    \
            found.push_back(s);                                                                   \
            hit = true;                                                                           \
        }                                                                                         \
    }
            OPENMINE_ENTITY_TYPES(OPENMINE_FIND)
#undef OPENMINE_FIND
            if (!hit) throw ValidationError("Nothing matches '" + sstr(key) + "'");
        }
        ctx_.selectMany(found, mode);
        ctx_.report(tr("%1 selected").arg(found.size()));
    }, {"SEL"});
    con.registerCommand("PROP", "PROP [column value…]", "Set a property of the selection (PROP alone lists the editable columns)", [=, this](const QStringList& a) {
        needProject();
        if (a.isEmpty()) return ctx_.report(tr("Columns: %1").arg(properties_->columns().join(", ")));
        if (a.size() == 1) return ctx_.report(QStringLiteral("%1 = %2").arg(a[0], properties_->value(a[0])));
        if (!properties_->setField(a[0], a.mid(1).join(' '))) throw ValidationError("The property was not changed");
        ctx_.report(QStringLiteral("%1 = %2").arg(a[0], properties_->value(a[0])));
    }, {"SET"});

    // ---- view ------------------------------------------------------------------------------------
    con.registerCommand("ZOOM", "ZOOM [EXTENTS | SELECTION | factor]", "Zoom the active view", [this](const QStringList& a) {
        if (a.isEmpty() || a[0].startsWith("E", Qt::CaseInsensitive)) return activeView()->fitAll();
        if (a[0].startsWith("S", Qt::CaseInsensitive)) return activeView()->fitSelection();
        activeView()->zoomBy(number(a[0], "Zoom factor"));
    }, {"Z", "ZE", "FIT"});
    con.registerCommand("VIEW", "VIEW TOP|NORTH|EAST|SOUTH|WEST|ISO|ISO-NW", "Set a standard view direction", [this](const QStringList& a) {
        if (a.isEmpty()) throw ValidationError("VIEW needs a direction");
        activeView()->setNamedView(a[0].toLower());
    }, {"V"});
    con.registerCommand("LAYOUT", "LAYOUT 1|2|2H|4", "Number of views", [this](const QStringList& a) {
        const QString k = a.isEmpty() ? QStringLiteral("1") : a[0].toUpper();
        viewports_->setLayoutKind(k == "4" ? ViewportArea::Layout::Quad : k == "2" ? ViewportArea::Layout::TwoColumns : k == "2H" ? ViewportArea::Layout::TwoRows : ViewportArea::Layout::Single);
    });
    con.registerCommand("SECTION", "SECTION <heading id> <chainage> | SECTION OFF", "Vertical section across a heading at a chainage", [=, this](const QStringList& a) {
        needProject();
        if (!a.isEmpty() && a[0].compare("OFF", Qt::CaseInsensitive) == 0) {
            activeView()->setMode(ViewMode::Perspective);
            return;
        }
        if (a.size() != 2) throw ValidationError("SECTION needs a heading ID and a chainage");
        for (const auto& d : ctx_.project().model().developments.all())
            if (qstr(d.headingId).compare(a[0], Qt::CaseInsensitive) == 0) {
                ctx_.select(EntityKind::Development, d.id);
                handleEntityAction("section-chainage:" + QString::number(number(a[1], "Chainage"), 'f', 6), {{EntityKind::Development, d.id}});
                return;
            }
        throw ValidationError("No heading " + sstr(a[0]));
    }, {"SEC"});
    con.registerCommand("THEME", "THEME DARK|LIGHT", "Switch the colour theme", [this](const QStringList& a) {
        setTheme(a.isEmpty() || a[0].compare("LIGHT", Qt::CaseInsensitive) != 0);
    });
    con.registerCommand("SCREENSHOT", "SCREENSHOT <path.png>", "Save the active view as an image", [this](const QStringList& a) {
        if (a.isEmpty()) throw ValidationError("SCREENSHOT needs a file path");
        if (!activeView()->grab().save(a.join(' '))) throw ValidationError("The image could not be written");
        ctx_.report("Saved " + a.join(' '));
    });

    // ---- design and scheduling --------------------------------------------------------------------
    con.registerCommand("HEADING", "HEADING <id> [width height] [level code]", "Make the selected centreline a development heading", [=, this](const QStringList& a) {
        needProject();
        if (a.isEmpty()) throw ValidationError("HEADING needs a heading ID");
        const auto geoms = ctx_.selectedGeometry();
        if (geoms.size() != 1) throw ValidationError("Select exactly one centreline");
        mine_design::NewDevelopment spec;
        spec.headingId = sstr(a[0]);
        if (a.size() > 1) spec.width = number(a[1], "Width");
        spec.height = a.size() > 2 ? number(a[2], "Height") : spec.width;
        if (a.size() > 3) {
            for (const auto& l : ctx_.project().model().levels.all())
                if (qstr(l.code).compare(a[3], Qt::CaseInsensitive) == 0 || qstr(l.name).compare(a[3], Qt::CaseInsensitive) == 0) spec.levelId = l.id;
            if (spec.levelId.isNil()) {
                // No level object with that code or name: keep the level as text (as the Python API
                // does) and say so, rather than dropping it.
                spec.level = sstr(a[3]);
                ctx_.report(tr("No level '%1' is defined: recorded as the heading's level text").arg(a[3]), MessageLevel::Warning);
            }
        }
        ctx_.tryEdit(tr("Create heading"), [&] {
            const Uuid id = mine_design::createDevelopment(ctx_.project(), geoms.front(), spec);
            const auto mt = mine_design::computeMetrics(ctx_.project(), *ctx_.project().model().developments.find(id));
            ctx_.report(tr("Heading %1: %2 m, %3 m³").arg(a[0], formatNumber(mt.length3d, 2), formatNumber(mt.volume, 1)));
            ctx_.select(EntityKind::Development, id);
        });
    }, {"DEV"});
    con.registerCommand("ACTIVITY", "ACTIVITY [rate m/day]", "Schedule activity linked to the selected heading", [=, this](const QStringList& a) {
        needProject();
        const auto devs = ctx_.selected(EntityKind::Development);
        if (devs.empty()) throw ValidationError("Select a heading");
        const double rate = a.isEmpty() ? 4.0 : number(a[0], "Rate");
        ctx_.tryEdit(tr("Create linked activity"), [&] {
            CommandStack::MacroScope macro(ctx_.project().commands(), "Create linked activities");
            std::vector<SelectionItem> sel;
            for (const auto& d : devs) sel.push_back({EntityKind::Activity, mine_design::createLinkedActivity(ctx_.project(), d, rate)});
            macro.commit();
            ctx_.selectMany(sel);
            ctx_.report(tr("%1 activit%2 created at %3 m/day").arg(sel.size()).arg(sel.size() == 1 ? "y" : "ies").arg(formatNumber(rate, 2)));
        });
    }, {"ACT"});
    con.registerCommand("ACTUAL", "ACTUAL <quantity> [YYYY-MM-DD] [face chainage]", "Record dated actual progress on the selected activity (or the heading's activity)",
                        [=, this](const QStringList& a) {
                            needProject();
                            if (a.isEmpty()) throw ValidationError("ACTUAL needs a quantity");
                            const auto& m = ctx_.project().model();
                            Uuid act;
                            if (ctx_.primary().kind == EntityKind::Activity) act = ctx_.primary().id;
                            else if (ctx_.primary().kind == EntityKind::Development) {
                                const auto acts = m.activitiesForDevelopment(ctx_.primary().id);
                                if (acts.size() == 1) act = acts.front();
                            }
                            if (act.isNil()) throw ValidationError("Select one activity (or a heading with one linked activity)");
                            ProductionActual x;
                            x.activityId = act;
                            x.quantity = number(a[0], "Quantity");
                            x.date = Date::todayUtc();
                            if (a.size() > 1) {
                                const auto d = Date::parse(sstr(a[1]));
                                if (!d) throw ValidationError("The date must be YYYY-MM-DD");
                                x.date = *d;
                            }
                            if (a.size() > 2) x.faceChainage = number(a[2], "Face chainage");
                            const auto* activity = m.activities.find(act);
                            x.unit = activity->quantityUnit.empty() ? "m" : activity->quantityUnit;
                            x.enteredBy = ctx_.project().userName();
                            const auto devs = m.developmentsForActivity(act);
                            if (devs.size() == 1) x.developmentId = devs.front();
                            ctx_.tryEdit(tr("Record actual"), [&] {
                                ProjectEditor(ctx_.project()).add(x, "Record actual " + sstr(a[0]) + " " + x.unit + " on '" + activity->name + "'");
                                ctx_.report(tr("Actual %1 %2 recorded on '%3' for %4").arg(a[0], qstr(x.unit), qstr(activity->name), qstr(x.date.toString())));
                            });
                        });
    con.registerCommand("STATUSDATE", "STATUSDATE <YYYY-MM-DD [HH:MM] | NONE>", "Set the data date for reforecasting remaining work", [=, this](const QStringList& a) {
        needProject();
        ProjectInfo i = ctx_.project().model().info;
        if (a.isEmpty() || a[0].compare("NONE", Qt::CaseInsensitive) == 0) i.statusDate.reset();
        else {
            auto dt = DateTime::parse(sstr(a.join(' ')));
            if (!dt)
                if (auto d = Date::parse(sstr(a[0]))) dt = DateTime::fromDate(*d);
            if (!dt) throw ValidationError("The status date must be YYYY-MM-DD [HH:MM]");
            i.statusDate = *dt;
        }
        ctx_.tryEdit(tr("Status date"), [&] {
            ProjectEditor(ctx_.project()).setProjectInfo(i, "Set status date");
            ctx_.report(i.statusDate ? tr("Status date %1 — remaining work is reforecast from it").arg(qstr(i.statusDate->toString())) : tr("Status date cleared"));
        });
    });
    con.registerCommand("LINK", "LINK <heading id>", "Link the selected activity to a heading", [=, this](const QStringList& a) {
        needProject();
        const auto acts = ctx_.selected(EntityKind::Activity);
        if (acts.empty() || a.isEmpty()) throw ValidationError("Select an activity and give a heading ID");
        for (const auto& d : ctx_.project().model().developments.all())
            if (qstr(d.headingId).compare(a[0], Qt::CaseInsensitive) == 0) {
                ctx_.tryEdit(tr("Link"), [&] {
                    ProjectEditor(ctx_.project()).linkActivityToDevelopment(acts.back(), d.id);
                    ctx_.report(tr("Linked to %1").arg(qstr(d.headingId)));
                });
                return;
            }
        throw ValidationError("No heading " + sstr(a[0]));
    });
    con.registerCommand("SCHEDULE", "SCHEDULE", "Recalculate the schedule of the active scenario", [=, this](const QStringList&) {
        needProject();
        ctx_.recalculateSchedule();
        const auto& s = ctx_.schedule();
        if (!s.result.ok) throw ValidationError("The schedule does not calculate: " + (s.result.errors.empty() ? std::string("unknown error") : s.result.errors.front()));
        ctx_.report(tr("Schedule: %1 activities, finish %2, %3 critical (%4 ms)")
                        .arg(s.result.timings.size())
                        .arg(formatDateTimeMinutes(s.result.projectFinish))
                        .arg(std::count_if(s.result.timings.begin(), s.result.timings.end(), [](const auto& kv) { return kv.second.critical; }))
                        .arg(s.milliseconds, 0, 'f', 1));
    }, {"CPM", "CALC"});
    con.registerCommand("SCENARIO", "SCENARIO [name | BASE]", "Make a scenario active (no args lists them)", [=, this](const QStringList& a) {
        needProject();
        const auto& m = ctx_.project().model();
        if (a.isEmpty()) {
            QStringList names{"BASE"};
            for (const auto& s : m.scenarios.all()) names << qstr(s.name);
            return ctx_.report(tr("Scenarios: %1").arg(names.join(", ")));
        }
        const QString name = a.join(' ');
        if (name.compare("BASE", Qt::CaseInsensitive) == 0) return ctx_.setActiveScenario({});
        for (const auto& s : m.scenarios.all())
            if (qstr(s.name).compare(name, Qt::CaseInsensitive) == 0) return ctx_.setActiveScenario(s.id);
        throw ValidationError("No scenario " + sstr(name));
    });
    con.registerCommand("BASELINE", "BASELINE <name>", "Capture an approved, immutable baseline of the active schedule", [=, this](const QStringList& a) {
        needProject();
        if (a.isEmpty()) throw ValidationError("BASELINE needs a name");
        scheduler_->captureBaseline(a.join(' '));
    });
    con.registerCommand("IMPACTS", "IMPACTS", "Headings whose design changed since their schedule quantities were approved", [this](const QStringList&) {
        handleEntityAction("impacts", {});
    });
    con.registerCommand("APPLYDESIGN", "APPLYDESIGN", "Set flagged (or selected) activity quantities from the current design (audited)", [this](const QStringList&) {
        handleEntityAction("apply-quantities", ctx_.selection());
    });
    con.registerCommand("VOLUME", "VOLUME", "Design quantities of the selected headings", [=, this](const QStringList&) {
        needProject();
        const auto devs = ctx_.selected(EntityKind::Development);
        if (devs.empty()) throw ValidationError("Select development headings");
        double v = 0, t = 0, l = 0;
        for (const auto& id : devs) {
            const auto* d = ctx_.project().model().developments.find(id);
            const auto mt = mine_design::computeMetrics(ctx_.project(), *d);
            v += mt.volume;
            t += mt.tonnes;
            l += mt.length3d;
            ctx_.report(tr("%1: %2 m · section %3 m² · %4 m³ · %5 t").arg(qstr(d->headingId), formatNumber(mt.length3d, 2), formatNumber(mt.sectionArea, 2),
                                                                         formatNumber(mt.volume, 1), formatNumber(mt.tonnes, 0)));
        }
        if (devs.size() > 1) ctx_.report(tr("Total: %1 m · %2 m³ · %3 t").arg(formatNumber(l, 2), formatNumber(v, 1), formatNumber(t, 0)));
    }, {"VOL"});
    con.registerCommand("REPORT", "REPORT <kind> [pdf|html|xlsx]", "Generate a report: conformance, registration, pointcloud, gradetonnage, gantt, schedule-xlsx, production, scenarios, quantities",
                        [=, this](const QStringList& a) {
                            needProject();
                            if (a.isEmpty()) throw ValidationError("REPORT needs a kind");
                            reports_->generate(a[0].toLower(), a.size() > 1 ? a[1].toLower() : QStringLiteral("pdf"));
                        });

    // ---- scripting, plugins, jobs ----------------------------------------------------------------
    con.registerCommand("PYTHON", "PYTHON <statement>", "Run a Python statement in the embedded interpreter", [this](const QStringList& a) {
        if (!PythonConsolePanel::available()) throw ValidationError("This build has no embedded Python");
        if (a.isEmpty()) {
            dock("python")->show();
            dock("python")->raise();
            return;
        }
        if (!python_->executeLine(a.join(' '))) throw ValidationError("The Python statement raised an error (see the Python panel)");
    }, {"PY"});
    con.registerCommand("RUNSCRIPT", "RUNSCRIPT <path.py>", "Run a Python script file", [this](const QStringList& a) {
        if (!PythonConsolePanel::available()) throw ValidationError("This build has no embedded Python");
        if (a.isEmpty()) throw ValidationError("RUNSCRIPT needs a file");
        if (!python_->runFile(a.join(' '))) throw ValidationError("The script failed (see the Python panel)");
    });
    con.registerCommand("PLUGIN", "PLUGIN [command [json arguments]]", "Run a plugin command (no args lists them)", [=, this](const QStringList& a) {
        if (a.isEmpty()) {
            QStringList names;
            for (const auto& c : ctx_.pluginCommands()) names << qstr(c.name) + (c.python ? " (Python)" : "");
            return ctx_.report(names.isEmpty() ? tr("No plugin commands are loaded") : tr("Plugin commands: %1").arg(names.join(", ")));
        }
        needProject();
        const auto r = ctx_.runPluginCommand(sstr(a[0]), a.size() > 1 ? sstr(a.mid(1).join(' ')) : "{}");
        for (const auto& l : r.log) ctx_.report(qstr(l));
        if (!r.ok) throw ValidationError(r.error + (r.rolledBack ? " (its changes were rolled back)" : ""));
        ctx_.report(qstr(r.text));
    });
    con.registerCommand("JOBS", "JOBS [CANCEL]", "List background jobs, or cancel them all", [this](const QStringList& a) {
        if (!a.isEmpty() && a[0].compare("CANCEL", Qt::CaseInsensitive) == 0) {
            ctx_.jobs().cancelAll();
            return ctx_.report(tr("Cancellation requested"));
        }
        int n = 0;
        for (const auto& j : ctx_.jobs().jobs()) {
            static const char* states[] = {"queued", "running", "done", "failed", "cancelled"};
            const double f = j.handle ? j.handle->progress->fraction() : 0.0;
            ctx_.report(QStringLiteral("#%1 %2 — %3 %4%%5")
                            .arg(j.id)
                            .arg(j.name, tr(states[static_cast<int>(j.state())]))
                            .arg(static_cast<int>(f * 100))
                            .arg(j.resultText.isEmpty() ? QString() : " — " + j.resultText));
            ++n;
        }
        if (!n) ctx_.report(tr("No jobs"));
    });
    con.registerCommand("WAIT", "WAIT [seconds]", "Wait for the background jobs to finish (scripts)", [this](const QStringList& a) {
        const int ms = a.isEmpty() ? -1 : static_cast<int>(number(a[0], "Seconds") * 1000);
        if (!ctx_.jobs().waitAll(ms)) throw ValidationError("Jobs are still running");
        ctx_.report(tr("All jobs finished"));
    });
    // ---- point clouds, surfaces and solids (the spec's FILTER / MESH / REGISTER / EXTRUDE …) ---------
    auto cloud = [this, needProject]() {
        needProject();
        const Uuid id = primaryOf(EntityKind::PointCloud);
        if (id.isNil()) throw ValidationError("Select a point cloud first (SELECT <name>)");
        return id;
    };
    using CO = CloudProcessDialog::Operation;
    // Runs a point-cloud operation through the same dialog (and job) as the menus.
    auto runCloud = [this](CO op, const Uuid& id, const std::function<void(CloudProcessDialog&)>& configure) {
        CloudProcessDialog d(ctx_, op, id, this);
        configure(d);
        if (d.start() <= 0) throw ValidationError("The operation did not start (see the messages above)");
        ctx_.report(tr("%1 started as a background job (WAIT to wait for it)").arg(CloudProcessDialog::title(op)));
    };
    auto hasWord = [](const QStringList& a, const char* w) {
        for (const auto& x : a)
            if (x.compare(QLatin1String(w), Qt::CaseInsensitive) == 0) return true;
        return false;
    };
    auto numbers = [](const QStringList& a) {
        std::vector<double> v;
        for (const auto& x : a) {
            bool ok = false;
            const double d = x.toDouble(&ok);
            if (ok) v.push_back(d);
        }
        return v;
    };
    auto engineOf = [hasWord](const QStringList& a) { return hasWord(a, "OPEN3D") ? QStringLiteral("open3d") : hasWord(a, "PDAL") ? QStringLiteral("pdal") : QString(); };
    con.registerCommand("FILTER", "FILTER OUTLIERS [k ratio] | RADIUS r n | VOXEL size [CENTROID] | SPACING d | RANDOM n | GROUND  [OPEN3D]",
                        "Filter the selected point cloud into a derived dataset (the original is never changed)", [=, this](const QStringList& a) {
                            const Uuid id = cloud();
                            if (a.isEmpty()) throw ValidationError("FILTER needs OUTLIERS, RADIUS, VOXEL, SPACING, RANDOM or GROUND");
                            const QString kind = a[0].toUpper();
                            const auto n = numbers(a.mid(1));
                            const QString engine = engineOf(a);
                            if (kind == "OUTLIERS" || kind == "RADIUS") {
                                runCloud(CO::Outliers, id, [&](CloudProcessDialog& d) {
                                    d.setMode(kind == "RADIUS" ? 1 : 0);
                                    if (kind == "RADIUS") {
                                        if (n.size() < 2) throw ValidationError("FILTER RADIUS needs a radius and a neighbour count");
                                        d.setValue("b", n[0]);
                                        d.setValue("k", n[1]);
                                    } else {
                                        if (!n.empty()) d.setValue("k", n[0]);
                                        if (n.size() > 1) d.setValue("a", n[1]);
                                    }
                                    if (!engine.isEmpty() && !d.setEngine(engine)) throw ValidationError("That engine is not available in this build");
                                });
                            } else if (kind == "VOXEL" || kind == "SPACING" || kind == "RANDOM") {
                                if (n.empty()) throw ValidationError("Give the voxel size, spacing or point count");
                                runCloud(CO::Subsample, id, [&](CloudProcessDialog& d) {
                                    if (kind == "RANDOM") {
                                        d.setMode(3);
                                        d.setValue("k", n[0]);
                                    } else {
                                        d.setMode(kind == "SPACING" ? 2 : (hasWord(a, "CENTROID") || engine == "open3d" ? 1 : 0));
                                        d.setValue("a", n[0]);
                                    }
                                    if (!engine.isEmpty() && !d.setEngine(engine)) throw ValidationError("That engine is not available in this build");
                                });
                            } else if (kind == "GROUND") {
                                runCloud(CO::Ground, id, [](CloudProcessDialog&) {});
                            } else {
                                throw ValidationError("Unknown filter " + sstr(kind));
                            }
                        });
    con.registerCommand("NORMALS", "NORMALS [k] [UP|IN|OUT] [OPEN3D]", "Estimate normals of the selected point cloud", [=, this](const QStringList& a) {
        const Uuid id = cloud();
        const auto n = numbers(a);
        runCloud(CO::Normals, id, [&](CloudProcessDialog& d) {
            if (!n.empty()) d.setValue("k", n[0]);
            d.setMode(hasWord(a, "IN") ? 1 : hasWord(a, "OUT") ? 2 : 0);
            if (!engineOf(a).isEmpty() && !d.setEngine(engineOf(a))) throw ValidationError("That engine is not available in this build");
        });
    });
    con.registerCommand("MESH", "MESH DELAUNAY [maxEdge] | MESH POISSON [depth] [OPEN3D|PDAL]", "Surface from the selected point cloud",
                        [=, this](const QStringList& a) {
                            const Uuid id = cloud();
                            const bool poisson = !a.isEmpty() && a[0].compare("POISSON", Qt::CaseInsensitive) == 0;
                            const auto n = numbers(a.mid(1));
                            runCloud(poisson ? CO::Poisson : CO::Delaunay, id, [&](CloudProcessDialog& d) {
                                if (poisson) {
                                    if (!n.empty()) d.setValue("k", n[0]);
                                    if (!engineOf(a).isEmpty() && !d.setEngine(engineOf(a))) throw ValidationError("That engine is not available in this build");
                                } else if (!n.empty()) {
                                    d.setValue("b", n[0]);
                                }
                            });
                        });
    con.registerCommand("SEGMENT", "SEGMENT tolerance [minSize] [GROW angle] [DROP]", "Cluster the selected point cloud (connected components or region growing)",
                        [=, this](const QStringList& a) {
                            const Uuid id = cloud();
                            const auto n = numbers(a);
                            runCloud(CO::Segment, id, [&](CloudProcessDialog& d) {
                                const bool grow = hasWord(a, "GROW");
                                d.setMode(grow ? 1 : 0);
                                if (grow) {
                                    if (!n.empty()) d.setValue("b", n[0]);
                                    if (n.size() > 1) d.setValue("k", n[1]);
                                } else {
                                    if (n.empty()) throw ValidationError("SEGMENT needs a distance tolerance");
                                    d.setValue("a", n[0]);
                                    if (n.size() > 1) d.setValue("k", n[1]);
                                }
                                d.setFlag(hasWord(a, "DROP"));
                            });
                        }, {"CLUSTER"});
    con.registerCommand("CROP", "CROP x1,y1,z1 x2,y2,z2 [OUTSIDE] | CROP (draw a polygon)", "Crop the selected point cloud by a box or a drawn polygon",
                        [=, this](const QStringList& a) {
                            const Uuid id = cloud();
                            if (a.isEmpty()) return tool(Tool::CropPolygon);
                            const auto pts = parsePoints(a.filter(QRegularExpression("^[-@0-9.,<]+$")), workZ());
                            if (pts.size() != 2) throw ValidationError("CROP needs two opposite corners");
                            BoundingBox bb = BoundingBox::empty();
                            bb.expand(pts[0]);
                            bb.expand(pts[1]);
                            runCloud(CO::CropBox, id, [&](CloudProcessDialog& d) { d.setBox(bb, !hasWord(a, "OUTSIDE")); });
                        });
    con.registerCommand("REGISTER", "REGISTER ICP <target cloud> [maxCorr] [OPEN3D] [APPLY] | REGISTER PAIRS",
                        "Register the selected point cloud (ICP onto a target, or open point-pair registration)", [=, this](const QStringList& a) {
                            const Uuid moving = cloud();
                            if (a.isEmpty() || a[0].compare("PAIRS", Qt::CaseInsensitive) == 0) return handleEntityAction("register-pairs", {{EntityKind::PointCloud, moving}});
                            if (a[0].compare("ICP", Qt::CaseInsensitive) != 0 || a.size() < 2) throw ValidationError("REGISTER ICP needs the target cloud's name");
                            Uuid target;
                            for (const auto& pc : ctx_.project().model().pointClouds.all())
                                if (qstr(pc.name).compare(a[1], Qt::CaseInsensitive) == 0) target = pc.id;
                            if (target.isNil()) throw ValidationError("No point cloud named " + sstr(a[1]));
                            survey::IcpOptions o;
                            const auto n = numbers(a.mid(2));
                            if (!n.empty()) o.maxCorrespondence = n[0];
                            if (hasWord(a, "OPEN3D")) o.engine = "Open3D";
                            if (hasWord(a, "POINT")) o.mode = survey::IcpMode::PointToPoint;
                            const bool apply = hasWord(a, "APPLY");
                            auto snap = ctx_.snapshot();
                            AppContext* c = &ctx_;
                            ctx_.jobs().start(tr("ICP registration"), tr("Registration"), [snap, moving, target, o, apply, c](jobs::Progress& pr) -> UiApply {
                                auto r = std::make_shared<Registration>(survey::computeIcpRegistration(*snap, moving, target, o, {}, pr));
                                return [c, r, apply] {
                                    c->tryEdit(apply ? QObject::tr("Apply registration") : QObject::tr("Store registration"), [&] {
                                        survey::commitRegistration(c->project(), *r, apply);
                                        c->report(QObject::tr("ICP %1: fitness %2, inlier RMS %3 m, %4 iterations%5 (%6)")
                                                      .arg(apply ? QObject::tr("applied") : QObject::tr("stored"))
                                                      .arg(r->icpFitness, 0, 'f', 3)
                                                      .arg(r->icpInlierRms, 0, 'g', 4)
                                                      .arg(r->icpIterations)
                                                      .arg(r->icpConverged ? QString() : QObject::tr(", not converged"))
                                                      .arg(qstr(r->notes)));
                                    });
                                };
                            });
                            ctx_.report(tr("ICP started as a background job (WAIT to wait for it)"));
                        }, {"REG"});
    using SO = SolidModelDialog::Operation;
    auto solidOp = [this, needProject](SO op, const std::function<void(SolidModelDialog&)>& configure) {
        needProject();
        SolidModelDialog d(ctx_, op, this);
        configure(d);
        if (d.create().empty()) throw ValidationError("No solid was created (see the messages above)");
    };
    con.registerCommand("EXTRUDE", "EXTRUDE height | dx,dy,dz [SYM] [NOHOLES]", "Extrude the selected closed curves into solids", [=](const QStringList& a) {
        if (a.isEmpty()) throw ValidationError("EXTRUDE needs a height or a vector");
        Vec3 v;
        if (a[0].contains(',')) v = vectorArg(a[0], "The vector");
        else v = {0, 0, number(a[0], "Height")};
        solidOp(SO::Extrude, [&](SolidModelDialog& d) { d.setExtrusion(v, hasWord(a, "SYM"), !hasWord(a, "NOHOLES")); });
    }, {"EXT"});
    con.registerCommand("REVOLVE", "REVOLVE [angle] [px,py,pz dx,dy,dz]", "Revolve the selected closed curves (axis: a selected line, or the given one)",
                        [=](const QStringList& a) {
                            const double ang = a.isEmpty() ? 360.0 : number(a[0], "Angle");
                            solidOp(SO::Revolve, [&](SolidModelDialog& d) {
                                if (a.size() >= 3) d.setRevolution(parsePoints({a[1]}, workZ()).front(), vectorArg(a[2], "Axis direction"), ang);
                                else d.setRevolutionAngle(ang);
                            });
                        }, {"REV"});
    con.registerCommand("SWEEP", "SWEEP width height | SWEEP PROFILE <name>", "Sweep a tunnel profile along the selected path curves", [=, this](const QStringList& a) {
        solidOp(SO::Sweep, [&](SolidModelDialog& d) {
            if (!a.isEmpty() && a[0].compare("PROFILE", Qt::CaseInsensitive) == 0) {
                const QString name = a.mid(1).join(' ');
                for (const auto& pr : ctx_.project().model().profiles.all())
                    if (qstr(pr.name).compare(name, Qt::CaseInsensitive) == 0 && d.setSweepProfile(pr.id)) return;
                throw ValidationError("No tunnel profile named " + sstr(name));
            }
            if (a.size() < 2) throw ValidationError("SWEEP needs a width and a height (or PROFILE <name>)");
            d.setSweepRectangle(number(a[0], "Width"), number(a[1], "Height"));
        });
    });
    con.registerCommand("LOFT", "LOFT [RULED] [SHELL]", "Loft through the selected closed sections (in selection order)", [=](const QStringList& a) {
        solidOp(SO::Loft, [&](SolidModelDialog& d) { d.setLoft(!hasWord(a, "SHELL"), hasWord(a, "RULED")); });
    });
    con.registerCommand("SOLIDCHECK", "SOLIDCHECK", "Validity, volume, area and topology of the selected solids and meshes",
                        [this](const QStringList&) { handleEntityAction("solid-check", ctx_.selection()); }, {"CHECKSOLID"});
    con.registerCommand("HEAL", "HEAL", "Heal the selected solids (shape fixing, orientation, face merging)",
                        [this](const QStringList&) { handleEntityAction("solid-heal", ctx_.selection()); });
    con.registerCommand("SOLIDFILLET", "SOLIDFILLET <radius>", "Round every edge of the selected solids", [this](const QStringList& a) {
        if (a.isEmpty()) throw ValidationError("SOLIDFILLET needs a radius");
        handleEntityAction("solid-fillet:" + QString::number(number(a[0], "Radius"), 'g', 17), ctx_.selection());
    });
    con.registerCommand("SOLIDOFFSET", "SOLIDOFFSET <distance>", "Offset the faces of the selected solids (negative: inwards)", [this](const QStringList& a) {
        if (a.isEmpty()) throw ValidationError("SOLIDOFFSET needs a distance");
        handleEntityAction("solid-offset:" + QString::number(number(a[0], "Distance"), 'g', 17), ctx_.selection());
    });
    con.registerCommand("SECTIONOBJ", "SECTIONOBJ", "Section the selected solids and meshes by the section plane (or the work plane)",
                        [this](const QStringList&) { handleEntityAction("section-objects", ctx_.selection()); });
    con.registerCommand("CHAMFER", "CHAMFER [d1 [d2]]", "Chamfer two curves (pick them in the viewport)", [=, this](const QStringList& a) {
        needProject();
        auto& t = viewports_->tools();
        if (!a.isEmpty()) {
            const double d1 = number(a[0], "Distance");
            t.setChamferDistances(d1, a.size() > 1 ? number(a[1], "Distance") : d1);
        }
        tool(Tool::Chamfer);
    }, {"CHA"});
    using MO = MeshToolDialog::Operation;
    auto surface = [this, needProject]() {
        needProject();
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* g = ctx_.project().model().geometry.find(id); g && g->mesh) return id;
        throw ValidationError("Select a surface (mesh) first");
    };
    auto runMesh = [this](MO op, const Uuid& id, const std::function<void(MeshToolDialog&)>& configure) {
        MeshToolDialog d(ctx_, op, id, this);
        configure(d);
        if (d.start() <= 0) throw ValidationError("The operation did not start (see the messages above)");
    };
    con.registerCommand("DECIMATE", "DECIMATE triangles [maxError]", "Decimate the selected surface", [=](const QStringList& a) {
        const auto n = numbers(a);
        if (n.empty()) throw ValidationError("DECIMATE needs a target triangle count");
        runMesh(MO::Decimate, surface(), [&](MeshToolDialog& d) { d.setDecimation(static_cast<std::size_t>(n[0]), n.size() > 1 ? n[1] : 0.0); });
    });
    con.registerCommand("SMOOTH", "SMOOTH [iterations]", "Taubin-smooth the selected surface", [=](const QStringList& a) {
        const auto n = numbers(a);
        runMesh(MO::Smooth, surface(), [&](MeshToolDialog& d) { d.setSmoothing(n.empty() ? 10 : static_cast<int>(n[0])); });
    });
    con.registerCommand("REPAIR", "REPAIR [fillHolesUpToEdges]", "Repair the selected surface", [=](const QStringList& a) {
        const auto n = numbers(a);
        runMesh(MO::Repair, surface(), [&](MeshToolDialog& d) { d.setRepair(n.empty() ? 0 : static_cast<std::size_t>(n[0]), true); });
    });
    con.registerCommand("COMPARE", "COMPARE <reference surface> [maxDistance]", "Distance of the selected surface to a reference surface", [=, this](const QStringList& a) {
        const Uuid id = surface();
        if (a.isEmpty()) throw ValidationError("COMPARE needs the reference surface's name");
        const auto n = numbers(a.mid(1));
        Uuid ref;
        for (const auto& g : ctx_.project().model().geometry.all())
            if (g.mesh && g.id != id && qstr(g.name).compare(a[0], Qt::CaseInsensitive) == 0) ref = g.id;
        if (ref.isNil()) throw ValidationError("No surface named " + sstr(a[0]));
        runMesh(MO::Compare, id, [&](MeshToolDialog& d) { d.setReference(ref, n.empty() ? 0.0 : n[0]); });
    });
    con.registerCommand("RESOURCE", "RESOURCE <name> [capacity] [EQUIPMENT|CREW|MATERIAL|OTHER]", "Add a resource to the library", [=, this](const QStringList& a) {
        needProject();
        if (a.isEmpty()) throw ValidationError("RESOURCE needs a name");
        QStringList words = a;
        ResourceKind kind = ResourceKind::Equipment;
        if (const auto k = enumFromString<ResourceKind>(sstr(words.last().left(1).toUpper() + words.last().mid(1).toLower()))) {
            kind = *k;
            words.removeLast();
        }
        double capacity = 1;
        bool ok = false;
        if (words.size() > 1) {
            capacity = words.last().toDouble(&ok);
            if (ok) words.removeLast();
            else capacity = 1;
        }
        ResourceLibraryDialog d(ctx_, this);
        if (d.addResource(words.join(' '), kind, capacity).isNil()) throw ValidationError("The resource was not added");
        ctx_.report(tr("Resource '%1' (%2, capacity %3)").arg(words.join(' '), qstr(toString(kind))).arg(capacity));
    });
    con.registerCommand("ASSIGN", "ASSIGN <resource> [units] [DRIVES]", "Assign a resource to the selected activities", [=, this](const QStringList& a) {
        needProject();
        const auto acts = ctx_.selected(EntityKind::Activity);
        if (acts.empty() || a.isEmpty()) throw ValidationError("Select activities and give a resource name");
        QStringList words = a;
        const bool drives = hasWord(words, "DRIVES");
        words.removeIf([](const QString& w) { return w.compare("DRIVES", Qt::CaseInsensitive) == 0; });
        double units = 1;
        bool ok = false;
        if (words.size() > 1) {
            units = words.last().toDouble(&ok);
            if (ok) words.removeLast();
            else units = 1;
        }
        Uuid res;
        for (const auto& r : ctx_.project().model().resources.all())
            if (qstr(r.name).compare(words.join(' '), Qt::CaseInsensitive) == 0) res = r.id;
        if (res.isNil()) throw ValidationError("No resource named " + sstr(words.join(' ')));
        ResourceLibraryDialog d(ctx_, this);
        for (const auto& act : acts) d.assign(act, res, units, drives);
        ctx_.report(tr("%1 assigned to %2 activit%3").arg(words.join(' ')).arg(acts.size()).arg(acts.size() == 1 ? "y" : "ies"));
    });
#ifdef OPENMINE_APP_HAVE_CRS
    con.registerCommand("CRS", "CRS [EPSG:code | definition]", "Show or declare and set the project CRS (coordinates are never transformed)", [=, this](const QStringList& a) {
        needProject();
        if (a.isEmpty()) {
            const auto* c = ctx_.project().model().projectCrs();
            return ctx_.report(c ? tr("Project CRS: %1 (%2)").arg(qstr(c->name), qstr(c->authorityCode)) : tr("No project CRS declared"));
        }
        ctx_.tryEdit(tr("Set project CRS"), [&] {
            CommandStack::MacroScope macro(ctx_.project().commands(), "Set project CRS");
            const Uuid id = crs::declareCrs(ctx_.project(), sstr(a.join(' ')));
            crs::setProjectCrs(ctx_.project(), id);
            macro.commit();
            ctx_.report(tr("Project CRS: %1").arg(qstr(ctx_.project().model().crs.find(id)->name)));
        });
    });
#endif
}

} // namespace openmine::app
