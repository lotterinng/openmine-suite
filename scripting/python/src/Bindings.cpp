// SPDX-License-Identifier: Apache-2.0
// Native bindings. Conventions: identifiers are uuid strings; dates "YYYY-MM-DD" and date-times
// "YYYY-MM-DD HH:MM"; points are numpy float64 arrays of shape (n, 3) (any n×3 sequence is
// accepted); meshes are (vertices (n,3) float64, triangles (m,3) uint32). Long operations release
// the GIL and accept an optional Progress (cancel() from another thread). Everything that changes
// a project goes through the editor: validated, undoable and audited exactly as in the application.
#include "Bindings.h"

#include "openmine/cad/Edit.h"
#include "openmine/formats/DxfProject.h"
#include "openmine/formats/MeshFormats.h"
#include "openmine/geology/BlockModel.h"
#include "openmine/geology/Compositing.h"
#include "openmine/geology/Desurvey.h"
#include "openmine/geology/Drillholes.h"
#include "openmine/geology/Estimation.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/geotechnical/RockMass.h"
#include "openmine/geotechnical/Stereonet.h"
#include "openmine/mine_design/Development.h"
#include "openmine/omf/OmfProject.h"
#include "openmine/persistence/EntityCodec.h"
#include "openmine/persistence/ProjectFile.h"
#include "openmine/planning/FourD.h"
#include "openmine/plugins/PluginManager.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/reporting/ScheduleExport.h"
#include "openmine/reporting/SurveyReports.h"
#include "openmine/scheduling/Cpm.h"
#include "openmine/scheduling/ScheduleTable.h"
#include "openmine/survey/Conformance.h"
#include "openmine/survey/Meshing.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/Processing.h"
#include "openmine/survey/Registration.h"
#include "openmine/util/Csv.h"

#ifdef OPENMINE_PY_HAVE_OPTIMISER
#include "openmine/optimisation/Optimiser.h"
#endif
#ifdef OPENMINE_PY_HAVE_CRS
#include "openmine/crs/Crs.h"
#endif
#ifdef OPENMINE_PY_HAVE_GIS
#include "openmine/gis/Gis.h"
#endif
#ifdef OPENMINE_PY_HAVE_REPOSITORY
#include "openmine/repository/PgRepository.h"
#endif
#ifdef OPENMINE_PY_HAVE_OCCT
#include "openmine/occt/SolidObjects.h"
#endif
#ifdef OPENMINE_PY_HAVE_PDAL
#include "openmine/pdalio/Pdal.h"
#endif
#ifdef OPENMINE_PY_HAVE_OPEN3D
#include "openmine/o3d/Open3d.h"
#endif

#include <nlohmann/json.hpp>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <cmath>
#include <fstream>
#include <sstream>

namespace py = pybind11;
using nlohmann::json;
namespace fs = std::filesystem;
namespace pe = openmine::persistence;

namespace openmine::python {

Project& PyProject::get() const {
    if (!p_) throw py::value_error("The project is closed");
    return *p_;
}

namespace {
using Points = py::array_t<double, py::array::c_style | py::array::forcecast>;
using Tris = py::array_t<std::uint32_t, py::array::c_style | py::array::forcecast>;

// ---- conversions ----------------------------------------------------------------------------------
Uuid uuidArg(const std::string& s) {
    auto id = Uuid::parse(s);
    if (!id) throw py::value_error("Not a valid identifier: '" + s + "'");
    return *id;
}
std::string idStr(const Uuid& u) { return u.isNil() ? std::string() : u.toString(); }
Date dateArg(const std::string& s) {
    auto d = Date::parse(s);
    if (!d) throw py::value_error("Dates must be YYYY-MM-DD, got '" + s + "'");
    return *d;
}
DateTime dateTimeArg(const std::string& s) {
    if (auto dt = DateTime::parse(s)) return *dt;
    return DateTime::fromDate(dateArg(s));
}

std::vector<Vec3> toPoints(const Points& a) {
    if (a.ndim() == 1 && a.shape(0) == 0) return {};
    if (a.ndim() != 2 || (a.shape(1) != 3 && a.shape(1) != 2)) throw py::value_error("Points must have shape (n, 3) or (n, 2)");
    const auto r = a.unchecked<2>();
    std::vector<Vec3> v(static_cast<std::size_t>(a.shape(0)));
    for (py::ssize_t i = 0; i < a.shape(0); ++i) v[static_cast<std::size_t>(i)] = {r(i, 0), r(i, 1), a.shape(1) == 3 ? r(i, 2) : 0.0};
    return v;
}
py::array_t<double> fromPoints(const std::vector<Vec3>& v) {
    py::array_t<double> a({static_cast<py::ssize_t>(v.size()), static_cast<py::ssize_t>(3)});
    auto w = a.mutable_unchecked<2>();
    for (std::size_t i = 0; i < v.size(); ++i) {
        w(static_cast<py::ssize_t>(i), 0) = v[i].x;
        w(static_cast<py::ssize_t>(i), 1) = v[i].y;
        w(static_cast<py::ssize_t>(i), 2) = v[i].z;
    }
    return a;
}
MeshData toMesh(const Points& v, const Tris& t) {
    MeshData m;
    m.vertices = toPoints(v);
    if (t.ndim() != 2 || t.shape(1) != 3) throw py::value_error("Triangles must have shape (m, 3)");
    const auto r = t.unchecked<2>();
    for (py::ssize_t i = 0; i < t.shape(0); ++i) {
        std::array<std::uint32_t, 3> tri{r(i, 0), r(i, 1), r(i, 2)};
        for (auto k : tri)
            if (k >= m.vertices.size()) throw py::value_error("Triangle index out of range");
        m.triangles.push_back(tri);
    }
    return m;
}
py::tuple fromMesh(const MeshData& m) {
    py::array_t<std::uint32_t> t({static_cast<py::ssize_t>(m.triangles.size()), static_cast<py::ssize_t>(3)});
    auto w = t.mutable_unchecked<2>();
    for (std::size_t i = 0; i < m.triangles.size(); ++i)
        for (int k = 0; k < 3; ++k) w(static_cast<py::ssize_t>(i), k) = m.triangles[i][static_cast<std::size_t>(k)];
    return py::make_tuple(fromPoints(m.vertices), t);
}
py::array_t<double> fromVector(const std::vector<double>& v) {
    py::array_t<double> a(static_cast<py::ssize_t>(v.size()));
    std::copy(v.begin(), v.end(), a.mutable_data());
    return a;
}
std::vector<double> toVector(const py::array_t<double, py::array::c_style | py::array::forcecast>& a) {
    return std::vector<double>(a.data(), a.data() + a.size());
}

/// nlohmann::json ↔ Python objects (via the json module for exactness and simplicity).
py::object toPy(const json& j) { return py::module_::import("json").attr("loads")(j.dump()); }
json fromPy(const py::handle& o) {
    if (o.is_none()) return json::object();
    return json::parse(py::module_::import("json").attr("dumps")(o).cast<std::string>());
}

const GeometryObject& geometryOf(const Project& p, const std::string& id) {
    const auto* g = p.model().geometry.find(uuidArg(id));
    if (!g) throw py::key_error("No geometry object " + id);
    return *g;
}
const MineDevelopment& developmentOf(const Project& p, const std::string& id) {
    const auto* d = p.model().developments.find(uuidArg(id));
    if (!d) throw py::key_error("No development " + id);
    return *d;
}
Uuid layerArg(const Project& p, const std::string& layer) {
    if (auto u = Uuid::parse(layer); u && p.model().layers.find(*u)) return *u;
    for (const auto& l : p.model().layers.all())
        if (l.name == layer) return l.id;
    throw py::key_error("No layer '" + layer + "'");
}

py::dict timingDict(const scheduling::ActivityTiming& t) {
    py::dict d;
    d["start"] = scheduling::toDateTime(t.es).toString();
    d["finish"] = scheduling::toDateTime(t.ef).toString();
    d["late_start"] = scheduling::toDateTime(t.ls).toString();
    d["late_finish"] = scheduling::toDateTime(t.lf).toString();
    d["start_date"] = t.startDate.toString();
    d["finish_date"] = t.finishDate.toString();
    d["duration_hours"] = t.durationHours;
    d["total_hours"] = t.totalHours;
    d["total_float_hours"] = t.totalFloatHours;
    d["free_float_hours"] = t.freeFloatHours;
    d["critical"] = t.critical;
    d["percent_complete"] = t.percentComplete;
    d["actual_quantity"] = t.actualQuantity;
    d["remaining_quantity"] = t.remainingQuantity;
    d["started"] = t.started;
    d["completed"] = t.completed;
    d["duration_basis"] = t.durationBasis;
    return d;
}
py::dict scheduleDict(const scheduling::ScheduleResult& r) {
    py::dict d;
    d["ok"] = r.ok;
    d["errors"] = r.errors;
    d["warnings"] = r.warnings;
    if (r.ok) {
        d["start"] = scheduling::toDateTime(r.projectStart).toString();
        d["finish"] = scheduling::toDateTime(r.projectFinish).toString();
        d["makespan_hours"] = r.makespanHours();
    }
    py::dict acts;
    for (const auto& [id, t] : r.timings) acts[py::str(id.toString())] = timingDict(t);
    d["activities"] = acts;
    return d;
}
const Scenario* scenarioArg(const Project& p, const std::optional<std::string>& s) {
    if (!s || s->empty()) return nullptr;
    const auto* sc = p.model().scenarios.find(uuidArg(*s));
    if (!sc) throw py::key_error("No scenario " + *s);
    return sc;
}

survey::PointBuffer bufferFrom(const Points& xyz) {
    survey::PointBuffer b;
    b.xyz = toPoints(xyz);
    return b;
}
py::dict bufferDict(const survey::PointBuffer& b) {
    py::dict d;
    d["xyz"] = fromPoints(b.xyz);
    if (!b.intensity.empty()) {
        py::array_t<std::uint16_t> a(static_cast<py::ssize_t>(b.intensity.size()));
        std::copy(b.intensity.begin(), b.intensity.end(), a.mutable_data());
        d["intensity"] = a;
    }
    if (!b.rgb.empty()) {
        py::array_t<std::uint16_t> a({static_cast<py::ssize_t>(b.rgb.size()), static_cast<py::ssize_t>(3)});
        auto w = a.mutable_unchecked<2>();
        for (std::size_t i = 0; i < b.rgb.size(); ++i) {
            w(static_cast<py::ssize_t>(i), 0) = b.rgb[i].r;
            w(static_cast<py::ssize_t>(i), 1) = b.rgb[i].g;
            w(static_cast<py::ssize_t>(i), 2) = b.rgb[i].b;
        }
        d["rgb"] = a;
    }
    if (!b.classification.empty()) {
        py::array_t<std::uint8_t> a(static_cast<py::ssize_t>(b.classification.size()));
        std::copy(b.classification.begin(), b.classification.end(), a.mutable_data());
        d["classification"] = a;
    }
    for (std::size_t s = 0; s < b.scalars.size(); ++s) {
        py::array_t<float> a(static_cast<py::ssize_t>(b.scalars[s].size()));
        std::copy(b.scalars[s].begin(), b.scalars[s].end(), a.mutable_data());
        d[py::str(b.schema().scalarFields[s])] = a;
    }
    return d;
}

py::dict conformanceDict(const survey::ConformanceReport& r) {
    py::dict d;
    d["design_volume"] = r.designVolume;
    d["as_built_volume"] = r.asBuiltVolume;
    d["overbreak_volume"] = r.overbreakVolume;
    d["underbreak_volume"] = r.underbreakVolume;
    d["overbreak_percent"] = r.overbreakPercent;
    d["mean_overbreak_depth"] = r.meanOverbreakDepth;
    d["max_overbreak_depth"] = r.maxOverbreakDepth;
    d["max_underbreak_depth"] = r.maxUnderbreakDepth;
    d["min_clearance"] = r.minClearance ? py::object(py::float_(*r.minClearance)) : py::none();
    d["valid_stations"] = r.validStations;
    py::list st;
    for (const auto& s : r.stations) {
        py::dict x;
        x["chainage"] = s.chainage;
        x["valid"] = s.valid;
        x["issue"] = s.issue;
        x["design_area"] = s.designArea;
        x["as_built_area"] = s.asBuiltArea;
        x["overbreak_area"] = s.overbreakArea;
        x["underbreak_area"] = s.underbreakArea;
        x["max_overbreak_depth"] = s.maxOverbreakDepth;
        x["max_underbreak_depth"] = s.maxUnderbreakDepth;
        x["clearance"] = s.clearance ? py::object(py::float_(*s.clearance)) : py::none();
        st.append(x);
    }
    d["stations"] = st;
    return d;
}

geology::Anisotropy anisoArg(const py::dict& d) {
    geology::Anisotropy a;
    if (d.contains("range")) a.major = a.semi = a.minor = d["range"].cast<double>();
    if (d.contains("major")) a.major = d["major"].cast<double>();
    if (d.contains("semi")) a.semi = d["semi"].cast<double>();
    if (d.contains("minor")) a.minor = d["minor"].cast<double>();
    if (d.contains("azimuth")) a.azimuth = d["azimuth"].cast<double>();
    if (d.contains("dip")) a.dip = d["dip"].cast<double>();
    if (d.contains("tilt")) a.tilt = d["tilt"].cast<double>();
    return a;
}
py::dict anisoDict(const geology::Anisotropy& a) {
    py::dict d;
    d["major"] = a.major;
    d["semi"] = a.semi;
    d["minor"] = a.minor;
    d["azimuth"] = a.azimuth;
    d["dip"] = a.dip;
    d["tilt"] = a.tilt;
    return d;
}
geology::VariogramType vtype(const std::string& s) {
    if (s == "spherical") return geology::VariogramType::Spherical;
    if (s == "exponential") return geology::VariogramType::Exponential;
    if (s == "gaussian") return geology::VariogramType::Gaussian;
    throw py::value_error("Variogram structure must be spherical, exponential or gaussian");
}
geology::VariogramModel variogramArg(const py::dict& d) {
    geology::VariogramModel m;
    m.nugget = d.contains("nugget") ? d["nugget"].cast<double>() : 0.0;
    if (d.contains("structures"))
        for (const auto& s : d["structures"]) {
            const auto sd = s.cast<py::dict>();
            m.structures.push_back({vtype(sd.contains("type") ? sd["type"].cast<std::string>() : "spherical"), sd["sill"].cast<double>(), anisoArg(sd)});
        }
    return m;
}
py::dict variogramDict(const geology::VariogramModel& m) {
    py::dict d;
    d["nugget"] = m.nugget;
    py::list s;
    for (const auto& st : m.structures) {
        auto x = anisoDict(st.ranges);
        std::string t = geology::toString(st.type);
        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
        x["type"] = t;
        x["sill"] = st.sill;
        s.append(x);
    }
    d["structures"] = s;
    return d;
}
geology::CompositeOptions compositeArg(const py::dict& d) {
    geology::CompositeOptions o;
    if (d.contains("length")) o.length = d["length"].cast<double>();
    if (d.contains("fields")) o.fields = d["fields"].cast<std::vector<std::string>>();
    if (d.contains("min_coverage")) o.minCoverage = d["min_coverage"].cast<double>();
    if (d.contains("residual")) {
        const auto r = d["residual"].cast<std::string>();
        o.residual = r == "discard" ? geology::ResidualMode::Discard : r == "add_to_previous" ? geology::ResidualMode::AddToPrevious : geology::ResidualMode::Distribute;
    }
    if (d.contains("min_residual_fraction")) o.minResidualFraction = d["min_residual_fraction"].cast<double>();
    if (d.contains("break_field")) o.breakField = d["break_field"].cast<std::string>();
    if (d.contains("only_code")) o.onlyCode = d["only_code"].cast<std::string>();
    if (d.contains("density_field")) o.densityField = d["density_field"].cast<std::string>();
    if (d.contains("unsampled_as_zero")) o.unsampledAsZero = d["unsampled_as_zero"].cast<bool>();
    return o;
}
py::dict compositeDict(const geology::Composite& c) {
    py::dict d;
    d["hole"] = c.holeId;
    d["from"] = c.from;
    d["to"] = c.to;
    d["x"] = c.mid.x;
    d["y"] = c.mid.y;
    d["z"] = c.mid.z;
    d["values"] = c.values;
    d["coverage"] = c.coverage;
    d["codes"] = c.codes;
    return d;
}
geology::Composite compositeFrom(const py::dict& d) {
    geology::Composite c;
    c.holeId = d["hole"].cast<std::string>();
    c.from = d.contains("from") ? d["from"].cast<double>() : 0.0;
    c.to = d.contains("to") ? d["to"].cast<double>() : 0.0;
    c.mid = {d["x"].cast<double>(), d["y"].cast<double>(), d["z"].cast<double>()};
    c.values = d["values"].cast<std::map<std::string, double>>();
    if (d.contains("codes")) c.codes = d["codes"].cast<std::map<std::string, std::string>>();
    return c;
}
py::dict blockModelDict(const geology::BlockModelData& d) {
    py::dict r;
    const auto& h = d.header;
    r["id"] = idStr(h.id);
    r["name"] = h.name;
    r["origin"] = py::make_tuple(h.origin.x, h.origin.y, h.origin.z);
    r["block_size"] = py::make_tuple(h.blockSize.x, h.blockSize.y, h.blockSize.z);
    r["shape"] = py::make_tuple(h.nx, h.ny, h.nz);
    r["rotation_deg"] = h.rotationDeg;
    r["description"] = h.description;
    py::dict values;
    for (const auto& a : h.attributes) values[py::str(a)] = fromVector(d.values.at(a));
    r["values"] = values;
    return r;
}

geotechnical::QInputs qArg(const py::dict& d) {
    geotechnical::QInputs q;
    q.rqd = d["rqd"].cast<double>();
    q.jn = d["jn"].cast<double>();
    q.jr = d["jr"].cast<double>();
    q.ja = d["ja"].cast<double>();
    q.jw = d.contains("jw") ? d["jw"].cast<double>() : 1.0;
    q.srf = d.contains("srf") ? d["srf"].cast<double>() : 1.0;
    return q;
}
std::vector<Vec3> polesArg(const std::vector<std::pair<double, double>>& planes) {
    std::vector<Vec3> v;
    for (auto [dip, dd] : planes) v.push_back(geotechnical::poleVector({dip, dd}));
    return v;
}
py::dict planeDict(const geotechnical::Plane& p) {
    py::dict d;
    d["dip"] = p.dip;
    d["dip_direction"] = p.dipDirection;
    return d;
}
py::dict setDict(const geotechnical::JointSet& s) {
    py::dict d;
    d["name"] = s.name;
    d["mean_plane"] = planeDict(s.meanPlane);
    d["mean_pole"] = py::make_tuple(s.meanPole.trend, s.meanPole.plunge);
    d["count"] = s.members.size();
    d["k"] = s.stats.k;
    d["alpha95"] = s.stats.alpha95;
    d["members"] = s.members;
    return d;
}

std::string pluginResultText(const plugins::PluginCallResult& r) { return r.text; }
py::dict pluginResult(const plugins::PluginCallResult& r) {
    py::dict d;
    d["ok"] = r.ok;
    d["error"] = r.error;
    d["log"] = r.log;
    d["rolled_back"] = r.rolledBack;
    try {
        d["result"] = r.text.empty() ? py::object(py::none()) : toPy(json::parse(r.text));
    } catch (...) {
        d["result"] = r.text;
    }
    return d;
}
} // namespace

void defineBindings(py::module_& m) {
    m.attr("SCHEMA_VERSION") = persistence::kCurrentSchemaVersion;
    m.attr("APP_VERSION") = persistence::kAppVersion;
    m.attr("PLUGIN_API_VERSION") = plugins::PluginManager::apiVersion();
    py::dict features;
#ifdef OPENMINE_PY_HAVE_OPTIMISER
    features["optimiser"] = true;
#else
    features["optimiser"] = false;
#endif
#ifdef OPENMINE_PY_HAVE_CRS
    features["crs"] = true;
#else
    features["crs"] = false;
#endif
#ifdef OPENMINE_PY_HAVE_GIS
    features["gis"] = true;
#else
    features["gis"] = false;
#endif
#ifdef OPENMINE_PY_HAVE_REPOSITORY
    features["repository"] = true;
#else
    features["repository"] = false;
#endif
#ifdef OPENMINE_PY_HAVE_OCCT
    features["solids"] = true;
#else
    features["solids"] = false;
#endif
#ifdef OPENMINE_PY_HAVE_PDAL
    features["pdal"] = true;
    pdalio::registerFormats(); // LAS/LAZ/E57/PLY point clouds
#else
    features["pdal"] = false;
#endif
#ifdef OPENMINE_PY_HAVE_OPEN3D
    features["open3d"] = true;
    o3d::registerEngines(); // Open3D ICP engine
#else
    features["open3d"] = false;
#endif
    m.attr("FEATURES") = features;
    m.def("icp_engines", [] { return survey::icpEngines(); }, "Available ICP engines (\"OpenMine\" first; \"Open3D\" when built).");

    py::register_exception<ValidationError>(m, "ValidationError", PyExc_ValueError);
    py::register_exception<persistence::PersistenceError>(m, "PersistenceError", PyExc_IOError);
    py::register_exception<jobs::CancelledError>(m, "CancelledError", PyExc_RuntimeError);

    py::class_<jobs::Progress>(m, "Progress", "Progress and cancellation of a long operation (cancel() may be called from another thread).")
        .def(py::init<>())
        .def("cancel", &jobs::Progress::cancel)
        .def_property_readonly("cancelled", &jobs::Progress::isCancelled)
        .def_property_readonly("fraction", &jobs::Progress::fraction)
        .def_property_readonly("stage", &jobs::Progress::stage);
    auto progressOr = [](jobs::Progress* p) -> jobs::Progress& { return p ? *p : jobs::nullProgress(); };

    auto P = py::class_<PyProject>(m, "NativeProject");
    // ---- lifecycle ----------------------------------------------------------------------------------
    P.def_static("create", [](const std::string& name, const std::string& start) { return PyProject(Project::createNew(name, dateArg(start))); })
        .def_static("load", [](const fs::path& path) {
            persistence::LoadReport rep;
            auto p = persistence::loadProject(path, &rep);
            py::dict d;
            d["schema_version"] = rep.fileSchemaVersion;
            d["migrated"] = rep.migrated;
            d["warnings"] = rep.warnings;
            return py::make_tuple(PyProject(std::move(p)), d);
        })
        .def("save", [](PyProject& s, const fs::path& path) { persistence::saveProject(s.get(), path); })
        .def("close", &PyProject::close)
        .def_property_readonly("owned", &PyProject::owned)
        .def_property_readonly("name", [](PyProject& s) { return s.get().model().info.name; })
        .def_property_readonly("id", [](PyProject& s) { return s.get().model().info.id.toString(); })
        .def_property_readonly("file_path", [](PyProject& s) { return s.get().filePath(); })
        .def_property_readonly("is_modified", [](PyProject& s) { return s.get().isModified(); })
        .def_property("user", [](PyProject& s) { return s.get().userName(); }, [](PyProject& s, const std::string& u) { s.get().setUserName(u); })
        .def("undo", [](PyProject& s) { s.get().commands().undo(); })
        .def("redo", [](PyProject& s) { s.get().commands().redo(); })
        .def_property_readonly("can_undo", [](PyProject& s) { return s.get().commands().canUndo(); })
        .def_property_readonly("can_redo", [](PyProject& s) { return s.get().commands().canRedo(); })
        .def_property_readonly("undo_text", [](PyProject& s) { return s.get().commands().undoText(); })
        .def_property_readonly("redo_text", [](PyProject& s) { return s.get().commands().redoText(); })
        .def("begin_macro", [](PyProject& s, const std::string& text) { s.get().commands().beginMacro(text); })
        .def("end_macro", [](PyProject& s, bool commit) {
            if (commit) s.get().commands().endMacro();
            else s.get().commands().cancelMacro();
        })
        .def("audit_log", [](PyProject& s) {
            py::list l;
            for (const auto& e : s.get().model().audit) {
                py::dict d;
                d["timestamp"] = e.timestamp;
                d["user"] = e.user;
                d["action"] = e.action;
                d["entity_type"] = e.entityType;
                d["entity_id"] = idStr(e.entityId);
                d["detail"] = e.detail;
                l.append(d);
            }
            return l;
        })
        .def("info", [](PyProject& s) { return toPy(json::parse(*pe::encodeDocument(s.get().model(), "ProjectInfo", "project"))); })
        .def("set_info", [](PyProject& s, const py::dict& d) {
            ProjectInfo i = s.get().model().info;
            if (d.contains("name")) i.name = d["name"].cast<std::string>();
            if (d.contains("description")) i.description = d["description"].cast<std::string>();
            if (d.contains("schedule_start")) i.scheduleStart = dateTimeArg(d["schedule_start"].cast<std::string>());
            if (d.contains("status_date")) {
                if (d["status_date"].is_none()) i.statusDate.reset();
                else i.statusDate = dateTimeArg(d["status_date"].cast<std::string>());
            }
            if (d.contains("default_calendar")) i.defaultCalendarId = uuidArg(d["default_calendar"].cast<std::string>());
            if (d.contains("coordinate_display_decimals")) i.coordinateDisplayDecimals = d["coordinate_display_decimals"].cast<int>();
            if (d.contains("south_orientated_axes")) i.southOrientatedAxes = d["south_orientated_axes"].cast<bool>();
            ProjectEditor(s.get()).setProjectInfo(i, "Edit project information");
        });

    // ---- generic entity documents ---------------------------------------------------------------------
    P.def_static("document_types", [] {
         std::vector<std::string> t;
         for (const auto& x : pe::documentTypes())
             if (x != "ProjectInfo" && x != "ActivityDesignLink") t.push_back(x);
         return t;
     })
        .def("documents", [](PyProject& s, const std::string& type) {
            py::list l;
            for (const auto& d : pe::encodeModel(s.get().model()))
                if (d.type == type) l.append(toPy(json::parse(d.json)));
            return l;
        })
        .def("document", [](PyProject& s, const std::string& type, const std::string& key) -> py::object {
            const auto d = pe::encodeDocument(s.get().model(), type, key);
            return d ? toPy(json::parse(*d)) : py::none();
        })
        .def_static("template", [](const std::string& type) { return toPy(json::parse(pe::templateDocument(type))); })
        .def("put", [](PyProject& s, const std::string& type, const py::object& doc, const std::string& text) {
            return pe::putDocument(s.get(), type, fromPy(doc).dump(), text).toString();
        }, py::arg("type"), py::arg("document"), py::arg("text") = "")
        .def("delete", [](PyProject& s, const std::string& type, const std::string& key) { pe::removeDocument(s.get(), type, key); })
        .def("check", [](PyProject& s) { return pe::checkModel(s.get()); })
        .def("order", [](PyProject& s, const std::string& type) { return pe::entityOrder(s.get().model(), type); });

    // ---- CAD --------------------------------------------------------------------------------------------
    P.def("layers", [](PyProject& s) {
         py::list l;
         for (const auto& x : s.get().model().layers.all()) {
             py::dict d;
             d["id"] = x.id.toString();
             d["name"] = x.name;
             d["color"] = x.colorRgb;
             d["visible"] = x.visible;
             d["locked"] = x.locked;
             l.append(d);
         }
         return l;
     })
        .def("add_layer", [](PyProject& s, const std::string& name, std::uint32_t color) { return ProjectEditor(s.get()).addLayer(name, color).toString(); },
             py::arg("name"), py::arg("color") = 0xD0D0D0)
        .def("add_geometry", [](PyProject& s, const std::string& type, const Points& pts, const std::string& name, const std::string& layer, bool closed,
                                const std::map<std::string, std::string>& attrs, const std::vector<double>& bulges) {
            GeometryObject g;
            if (type == "Point") g.type = GeometryType::Point;
            else if (type == "Line") g.type = GeometryType::Line;
            else if (type == "Polyline") g.type = GeometryType::Polyline;
            else if (type == "Spline") g.type = GeometryType::Spline;
            else throw py::value_error("Use add_mesh for meshes, or type Point, Line, Polyline or Spline");
            g.vertices = toPoints(pts);
            g.name = name;
            g.layerId = layerArg(s.get(), layer);
            g.closed = closed;
            g.attributes = attrs;
            g.bulges = bulges;
            return ProjectEditor(s.get()).addGeometry(std::move(g)).toString();
        }, py::arg("type"), py::arg("points"), py::arg("name") = "", py::arg("layer") = "0", py::arg("closed") = false,
             py::arg("attributes") = std::map<std::string, std::string>{}, py::arg("bulges") = std::vector<double>{})
        .def("add_mesh", [](PyProject& s, const Points& v, const Tris& t, const std::string& name, const std::string& layer) {
            GeometryObject g;
            g.type = GeometryType::Mesh;
            g.mesh = std::make_shared<MeshData>(toMesh(v, t));
            g.name = name;
            g.layerId = layerArg(s.get(), layer);
            return ProjectEditor(s.get()).addGeometry(std::move(g)).toString();
        }, py::arg("vertices"), py::arg("triangles"), py::arg("name") = "", py::arg("layer") = "0")
        .def("geometry", [](PyProject& s, const std::string& id) {
            const auto& g = geometryOf(s.get(), id);
            py::dict d;
            d["id"] = g.id.toString();
            d["name"] = g.name;
            d["type"] = toString(g.type);
            d["layer"] = g.layerId.toString();
            d["vertices"] = fromPoints(g.vertices);
            d["closed"] = g.closed;
            d["bulges"] = g.bulges;
            d["attributes"] = g.attributes;
            if (g.mesh) d["mesh"] = fromMesh(*g.mesh);
            if (!g.vertices.empty() && geometry::isCurve(g.type))
                d["length"] = g.bulges.empty() ? geometry::length3d(g.vertices, g.closed) : geometry::length3dBulged(g.vertices, g.bulges, g.closed);
            if (g.mesh) {
                d["area"] = geometry::meshArea(*g.mesh);
                d["volume"] = geometry::meshSignedVolume(*g.mesh);
            }
            return d;
        })
        .def("geometry_ids", [](PyProject& s, const std::optional<std::string>& type, const std::optional<std::string>& layer) {
            std::vector<std::string> out;
            const auto lid = layer ? std::optional<Uuid>(layerArg(s.get(), *layer)) : std::nullopt;
            for (const auto& g : s.get().model().geometry.all())
                if ((!type || toString(g.type) == *type) && (!lid || g.layerId == *lid)) out.push_back(g.id.toString());
            return out;
        }, py::arg("type") = py::none(), py::arg("layer") = py::none())
        .def("remove_geometry", [](PyProject& s, const std::string& id) { ProjectEditor(s.get()).removeGeometry(uuidArg(id)); })
        .def("offset_curve", [](PyProject& s, const std::string& id, double distance, std::array<double, 3> side) {
            auto r = cad::offset(geometryOf(s.get(), id), distance, {side[0], side[1], side[2]});
            r.id = Uuid{};
            return ProjectEditor(s.get()).addGeometry(std::move(r), "Offset").toString();
        }, py::arg("id"), py::arg("distance"), py::arg("side_point"));

    // ---- mine design -----------------------------------------------------------------------------------------
    P.def("create_development", [](PyProject& s, const std::string& geometryId, const py::dict& spec) {
         mine_design::NewDevelopment n;
         if (spec.contains("heading_id")) n.headingId = spec["heading_id"].cast<std::string>();
         if (spec.contains("name")) n.name = spec["name"].cast<std::string>();
         if (spec.contains("level")) n.level = spec["level"].cast<std::string>();
         if (spec.contains("zone")) n.zone = spec["zone"].cast<std::string>();
         if (spec.contains("activity_type")) n.activityType = spec["activity_type"].cast<std::string>();
         if (spec.contains("width")) n.width = spec["width"].cast<double>();
         if (spec.contains("height")) n.height = spec["height"].cast<double>();
         if (spec.contains("chainage_from")) n.chainageFrom = spec["chainage_from"].cast<double>();
         if (spec.contains("profile")) n.profileId = uuidArg(spec["profile"].cast<std::string>());
         if (spec.contains("material")) n.materialId = uuidArg(spec["material"].cast<std::string>());
         else if (!s.get().model().materials.empty()) n.materialId = s.get().model().materials.all().front().id;
         if (spec.contains("support_class")) n.supportClassId = uuidArg(spec["support_class"].cast<std::string>());
         return mine_design::createDevelopment(s.get(), uuidArg(geometryId), n).toString();
     })
        .def("create_linked_activity", [](PyProject& s, const std::string& dev, double rate, const std::string& name) {
            return mine_design::createLinkedActivity(s.get(), uuidArg(dev), rate, name).toString();
        }, py::arg("development"), py::arg("rate_per_day"), py::arg("name") = "")
        .def("development_metrics", [](PyProject& s, const std::string& id) {
            const auto m = mine_design::computeMetrics(s.get(), developmentOf(s.get(), id));
            py::dict d;
            d["length"] = m.length3d;
            d["horizontal_length"] = m.horizontalLength;
            d["gradient_percent"] = m.gradientPercent ? py::object(py::float_(*m.gradientPercent)) : py::none();
            d["chainage_to"] = m.chainageTo;
            d["section_area"] = m.sectionArea;
            d["volume"] = m.volume;
            d["tonnes"] = m.tonnes;
            d["loose_volume"] = m.looseVolume;
            return d;
        })
        .def("development_centreline", [](PyProject& s, const std::string& id) { return fromPoints(mine_design::centrelinePoints(s.get(), developmentOf(s.get(), id))); })
        .def("development_stations", [](PyProject& s, const std::string& id, double interval) {
            py::list l;
            for (const auto& st : mine_design::stations(s.get(), developmentOf(s.get(), id), interval)) {
                py::dict d;
                d["chainage"] = st.chainage;
                d["label"] = st.label;
                d["position"] = py::make_tuple(st.position.x, st.position.y, st.position.z);
                l.append(d);
            }
            return l;
        })
        .def("development_envelope", [](PyProject& s, const std::string& id) {
            return fromMesh(mine_design::excavationEnvelope(s.get(), developmentOf(s.get(), id)));
        });

    // ---- scheduling ----------------------------------------------------------------------------------------
    P.def("add_activity", [](PyProject& s, const py::dict& d) {
         ScheduleActivity a;
         a.name = d["name"].cast<std::string>();
         if (d.contains("wbs")) a.wbsCode = d["wbs"].cast<std::string>();
         if (d.contains("duration_hours")) a.durationHours = d["duration_hours"].cast<double>();
         if (d.contains("duration_days")) a.durationHours = d["duration_days"].cast<double>() * 24;
         if (d.contains("quantity")) a.quantity = d["quantity"].cast<double>();
         if (d.contains("unit")) a.quantityUnit = d["unit"].cast<std::string>();
         if (d.contains("rate_per_day")) {
             a.rate = d["rate_per_day"].cast<double>();
             a.durationFromRate = true;
         }
         if (d.contains("priority")) a.priority = d["priority"].cast<int>();
         if (d.contains("work_area")) a.workArea = d["work_area"].cast<std::string>();
         if (d.contains("calendar")) a.calendarId = uuidArg(d["calendar"].cast<std::string>());
         if (d.contains("parent")) a.parentId = uuidArg(d["parent"].cast<std::string>());
         if (d.contains("milestone") && d["milestone"].cast<bool>()) {
             a.kind = ActivityKind::Milestone;
             a.durationHours = 0;
         }
         if (d.contains("constraint")) {
             auto c = enumFromString<ConstraintType>(d["constraint"].cast<std::string>());
             if (!c) throw py::value_error("Unknown constraint type");
             a.constraint = *c;
             a.constraintDate = dateTimeArg(d["constraint_date"].cast<std::string>());
         }
         if (d.contains("actual_start")) a.actualStart = dateTimeArg(d["actual_start"].cast<std::string>());
         return ProjectEditor(s.get()).addActivity(a).toString();
     })
        .def("add_dependency", [](PyProject& s, const std::string& pred, const std::string& succ, const std::string& type, double lagHours) {
            ActivityDependency d;
            d.predecessorId = uuidArg(pred);
            d.successorId = uuidArg(succ);
            static const std::map<std::string, std::string> longNames = {
                {"FinishToStart", "FS"}, {"StartToStart", "SS"}, {"FinishToFinish", "FF"}, {"StartToFinish", "SF"}};
            const auto ln = longNames.find(type);
            auto t = enumFromString<DependencyType>(ln == longNames.end() ? type : ln->second);
            if (!t) throw py::value_error("Dependency type must be FS, SS, FF, SF (or FinishToStart, StartToStart, FinishToFinish, StartToFinish)");
            d.type = *t;
            d.lagHours = lagHours;
            return ProjectEditor(s.get()).addDependency(d).toString();
        }, py::arg("predecessor"), py::arg("successor"), py::arg("type") = "FinishToStart", py::arg("lag_hours") = 0.0)
        .def("add_resource", [](PyProject& s, const std::string& name, double capacity) {
            Resource r;
            r.name = name;
            r.capacity = capacity;
            return ProjectEditor(s.get()).add(r).toString();
        }, py::arg("name"), py::arg("capacity") = 1.0)
        .def("assign_resource", [](PyProject& s, const std::string& act, const std::string& res, double units) {
            ActivityResourceAssignment a;
            a.activityId = uuidArg(act);
            a.resourceId = uuidArg(res);
            a.units = units;
            return ProjectEditor(s.get()).add(a).toString();
        }, py::arg("activity"), py::arg("resource"), py::arg("units") = 1.0)
        .def("record_actual", [](PyProject& s, const std::string& act, const std::string& date, double qty, const std::string& note) {
            ProductionActual a;
            a.activityId = uuidArg(act);
            a.date = dateArg(date);
            a.quantity = qty;
            a.note = note;
            return ProjectEditor(s.get()).add(a).toString();
        }, py::arg("activity"), py::arg("date"), py::arg("quantity"), py::arg("note") = "")
        .def("calculate_schedule", [](PyProject& s, const std::optional<std::string>& scenario) {
            const auto in = scheduling::buildScheduleInput(s.get(), scenarioArg(s.get(), scenario));
            return scheduleDict(scheduling::calculateSchedule(in));
        }, py::arg("scenario") = py::none())
        .def("schedule_table", [](PyProject& s, const std::optional<std::string>& scenario, const std::optional<std::string>& baseline) {
            const auto in = scheduling::buildScheduleInput(s.get(), scenarioArg(s.get(), scenario));
            const auto r = scheduling::calculateSchedule(in);
            const ScheduleBaseline* b = baseline ? s.get().model().baselines.find(uuidArg(*baseline)) : nullptr;
            py::list l;
            for (const auto& row : scheduling::scheduleTable(s.get(), in, r, b)) {
                py::dict d;
                d["id"] = row.id.toString();
                d["wbs"] = row.wbs;
                d["name"] = row.name;
                d["kind"] = row.kind;
                d["start"] = row.start;
                d["finish"] = row.finish;
                d["duration_days"] = row.durationDays;
                d["total_float_days"] = row.totalFloatDays;
                d["critical"] = row.critical;
                d["quantity"] = row.quantity;
                d["actual"] = row.actual;
                d["percent"] = row.percent;
                d["resources"] = row.resources;
                d["linked_design"] = row.linkedDesign;
                d["baseline_finish"] = row.baselineFinish;
                d["finish_variance_days"] = row.finishVarianceDays;
                l.append(d);
            }
            return l;
        }, py::arg("scenario") = py::none(), py::arg("baseline") = py::none())
        .def("create_baseline", [](PyProject& s, const std::string& name) {
            const auto r = scheduling::calculateSchedule(s.get());
            if (!r.ok) throw py::value_error("The schedule cannot be calculated: " + (r.errors.empty() ? std::string() : r.errors.front()));
            return ProjectEditor(s.get()).add(scheduling::makeBaseline(s.get(), r, name)).toString();
        })
        .def("compare_to_baseline", [](PyProject& s, const std::string& baseline) {
            const auto* b = s.get().model().baselines.find(uuidArg(baseline));
            if (!b) throw py::key_error("No baseline " + baseline);
            py::list l;
            for (const auto& v : scheduling::compareToBaseline(s.get(), scheduling::calculateSchedule(s.get()), *b)) {
                py::dict d;
                d["id"] = v.activityId.toString();
                d["name"] = v.name;
                d["start_variance_hours"] = v.startVarianceHours;
                d["finish_variance_hours"] = v.finishVarianceHours;
                d["in_baseline"] = v.inBaseline;
                l.append(d);
            }
            return l;
        })
        .def("add_scenario", [](PyProject& s, const std::string& name, const std::vector<std::tuple<std::string, std::string, std::string>>& overrides) {
            Scenario sc;
            sc.name = name;
            for (const auto& [e, f, v] : overrides) sc.overrides.push_back({e.empty() ? Uuid{} : uuidArg(e), f, v});
            return ProjectEditor(s.get()).add(sc).toString();
        })
        .def("export_gantt_pdf", [](PyProject& s, const fs::path& file, const std::optional<std::string>& scenario, const std::string& title,
                                    const std::optional<std::string>& baseline) {
            const auto in = scheduling::buildScheduleInput(s.get(), scenarioArg(s.get(), scenario));
            const auto r = scheduling::calculateSchedule(in);
            if (!r.ok) throw py::value_error("The schedule cannot be calculated");
            reporting::GanttOptions o;
            o.title = title;
            const ScheduleBaseline* b = baseline ? s.get().model().baselines.find(uuidArg(*baseline)) : nullptr;
            return reporting::writeGanttPdf(s.get(), in, r, b, file, o);
        }, py::arg("file"), py::arg("scenario") = py::none(), py::arg("title") = "", py::arg("baseline") = py::none())
        .def("export_schedule_xlsx", [](PyProject& s, const fs::path& file) {
            const auto in = scheduling::buildScheduleInput(s.get());
            const auto r = scheduling::calculateSchedule(in);
            reporting::writeScheduleXlsx(s.get(), scheduling::scheduleTable(s.get(), in, r), file);
        })
        .def("plan_snapshot", [](PyProject& s, const std::string& at, const std::optional<std::string>& baseline, const std::optional<std::string>& scenario) {
            const auto in = scheduling::buildScheduleInput(s.get(), scenarioArg(s.get(), scenario));
            const auto r = scheduling::calculateSchedule(in);
            const ScheduleBaseline* b = baseline ? s.get().model().baselines.find(uuidArg(*baseline)) : nullptr;
            const auto snap = planning::snapshot(s.get(), in, r, dateTimeArg(at).minutes(), planning::ColorMode::State, b);
            py::list l;
            for (const auto& o : snap.objects) {
                py::dict d;
                d["id"] = o.designId.toString();
                d["type"] = o.designType;
                d["name"] = o.name;
                d["state"] = planning::toString(o.state);
                d["planned_fraction"] = o.plannedFraction;
                d["actual_fraction"] = o.actualFraction;
                d["baseline_fraction"] = o.baselineFraction;
                d["behind_plan"] = o.behindPlan;
                d["planned_face"] = o.plannedFace;
                d["color"] = o.color;
                l.append(d);
            }
            py::dict out;
            out["objects"] = l;
            out["planned_metres"] = snap.plannedMetres;
            out["actual_metres"] = snap.actualMetres;
            return out;
        }, py::arg("at"), py::arg("baseline") = py::none(), py::arg("scenario") = py::none());
#ifdef OPENMINE_PY_HAVE_OPTIMISER
    P.def("optimise_schedule", [](PyProject& s, const py::dict& o, const std::optional<std::string>& scenario, const std::optional<std::string>& storeAs,
                                  jobs::Progress* progress) {
        optimisation::OptimiserOptions opt;
        if (o.contains("slot_hours")) opt.slotHours = o["slot_hours"].cast<double>();
        if (o.contains("time_limit_seconds")) opt.timeLimitSeconds = o["time_limit_seconds"].cast<double>();
        if (o.contains("objective")) {
            const auto ob = o["objective"].cast<std::string>();
            opt.objective = ob == "weighted_completion" ? optimisation::Objective::WeightedCompletion
                            : ob == "lateness"          ? optimisation::Objective::Lateness
                                                        : optimisation::Objective::Makespan;
        }
        if (o.contains("workers")) opt.workers = o["workers"].cast<int>();
        const auto* sc = scenarioArg(s.get(), scenario);
        optimisation::OptimisationResult r;
        {
            py::gil_scoped_release rel;
            r = optimisation::optimise(s.get(), sc, opt, progress ? *progress : jobs::nullProgress());
        }
        py::dict d;
        static const char* st[] = {"optimal", "feasible", "infeasible", "no_solution_in_time", "invalid_model"};
        d["status"] = st[static_cast<int>(r.status)];
        d["status_text"] = r.statusText;
        d["makespan_hours"] = r.makespanHours;
        d["cpm_makespan_hours"] = r.cpmMakespanHours;
        d["solve_seconds"] = r.solveSeconds;
        d["variables"] = r.variables;
        d["constraints"] = r.constraints;
        d["notes"] = r.notes;
        py::list conflicts;
        for (const auto& c : r.conflicts) conflicts.append(py::make_tuple(c.kind, c.description));
        d["conflicts"] = conflicts;
        py::dict acts;
        for (const auto& [id, a] : r.activities)
            acts[py::str(id.toString())] = py::make_tuple(scheduling::toDateTime(a.start).toString(), scheduling::toDateTime(a.finish).toString());
        d["activities"] = acts;
        if (storeAs && (r.status == optimisation::OptimisationResult::Status::Optimal || r.status == optimisation::OptimisationResult::Status::Feasible))
            d["scenario"] = optimisation::storeAsScenario(s.get(), r, *storeAs, sc).toString();
        return d;
    }, py::arg("options") = py::dict(), py::arg("scenario") = py::none(), py::arg("store_as") = py::none(), py::arg("progress") = nullptr);
#endif

    // ---- survey ------------------------------------------------------------------------------------------
    P.def("import_point_cloud", [progressOr](PyProject& s, const fs::path& file, const std::string& name, const std::string& crs, jobs::Progress* progress) {
         survey::ImportOptions o;
         o.name = name;
         o.sourceCrs = crs;
         survey::PreparedDataset prep;
         {
             py::gil_scoped_release rel;
             prep = survey::prepareImport(s.get(), file, o, progressOr(progress));
         }
         return survey::commitDataset(s.get(), std::move(prep), "Import " + file.filename().string()).toString();
     }, py::arg("file"), py::arg("name") = "", py::arg("source_crs") = "", py::arg("progress") = nullptr)
        .def("point_cloud_points", [](PyProject& s, const std::string& id, std::optional<std::array<double, 6>> box) {
            std::optional<BoundingBox> b;
            if (box) b = BoundingBox{{(*box)[0], (*box)[1], (*box)[2]}, {(*box)[3], (*box)[4], (*box)[5]}};
            survey::PointBuffer buf;
            {
                py::gil_scoped_release rel;
                buf = survey::datasetPoints(s.get(), uuidArg(id), true, b);
            }
            return bufferDict(buf);
        }, py::arg("id"), py::arg("box") = py::none())
        .def("export_point_cloud", [progressOr](PyProject& s, const std::string& id, const fs::path& file, jobs::Progress* progress) {
            py::gil_scoped_release rel;
            survey::exportDataset(s.get(), uuidArg(id), file, progressOr(progress));
        }, py::arg("id"), py::arg("file"), py::arg("progress") = nullptr)
        .def("add_point_cloud", [](PyProject& s, const Points& xyz, const std::string& name) {
            auto buf = bufferFrom(xyz);
            survey::PreparedDataset prep;
            {
                py::gil_scoped_release rel;
                prep = survey::prepareFromPoints(s.get(), buf, name, "", "Python");
            }
            return survey::commitDataset(s.get(), std::move(prep), "Add point cloud " + name).toString();
        })
        .def("icp_registration", [progressOr](PyProject& s, const std::string& moving, const std::string& target, const py::dict& o, bool apply,
                                              jobs::Progress* progress) {
            survey::IcpOptions opt;
            if (o.contains("mode")) opt.mode = o["mode"].cast<std::string>() == "point_to_point" ? survey::IcpMode::PointToPoint : survey::IcpMode::PointToPlane;
            if (o.contains("max_correspondence")) opt.maxCorrespondence = o["max_correspondence"].cast<double>();
            if (o.contains("max_iterations")) opt.maxIterations = o["max_iterations"].cast<int>();
            if (o.contains("sample_size")) opt.sampleSize = o["sample_size"].cast<std::size_t>();
            if (o.contains("trim_fraction")) opt.trimFraction = o["trim_fraction"].cast<double>();
            if (o.contains("engine")) opt.engine = o["engine"].cast<std::string>();
            Registration r;
            {
                py::gil_scoped_release rel;
                r = survey::computeIcpRegistration(s.get(), uuidArg(moving), uuidArg(target), opt, {}, progressOr(progress));
            }
            py::dict d;
            d["transform"] = r.transform.toString();
            d["fitness"] = r.icpFitness;
            d["inlier_rms"] = r.icpInlierRms;
            d["iterations"] = r.icpIterations;
            d["converged"] = r.icpConverged;
            d["notes"] = r.notes;
            d["registration"] = survey::commitRegistration(s.get(), r, apply).toString();
            return d;
        }, py::arg("moving"), py::arg("target"), py::arg("options") = py::dict(), py::arg("apply") = false, py::arg("progress") = nullptr)
        .def("add_as_built", [](PyProject& s, const std::string& dataset, const Points& v, const Tris& t, const std::string& name, const std::string& layer,
                                const std::string& development, const std::string& method) {
            return survey::addAsBuiltModel(s.get(), uuidArg(dataset), toMesh(v, t), name, layerArg(s.get(), layer), development.empty() ? Uuid{} : uuidArg(development),
                                           method)
                .toString();
        }, py::arg("dataset"), py::arg("vertices"), py::arg("triangles"), py::arg("name"), py::arg("layer") = "0", py::arg("development") = "",
             py::arg("method") = "Python")
        .def("conformance", [](PyProject& s, const std::string& asBuilt, const std::string& dev, double interval, bool commit) {
            survey::ConformanceOptions o;
            o.interval = interval;
            survey::ConformanceReport r;
            {
                py::gil_scoped_release rel;
                r = survey::analyseAsBuilt(s.get(), uuidArg(asBuilt), uuidArg(dev), o);
            }
            auto d = conformanceDict(r);
            if (commit) d["result"] = survey::commitConformance(s.get(), uuidArg(asBuilt), uuidArg(dev), r, o).toString();
            return d;
        }, py::arg("as_built"), py::arg("development"), py::arg("interval") = 1.0, py::arg("commit") = false);

    m.def("fit_transform", [](const Points& src, const Points& tgt, bool scale) {
        const auto f = survey::fitTransform(toPoints(src), toPoints(tgt), scale);
        py::dict d;
        d["transform"] = f.transform.toString();
        d["scale"] = f.scale;
        d["rms"] = f.rms;
        d["max"] = f.max;
        d["residuals"] = fromPoints(f.residuals);
        return d;
    }, py::arg("source"), py::arg("target"), py::arg("allow_scale") = false);
    m.def("delaunay", [](const Points& pts, double maxEdge) {
        survey::DelaunayOptions o;
        o.maxEdgeLength = maxEdge;
        MeshData mesh;
        {
            const auto p = toPoints(pts);
            py::gil_scoped_release rel;
            mesh = survey::delaunay25D(p, o);
        }
        return fromMesh(mesh);
    }, py::arg("points"), py::arg("max_edge_length") = 0.0);
    m.def("decimate", [](const Points& v, const Tris& t, std::size_t target, double maxError) {
        survey::DecimateOptions o;
        o.targetTriangles = target;
        o.maxError = maxError;
        return fromMesh(survey::decimate(toMesh(v, t), o));
    }, py::arg("vertices"), py::arg("triangles"), py::arg("target_triangles") = 0, py::arg("max_error") = 1e300);
    m.def("volume_between", [](const Points& v1, const Tris& t1, const Points& v2, const Tris& t2, double cell) {
        const auto r = survey::volumeBetween(toMesh(v1, t1), toMesh(v2, t2), cell);
        py::dict d;
        d["fill"] = r.above;
        d["cut"] = r.below;
        d["plan_area"] = r.planArea;
        return d;
    }, py::arg("earlier_vertices"), py::arg("earlier_triangles"), py::arg("later_vertices"), py::arg("later_triangles"), py::arg("cell") = 0.5);
    m.def("cloud_statistics", [](const Points& xyz) {
        const auto st = survey::statistics(bufferFrom(xyz));
        py::dict d;
        d["count"] = st.count;
        d["min"] = py::make_tuple(st.bounds.min.x, st.bounds.min.y, st.bounds.min.z);
        d["max"] = py::make_tuple(st.bounds.max.x, st.bounds.max.y, st.bounds.max.z);
        d["centroid"] = py::make_tuple(st.centroid.x, st.centroid.y, st.centroid.z);
        d["mean_spacing"] = st.meanSpacing;
        d["median_spacing"] = st.medianSpacing;
        return d;
    });
    m.def("voxel_downsample", [](const Points& xyz, double voxel) { return fromPoints(survey::voxelDownsample(bufferFrom(xyz), voxel).xyz); });
    m.def("cloud_to_mesh", [](const Points& xyz, const Points& v, const Tris& t, bool signedDistance) {
        const auto d = survey::cloudToMesh(bufferFrom(xyz), toMesh(v, t), signedDistance);
        py::array_t<float> a(static_cast<py::ssize_t>(d.size()));
        std::copy(d.begin(), d.end(), a.mutable_data());
        return a;
    }, py::arg("points"), py::arg("vertices"), py::arg("triangles"), py::arg("signed") = true);

    // ---- formats -----------------------------------------------------------------------------------------
    P.def("import_dxf", [](PyProject& s, const fs::path& file, const std::string& layerPrefix, double chordTolerance) {
         formats::DxfImportOptions o;
         o.layerPrefix = layerPrefix;
         o.chordTolerance = chordTolerance;
         const auto r = formats::readDxf(file, o);
         const auto sum = formats::importDxfIntoProject(s.get(), r);
         py::dict d;
         d["objects"] = sum.objects.size();
         d["created_layers"] = sum.createdLayers.size();
         d["imported"] = r.imported;
         d["skipped"] = r.skipped;
         d["warnings"] = r.warnings;
         d["unit_scale"] = r.unitScale;
         return d;
     }, py::arg("file"), py::arg("layer_prefix") = "", py::arg("chord_tolerance") = 0.001)
        .def("export_dxf", [](PyProject& s, const fs::path& file, const std::vector<std::string>& ids) {
            std::vector<Uuid> u;
            for (const auto& i : ids) u.push_back(uuidArg(i));
            const auto r = formats::exportProjectDxf(s.get(), file, u);
            py::dict d;
            d["written"] = r.written;
            d["warnings"] = r.warnings;
            d["skipped"] = r.skipped;
            return d;
        }, py::arg("file"), py::arg("ids") = std::vector<std::string>{})
        .def("export_omf", [](PyProject& s, const fs::path& file, bool drillholes, std::vector<std::string> pointClouds) {
            omf::OmfExportOptions o;
            o.drillholes = drillholes;
            for (const auto& p : pointClouds) o.pointClouds.push_back(uuidArg(p));
            std::vector<std::string> notes;
            {
                py::gil_scoped_release rel;
                omf::writeOmf(omf::exportProjectOmf(s.get(), o, &notes), file);
            }
            return notes;
        }, py::arg("file"), py::arg("drillholes") = true, py::arg("point_clouds") = std::vector<std::string>{})
        .def("import_omf", [](PyProject& s, const fs::path& file) {
            const auto doc = omf::readOmf(file);
            omf::OmfImportOptions o;
            o.sourceFile = file.filename().string();
            const auto r = omf::importOmf(s.get(), doc, o);
            py::dict d;
            std::vector<std::string> g, b, c;
            for (const auto& x : r.geometry) g.push_back(x.toString());
            for (const auto& x : r.blockModels) b.push_back(x.toString());
            for (const auto& x : r.pointClouds) c.push_back(x.toString());
            d["geometry"] = g;
            d["block_models"] = b;
            d["point_clouds"] = c;
            d["notes"] = r.notes;
            return d;
        });
    m.def("read_mesh", [](const fs::path& file) {
        const auto f = formats::readMesh(file);
        auto t = fromMesh(f.mesh);
        py::dict d;
        d["vertices"] = t[0];
        d["triangles"] = t[1];
        d["name"] = f.name;
        d["warnings"] = f.warnings;
        return d;
    });
    m.def("write_mesh", [](const fs::path& file, const Points& v, const Tris& t, const std::string& name) {
        return formats::writeMesh(file, toMesh(v, t), name);
    }, py::arg("file"), py::arg("vertices"), py::arg("triangles"), py::arg("name") = "");

    // ---- geology -----------------------------------------------------------------------------------------
    P.def("import_drillholes", [](PyProject& s, const std::string& collars, const std::string& surveys, const std::map<std::string, std::string>& intervals,
                                  const py::dict& opts) {
         geology::DrillholeTables t;
         t.collars = util::parseCsv(collars);
         if (!surveys.empty()) t.surveys = util::parseCsv(surveys);
         for (const auto& [name, text] : intervals) t.intervals.push_back({name, util::parseCsv(text)});
         geology::DrillholeImportOptions o;
         if (opts.contains("code_columns")) o.codeColumns = opts["code_columns"].cast<std::vector<std::string>>();
         if (opts.contains("ignore_columns")) o.ignoreColumns = opts["ignore_columns"].cast<std::vector<std::string>>();
         if (opts.contains("columns")) o.columns = opts["columns"].cast<std::map<std::string, std::string>>();
         if (opts.contains("below_detection")) {
             const auto b = opts["below_detection"].cast<std::string>();
             o.belowDetection = b == "limit" ? geology::BelowDetection::Limit : b == "zero" ? geology::BelowDetection::Zero
                                : b == "missing" ? geology::BelowDetection::Missing : geology::BelowDetection::HalfLimit;
         }
         const bool replace = opts.contains("replace_existing") && opts["replace_existing"].cast<bool>();
         const auto r = geology::readDrillholes(t, o);
         const auto sum = geology::importDrillholes(s.get(), r, replace);
         py::dict d;
         d["created"] = sum.created.size();
         d["updated"] = sum.updated.size();
         d["skipped"] = sum.skipped;
         d["rejected_rows"] = r.rejectedRows;
         d["rejected_holes"] = r.rejectedHoles;
         d["below_detection"] = r.belowDetectionCount;
         d["numeric_fields"] = r.numericFields;
         d["code_fields"] = r.codeFields;
         py::list issues;
         static const char* sev[] = {"info", "warning", "error"};
         for (const auto& i : r.issues) {
             py::dict x;
             x["severity"] = sev[static_cast<int>(i.severity)];
             x["table"] = i.table;
             x["row"] = i.row;
             x["hole"] = i.holeId;
             x["message"] = i.message;
             issues.append(x);
         }
         d["issues"] = issues;
         return d;
     }, py::arg("collars"), py::arg("surveys") = "", py::arg("intervals") = std::map<std::string, std::string>{}, py::arg("options") = py::dict())
        .def("drillhole_ids", [](PyProject& s) {
            std::vector<std::string> v;
            for (const auto& h : s.get().model().drillholes.all()) v.push_back(h.holeId);
            return v;
        })
        .def("drillhole_trace", [](PyProject& s, const std::string& holeId, double step, const std::string& method) {
            for (const auto& h : s.get().model().drillholes.all())
                if (h.holeId == holeId) {
                    const auto m = method == "tangent" ? geology::DesurveyMethod::Tangent
                                   : method == "balanced_tangent" ? geology::DesurveyMethod::BalancedTangent
                                                                  : geology::DesurveyMethod::MinimumCurvature;
                    return fromPoints(geology::DesurveyedHole(h, m).trace(step));
                }
            throw py::key_error("No drillhole " + holeId);
        }, py::arg("hole"), py::arg("step") = 1.0, py::arg("method") = "minimum_curvature")
        .def("composites", [](PyProject& s, const py::dict& opts, const std::string& mode, const std::string& codeField, double benchBase, double benchHeight) {
            const auto o = compositeArg(opts);
            py::list l;
            for (const auto& h : s.get().model().drillholes.all()) {
                std::vector<geology::Composite> cs;
                if (mode == "length") cs = geology::compositeByLength(h, o);
                else if (mode == "code") cs = geology::compositeByCode(h, codeField, o);
                else if (mode == "bench") cs = geology::compositeByBench(h, benchBase, benchHeight, o);
                else throw py::value_error("mode must be length, code or bench");
                for (const auto& c : cs) l.append(compositeDict(c));
            }
            return l;
        }, py::arg("options") = py::dict(), py::arg("mode") = "length", py::arg("code_field") = "", py::arg("bench_base") = 0.0, py::arg("bench_height") = 5.0)
        .def("create_block_model", [](PyProject& s, const py::dict& d) {
            BlockModel b;
            b.name = d["name"].cast<std::string>();
            const auto o = d["origin"].cast<std::array<double, 3>>();
            const auto bs = d["block_size"].cast<std::array<double, 3>>();
            const auto n = d["shape"].cast<std::array<int, 3>>();
            b.origin = {o[0], o[1], o[2]};
            b.blockSize = {bs[0], bs[1], bs[2]};
            b.nx = n[0];
            b.ny = n[1];
            b.nz = n[2];
            if (d.contains("rotation_deg")) b.rotationDeg = d["rotation_deg"].cast<double>();
            if (d.contains("description")) b.description = d["description"].cast<std::string>();
            return geology::createBlockModel(s.get(), b).toString();
        })
        .def("block_model", [](PyProject& s, const std::string& id) { return blockModelDict(geology::loadBlockModel(s.get(), uuidArg(id))); })
        .def("set_block_values", [](PyProject& s, const std::string& id, const std::map<std::string, py::array_t<double, py::array::c_style | py::array::forcecast>>& values,
                                    const std::vector<std::string>& remove) {
            auto d = geology::loadBlockModel(s.get(), uuidArg(id));
            for (const auto& [k, a] : values) {
                auto v = toVector(a);
                if (v.size() != d.count()) throw py::value_error("Attribute '" + k + "' must have " + std::to_string(d.count()) + " values");
                d.attribute(k) = std::move(v);
            }
            for (const auto& r : remove) d.remove(r);
            geology::saveBlockModel(s.get(), d, "Set block model values");
        }, py::arg("id"), py::arg("values"), py::arg("remove") = std::vector<std::string>{})
        .def("block_centroids", [](PyProject& s, const std::string& id) {
            const auto* b = s.get().model().blockModels.find(uuidArg(id));
            if (!b) throw py::key_error("No block model " + id);
            const geology::BlockGrid g(*b);
            std::vector<Vec3> c(g.count());
            for (std::size_t i = 0; i < c.size(); ++i) c[i] = g.centroid(i);
            return fromPoints(c);
        })
        .def("domain_proportions", [](PyProject& s, const std::string& id, const std::string& meshId, const std::string& field, int sub) {
            auto d = geology::loadBlockModel(s.get(), uuidArg(id));
            const auto& g = geometryOf(s.get(), meshId);
            if (!g.mesh) throw py::value_error("The object has no mesh");
            std::vector<double> f;
            {
                py::gil_scoped_release rel;
                f = geology::fractionInside(geology::BlockGrid(d.header), *g.mesh, sub);
            }
            d.attribute(field) = f;
            geology::saveBlockModel(s.get(), d, "Domain proportions from " + g.name);
            return fromVector(f);
        }, py::arg("block_model"), py::arg("mesh"), py::arg("field"), py::arg("subdivisions") = 4)
        .def("estimate", [](PyProject& s, const std::string& id, const py::list& composites, const std::string& field, const py::dict& opts, jobs::Progress* progress) {
            std::vector<geology::Composite> cs;
            for (const auto& c : composites) cs.push_back(compositeFrom(c.cast<py::dict>()));
            const auto samples = geology::samplesFromComposites(cs, field, opts.contains("code_field") ? opts["code_field"].cast<std::string>() : "",
                                                                opts.contains("code") ? opts["code"].cast<std::string>() : "");
            geology::EstimationOptions o;
            const auto method = opts.contains("method") ? opts["method"].cast<std::string>() : "ordinary_kriging";
            o.method = method == "nearest" ? geology::EstimationMethod::NearestNeighbour
                       : method == "idw"   ? geology::EstimationMethod::InverseDistance
                       : method == "simple_kriging" ? geology::EstimationMethod::SimpleKriging
                                                    : geology::EstimationMethod::OrdinaryKriging;
            if (opts.contains("output")) o.outputField = opts["output"].cast<std::string>();
            if (opts.contains("power")) o.power = opts["power"].cast<double>();
            if (opts.contains("variogram")) o.variogram = variogramArg(opts["variogram"].cast<py::dict>());
            if (opts.contains("mean")) o.simpleMean = opts["mean"].cast<double>();
            if (opts.contains("discretisation")) o.discretisation = opts["discretisation"].cast<std::array<int, 3>>();
            if (opts.contains("mask")) o.maskField = opts["mask"].cast<std::string>();
            if (opts.contains("passes")) {
                o.passes.clear();
                for (const auto& p : opts["passes"]) {
                    const auto pd = p.cast<py::dict>();
                    geology::SearchPass sp;
                    sp.ellipse = anisoArg(pd);
                    if (pd.contains("min_samples")) sp.minSamples = pd["min_samples"].cast<int>();
                    if (pd.contains("max_samples")) sp.maxSamples = pd["max_samples"].cast<int>();
                    if (pd.contains("max_per_hole")) sp.maxPerHole = pd["max_per_hole"].cast<int>();
                    if (pd.contains("max_per_octant")) sp.maxPerOctant = pd["max_per_octant"].cast<int>();
                    o.passes.push_back(sp);
                }
            }
            auto d = geology::loadBlockModel(s.get(), uuidArg(id));
            geology::EstimationSummary sum;
            {
                py::gil_scoped_release rel;
                sum = geology::estimate(d, samples, o, progress ? *progress : jobs::nullProgress());
            }
            geology::saveBlockModel(s.get(), d, std::string("Estimate ") + o.outputField + " (" + geology::toString(o.method) + ")");
            py::dict r;
            r["estimated"] = sum.estimated;
            r["not_estimated"] = sum.notEstimated;
            r["skipped_mask"] = sum.skippedMask;
            r["per_pass"] = sum.perPass;
            r["singular_systems"] = sum.singularSystems;
            r["samples"] = samples.points.size();
            r["merged_duplicates"] = samples.mergedDuplicates;
            return r;
        }, py::arg("block_model"), py::arg("composites"), py::arg("field"), py::arg("options") = py::dict(), py::arg("progress") = nullptr)
        .def("grade_tonnage", [](PyProject& s, const std::string& id, const std::string& grade, const std::vector<double>& cutoffs, const py::dict& opts) {
            const auto d = geology::loadBlockModel(s.get(), uuidArg(id));
            geology::GradeTonnageOptions o;
            o.gradeField = grade;
            o.cutoffs = cutoffs;
            if (opts.contains("density_field")) o.densityField = opts["density_field"].cast<std::string>();
            if (opts.contains("density")) o.defaultDensity = opts["density"].cast<double>();
            if (opts.contains("proportion_field")) o.proportionField = opts["proportion_field"].cast<std::string>();
            if (opts.contains("other_fields")) o.otherFields = opts["other_fields"].cast<std::vector<std::string>>();
            py::list l;
            for (const auto& r : geology::gradeTonnage(d, o)) {
                py::dict x;
                x["cutoff"] = r.cutoff;
                x["blocks"] = r.blocks;
                x["volume"] = r.volume;
                x["tonnes"] = r.tonnes;
                x["grades"] = r.grades;
                x["metal"] = r.metal;
                l.append(x);
            }
            return l;
        }, py::arg("block_model"), py::arg("grade"), py::arg("cutoffs"), py::arg("options") = py::dict());

    m.def("experimental_variogram", [](const Points& pts, const std::vector<double>& values, double lag, int lags, double azimuth, double dip, double angleTol,
                                        double bandwidth, bool pairwiseRelative) {
        geology::SampleSet s;
        s.points = toPoints(pts);
        s.values = values;
        s.group.assign(values.size(), 0);
        geology::ExperimentalOptions o;
        o.lag = lag;
        o.lags = lags;
        o.azimuth = azimuth;
        o.dip = dip;
        o.angleTolerance = angleTol;
        o.bandwidth = bandwidth;
        o.kind = pairwiseRelative ? geology::VariogramKind::PairwiseRelative : geology::VariogramKind::Semivariogram;
        py::list l;
        for (const auto& p : geology::experimentalVariogram(s, o)) l.append(py::make_tuple(p.distance, p.gamma, p.pairs));
        return l;
    }, py::arg("points"), py::arg("values"), py::arg("lag"), py::arg("lags"), py::arg("azimuth") = 0.0, py::arg("dip") = 0.0, py::arg("angle_tolerance") = 90.0,
       py::arg("bandwidth") = 1e300, py::arg("pairwise_relative") = false);
    m.def("fit_variogram", [](const std::vector<std::tuple<double, double, std::size_t>>& lags, const py::dict& initial, std::array<double, 3> direction, bool fixNugget) {
        std::vector<geology::LagPoint> lp;
        for (auto [d, g, n] : lags) lp.push_back({d, g, n});
        const auto f = geology::fitVariogram(lp, variogramArg(initial), {direction[0], direction[1], direction[2]}, fixNugget);
        auto d = variogramDict(f.model);
        d["weighted_sse"] = f.weightedSse;
        d["converged"] = f.converged;
        return d;
    }, py::arg("lags"), py::arg("initial"), py::arg("direction") = std::array<double, 3>{1, 0, 0}, py::arg("fix_nugget") = false);
    m.def("statistics", [](const std::vector<double>& v, const std::vector<double>& w) {
        const auto s = geology::statistics(v, w);
        py::dict d;
        d["count"] = s.count;
        d["mean"] = s.mean;
        d["variance"] = s.variance;
        d["std"] = s.stdDev;
        d["cv"] = s.cv;
        d["min"] = s.min;
        d["max"] = s.max;
        d["p10"] = s.p10;
        d["p25"] = s.p25;
        d["median"] = s.median;
        d["p75"] = s.p75;
        d["p90"] = s.p90;
        d["p95"] = s.p95;
        d["p99"] = s.p99;
        return d;
    }, py::arg("values"), py::arg("weights") = std::vector<double>{});

    // ---- geotechnical ----------------------------------------------------------------------------------------
    m.def("q_system", [](const py::dict& in) {
        const auto r = geotechnical::computeQ(qArg(in));
        py::dict d;
        d["q"] = r.q;
        d["class"] = r.rockClass;
        d["block_size"] = r.blockSize;
        d["shear_strength"] = r.shearStrength;
        d["active_stress"] = r.activeStress;
        d["warnings"] = r.warnings;
        return d;
    });
    m.def("q_support", [](const py::dict& in, double span, double height, double esr) {
        const auto s = geotechnical::qSupport(qArg(in), span, height, esr);
        py::dict d;
        d["equivalent_dimension"] = s.equivalentDimension;
        d["max_unsupported_span"] = s.maxUnsupportedSpan;
        d["roof_bolt_length"] = s.roofBoltLength;
        d["wall_bolt_length"] = s.wallBoltLength;
        d["q_wall"] = s.qWall;
        d["roof_pressure_mpa"] = s.roofPressureMPa;
        d["wall_pressure_mpa"] = s.wallPressureMPa;
        d["unsupported_span_exceeded"] = s.unsupportedSpanExceeded;
        return d;
    }, py::arg("inputs"), py::arg("span"), py::arg("height") = 0.0, py::arg("esr") = 1.6);
    m.def("rmr89", [](const py::dict& d) {
        geotechnical::RmrInputs in;
        if (d.contains("ucs_mpa")) in.ucsMPa = d["ucs_mpa"].cast<double>();
        if (d.contains("point_load_mpa")) in.pointLoadMPa = d["point_load_mpa"].cast<double>();
        in.rqd = d["rqd"].cast<double>();
        in.spacingM = d["spacing_m"].cast<double>();
        if (d.contains("condition_rating")) {
            const int c = d["condition_rating"].cast<int>();
            in.condition = c >= 30 ? geotechnical::JointCondition::VeryRough : c >= 25 ? geotechnical::JointCondition::SlightlyRough
                           : c >= 20 ? geotechnical::JointCondition::SlightlyRoughHighlyWeathered : c >= 10 ? geotechnical::JointCondition::Slickensided
                                                                                                 : geotechnical::JointCondition::SoftGouge;
        }
        if (d.contains("condition")) {
            const auto c = d["condition"].cast<py::dict>();
            geotechnical::RmrConditionDetail cd;
            cd.persistence = c["persistence_m"].cast<double>();
            cd.aperture = c["aperture_mm"].cast<double>();
            const auto ro = c["roughness"].cast<std::string>();
            cd.roughness = ro == "very_rough" ? geotechnical::Roughness::VeryRough : ro == "rough" ? geotechnical::Roughness::Rough
                           : ro == "slightly_rough" ? geotechnical::Roughness::SlightlyRough : ro == "smooth" ? geotechnical::Roughness::Smooth
                                                                                              : geotechnical::Roughness::Slickensided;
            const auto inf = c["infilling"].cast<std::string>();
            cd.infilling = inf == "none" ? geotechnical::Infilling::None : inf == "hard_under_5mm" ? geotechnical::Infilling::HardUnder5mm
                           : inf == "hard_over_5mm" ? geotechnical::Infilling::HardOver5mm : inf == "soft_under_5mm" ? geotechnical::Infilling::SoftUnder5mm
                                                                                              : geotechnical::Infilling::SoftOver5mm;
            const auto w = c["weathering"].cast<std::string>();
            cd.weathering = w == "unweathered" ? geotechnical::Weathering::Unweathered : w == "slightly" ? geotechnical::Weathering::Slightly
                            : w == "moderately" ? geotechnical::Weathering::Moderately : w == "highly" ? geotechnical::Weathering::Highly
                                                                                       : geotechnical::Weathering::Decomposed;
            in.conditionDetail = cd;
        }
        if (d.contains("inflow_l_min_10m")) in.inflowLitresPerMinPer10m = d["inflow_l_min_10m"].cast<double>();
        if (d.contains("water_pressure_ratio")) in.waterPressureRatio = d["water_pressure_ratio"].cast<double>();
        if (d.contains("groundwater")) {
            const auto g = d["groundwater"].cast<std::string>();
            in.groundwater = g == "dry" ? geotechnical::GroundwaterCondition::CompletelyDry : g == "damp" ? geotechnical::GroundwaterCondition::Damp
                             : g == "wet" ? geotechnical::GroundwaterCondition::Wet : g == "dripping" ? geotechnical::GroundwaterCondition::Dripping
                                                                                     : geotechnical::GroundwaterCondition::Flowing;
        }
        if (d.contains("joint_dip") && d.contains("joint_dip_direction") && d.contains("drive_trend"))
            in.orientation = geotechnical::tunnelOrientationClass(d["joint_dip"].cast<double>(), d["joint_dip_direction"].cast<double>(), d["drive_trend"].cast<double>());
        const auto r = geotechnical::computeRmr89(in);
        py::dict o;
        o["rmr"] = r.rmr;
        o["basic"] = r.basic;
        o["adjustment"] = r.adjustment;
        o["class"] = r.rockClass;
        o["class_number"] = r.classNumber;
        o["stand_up_time"] = r.standUpTime;
        o["ratings"] = py::make_tuple(r.ucsRating, r.rqdRating, r.spacingRating, r.conditionRating, r.groundwaterRating);
        if (in.orientation) o["orientation"] = geotechnical::toString(*in.orientation);
        try {
            o["gsi"] = geotechnical::gsiFromRmr89(r);
        } catch (const geotechnical::ClassificationError&) {
            o["gsi"] = py::none();
        }
        return o;
    });
    m.def("hoek_brown", [](double gsi, double mi, double sci, double D, double Ei) {
        const auto h = geotechnical::hoekBrown(gsi, mi, sci, D, Ei);
        py::dict d;
        d["mb"] = h.mb;
        d["s"] = h.s;
        d["a"] = h.a;
        d["sigma_c_mass"] = h.sigmaC;
        d["sigma_t"] = h.sigmaT;
        d["sigma_cm"] = h.sigmaCm;
        d["modulus_mpa"] = h.modulusMPa;
        return d;
    }, py::arg("gsi"), py::arg("mi"), py::arg("sigma_ci"), py::arg("disturbance") = 0.0, py::arg("intact_modulus") = 0.0);
    m.def("fisher", [](const std::vector<std::pair<double, double>>& planes) {
        const auto f = geotechnical::fisher(polesArg(planes));
        py::dict d;
        d["mean_pole"] = py::make_tuple(f.mean.trend, f.mean.plunge);
        d["mean_plane"] = planeDict(geotechnical::planeOfPole(f.mean));
        d["k"] = f.k;
        d["alpha95"] = f.alpha95;
        d["n"] = f.n;
        return d;
    }, "Fisher statistics of planes given as (dip, dip direction)");
    m.def("joint_sets", [](const std::vector<std::pair<double, double>>& planes, int k, double maxAngle, unsigned seed) {
        const auto r = geotechnical::identifyJointSets(polesArg(planes), k, maxAngle, seed);
        py::list sets;
        for (const auto& s : r.sets) sets.append(setDict(s));
        py::dict d;
        d["sets"] = sets;
        d["assignment"] = r.assignment;
        return d;
    }, py::arg("planes"), py::arg("k"), py::arg("max_angle") = 0.0, py::arg("seed") = 1u);
    m.def("kinematics", [](const std::vector<std::pair<double, double>>& planes, std::pair<double, double> face, double friction,
                           const std::vector<std::pair<double, double>>& setPlanes) {
        geotechnical::KinematicOptions o;
        o.face = {face.first, face.second};
        o.frictionDeg = friction;
        std::vector<geotechnical::Plane> sp;
        for (auto [a, b] : setPlanes) sp.push_back({a, b});
        const auto r = geotechnical::kinematicAnalysis(polesArg(planes), sp, o);
        py::dict d;
        d["planar_percent"] = r.planar.percent;
        d["planar"] = r.planar.critical;
        d["toppling_percent"] = r.toppling.percent;
        d["toppling"] = r.toppling.critical;
        py::list w;
        for (const auto& x : r.wedges) {
            py::dict e;
            e["sets"] = py::make_tuple(x.setA, x.setB);
            e["intersection"] = py::make_tuple(x.intersection.trend, x.intersection.plunge);
            e["critical"] = x.critical;
            e["single_plane_sliding"] = x.singlePlaneSliding;
            w.append(e);
        }
        d["wedges"] = w;
        return d;
    }, py::arg("planes"), py::arg("face"), py::arg("friction") = 30.0, py::arg("set_planes") = std::vector<std::pair<double, double>>{});
    m.def("stereonet_svg", [](const std::vector<std::pair<double, double>>& planes, int sets, const std::vector<double>& contours, std::optional<double> friction,
                              const std::string& title) {
        const auto poles = polesArg(planes);
        std::vector<geotechnical::JointSet> js;
        if (sets > 0) js = geotechnical::identifyJointSets(poles, sets).sets;
        const auto dens = geotechnical::poleDensity(poles);
        geotechnical::StereonetSvgOptions o;
        o.contourLevels = contours;
        o.frictionDeg = friction;
        o.title = title;
        return geotechnical::stereonetSvg(poles, js, &dens, o);
    }, py::arg("planes"), py::arg("sets") = 0, py::arg("contours") = std::vector<double>{2, 4, 6, 8}, py::arg("friction") = py::none(), py::arg("title") = "");
    P.def("record_q", [](PyProject& s, const std::string& dev, double chainage, const py::dict& in, const std::string& by) {
        return geotechnical::recordQ(s.get(), dev.empty() ? Uuid{} : uuidArg(dev), chainage, {}, qArg(in), by).toString();
    }, py::arg("development"), py::arg("chainage"), py::arg("inputs"), py::arg("mapped_by") = "");

#ifdef OPENMINE_PY_HAVE_CRS
    // ---- coordinates -----------------------------------------------------------------------------------------
    m.def("proj_version", &crs::projVersion);
    m.def("describe_crs", [](const std::string& def) {
        const auto i = crs::describe(def);
        return toPy(json::parse(pe::encodeEntity(crs::toEntity(i))));
    });
    P.def("declare_crs", [](PyProject& s, const std::string& def, bool makeProjectCrs) {
         const Uuid id = crs::declareCrs(s.get(), def);
         if (makeProjectCrs) crs::setProjectCrs(s.get(), id);
         return id.toString();
     }, py::arg("definition"), py::arg("set_as_project_crs") = false)
        .def("transform_points", [](PyProject& s, const Points& pts, const std::string& src, const std::string& tgt) {
            return fromPoints(crs::transformPoints(s.get(), toPoints(pts), uuidArg(src), uuidArg(tgt)));
        });
#endif
#ifdef OPENMINE_PY_HAVE_GIS
    m.def("import_dem", [](const fs::path& file, int step) {
        gis::DemImportOptions o;
        o.step = step;
        return fromMesh(gis::importDem(file, o));
    }, py::arg("file"), py::arg("step") = 1);
#endif
#ifdef OPENMINE_PY_HAVE_OCCT
    P.def("solid_from_mesh", [](PyProject& s, const std::string& meshId, bool requireClosed) {
        return occt::solidFromMeshObject(s.get(), uuidArg(meshId), requireClosed).toString();
    }, py::arg("mesh"), py::arg("require_closed") = true);
#endif

    // ---- reports ----------------------------------------------------------------------------------------
    // kind: "conformance" (id = DesignConformanceResult), "registration", "point_cloud".
    // Returns the stored Report id when `store`, and writes `file` when given.
    P.def("survey_report", [](PyProject& s, const std::string& kind, const std::string& id, const std::string& format, bool store, const std::optional<fs::path>& file,
                              bool spacing) {
         reporting::ReportDocument doc;
         if (kind == "conformance") doc = reporting::conformanceReport(s.get(), uuidArg(id));
         else if (kind == "registration") doc = reporting::registrationReport(s.get(), uuidArg(id));
         else if (kind == "point_cloud") doc = reporting::pointCloudReport(s.get(), uuidArg(id), spacing);
         else throw py::value_error("kind must be conformance, registration or point_cloud");
         if (format != "html" && format != "pdf") throw py::value_error("format must be html or pdf");
         py::dict d;
         d["title"] = doc.title;
         if (file) {
             std::ofstream o(*file, std::ios::binary);
             o << (format == "html" ? reporting::renderHtml(doc) : reporting::renderPdf(doc));
             if (!o) throw py::value_error("Cannot write " + file->string());
         }
         if (store) d["report"] = reporting::storeReport(s.get(), doc, "Survey", format).toString();
         return d;
     }, py::arg("kind"), py::arg("id"), py::arg("format") = "html", py::arg("store") = true, py::arg("file") = py::none(), py::arg("spacing_statistics") = false)
        .def("reports", [](PyProject& s) {
            py::list l;
            for (const auto& r : s.get().model().reports.all()) {
                py::dict d;
                d["id"] = r.id.toString();
                d["name"] = r.name;
                d["kind"] = r.kind;
                d["format"] = r.format;
                d["created_at"] = r.createdAt;
                l.append(d);
            }
            return l;
        })
        .def("report_bytes", [](PyProject& s, const std::string& id) {
            const auto* r = s.get().model().reports.find(uuidArg(id));
            if (!r) throw py::key_error("No report " + id);
            const auto* a = s.get().model().assets.find(r->asset.id);
            if (!a) throw py::key_error("The report's file is missing");
            return py::bytes(s.get().assets().readBytes(*a));
        });

    // ---- plugins -----------------------------------------------------------------------------------------
    py::class_<plugins::PluginManager>(m, "PluginManager")
        .def(py::init<>())
        .def("load_all", [](plugins::PluginManager& pm, const std::vector<fs::path>& dirs, const std::vector<std::string>& disabled) { return pm.loadAll(dirs, disabled); },
             py::arg("directories"), py::arg("disabled") = std::vector<std::string>{})
        .def("plugins", [](plugins::PluginManager& pm) {
            py::list l;
            for (const auto& s : pm.plugins()) {
                py::dict d;
                d["id"] = s.manifest.id;
                d["name"] = s.manifest.name;
                d["version"] = s.manifest.version;
                d["loaded"] = s.loaded;
                d["error"] = s.error;
                d["commands"] = s.commands;
                d["importers"] = s.importers;
                d["exporters"] = s.exporters;
                l.append(d);
            }
            return l;
        })
        .def("commands", [](plugins::PluginManager& pm) {
            py::list l;
            for (const auto& c : pm.commands()) l.append(py::make_tuple(c.name, c.description, c.pluginId));
            return l;
        })
        .def("run_command", [](plugins::PluginManager& pm, PyProject& p, const std::string& name, const py::object& args) {
            return pluginResult(pm.runCommand(p.get(), name, fromPy(args).dump()));
        }, py::arg("project"), py::arg("name"), py::arg("arguments") = py::none())
        .def("run_import", [](plugins::PluginManager& pm, PyProject& p, const fs::path& file, const py::object& opts) {
            return pluginResult(pm.runImport(p.get(), file, fromPy(opts).dump()));
        }, py::arg("project"), py::arg("file"), py::arg("options") = py::none())
        .def("run_export", [](plugins::PluginManager& pm, PyProject& p, const fs::path& file, const py::object& opts) {
            return pluginResult(pm.runExport(p.get(), file, fromPy(opts).dump()));
        }, py::arg("project"), py::arg("file"), py::arg("options") = py::none());

#ifdef OPENMINE_PY_HAVE_REPOSITORY
    // ---- shared repository ---------------------------------------------------------------------------------
    using repository::PgRepository;
    using repository::WorkingCopy;
    py::class_<PgRepository>(m, "Repository")
        .def(py::init<const std::string&>(), py::arg("connection"))
        .def("initialise", &PgRepository::initialise)
        .def_property_readonly("schema_version", &PgRepository::schemaVersion)
        .def("create_project", [](PgRepository& r, PyProject& p, const std::string& author, const std::string& msg) {
            return r.createProject(p.get(), author, msg).toJson();
        }, py::arg("project"), py::arg("author"), py::arg("message") = "Initial import")
        .def("projects", [](PgRepository& r) {
            py::list l;
            for (const auto& [id, name] : r.projects()) l.append(py::make_tuple(id.toString(), name));
            return l;
        })
        .def("checkout", [](PgRepository& r, const std::string& id, std::int64_t rev) {
            WorkingCopy wc;
            auto p = r.checkout(uuidArg(id), wc, rev);
            return py::make_tuple(PyProject(std::move(p)), wc.toJson());
        }, py::arg("project_id"), py::arg("revision") = 0)
        .def("head_revision", [](PgRepository& r, const std::string& id) { return r.headRevision(uuidArg(id)); })
        .def("delete_project", [](PgRepository& r, const std::string& id) { r.deleteProject(uuidArg(id)); })
        .def("revisions", [](PgRepository& r, const std::string& id) {
            py::list l;
            for (const auto& v : r.revisions(uuidArg(id))) l.append(py::make_tuple(v.revision, v.author, v.message, v.createdAt, v.changes));
            return l;
        })
        .def("pull", [](PgRepository& r, PyProject& p, const std::string& wcJson, const std::string& policy) {
            auto wc = WorkingCopy::fromJson(wcJson);
            const auto pol = policy == "take_remote" ? repository::ConflictPolicy::TakeRemote
                             : policy == "keep_local" ? repository::ConflictPolicy::KeepLocal
                                                      : repository::ConflictPolicy::Abort;
            const auto res = r.pull(p.get(), wc, pol);
            py::dict d;
            d["applied"] = res.applied;
            d["aborted"] = res.aborted;
            d["to_revision"] = res.toRevision;
            py::list c;
            for (const auto& x : res.conflicts) c.append(py::make_tuple(x.type, x.key, x.name, x.reason));
            d["conflicts"] = c;
            return py::make_tuple(d, wc.toJson());
        }, py::arg("project"), py::arg("working_copy"), py::arg("policy") = "abort")
        .def("publish", [](PgRepository& r, PyProject& p, const std::string& wcJson, const std::string& author, const std::string& msg) {
            auto wc = WorkingCopy::fromJson(wcJson);
            const auto res = r.publish(p.get(), wc, author, msg);
            py::dict d;
            d["published"] = res.published;
            d["revision"] = res.revision;
            d["changes"] = res.changes;
            d["refusal"] = res.refusal;
            py::list c;
            for (const auto& x : res.conflicts) c.append(py::make_tuple(x.type, x.key, x.name, x.reason));
            d["conflicts"] = c;
            return py::make_tuple(d, wc.toJson());
        })
        .def("intersecting", [](PgRepository& r, const std::string& id, std::array<double, 4> box) {
            py::list l;
            for (const auto& h : r.intersecting(uuidArg(id), box[0], box[1], box[2], box[3])) l.append(py::make_tuple(h.type, h.key, h.name));
            return l;
        })
        .def("lock", [](PgRepository& r, const std::string& id, const std::string& type, const std::string& key, const std::string& holder, int seconds) {
            return r.lock(uuidArg(id), type, key, holder, std::chrono::seconds(seconds));
        }, py::arg("project_id"), py::arg("type"), py::arg("key"), py::arg("holder"), py::arg("seconds") = 3600)
        .def("unlock", [](PgRepository& r, const std::string& id, const std::string& type, const std::string& key, const std::string& holder) {
            r.unlock(uuidArg(id), type, key, holder);
        });
#endif
}

} // namespace openmine::python
