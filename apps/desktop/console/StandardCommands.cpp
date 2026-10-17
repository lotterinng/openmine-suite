// SPDX-License-Identifier: Apache-2.0
// Console commands. Each one calls the real, validated editing/analysis code.
#include "../MainWindow.h"
#include "../MainWindowActions.h"
#include "../project_explorer/ProjectExplorer.h"
#include "../properties/PropertiesPanel.h"
#include "../scheduler/SchedulerPanel.h"
#include "../theme/Theme.h"
#include "../viewport/Viewport3D.h"
#include "CommandConsole.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"
#ifdef OPENMINE_HAS_OCCT
#include "openmine/occt/SweptSolid.h"
#endif

#include <QApplication>
#include <QDockWidget>

#include <cmath>
#include <numbers>

namespace openmine::app {

namespace {
/// "x,y,z" | "x,y" (z = work plane) | "@dx,dy[,dz]" relative to the previous point.
std::optional<Vec3> parsePoint(const QString& tok, const std::optional<Vec3>& prev, double workZ) {
    QString t = tok.trimmed();
    bool rel = t.startsWith('@');
    if (rel) t = t.mid(1);
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

std::vector<Vec3> parsePoints(const QStringList& args, double workZ, bool& closed) {
    std::vector<Vec3> pts;
    closed = false;
    std::optional<Vec3> prev;
    for (const auto& a : args) {
        if (a.compare("CLOSE", Qt::CaseInsensitive) == 0 || a.compare("C", Qt::CaseInsensitive) == 0) {
            closed = true;
            continue;
        }
        auto p = parsePoint(a, prev, workZ);
        if (!p) throw ValidationError("Cannot read point '" + a.toStdString() + "' (use x,y,z or @dx,dy,dz)");
        pts.push_back(*p);
        prev = p;
    }
    return pts;
}

double requireDouble(const QString& s, const char* what) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (!ok || !std::isfinite(v)) throw ValidationError(std::string(what) + " must be a number");
    return v;
}
} // namespace

void MainWindow::registerCommands() {
    auto& con = *console_;
    auto needProject = [this] {
        if (!ctx_.hasProject()) throw ValidationError("No project is open (NEW or OPEN first)");
    };
    auto selectedGeometry = [this]() -> GeometryObject {
        const auto& sel = ctx_.selection();
        const auto& m = ctx_.project().model();
        if (sel.kind == EntityKind::Geometry) return *m.geometry.find(sel.id);
        if (sel.kind == EntityKind::Development) return *m.geometry.find(m.developments.find(sel.id)->geometryId);
        throw ValidationError("Select a line, polyline or heading first");
    };

    con.registerCommand("NEW", "NEW <name> [YYYY-MM-DD]", "Create a new project (schedule starts today unless given)",
                        [this](const QStringList& a) {
                            if (!maybeSave()) return;
                            QStringList words = a;
                            std::optional<Date> start;
                            if (!words.isEmpty()) start = Date::parse(words.last().toStdString());
                            if (start) words.removeLast();
                            ctx_.newProject(words.isEmpty() ? "New Mine Project" : words.join(' '), start.value_or(Date::todayUtc()));
                        });
    con.registerCommand("OPEN", "OPEN <path>", "Open an .openmine project", [this](const QStringList& a) {
        if (a.isEmpty()) throw ValidationError("OPEN needs a file path");
        if (!maybeSave()) return;
        openProjectFrom(a.join(' '));
    });
    con.registerCommand("SAVE", "SAVE [path]", "Save the project (atomic, verified)", [this, needProject](const QStringList& a) {
        needProject();
        QString path = a.join(' ');
        if (path.isEmpty()) path = QString::fromStdU16String(ctx_.project().filePath().u16string());
        if (path.isEmpty()) throw ValidationError("First save needs a path: SAVE C:/Mines/MyMine.openmine");
        if (!path.endsWith(".openmine")) path += ".openmine";
        saveProjectTo(path);
    });
    con.registerCommand("CLOSE", "CLOSE", "Close the project", [this](const QStringList&) { closeProject(); });
    con.registerCommand("UNDO", "UNDO", "Undo the last change", [this, needProject](const QStringList&) {
        needProject();
        if (!ctx_.project().commands().canUndo()) throw ValidationError("Nothing to undo");
        const auto t = ctx_.project().commands().undoText();
        ctx_.project().commands().undo();
        ctx_.report("Undone: " + qstr(t));
    }, {"U"});
    con.registerCommand("REDO", "REDO", "Redo the last undone change", [this, needProject](const QStringList&) {
        needProject();
        if (!ctx_.project().commands().canRedo()) throw ValidationError("Nothing to redo");
        const auto t = ctx_.project().commands().redoText();
        ctx_.project().commands().redo();
        ctx_.report("Redone: " + qstr(t));
    });

    // ---- CAD -----------------------------------------------------------------
    auto create = [this, needProject](const QStringList& a, bool line) {
        needProject();
        if (a.isEmpty()) {
            viewport_->setTool(line ? Viewport3D::Tool::DrawLine : Viewport3D::Tool::DrawPolyline);
            return;
        }
        bool closed = false;
        auto pts = parsePoints(a, viewport_->workPlaneZ(), closed);
        if (line && pts.size() != 2) throw ValidationError("LINE needs exactly two points");
        GeometryObject g;
        g.type = line ? GeometryType::Line : GeometryType::Polyline;
        g.vertices = pts;
        g.closed = closed && !line;
        const auto& m = ctx_.project().model();
        g.layerId = ctx_.selection().kind == EntityKind::Layer ? ctx_.selection().id : m.layers.all().front().id;
        for (const auto& l : m.layers.all())
            if (!g.layerId && !l.locked) g.layerId = l.id;
        const Uuid id = ProjectEditor(ctx_.project()).addGeometry(g);
        ctx_.select(EntityKind::Geometry, id);
        ctx_.report(QStringLiteral("%1 %2 created: %3 vertices, 3D length %4 m")
                        .arg(line ? "Line" : "Polyline", qstr(id.toString()))
                        .arg(pts.size())
                        .arg(formatNumber(geometry::length3d(pts, g.closed))));
    };
    con.registerCommand("LINE", "LINE [p1 p2]", "Create a line (no args: draw in the viewport)",
                        [create](const QStringList& a) { create(a, true); }, {"L"});
    con.registerCommand("POLYLINE", "POLYLINE [p1 p2 … [CLOSE]]", "Create a polyline (no args: draw in the viewport)",
                        [create](const QStringList& a) { create(a, false); }, {"PLINE", "PL"});
    con.registerCommand("DISTANCE", "DISTANCE p1 p2", "Measure between two points (no args: pick in viewport)",
                        [this](const QStringList& a) {
                            if (a.isEmpty()) {
                                a_->measure->trigger();
                                return;
                            }
                            bool closed;
                            auto pts = parsePoints(a, viewport_->workPlaneZ(), closed);
                            if (pts.size() != 2) throw ValidationError("DISTANCE needs two points");
                            const Vec3 d = pts[1] - pts[0];
                            ctx_.report(QStringLiteral("Distance: 3D %1 m · plan %2 m · ΔZ %3 m")
                                            .arg(formatNumber(d.length()), formatNumber(d.horizontalLength()), formatNumber(d.z)));
                        }, {"DI", "DIST"});
    con.registerCommand("LENGTH", "LENGTH", "Report length and gradient of the selected object", [this, needProject, selectedGeometry](const QStringList&) {
        needProject();
        const auto g = selectedGeometry();
        auto grad = geometry::gradientPercent(g.vertices);
        ctx_.report(QStringLiteral("%1: 3D %2 m · plan %3 m · gradient %4")
                        .arg(qstr(g.name), formatNumber(geometry::length3d(g.vertices, g.closed)),
                             formatNumber(geometry::horizontalLength(g.vertices, g.closed)),
                             grad ? formatNumber(*grad, 2) + " %" : "n/a"));
    });
    con.registerCommand("AREA", "AREA", "Plan area and perimeter of the selected closed polyline", [this, needProject, selectedGeometry](const QStringList&) {
        needProject();
        const auto g = selectedGeometry();
        if (!g.closed || g.vertices.size() < 3) throw ValidationError("AREA needs a closed polyline with at least 3 vertices");
        double s = 0; // shoelace, relative to the first vertex for precision
        const Vec3 o = g.vertices.front();
        for (std::size_t i = 0; i < g.vertices.size(); ++i) {
            const Vec3 p = g.vertices[i] - o, q = g.vertices[(i + 1) % g.vertices.size()] - o;
            s += p.x * q.y - q.x * p.y;
        }
        ctx_.report(QStringLiteral("%1: plan area %2 m² · perimeter (3D) %3 m")
                        .arg(qstr(g.name), formatNumber(std::abs(s) / 2.0), formatNumber(geometry::length3d(g.vertices, true))));
    });
    auto transform = [this, needProject, selectedGeometry](const QStringList& a, bool copy) {
        needProject();
        if (a.size() != 1) throw ValidationError("Give a displacement dx,dy[,dz]");
        auto d = parsePoint("@" + a.front().trimmed().remove('@'), Vec3{}, 0);
        if (!d) throw ValidationError("Displacement must be dx,dy[,dz]");
        GeometryObject g = selectedGeometry();
        for (auto& v : g.vertices) v = v + *d;
        if (copy) {
            g.id = Uuid{};
            g.name += " (copy)";
            ctx_.select(EntityKind::Geometry, ProjectEditor(ctx_.project()).addGeometry(g));
            ctx_.report("Copied by " + a.front());
        } else {
            const int flagged = mine_design::updateGeometryWithImpact(ctx_.project(), g, "Move " + g.name);
            ctx_.report("Moved by " + a.front() + (flagged ? QStringLiteral(" — %1 schedule activities flagged").arg(flagged) : QString()));
        }
    };
    con.registerCommand("MOVE", "MOVE dx,dy[,dz]", "Move the selected object", [transform](const QStringList& a) { transform(a, false); }, {"M"});
    con.registerCommand("COPY", "COPY dx,dy[,dz]", "Copy the selected object with an offset", [transform](const QStringList& a) { transform(a, true); }, {"CO", "CP"});
    con.registerCommand("ROTATE", "ROTATE <degrees> [bx,by]", "Rotate the selected object about Z (base point default: first vertex)",
                        [this, needProject, selectedGeometry](const QStringList& a) {
                            needProject();
                            if (a.isEmpty()) throw ValidationError("ROTATE needs an angle in degrees");
                            const double deg = requireDouble(a[0], "Angle");
                            GeometryObject g = selectedGeometry();
                            Vec3 base = g.vertices.front();
                            if (a.size() > 1) {
                                auto b = parsePoint(a[1], std::nullopt, base.z);
                                if (!b) throw ValidationError("Base point must be x,y[,z]");
                                base = *b;
                            }
                            const double r = deg * std::numbers::pi / 180.0, cs = std::cos(r), sn = std::sin(r);
                            for (auto& v : g.vertices) {
                                const double x = v.x - base.x, y = v.y - base.y;
                                v.x = base.x + x * cs - y * sn;
                                v.y = base.y + x * sn + y * cs;
                            }
                            const int flagged = mine_design::updateGeometryWithImpact(ctx_.project(), g, "Rotate " + g.name);
                            ctx_.report(QStringLiteral("Rotated %1° about Z").arg(deg) + (flagged ? " (schedule impacts flagged)" : ""));
                        }, {"RO"});
    con.registerCommand("DELETE", "DELETE", "Delete the selected object", [this, needProject](const QStringList&) {
        needProject();
        const auto sel = ctx_.selection();
        ProjectEditor ed(ctx_.project());
        switch (sel.kind) {
        case EntityKind::Geometry: ed.removeGeometry(sel.id); break;
        case EntityKind::Development: ed.removeDevelopment(sel.id); break;
        case EntityKind::Activity: ed.removeActivity(sel.id); break;
        default: throw ValidationError("Select a geometry object, heading or activity to delete");
        }
        ctx_.report("Deleted (UNDO to restore).");
    }, {"ERASE", "E", "DEL"});
    con.registerCommand("ELEV", "ELEV <z>", "Set the work-plane elevation used for drawing", [this](const QStringList& a) {
        if (a.isEmpty()) {
            ctx_.report("Work plane Z = " + formatNumber(viewport_->workPlaneZ()));
            return;
        }
        viewport_->setWorkPlaneZ(requireDouble(a[0], "Elevation"));
        ctx_.report("Work plane Z = " + formatNumber(viewport_->workPlaneZ()));
    });
    con.registerCommand("LAYER", "LAYER <name> [#RRGGBB]", "Create a layer and make it current", [this, needProject](const QStringList& a) {
        needProject();
        if (a.isEmpty()) throw ValidationError("LAYER needs a name");
        QStringList words = a;
        std::uint32_t rgb = 0x9AD06B;
        if (words.size() > 1 && words.last().startsWith('#')) {
            bool ok = false;
            rgb = words.takeLast().mid(1).toUInt(&ok, 16);
            if (!ok) throw ValidationError("Colour must be #RRGGBB");
        }
        ProjectEditor ed(ctx_.project());
        const auto* existing = ed.findLayerByName(sstr(words.join(' ')));
        const Uuid id = existing ? existing->id : ed.addLayer(sstr(words.join(' ')), rgb);
        ctx_.select(EntityKind::Layer, id);
        ctx_.report("Current layer: " + words.join(' '));
    }, {"LA"});

    // ---- Selection / properties --------------------------------------------------
    con.registerCommand("SELECT", "SELECT <uuid | name | heading id>", "Select an entity", [this, needProject](const QStringList& a) {
        needProject();
        const QString key = a.join(' ');
        const auto& m = ctx_.project().model();
        auto id = Uuid::parse(sstr(key));
        for (const auto& g : m.geometry.all())
            if ((id && g.id == *id) || qstr(g.name) == key) return ctx_.select(EntityKind::Geometry, g.id);
        for (const auto& d : m.developments.all())
            if ((id && d.id == *id) || qstr(d.headingId) == key) return ctx_.select(EntityKind::Development, d.id);
        for (const auto& x : m.activities.all())
            if ((id && x.id == *id) || qstr(x.name) == key) return ctx_.select(EntityKind::Activity, x.id);
        for (const auto& l : m.layers.all())
            if ((id && l.id == *id) || qstr(l.name) == key) return ctx_.select(EntityKind::Layer, l.id);
        if (key.compare("PROJECT", Qt::CaseInsensitive) == 0) return ctx_.clearSelection();
        throw ValidationError("Nothing matches '" + sstr(key) + "'");
    }, {"SEL"});
    con.registerCommand("PROP", "PROP <key> <value…>", "Edit a property of the selection (PROP with no args lists keys)",
                        [this, needProject](const QStringList& a) {
                            needProject();
                            if (a.isEmpty()) {
                                ctx_.report("Editable: " + properties_->keys().join(", "));
                                return;
                            }
                            QStringList rest = a.mid(1);
                            properties_->setProperty(a[0], rest.join(' '));
                        });
    con.registerCommand("LIST", "LIST", "Summarise the project contents", [this, needProject](const QStringList&) {
        needProject();
        const auto& m = ctx_.project().model();
        ctx_.report(QStringLiteral("%1: %2 layers, %3 geometry objects, %4 headings, %5 activities, %6 dependencies, %7 links, %8 baselines, %9 audit entries")
                        .arg(qstr(m.info.name))
                        .arg(m.layers.size())
                        .arg(m.geometry.size())
                        .arg(m.developments.size())
                        .arg(m.activities.size())
                        .arg(m.dependencies.size())
                        .arg(m.designLinks.size())
                        .arg(m.baselines.size())
                        .arg(m.audit.size()));
    });

    // ---- Mine design + scheduling ------------------------------------------------
    con.registerCommand("HEADING", "HEADING <id> [width height] [level]", "Make the selected centreline a development heading",
                        [this, needProject](const QStringList& a) {
                            needProject();
                            if (a.isEmpty()) throw ValidationError("HEADING needs a heading ID");
                            const double w = a.size() > 1 ? requireDouble(a[1], "Width") : 5.5;
                            const double h = a.size() > 2 ? requireDouble(a[2], "Height") : w;
                            createHeadingFromSelection(a[0], w, h, a.size() > 3 ? a[3] : QString());
                        }, {"DEV"});
    con.registerCommand("ACTIVITY", "ACTIVITY <rate m/day>", "Create a schedule activity linked to the selected heading",
                        [this, needProject](const QStringList& a) {
                            needProject();
                            createLinkedActivityForSelection(a.isEmpty() ? 5.0 : requireDouble(a[0], "Rate"));
                        }, {"ACT"});
    con.registerCommand("ACTUAL", "ACTUAL <quantity>", "Record actual advance on the selected activity (or the heading's activity)",
                        [this, needProject](const QStringList& a) {
                            needProject();
                            if (a.isEmpty()) throw ValidationError("ACTUAL needs a quantity");
                            const auto& m = ctx_.project().model();
                            Uuid act;
                            if (ctx_.selection().kind == EntityKind::Activity) act = ctx_.selection().id;
                            else if (ctx_.selection().kind == EntityKind::Development) {
                                auto acts = m.activitiesForDevelopment(ctx_.selection().id);
                                if (acts.size() == 1) act = acts.front();
                            }
                            if (!act) throw ValidationError("Select one activity (or a heading with one linked activity)");
                            ScheduleActivity x = *m.activities.find(act);
                            x.actualQuantity = requireDouble(a[0], "Quantity");
                            ProjectEditor(ctx_.project()).updateActivity(x, "Record actual " + sstr(a[0]) + " " + x.quantityUnit + " on '" + x.name + "'");
                            ctx_.recalculateSchedule();
                            const auto& t = ctx_.schedule().timings.at(act);
                            ctx_.report(QStringLiteral("Actual recorded: %1% complete, %2 %3 remaining")
                                            .arg(formatNumber(t.percentComplete, 1), formatNumber(t.remainingQuantity, 2), qstr(x.quantityUnit)));
                        });
    con.registerCommand("STATUSDATE", "STATUSDATE <YYYY-MM-DD | NONE>", "Set the data date for reforecasting remaining work",
                        [this, needProject](const QStringList& a) {
                            needProject();
                            ProjectInfo i = ctx_.project().model().info;
                            if (a.isEmpty() || a[0].compare("NONE", Qt::CaseInsensitive) == 0) i.statusDate.reset();
                            else {
                                auto d = Date::parse(sstr(a[0]));
                                if (!d) throw ValidationError("Date must be YYYY-MM-DD");
                                i.statusDate = *d;
                            }
                            ProjectEditor(ctx_.project()).setProjectInfo(i, "Set status date");
                            ctx_.report(i.statusDate ? "Status date " + qstr(i.statusDate->toString()) + " — remaining work reforecast" : "Status date cleared");
                        });
    con.registerCommand("LINK", "LINK <heading id>", "Link the selected activity to a heading", [this, needProject](const QStringList& a) {
        needProject();
        if (ctx_.selection().kind != EntityKind::Activity || a.isEmpty()) throw ValidationError("Select an activity and give a heading ID");
        for (const auto& d : ctx_.project().model().developments.all())
            if (qstr(d.headingId) == a[0]) {
                ProjectEditor(ctx_.project()).linkActivityToDevelopment(ctx_.selection().id, d.id);
                ctx_.report("Linked to " + a[0]);
                return;
            }
        throw ValidationError("No heading " + sstr(a[0]));
    });
    con.registerCommand("SCHEDULE", "SCHEDULE", "Calculate the critical path schedule", [this](const QStringList&) { scheduler_->calculate(); }, {"CPM", "CALC"});
    con.registerCommand("BASELINE", "BASELINE <name>", "Capture an approved, immutable baseline", [this, needProject](const QStringList& a) {
        needProject();
        if (a.isEmpty()) throw ValidationError("BASELINE needs a name");
        ctx_.recalculateSchedule();
        if (!ctx_.schedule().ok) throw ValidationError("Schedule does not calculate");
        ProjectEditor(ctx_.project()).addBaseline(scheduling::makeBaseline(ctx_.project(), ctx_.schedule(), sstr(a.join(' '))));
        ctx_.report("Baseline '" + a.join(' ') + "' captured");
    });
    con.registerCommand("IMPACTS", "IMPACTS", "List headings whose design changed since schedule quantities were approved",
                        [this, needProject](const QStringList&) {
                            needProject();
                            auto list = mine_design::detectDesignImpacts(ctx_.project());
                            if (list.empty()) ctx_.report("No design changes pending schedule review.");
                            for (const auto& i : list)
                                ctx_.report(QStringLiteral("%1: approved %2 m → current %3 m; %4 linked activit%5 flagged")
                                                .arg(qstr(i.headingId), formatNumber(i.approvedLength), formatNumber(i.currentLength))
                                                .arg(i.activityIds.size())
                                                .arg(i.activityIds.size() == 1 ? "y" : "ies"));
                        });
    con.registerCommand("APPLYDESIGN", "APPLYDESIGN", "Recalculate the selected activity quantity from its linked design (audited)",
                        [this](const QStringList&) { scheduler_->applyDesignQuantities(); });
    con.registerCommand("VOLUME", "VOLUME", "Design excavation volume of the selected heading", [this, needProject](const QStringList&) {
        needProject();
        if (ctx_.selection().kind != EntityKind::Development) throw ValidationError("Select a development heading");
        const auto& d = *ctx_.project().model().developments.find(ctx_.selection().id);
        const auto mt = mine_design::computeMetrics(ctx_.project(), d);
        QString msg = QStringLiteral("%1: nominal %2 m³ (w×h×L)").arg(qstr(d.headingId), formatNumber(mt.nominalVolume, 1));
#ifdef OPENMINE_HAS_OCCT
        auto s = occt::sweepRectangularProfile(ctx_.project().model().geometry.find(d.geometryId)->vertices, d.designWidth, d.designHeight);
        msg += s.ok ? QStringLiteral(" · swept solid %1 m³, surface %2 m²%3").arg(formatNumber(s.volume, 1), formatNumber(s.surfaceArea, 1), s.valid ? "" : " (solid invalid)")
                    : " · swept solid unavailable: " + qstr(s.error);
#endif
        ctx_.report(msg);
    }, {"VOL"});
    con.registerCommand("EXPORTCSV", "EXPORTCSV <path>", "Export the calculated schedule to CSV", [this](const QStringList& a) {
        if (a.isEmpty()) throw ValidationError("EXPORTCSV needs a path");
        scheduler_->exportCsv(a.join(' '));
    });

    // ---- View ------------------------------------------------------------------
    con.registerCommand("ZOOMEXTENTS", "ZOOMEXTENTS", "Fit everything in the viewport", [this](const QStringList&) { viewport_->fitAll(); }, {"ZE", "FIT"});
    con.registerCommand("VIEW", "VIEW TOP|FRONT|SIDE|ISO", "Set a standard view", [this](const QStringList& a) {
        if (a.isEmpty()) throw ValidationError("VIEW needs TOP, FRONT, SIDE or ISO");
        viewport_->setView(a[0].toLower());
    });
    con.registerCommand("THEME", "THEME DARK|LIGHT", "Switch colour theme", [this](const QStringList& a) {
        setTheme(a.isEmpty() || a[0].compare("LIGHT", Qt::CaseInsensitive) != 0);
    });

    // Planned commands: discoverable, never simulated.
    for (const auto& [n, phase] : std::vector<std::pair<QString, QString>>{
             {"ARC", "Phase 2"}, {"CIRCLE", "Phase 2"}, {"OFFSET", "Phase 2"}, {"TRIM", "Phase 2"}, {"EXTEND", "Phase 2"},
             {"FILLET", "Phase 2"}, {"EXTRUDE", "Phase 6"}, {"SECTION", "Phase 6"}, {"REGISTER", "Phase 3"},
             {"FILTER", "Phase 3"}, {"MESH", "Phase 3"}, {"IMPORT", "Phase 3 (LAS/LAZ, E57)"}, {"DXFOUT", "Phase 2"}})
        con.registerPlanned(n, phase);
}

} // namespace openmine::app
