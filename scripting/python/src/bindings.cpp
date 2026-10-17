// SPDX-License-Identifier: Apache-2.0
// Low-level pybind11 bindings (module openmine._core). The public Python API is the
// pure-Python facade in openmine/__init__.py, which is built on these functions.
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Cpm.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace openmine;

namespace {
Uuid uuidArg(const std::string& s) {
    auto id = Uuid::parse(s);
    if (!id) throw py::value_error("Not a valid UUID: '" + s + "'");
    return *id;
}
Date dateArg(const std::string& s) {
    auto d = Date::parse(s);
    if (!d) throw py::value_error("Dates must be YYYY-MM-DD, got '" + s + "'");
    return *d;
}
std::vector<Vec3> pointsArg(const std::vector<std::tuple<double, double, double>>& pts) {
    std::vector<Vec3> out;
    for (auto [x, y, z] : pts) out.push_back({x, y, z});
    return out;
}
py::list pointsOut(const std::vector<Vec3>& v) {
    py::list l;
    for (const auto& p : v) l.append(py::make_tuple(p.x, p.y, p.z));
    return l;
}

struct PyProject {
    std::unique_ptr<Project> p;
    Project& get() {
        if (!p) throw py::value_error("Project is closed");
        return *p;
    }
};

py::dict activityDict(const Project& p, const ScheduleActivity& a) {
    py::dict d;
    d["id"] = a.id.toString();
    d["name"] = a.name;
    d["wbs"] = a.wbsCode;
    d["kind"] = toString(a.kind);
    d["duration_days"] = a.durationDays;
    d["duration_from_rate"] = a.durationFromRate;
    d["quantity"] = a.quantity;
    d["unit"] = a.quantityUnit;
    d["rate_per_day"] = a.ratePerDay;
    d["actual_quantity"] = a.actualQuantity;
    d["design_impact"] = a.designImpactFlag;
    d["design_impact_note"] = a.designImpactNote;
    py::list devs;
    for (const auto& id : p.model().developmentsForActivity(a.id)) devs.append(id.toString());
    d["linked_developments"] = devs;
    return d;
}
} // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "OpenMine Suite native core (internal; use the 'openmine' package)";
    m.attr("__version__") = "0.1.0";
    m.attr("SCHEMA_VERSION") = persistence::kCurrentSchemaVersion;

    py::register_exception<ValidationError>(m, "ValidationError", PyExc_ValueError);
    py::register_exception<persistence::PersistenceError>(m, "PersistenceError", PyExc_IOError);

    py::class_<PyProject>(m, "NativeProject")
        .def_static("create", [](const std::string& name, const std::string& start) {
            return PyProject{Project::createNew(name, dateArg(start))};
        })
        .def_static("load", [](const std::filesystem::path& path) { return PyProject{persistence::loadProject(path)}; })
        .def("save", [](PyProject& s, const std::filesystem::path& path) { persistence::saveProject(s.get(), path); })
        .def_property_readonly("name", [](PyProject& s) { return s.get().model().info.name; })
        .def_property_readonly("id", [](PyProject& s) { return s.get().model().info.id.toString(); })
        .def_property_readonly("file_path", [](PyProject& s) { return s.get().filePath(); })
        .def_property_readonly("is_modified", [](PyProject& s) { return s.get().isModified(); })
        .def("undo", [](PyProject& s) { s.get().commands().undo(); })
        .def("redo", [](PyProject& s) { s.get().commands().redo(); })
        .def("audit_log", [](PyProject& s) {
            py::list l;
            for (const auto& e : s.get().model().audit)
                l.append(py::make_tuple(e.timestamp, e.user, e.action, e.entityType, e.entityId.toString(), e.detail));
            return l;
        })
        .def("set_crs", [](PyProject& s, const std::string& name, const std::string& code) {
            ProjectInfo i = s.get().model().info;
            i.crs.name = name;
            i.crs.authorityCode = code;
            ProjectEditor(s.get()).setProjectInfo(i, "Set CRS");
        })
        .def("crs", [](PyProject& s) { return py::make_tuple(s.get().model().info.crs.name, s.get().model().info.crs.authorityCode); })
        .def("set_status_date", [](PyProject& s, const std::optional<std::string>& d) {
            ProjectInfo i = s.get().model().info;
            if (d) i.statusDate = dateArg(*d);
            else i.statusDate.reset();
            ProjectEditor(s.get()).setProjectInfo(i, "Set status date");
        })
        // ---- CAD
        .def("layer_names", [](PyProject& s) {
            std::vector<std::string> out;
            for (const auto& l : s.get().model().layers.all()) out.push_back(l.name);
            return out;
        })
        .def("add_polyline", [](PyProject& s, const std::vector<std::tuple<double, double, double>>& pts, const std::string& name,
                                const std::string& layer, bool closed) {
            ProjectEditor ed(s.get());
            GeometryObject g;
            g.type = GeometryType::Polyline; // extendable; explicit Line objects come from the LINE tool
            g.vertices = pointsArg(pts);
            g.closed = closed;
            g.name = name;
            const auto* l = ed.findLayerByName(layer);
            if (!l) throw py::value_error("No layer named '" + layer + "'");
            g.layerId = l->id;
            return ed.addGeometry(g).toString();
        }, py::arg("points"), py::arg("name") = "", py::arg("layer") = "0", py::arg("closed") = false)
        .def("geometry", [](PyProject& s, const std::string& id) {
            const auto* g = s.get().model().geometry.find(uuidArg(id));
            if (!g) throw py::key_error(id);
            py::dict d;
            d["id"] = g->id.toString();
            d["name"] = g->name;
            d["type"] = toString(g->type);
            d["vertices"] = pointsOut(g->vertices);
            d["closed"] = g->closed;
            d["length"] = geometry::length3d(g->vertices, g->closed);
            d["layer"] = s.get().model().layers.find(g->layerId)->name;
            return d;
        })
        .def("geometry_ids", [](PyProject& s) {
            std::vector<std::string> out;
            for (const auto& g : s.get().model().geometry.all()) out.push_back(g.id.toString());
            return out;
        })
        .def("update_vertices", [](PyProject& s, const std::string& id, const std::vector<std::tuple<double, double, double>>& pts) {
            const auto* g0 = s.get().model().geometry.find(uuidArg(id));
            if (!g0) throw py::key_error(id);
            GeometryObject g = *g0;
            g.vertices = pointsArg(pts);
            return mine_design::updateGeometryWithImpact(s.get(), g);
        })
        // ---- Mine design
        .def("create_development", [](PyProject& s, const std::string& geometryId, const std::string& headingId, double width,
                                      double height, const std::string& level) {
            mine_design::NewDevelopment spec;
            spec.headingId = headingId;
            spec.width = width;
            spec.height = height;
            spec.level = level;
            return mine_design::createDevelopment(s.get(), uuidArg(geometryId), spec).toString();
        }, py::arg("geometry_id"), py::arg("heading_id"), py::arg("width") = 5.0, py::arg("height") = 5.0, py::arg("level") = "")
        .def("development", [](PyProject& s, const std::string& id) {
            const auto* d = s.get().model().developments.find(uuidArg(id));
            if (!d) throw py::key_error(id);
            const auto mt = mine_design::computeMetrics(s.get(), *d);
            py::dict r;
            r["id"] = d->id.toString();
            r["heading_id"] = d->headingId;
            r["level"] = d->level;
            r["width"] = d->designWidth;
            r["height"] = d->designHeight;
            r["geometry_id"] = d->geometryId.toString();
            r["length"] = mt.length3d;
            r["plan_length"] = mt.horizontalLength;
            r["gradient_percent"] = mt.gradientPercent ? py::cast(*mt.gradientPercent) : py::none();
            r["chainage_from"] = d->chainageFrom;
            r["chainage_to"] = mt.chainageTo;
            r["nominal_volume"] = mt.nominalVolume;
            r["status"] = d->status;
            return r;
        })
        .def("design_impacts", [](PyProject& s) {
            py::list l;
            for (const auto& i : mine_design::detectDesignImpacts(s.get()))
                l.append(py::make_tuple(i.headingId, i.approvedLength, i.currentLength, i.activityIds.size()));
            return l;
        })
        // ---- Scheduling
        .def("create_linked_activity", [](PyProject& s, const std::string& devId, double rate, const std::string& name) {
            return mine_design::createLinkedActivity(s.get(), uuidArg(devId), rate, name).toString();
        }, py::arg("development_id"), py::arg("rate_per_day"), py::arg("name") = "")
        .def("add_activity", [](PyProject& s, const std::string& name, double duration, const std::string& kind) {
            ScheduleActivity a;
            a.name = name;
            a.durationDays = duration;
            auto k = activityKindFromString(kind);
            if (!k) throw py::value_error("kind must be Task, Milestone or Summary");
            a.kind = *k;
            return ProjectEditor(s.get()).addActivity(a).toString();
        }, py::arg("name"), py::arg("duration_days") = 1.0, py::arg("kind") = "Task")
        .def("add_dependency", [](PyProject& s, const std::string& pred, const std::string& succ, const std::string& type, double lag) {
            ActivityDependency d;
            d.predecessorId = uuidArg(pred);
            d.successorId = uuidArg(succ);
            auto t = dependencyTypeFromString(type);
            if (!t) throw py::value_error("type must be FS, SS, FF or SF");
            d.type = *t;
            d.lagDays = lag;
            return ProjectEditor(s.get()).addDependency(d).toString();
        }, py::arg("predecessor"), py::arg("successor"), py::arg("type") = "FS", py::arg("lag_days") = 0.0)
        .def("activity", [](PyProject& s, const std::string& id) {
            const auto* a = s.get().model().activities.find(uuidArg(id));
            if (!a) throw py::key_error(id);
            return activityDict(s.get(), *a);
        })
        .def("activities", [](PyProject& s) {
            py::list l;
            for (const auto& a : s.get().model().activities.all()) l.append(activityDict(s.get(), a));
            return l;
        })
        .def("set_actual", [](PyProject& s, const std::string& id, double qty) {
            const auto* a0 = s.get().model().activities.find(uuidArg(id));
            if (!a0) throw py::key_error(id);
            ScheduleActivity a = *a0;
            a.actualQuantity = qty;
            ProjectEditor(s.get()).updateActivity(a, "Record actual");
        })
        .def("apply_design_quantities", [](PyProject& s, const std::string& id) { mine_design::applyDesignQuantities(s.get(), uuidArg(id)); })
        .def("calculate", [](PyProject& s) {
            const auto r = scheduling::calculateSchedule(s.get());
            py::dict out;
            out["ok"] = r.ok;
            out["errors"] = r.errors;
            out["warnings"] = r.warnings;
            out["project_start"] = r.projectStartDate.toString();
            out["project_finish"] = r.projectFinishDate.toString();
            out["duration_days"] = r.projectFinish;
            py::dict t;
            for (const auto& [id, x] : r.timings) {
                py::dict d;
                d["duration"] = x.duration;
                d["es"] = x.es;
                d["ef"] = x.ef;
                d["ls"] = x.ls;
                d["lf"] = x.lf;
                d["total_float"] = x.totalFloat;
                d["free_float"] = x.freeFloat;
                d["critical"] = x.critical;
                d["start"] = x.startDate.toString();
                d["finish"] = x.finishDate.toString();
                d["remaining_quantity"] = x.remainingQuantity;
                d["percent_complete"] = x.percentComplete;
                t[py::str(id.toString())] = d;
            }
            out["activities"] = t;
            return out;
        })
        .def("capture_baseline", [](PyProject& s, const std::string& name) {
            const auto r = scheduling::calculateSchedule(s.get());
            if (!r.ok) throw ValidationError("Schedule does not calculate: " + (r.errors.empty() ? std::string() : r.errors.front()));
            ProjectEditor(s.get()).addBaseline(scheduling::makeBaseline(s.get(), r, name));
        })
        .def("baseline_names", [](PyProject& s) {
            std::vector<std::string> out;
            for (const auto& b : s.get().model().baselines) out.push_back(b.name);
            return out;
        });

    m.def("polyline_length", [](const std::vector<std::tuple<double, double, double>>& pts, bool closed) {
        auto v = pointsArg(pts);
        return geometry::length3d(v, closed);
    }, py::arg("points"), py::arg("closed") = false);
}
