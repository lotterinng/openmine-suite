// SPDX-License-Identifier: Apache-2.0
// Native bindings. Conventions: identifiers are uuid strings; dates "YYYY-MM-DD" and date-times
// "YYYY-MM-DD HH:MM"; points are numpy float64 arrays of shape (n, 3) (any n×3 sequence is
// accepted); meshes are (vertices (n,3) float64, triangles (m,3) uint32). Long operations release
// the GIL and accept an optional Progress (cancel() from another thread). Everything that changes
// a project goes through the editor: validated, undoable and audited exactly as in the application.
#include "Bindings.h"

#include "openmine/cad/Annotation.h"
#include "openmine/cad/Edit.h"
#include "openmine/formats/Dxf.h"
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
#include "openmine/geometry/Profile.h"
#include "openmine/geotechnical/RockMass.h"
#include "openmine/geotechnical/Stereonet.h"
#include "openmine/mine_design/Development.h"
#include "openmine/mine_design/GeometryEdits.h"
#include "openmine/mine_design/Stopes.h"
#include "openmine/mine_design/Revisions.h"
#include "openmine/mine_design/Layout.h"
#include "openmine/mine_design/Headings.h"
#include "openmine/omf/OmfProject.h"
#include "openmine/persistence/EntityCodec.h"
#include "openmine/persistence/ProjectFile.h"
#include "openmine/planning/FourD.h"
#include "openmine/plugins/PluginManager.h"
#include "openmine/assets/AssetStore.h"
#include "openmine/project/Attachments.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/reporting/ScheduleExport.h"
#include "openmine/reporting/SurveyReports.h"
#include "openmine/scheduling/Cpm.h"
#include "openmine/scheduling/Levelling.h"
#include "openmine/scheduling/Periods.h"
#include "openmine/scheduling/Production.h"
#include "openmine/scheduling/ScenarioSchedule.h"
#include "openmine/scheduling/ScheduleTable.h"
#include "openmine/survey/AsBuilt.h"
#include "openmine/survey/Conformance.h"
#include "openmine/survey/Derivations.h"
#include "openmine/survey/SurfaceTools.h"
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
#include "openmine/occt/Modelling.h"
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
#include <functional>
#include <optional>
#include <set>
#include <fstream>
#include <numbers>
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
    if (!b.normal.empty()) {
        py::array_t<float> a({static_cast<py::ssize_t>(b.normal.size()), static_cast<py::ssize_t>(3)});
        auto w = a.mutable_unchecked<2>();
        for (std::size_t i = 0; i < b.normal.size(); ++i)
            for (py::ssize_t k = 0; k < 3; ++k) w(static_cast<py::ssize_t>(i), k) = b.normal[i][static_cast<std::size_t>(k)];
        d["normals"] = a;
    }
    if (!b.returnNumber.empty()) {
        py::array_t<std::uint8_t> rn(static_cast<py::ssize_t>(b.returnNumber.size())), nr(static_cast<py::ssize_t>(b.numberOfReturns.size()));
        std::copy(b.returnNumber.begin(), b.returnNumber.end(), rn.mutable_data());
        std::copy(b.numberOfReturns.begin(), b.numberOfReturns.end(), nr.mutable_data());
        d["return_number"] = rn;
        d["number_of_returns"] = nr;
    }
    if (!b.gpsTime.empty()) {
        py::array_t<double> a(static_cast<py::ssize_t>(b.gpsTime.size()));
        std::copy(b.gpsTime.begin(), b.gpsTime.end(), a.mutable_data());
        d["gps_time"] = a;
    }
    for (std::size_t s = 0; s < b.scalars.size(); ++s) {
        py::array_t<float> a(static_cast<py::ssize_t>(b.scalars[s].size()));
        std::copy(b.scalars[s].begin(), b.scalars[s].end(), a.mutable_data());
        d[py::str(b.schema().scalarFields[s])] = a;
    }
    return d;
}

Vec3 vec(const std::array<double, 3>& a) { return {a[0], a[1], a[2]}; }

std::vector<Uuid> uuids(const std::vector<std::string>& ids) {
    std::vector<Uuid> out;
    out.reserve(ids.size());
    for (const auto& id : ids) out.push_back(uuidArg(id));
    return out;
}

py::dict editDict(const mine_design::EditResult& r) {
    auto strs = [](const std::vector<Uuid>& v) {
        std::vector<std::string> o;
        for (const auto& u : v) o.push_back(u.toString());
        return o;
    };
    py::dict d;
    d["changed"] = strs(r.changed);
    d["created"] = strs(r.created);
    d["removed"] = strs(r.removed);
    d["notes"] = r.notes;
    d["flagged_activities"] = r.flaggedActivities;
    return d;
}

/// A NewDevelopment from keyword data (heading_id, name, level — a MineLevel code/name/id or free
/// text —, zone, activity_type, width, height, chainage_from, profile, material, support_class, decline).
mine_design::NewDevelopment developmentSpec(const Project& p, const py::dict& spec) {
    static const std::set<std::string> known{"heading_id", "name", "level", "zone", "activity_type", "width", "height", "chainage_from",
                                             "profile", "material", "support_class", "decline", "decline_number"};
    for (const auto& kv : spec)
        if (!known.count(kv.first.cast<std::string>())) throw py::value_error("Unknown development field '" + kv.first.cast<std::string>() + "'");
    mine_design::NewDevelopment n;
    if (spec.contains("heading_id")) n.headingId = spec["heading_id"].cast<std::string>();
    if (spec.contains("name")) n.name = spec["name"].cast<std::string>();
    if (spec.contains("level")) {
        const auto lv = spec["level"].cast<std::string>();
        const auto u = Uuid::parse(lv);
        for (const auto& l : p.model().levels.all())
            if ((u && l.id == *u) || l.code == lv || l.name == lv) n.levelId = l.id;
        if (n.levelId.isNil()) n.level = lv;
    }
    if (spec.contains("zone")) n.zone = spec["zone"].cast<std::string>();
    if (spec.contains("activity_type")) n.activityType = spec["activity_type"].cast<std::string>();
    if (spec.contains("width")) n.width = spec["width"].cast<double>();
    if (spec.contains("height")) n.height = spec["height"].cast<double>();
    if (spec.contains("chainage_from")) n.chainageFrom = spec["chainage_from"].cast<double>();
    if (spec.contains("profile")) n.profileId = uuidArg(spec["profile"].cast<std::string>());
    if (spec.contains("material")) n.materialId = uuidArg(spec["material"].cast<std::string>());
    else if (!p.model().materials.empty()) n.materialId = p.model().materials.all().front().id;
    if (spec.contains("support_class")) n.supportClassId = uuidArg(spec["support_class"].cast<std::string>());
    if (spec.contains("decline")) n.declineId = uuidArg(spec["decline"].cast<std::string>());
    if (spec.contains("decline_number")) n.declineNumber = spec["decline_number"].cast<std::string>();
    return n;
}

py::dict layoutDict(const mine_design::LayoutResult& r) {
    py::dict d;
    d["vertices"] = fromPoints(r.vertices);
    d["bulges"] = r.bulges;
    d["plan_length"] = r.planLength;
    d["length_3d"] = r.length3d;
    d["notes"] = r.notes;
    return d;
}

mine_design::LayoutResult layoutFrom(const py::dict& d) {
    mine_design::LayoutResult r;
    r.vertices = toPoints(d["vertices"].cast<Points>());
    if (d.contains("bulges")) r.bulges = d["bulges"].cast<std::vector<double>>();
    r.bulges.resize(r.vertices.size(), 0.0);
    return r;
}

mine_design::Side sideFrom(const std::string& s) {
    if (s == "left") return mine_design::Side::Left;
    if (s == "right") return mine_design::Side::Right;
    if (s == "both") return mine_design::Side::Both;
    throw py::value_error("side must be 'left', 'right' or 'both'");
}

mine_design::TurnDirection turnFrom(const std::string& s) {
    if (s == "left") return mine_design::TurnDirection::Left;
    if (s == "right") return mine_design::TurnDirection::Right;
    throw py::value_error("turn must be 'left' or 'right'");
}

geometry::Ring planRing(const Points& pts) {
    geometry::Ring r;
    for (const auto& v : toPoints(pts)) r.push_back({v.x, v.y});
    if (r.size() > 1 && distance(r.front(), r.back()) < 1e-9) r.pop_back();
    return r;
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
    py::register_exception<AssetError>(m, "AssetError", PyExc_IOError);
    py::register_exception<jobs::CancelledError>(m, "CancelledError", PyExc_RuntimeError);
    py::register_exception<survey::PointCloudError>(m, "PointCloudError", PyExc_RuntimeError);
    // File-format errors (unreadable or invalid files): RuntimeError subclasses with their own names.
    py::register_exception<formats::DxfError>(m, "DxfError", PyExc_RuntimeError);
    py::register_exception<formats::MeshFormatError>(m, "MeshFormatError", PyExc_RuntimeError);
    py::register_exception<omf::OmfError>(m, "OmfError", PyExc_RuntimeError);
#ifdef OPENMINE_PY_HAVE_GIS
    py::register_exception<gis::GisError>(m, "GisError", PyExc_RuntimeError);
#endif
#ifdef OPENMINE_PY_HAVE_OCCT
    py::register_exception<occt::SolidError>(m, "SolidError", PyExc_RuntimeError);
#endif
#ifdef OPENMINE_PY_HAVE_CRS
    // An unknown or unsuitable CRS definition is invalid input.
    py::register_exception<crs::CrsError>(m, "CrsError", PyExc_ValueError);
#endif

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
        .def("order", [](PyProject& s, const std::string& type) { return pe::entityOrder(s.get().model(), type); })
        // ---- attachments and asset compaction ------------------------------------------------------------
        .def("attach_file", [](PyProject& s, const std::string& type, const std::string& entity, const fs::path& file, const std::string& description) {
            return attachFile(s.get(), type, entity.empty() ? Uuid{} : uuidArg(entity), file, description).toString();
        }, py::arg("type"), py::arg("entity"), py::arg("file"), py::arg("description") = "")
        .def("save_attachment", [](PyProject& s, const std::string& id, const fs::path& destination) {
            saveAttachment(s.get(), uuidArg(id), destination);
        })
        .def("compact", [](PyProject& s, bool dryRun) {
            const auto r = pe::compactProject(s.get(), dryRun);
            py::dict d;
            d["unreferenced_assets"] = r.unreferencedAssets;
            d["orphan_files"] = r.orphanFiles;
            d["bytes"] = r.bytes;
            d["dry_run"] = r.dryRun;
            return d;
        }, py::arg("dry_run") = false);

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
            if (geometry::isCurve(g.type)) d["length"] = geometry::curveLength(g);
            if (g.type == GeometryType::Arc || g.type == GeometryType::Circle) {
                py::dict a;
                a["centre"] = py::make_tuple(g.arc.center.x, g.arc.center.y, g.arc.center.z);
                a["radius"] = g.arc.radius;
                a["start_deg"] = g.arc.startAngle * 180.0 / std::numbers::pi;
                a["end_deg"] = g.arc.endAngle * 180.0 / std::numbers::pi;
                d["arc"] = a;
            }
            if (g.type == GeometryType::Hatch) d["hatch_area"] = cad::hatchArea(g);
            if (g.type == GeometryType::Text) {
                d["text"] = g.text.text;
                d["text_height"] = g.text.height;
                d["rotation_deg"] = g.text.rotation * 180.0 / std::numbers::pi;
            }
            if (g.type == GeometryType::Dimension) {
                d["dimension_kind"] = toString(g.dimension.kind);
                const auto dg = cad::dimensionGraphics(g);
                d["dimension_value"] = dg.value;
                d["dimension_text"] = dg.text;
            }
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
        .def("offset_curve", [](PyProject& s, const std::string& id, double distance, std::array<double, 3> side, bool round) {
            return mine_design::offsetObject(s.get(), uuidArg(id), distance, vec(side), round).toString();
        }, py::arg("id"), py::arg("distance"), py::arg("side_point"), py::arg("round_joins") = false)
        .def("add_cad_object", [](PyProject& s, const std::string& kind, const py::dict& a, const std::string& name, const std::string& layer,
                                  const std::map<std::string, std::string>& attrs) {
            auto pt = [&](const char* k) {
                if (!a.contains(k)) throw py::value_error(std::string("'") + kind + "' needs " + k);
                return vec(a[k].cast<std::array<double, 3>>());
            };
            auto num = [&](const char* k, std::optional<double> def = std::nullopt) {
                if (!a.contains(k)) {
                    if (def) return *def;
                    throw py::value_error(std::string("'") + kind + "' needs " + k);
                }
                return a[k].cast<double>();
            };
            constexpr double deg = std::numbers::pi / 180.0;
            GeometryObject g;
            if (kind == "arc3") g = cad::makeArc3(pt("p1"), pt("p2"), pt("p3"));
            else if (kind == "arc") g = cad::makeArc(pt("centre"), num("radius"), num("start_deg") * deg, num("end_deg") * deg);
            else if (kind == "circle") g = cad::makeCircle(pt("centre"), num("radius"));
            else if (kind == "rectangle") g = cad::makeRectangle(pt("corner1"), pt("corner2"));
            else if (kind == "polygon")
                g = cad::makePolygon(pt("centre"), num("radius"), static_cast<int>(num("sides")), num("rotation_deg", 0.0) * deg,
                                     a.contains("inscribed") ? a["inscribed"].cast<bool>() : true);
            else if (kind == "spline") {
                if (!a.contains("points")) throw py::value_error("'spline' needs points");
                g = cad::makeSpline(toPoints(a["points"].cast<Points>()), a.contains("closed") && a["closed"].cast<bool>());
            } else if (kind == "text") {
                const std::string al = a.contains("align") ? a["align"].cast<std::string>() : "left";
                const TextAlign align = al == "center" || al == "centre" ? TextAlign::Center : al == "right" ? TextAlign::Right : TextAlign::Left;
                if (al != "left" && al != "center" && al != "centre" && al != "right") throw py::value_error("align must be left, centre or right");
                g = cad::makeText(pt("insertion"), a.contains("text") ? a["text"].cast<std::string>() : std::string(), num("height", 1.0),
                                  num("rotation_deg", 0.0) * deg, align);
            } else if (kind == "dimension") {
                const auto k = enumFromString<DimensionKind>(a.contains("kind") ? a["kind"].cast<std::string>() : "Aligned");
                if (!k) throw py::value_error("Unknown dimension kind (Aligned, Horizontal, Vertical, Angular, Radius, Diameter, Elevation)");
                if (!a.contains("points")) throw py::value_error("'dimension' needs points");
                g = cad::makeDimension(*k, toPoints(a["points"].cast<Points>()), num("text_height", 1.0), static_cast<int>(num("precision", 3)));
            } else if (kind == "hatch") {
                if (!a.contains("loops")) throw py::value_error("'hatch' needs loops (a list of closed boundaries)");
                std::vector<std::vector<Vec3>> loops;
                for (const auto& l : a["loops"]) loops.push_back(toPoints(l.cast<Points>()));
                g = cad::makeHatch(loops, a.contains("pattern") ? a["pattern"].cast<std::string>() : "ANSI31", num("spacing", 1.0), num("angle_deg", 0.0) * deg);
            } else {
                throw py::value_error("Unknown object kind '" + kind + "' (arc3, arc, circle, rectangle, polygon, spline, text, dimension, hatch)");
            }
            g.name = name;
            g.layerId = layerArg(s.get(), layer);
            g.attributes = attrs;
            return ProjectEditor(s.get()).addGeometry(std::move(g), "Add " + kind).toString();
        }, py::arg("kind"), py::arg("parameters"), py::arg("name") = "", py::arg("layer") = "0",
             py::arg("attributes") = std::map<std::string, std::string>{})
        .def("transform_objects", [](PyProject& s, const std::vector<std::string>& ids, const std::vector<double>& m, bool copy, const std::string& text) {
            if (m.size() != 16) throw py::value_error("The transform must have 16 values (4×4, row-major)");
            std::array<double, 16> a{};
            std::copy(m.begin(), m.end(), a.begin());
            const Transform3 t(a);
            auto& p = s.get();
            CommandStack::MacroScope macro(p.commands(), text);
            mine_design::EditResult r;
            std::vector<Uuid> others;
            for (const auto& id : ids) {
                const Uuid u = uuidArg(id);
                if (geometryOf(p, id).type == GeometryType::Solid) {
#ifdef OPENMINE_PY_HAVE_OCCT
                    const Uuid n = occt::transformSolidObject(p, u, t, copy);
                    (copy ? r.created : r.changed).push_back(n);
                    continue;
#else
                    throw py::value_error("Solids can only be transformed in builds with Open CASCADE");
#endif
                }
                others.push_back(u);
            }
            if (!others.empty()) {
                auto o = mine_design::transformObjects(p, others, t, copy, text);
                r.changed.insert(r.changed.end(), o.changed.begin(), o.changed.end());
                r.created.insert(r.created.end(), o.created.begin(), o.created.end());
                r.flaggedActivities = o.flaggedActivities;
            }
            macro.commit();
            return editDict(r);
        }, py::arg("ids"), py::arg("matrix"), py::arg("copy") = false, py::arg("text") = "Transform")
        .def("trim_curve", [](PyProject& s, const std::string& id, const std::vector<std::string>& edges, std::array<double, 3> pick) {
            return editDict(mine_design::trimObject(s.get(), uuidArg(id), uuids(edges), vec(pick)));
        }, py::arg("id"), py::arg("cutting_edges"), py::arg("pick"))
        .def("extend_curve", [](PyProject& s, const std::string& id, const std::vector<std::string>& bounds, std::array<double, 3> pick) {
            return editDict(mine_design::extendObject(s.get(), uuidArg(id), uuids(bounds), vec(pick)));
        }, py::arg("id"), py::arg("boundaries"), py::arg("pick"))
        .def("grips", [](PyProject& s, const std::string& id) {
            const auto* g = s.get().model().geometry.find(uuidArg(id));
            if (!g) throw py::key_error("Object " + id + " does not exist");
            static const char* kinds[] = {"vertex", "centre", "radius", "start", "end"};
            py::list out;
            for (const auto& gr : cad::grips(*g)) {
                py::dict d;
                d["kind"] = kinds[static_cast<int>(gr.kind)];
                d["vertex"] = gr.vertex;
                d["point"] = std::array<double, 3>{gr.point.x, gr.point.y, gr.point.z};
                out.append(d);
            }
            return out;
        })
        .def("move_grip", [](PyProject& s, const std::string& id, std::size_t grip, std::array<double, 3> to) {
            return editDict(mine_design::moveGrip(s.get(), uuidArg(id), grip, {to[0], to[1], to[2]}));
        })
        .def("split_curve", [](PyProject& s, const std::string& id, std::array<double, 3> at) {
            return editDict(mine_design::splitObject(s.get(), uuidArg(id), vec(at)));
        }, py::arg("id"), py::arg("at"))
        .def("join_curves", [](PyProject& s, const std::vector<std::string>& ids, double tol) {
            return editDict(mine_design::joinObjects(s.get(), uuids(ids), tol));
        }, py::arg("ids"), py::arg("tolerance") = 1e-4)
        .def("explode_objects", [](PyProject& s, const std::vector<std::string>& ids) { return editDict(mine_design::explodeObjects(s.get(), uuids(ids))); },
             py::arg("ids"))
        .def("fillet_curves", [](PyProject& s, const std::string& a, std::array<double, 3> pa, const std::string& b, std::array<double, 3> pb, double r) {
            return editDict(mine_design::filletObjects(s.get(), uuidArg(a), vec(pa), uuidArg(b), vec(pb), r));
        }, py::arg("first"), py::arg("pick_first"), py::arg("second"), py::arg("pick_second"), py::arg("radius"))
        .def("chamfer_curves", [](PyProject& s, const std::string& a, std::array<double, 3> pa, const std::string& b, std::array<double, 3> pb, double d1,
                                  double d2) {
            return editDict(mine_design::chamferObjects(s.get(), uuidArg(a), vec(pa), uuidArg(b), vec(pb), d1, d2));
        }, py::arg("first"), py::arg("pick_first"), py::arg("second"), py::arg("pick_second"), py::arg("distance1"), py::arg("distance2"));

    // ---- mine design -----------------------------------------------------------------------------------------
    P.def("create_development", [](PyProject& s, const std::string& geometryId, const py::dict& spec) {
         return mine_design::createDevelopment(s.get(), uuidArg(geometryId), developmentSpec(s.get(), spec)).toString();
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
        })
        // layout generators (results are previews: nothing is added until create_headings)
        .def("layout", [](PyProject& s, const std::string& kind, const py::dict& a) {
            auto num = [&](const char* k, std::optional<double> def = std::nullopt) {
                if (!a.contains(k)) {
                    if (def) return *def;
                    throw py::value_error(kind + " layout needs " + k);
                }
                return a[k].cast<double>();
            };
            auto pt = [&](const char* k) {
                if (!a.contains(k)) throw py::value_error(kind + " layout needs " + k);
                return vec(a[k].cast<std::array<double, 3>>());
            };
            auto str = [&](const char* k, const char* def) { return a.contains(k) ? a[k].cast<std::string>() : std::string(def); };
            py::list out;
            if (kind == "horizontal") {
                out.append(layoutDict(mine_design::horizontalAlignment(toPoints(a["points"].cast<Points>()), num("radius"), num("grade_percent", 0.0),
                                                                       a.contains("use_point_elevations") && a["use_point_elevations"].cast<bool>())));
            } else if (kind == "decline") {
                mine_design::DeclineSpec d;
                d.start = pt("start");
                d.startAzimuthDeg = num("azimuth_deg", 0.0);
                d.gradePercent = num("grade_percent", -12.5);
                d.straightLength = num("straight_length", 80.0);
                d.curveRadius = num("radius", 25.0);
                d.turn = turnFrom(str("turn", "right"));
                d.targetElevation = num("target_elevation");
                d.maxLegs = static_cast<int>(num("max_legs", 200));
                out.append(layoutDict(mine_design::declineLayout(d)));
            } else if (kind == "spiral") {
                out.append(layoutDict(mine_design::spiralDecline(pt("start"), num("azimuth_deg", 0.0), num("grade_percent", -12.5), num("radius", 25.0),
                                                                 turnFrom(str("turn", "right")), num("target_elevation"))));
            } else if (kind == "ramp") {
                mine_design::RampSpec r;
                r.start = pt("start");
                r.startAzimuthDeg = num("azimuth_deg", 0.0);
                r.targetElevation = num("target_elevation");
                r.gradePercent = num("grade_percent", -14.3);
                r.radius = num("radius", 20.0);
                r.turn = turnFrom(str("turn", "right"));
                r.straightLength = num("straight_length", 40.0);
                out.append(layoutDict(mine_design::rampLayout(r)));
            } else if (kind == "access") {
                out.append(layoutDict(mine_design::accessDriveLayout(pt("start"), num("azimuth_deg", 0.0), pt("target"), num("radius", 20.0),
                                                                     num("max_grade_percent", 15.0))));
            } else if (kind == "ore_drive") {
                mine_design::OreDriveSpec o;
                o.offset = num("offset", 5.0);
                o.side = sideFrom(str("side", "left"));
                o.gradePercent = num("grade_percent", 1.0);
                if (a.contains("start_elevation")) o.startElevation = a["start_elevation"].cast<double>();
                o.followContactElevation = a.contains("follow_contact_elevation") && a["follow_contact_elevation"].cast<bool>();
                out.append(layoutDict(mine_design::oreDriveLayout(toPoints(a["contact"].cast<Points>()), o)));
            } else if (kind == "crosscuts") {
                mine_design::CrosscutSpec c;
                if (a.contains("chainages")) c.chainages = a["chainages"].cast<std::vector<double>>();
                c.interval = num("interval", 25.0);
                c.firstChainage = num("first_chainage", 10.0);
                c.length = num("length", 30.0);
                c.side = sideFrom(str("side", "right"));
                c.angleDeg = num("angle_deg", 90.0);
                c.gradePercent = num("grade_percent", 1.0);
                c.startOffset = num("start_offset", 0.0);
                const auto& parent = developmentOf(s.get(), a["parent"].cast<std::string>());
                const auto* g = s.get().model().geometry.find(parent.geometryId);
                if (!g) throw py::value_error("The parent heading has no centreline");
                for (const auto& l : mine_design::crosscutLayouts(g->vertices, g->bulges, c)) out.append(layoutDict(l));
            } else if (kind == "vertical") {
                std::vector<mine_design::Vpi> vpis;
                for (const auto& v : a["vpis"]) {
                    const auto t = v.cast<std::vector<double>>();
                    if (t.size() < 2) throw py::value_error("Each VPI is (chainage, elevation[, curve_length])");
                    vpis.push_back({t[0], t[1], t.size() > 2 ? t[2] : 0.0});
                }
                std::vector<double> bulges = a.contains("bulges") ? a["bulges"].cast<std::vector<double>>() : std::vector<double>{};
                out.append(layoutDict(mine_design::verticalAlignment(toPoints(a["points"].cast<Points>()), bulges, vpis, num("step", 1.0))));
            } else {
                throw py::value_error("Unknown layout '" + kind + "' (horizontal, decline, spiral, ramp, access, ore_drive, crosscuts, vertical)");
            }
            return out;
        }, py::arg("kind"), py::arg("parameters"))
        .def("create_headings", [](PyProject& s, const py::list& layouts, const py::dict& spec, const std::string& layer, const std::string& prefix) {
            std::vector<mine_design::LayoutResult> ls;
            for (const auto& l : layouts) ls.push_back(layoutFrom(l.cast<py::dict>()));
            if (ls.empty()) throw py::value_error("No layouts to create");
            std::vector<std::string> out;
            for (const auto& id : mine_design::createHeadings(s.get(), ls, developmentSpec(s.get(), spec), layerArg(s.get(), layer), prefix)) out.push_back(id.toString());
            return out;
        }, py::arg("layouts"), py::arg("template"), py::arg("layer") = "0", py::arg("prefix") = "")
        .def("create_crosscuts", [](PyProject& s, const std::string& parent, const py::dict& a, const std::string& layer) {
            mine_design::CrosscutSpec c;
            if (a.contains("chainages")) c.chainages = a["chainages"].cast<std::vector<double>>();
            if (a.contains("interval")) c.interval = a["interval"].cast<double>();
            if (a.contains("first_chainage")) c.firstChainage = a["first_chainage"].cast<double>();
            if (a.contains("length")) c.length = a["length"].cast<double>();
            if (a.contains("side")) c.side = sideFrom(a["side"].cast<std::string>());
            if (a.contains("angle_deg")) c.angleDeg = a["angle_deg"].cast<double>();
            if (a.contains("grade_percent")) c.gradePercent = a["grade_percent"].cast<double>();
            if (a.contains("start_offset")) c.startOffset = a["start_offset"].cast<double>();
            std::vector<std::string> out;
            for (const auto& id : mine_design::createCrosscuts(s.get(), uuidArg(parent), c, layerArg(s.get(), layer))) out.push_back(id.toString());
            return out;
        }, py::arg("parent"), py::arg("spec") = py::dict(), py::arg("layer") = "0")
        .def("gradient_profile", [](PyProject& s, const std::string& geometryId) {
            const auto& g = geometryOf(s.get(), geometryId);
            py::list out;
            for (const auto& seg : mine_design::gradientProfile(g.vertices, g.bulges)) {
                py::dict d;
                d["segment"] = seg.index;
                d["chainage_from"] = seg.chainageFrom;
                d["chainage_to"] = seg.chainageTo;
                d["plan_length"] = seg.planLength;
                d["rise"] = seg.rise;
                d["grade_percent"] = seg.gradePercent;
                d["radius"] = seg.radius;
                d["one_in"] = seg.oneIn();
                out.append(d);
            }
            return out;
        }, py::arg("geometry"))
        .def("check_design_rules", [](PyProject& s, const std::string& geometryId, const py::dict& r) {
            mine_design::DesignRules rules;
            if (r.contains("max_grade_percent")) rules.maxGradePercent = r["max_grade_percent"].cast<double>();
            if (r.contains("max_grade_on_curves_percent")) rules.maxGradeOnCurvesPercent = r["max_grade_on_curves_percent"].cast<double>();
            if (r.contains("min_radius")) rules.minRadius = r["min_radius"].cast<double>();
            if (r.contains("max_grade_change_percent")) rules.maxGradeChangePercent = r["max_grade_change_percent"].cast<double>();
            const auto& g = geometryOf(s.get(), geometryId);
            py::list out;
            for (const auto& v : mine_design::checkDesignRules(g.vertices, g.bulges, rules)) {
                py::dict d;
                d["chainage"] = v.chainage;
                d["rule"] = v.rule;
                d["value"] = v.value;
                d["limit"] = v.limit;
                out.append(d);
            }
            return out;
        }, py::arg("geometry"), py::arg("rules") = py::dict())
        .def("grade_curve", [](PyProject& s, const std::string& geometryId, std::optional<double> startElevation, std::optional<double> grade) {
            GeometryObject g = geometryOf(s.get(), geometryId);
            if (grade) g.vertices = mine_design::applyGrade(g.vertices, g.bulges, startElevation.value_or(g.vertices.empty() ? 0.0 : g.vertices.front().z), *grade);
            else g.vertices = mine_design::gradeBetweenEnds(g.vertices, g.bulges);
            return mine_design::updateGeometryWithImpact(s.get(), g, "Grade " + g.name); // flagged activities
        }, py::arg("geometry"), py::arg("start_elevation") = py::none(), py::arg("grade_percent") = py::none())
        .def("design_impacts", [](PyProject& s) {
            py::list out;
            for (const auto& i : mine_design::detectDesignImpacts(s.get())) {
                py::dict d;
                d["development"] = i.developmentId.toString();
                d["heading_id"] = i.headingId;
                d["approved_length"] = i.approvedLength;
                d["current_length"] = i.currentLength;
                std::vector<std::string> acts;
                for (const auto& a : i.activityIds) acts.push_back(a.toString());
                d["activities"] = acts;
                out.append(d);
            }
            return out;
        })
        .def("apply_design_quantities", [](PyProject& s, const std::string& activity) { mine_design::applyDesignQuantities(s.get(), uuidArg(activity)); },
             py::arg("activity"))
        .def("development_quantities", [](PyProject& s) {
            py::list out;
            for (const auto& q : mine_design::developmentQuantities(s.get())) {
                py::dict d;
                d["heading_id"] = q.headingId;
                d["level"] = q.level;
                d["zone"] = q.zone;
                d["activity_type"] = q.activityType;
                d["status"] = q.status;
                d["profile"] = q.profile;
                d["material"] = q.material;
                d["length"] = q.length;
                d["plan_length"] = q.planLength;
                d["gradient_percent"] = q.gradient;
                d["section_area"] = q.area;
                d["volume"] = q.volume;
                d["tonnes"] = q.tonnes;
                d["loose_volume"] = q.looseVolume;
                d["scheduled_quantity"] = q.scheduledQuantity;
                d["schedule_matches_design"] = q.scheduleMatchesDesign;
                out.append(d);
            }
            return out;
        })
        // levels
        .def("add_level", [](PyProject& s, const std::string& name, const std::string& code, double elevation, const std::string& zone) {
            MineLevel l;
            l.name = name.empty() ? (code.empty() ? "Level " + std::to_string(static_cast<int>(elevation)) : code + " Level") : name;
            l.code = code;
            l.elevation = elevation;
            l.zone = zone;
            return ProjectEditor(s.get()).add(l).toString();
        }, py::arg("name") = "", py::arg("code") = "", py::arg("elevation") = 0.0, py::arg("zone") = "")
        .def("assign_developments_to_levels", [](PyProject& s, double tol) { return mine_design::assignDevelopmentsToLevels(s.get(), tol); },
             py::arg("tolerance") = 5.0)
        // stopes
        .def("create_stope", [](PyProject& s, const py::dict& spec, const std::optional<Points>& outline, std::optional<double> floorZ,
                                std::optional<double> backZ, const std::optional<std::string>& geometry) {
            mine_design::NewStope n;
            static const std::set<std::string> known{"name", "code", "level", "material", "method", "zone", "grade", "grade_unit", "dilution_percent",
                                                     "recovery_percent", "layer"};
            for (const auto& kv : spec)
                if (!known.count(kv.first.cast<std::string>())) throw py::value_error("Unknown stope field '" + kv.first.cast<std::string>() + "'");
            if (spec.contains("name")) n.name = spec["name"].cast<std::string>();
            if (spec.contains("code")) n.code = spec["code"].cast<std::string>();
            if (spec.contains("level")) {
                const auto lv = spec["level"].cast<std::string>();
                for (const auto& l : s.get().model().levels.all())
                    if (l.code == lv || l.name == lv || l.id.toString() == lv) n.levelId = l.id;
                if (n.levelId.isNil()) throw py::key_error("No level '" + lv + "'");
            }
            if (spec.contains("material")) n.materialId = uuidArg(spec["material"].cast<std::string>());
            else if (!s.get().model().materials.empty()) n.materialId = s.get().model().materials.all().front().id;
            if (spec.contains("method")) {
                const auto m = enumFromString<StopeMethod>(spec["method"].cast<std::string>());
                if (!m) throw py::value_error("Unknown stope method");
                n.method = *m;
            }
            if (spec.contains("zone")) n.zone = spec["zone"].cast<std::string>();
            if (spec.contains("grade")) n.grade = spec["grade"].cast<double>();
            if (spec.contains("grade_unit")) n.gradeUnit = spec["grade_unit"].cast<std::string>();
            if (spec.contains("dilution_percent")) n.dilutionPercent = spec["dilution_percent"].cast<double>();
            if (spec.contains("recovery_percent")) n.recoveryPercent = spec["recovery_percent"].cast<double>();
            if (spec.contains("layer")) n.layerId = layerArg(s.get(), spec["layer"].cast<std::string>());
            if (geometry) return mine_design::createStopeFromGeometry(s.get(), uuidArg(*geometry), n).toString();
            if (!outline || !floorZ || !backZ) throw py::value_error("A stope needs an outline with floor and back elevations, or a closed geometry");
            return mine_design::createStopeFromOutline(s.get(), planRing(*outline), *floorZ, *backZ, n).toString();
        }, py::arg("spec"), py::arg("outline") = py::none(), py::arg("floor") = py::none(), py::arg("back") = py::none(), py::arg("geometry") = py::none())
        .def("stope_metrics", [](PyProject& s, const std::string& id) {
            const auto* st = s.get().model().stopes.find(uuidArg(id));
            if (!st) throw py::key_error("No stope '" + id + "'");
            const auto m = mine_design::stopeMetrics(s.get(), *st);
            py::dict d;
            d["volume"] = m.volume;
            d["tonnes"] = m.tonnes;
            d["diluted_tonnes"] = m.dilutedTonnes;
            d["recovered_tonnes"] = m.recoveredTonnes;
            d["contained_metal"] = m.containedMetal;
            d["mined_grade"] = m.minedGrade;
            d["watertight"] = m.watertight;
            return d;
        }, py::arg("id"))
        .def("subdivide_stope", [](PyProject& s, const std::string& id, const Points& outline, double floorZ, double backZ, double strike, double panel,
                                   double lift) {
            std::vector<std::string> out;
            for (const auto& b : mine_design::subdivideStope(s.get(), uuidArg(id), planRing(outline), floorZ, backZ, strike, panel, lift)) out.push_back(b.toString());
            return out;
        }, py::arg("id"), py::arg("outline"), py::arg("floor"), py::arg("back"), py::arg("strike_azimuth_deg"), py::arg("panel_length"),
             py::arg("lift_height") = 0.0)
        // revisions
        .def("capture_revision", [](PyProject& s, const std::string& entity, const std::string& code, const std::string& comment) {
            return mine_design::captureRevision(s.get(), uuidArg(entity), code, comment).toString();
        }, py::arg("entity"), py::arg("code"), py::arg("comment") = "")
        .def("approve_revision", [](PyProject& s, const std::string& id) { mine_design::approveRevision(s.get(), uuidArg(id)); }, py::arg("id"))
        .def("restore_revision", [](PyProject& s, const std::string& id) { mine_design::restoreRevision(s.get(), uuidArg(id)); }, py::arg("id"))
        .def("compare_revisions", [](PyProject& s, const std::string& from, const std::optional<std::string>& to) {
            const auto c = mine_design::compareRevisions(s.get(), uuidArg(from), to ? std::optional<Uuid>(uuidArg(*to)) : std::nullopt);
            py::dict d;
            d["from"] = c.fromCode;
            d["to"] = c.toCode;
            d["length_change"] = c.lengthChange;
            d["volume_change"] = c.volumeChange;
            d["max_vertex_deviation"] = c.maxVertexDeviation;
            d["vertex_count_from"] = c.vertexCountFrom;
            d["vertex_count_to"] = c.vertexCountTo;
            d["attribute_changes"] = c.attributeChanges;
            return d;
        }, py::arg("from_revision"), py::arg("to_revision") = py::none());

    // ---- scheduling ----------------------------------------------------------------------------------------
    P.def("add_activity", [](PyProject& s, const py::dict& d) {
         ScheduleActivity a;
         a.name = d["name"].cast<std::string>();
         if (d.contains("wbs")) a.wbsCode = d["wbs"].cast<std::string>();
         if (d.contains("duration_hours")) a.durationHours = d["duration_hours"].cast<double>();
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
         if (d.contains("duration_days")) {
             // A "day" is the standard working hours of the activity's calendar (else the project's).
             const auto& m = s.get().model();
             const auto* cal = m.calendars.find(a.calendarId.isNil() ? m.info.defaultCalendarId : a.calendarId);
             if (!a.calendarId.isNil() && !cal) throw py::key_error("No calendar " + a.calendarId.toString());
             a.durationHours = d["duration_days"].cast<double>() * (cal ? cal->standardHoursPerDay : 24.0);
         }
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
            const auto calc = scheduling::calculateScenario(s.get(), scenarioArg(s.get(), scenario));
            auto d = scheduleDict(calc.result);
            if (calc.levelling) {
                py::dict l;
                l["delayed_activities"] = calc.levelling->delayedActivities;
                l["total_delay_hours"] = calc.levelling->totalDelayHours;
                l["makespan_increase_hours"] = calc.levelling->makespanIncreaseHours;
                d["levelling"] = l;
            }
            return d;
        }, py::arg("scenario") = py::none())
        .def("schedule_table", [](PyProject& s, const std::optional<std::string>& scenario, const std::optional<std::string>& baseline) {
            const auto calc = scheduling::calculateScenario(s.get(), scenarioArg(s.get(), scenario));
            const auto& in = calc.input;
            const auto& r = calc.result;
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
        .def("add_scenario", [](PyProject& s, const std::string& name, const std::vector<std::tuple<std::string, std::string, std::string>>& overrides,
                                const std::string& levelling) {
            Scenario sc;
            sc.name = name;
            for (const auto& [e, f, v] : overrides) sc.overrides.push_back({e.empty() ? Uuid{} : uuidArg(e), f, v});
            const auto lm = enumFromString<LevellingMode>(levelling);
            if (!lm) throw py::value_error("levelling must be None, Heuristic or Optimised");
            sc.levelling = *lm;
            return ProjectEditor(s.get()).add(sc).toString();
        }, py::arg("name"), py::arg("overrides") = std::vector<std::tuple<std::string, std::string, std::string>>{}, py::arg("levelling") = "None")
        // calendars
        .def("add_calendar", [](PyProject& s, const std::string& name, const std::string& kind, const py::list& shifts, const py::list& exceptions,
                                double hoursPerDay, bool makeDefault) {
            WorkCalendar c;
            c.name = name;
            const auto k = enumFromString<CalendarKind>(kind);
            if (!k) throw py::value_error("kind must be Project, Shift or Resource");
            c.kind = *k;
            static const char* days[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
            auto minutes = [](const std::string& hhmm) {
                const auto colon = hhmm.find(':');
                if (colon == std::string::npos) throw py::value_error("Times are \"HH:MM\", got '" + hhmm + "'");
                const int h = std::stoi(hhmm.substr(0, colon)), m = std::stoi(hhmm.substr(colon + 1));
                if (h < 0 || h > 24 || m < 0 || m > 59 || (h == 24 && m)) throw py::value_error("Invalid time '" + hhmm + "'");
                return h * 60 + m;
            };
            for (const auto& item : shifts) {
                const auto d = item.cast<py::dict>();
                ShiftDefinition sh;
                sh.name = d.contains("name") ? d["name"].cast<std::string>() : "Shift";
                sh.startMinute = minutes(d["start"].cast<std::string>());
                sh.durationMinutes = static_cast<int>(std::lround(d["hours"].cast<double>() * 60));
                if (sh.durationMinutes <= 0 || sh.durationMinutes > 1440) throw py::value_error("A shift lasts more than 0 and at most 24 hours");
                if (d.contains("days")) {
                    sh.weekdays = 0;
                    for (const auto& dn : d["days"].cast<std::vector<std::string>>()) {
                        std::string l = dn.substr(0, 3);
                        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        const auto it = std::find(std::begin(days), std::end(days), l);
                        if (it == std::end(days)) throw py::value_error("Unknown weekday '" + dn + "'");
                        sh.weekdays |= static_cast<std::uint8_t>(1u << (it - std::begin(days))); // bit 0 = Monday
                    }
                } else {
                    sh.weekdays = 0x7F;
                }
                c.shifts.push_back(sh);
            }
            for (const auto& item : exceptions) {
                const auto d = item.cast<py::dict>();
                CalendarException e;
                e.date = dateArg(d["date"].cast<std::string>());
                e.reason = d.contains("reason") ? d["reason"].cast<std::string>() : "";
                if (d.contains("periods"))
                    for (const auto& per : d["periods"].cast<std::vector<std::pair<std::string, std::string>>>()) e.periods.push_back({minutes(per.first), minutes(per.second)});
                c.exceptions.push_back(e);
            }
            c.standardHoursPerDay = hoursPerDay;
            auto& p = s.get();
            CommandStack::MacroScope macro(p.commands(), "Add calendar " + name);
            ProjectEditor ed(p);
            const Uuid id = ed.addCalendar(c);
            if (makeDefault) {
                ProjectInfo info = p.model().info;
                info.defaultCalendarId = id;
                ed.setProjectInfo(info, "Set the project calendar");
            }
            macro.commit();
            return id.toString();
        }, py::arg("name"), py::arg("kind") = "Shift", py::arg("shifts") = py::list(), py::arg("exceptions") = py::list(), py::arg("hours_per_day") = 24.0,
             py::arg("make_default") = false)
        .def("set_activity_calendar", [](PyProject& s, const std::string& activity, const std::optional<std::string>& calendar) {
            auto* a = s.get().model().activities.find(uuidArg(activity));
            if (!a) throw py::key_error("No activity '" + activity + "'");
            ScheduleActivity u = *a;
            u.calendarId = calendar ? uuidArg(*calendar) : Uuid{};
            if (calendar && !s.get().model().calendars.find(u.calendarId)) throw py::key_error("No calendar '" + *calendar + "'");
            ProjectEditor(s.get()).update(u, "Set activity calendar");
        }, py::arg("activity"), py::arg("calendar") = py::none())
        // development cycles
        .def("add_cycle", [](PyProject& s, const std::string& name, const py::dict& f) {
            DevelopmentCycle c;
            c.name = name;
            std::map<std::string, double*> reals{{"round_length", &c.roundLength}, {"advance_efficiency", &c.advanceEfficiency},
                                                 {"drill_penetration_rate", &c.drillPenetrationRate}, {"drill_setup_minutes", &c.drillSetupMinutes},
                                                 {"collar_minutes_per_hole", &c.collarMinutesPerHole}, {"charge_minutes_per_hole", &c.chargeMinutesPerHole},
                                                 {"blast_and_reentry_minutes", &c.blastAndReentryMinutes}, {"swell_factor", &c.swellFactor},
                                                 {"bucket_capacity", &c.bucketCapacity}, {"bucket_fill_factor", &c.bucketFillFactor},
                                                 {"muck_cycle_minutes", &c.muckCycleMinutes}, {"minutes_per_bolt", &c.minutesPerBolt},
                                                 {"mesh_minutes", &c.meshMinutes}, {"shotcrete_minutes", &c.shotcreteMinutes},
                                                 {"scaling_minutes", &c.scalingMinutes}, {"survey_markup_minutes", &c.surveyMarkupMinutes},
                                                 {"service_minutes", &c.serviceMinutes}, {"utilisation", &c.utilisation}, {"shift_hours", &c.shiftHours}};
            std::map<std::string, int*> ints{{"holes_per_round", &c.holesPerRound}, {"drill_booms", &c.drillBooms}, {"bolts_per_round", &c.boltsPerRound},
                                             {"shifts_per_day", &c.shiftsPerDay}};
            for (const auto& kv : f) {
                const auto k = kv.first.cast<std::string>();
                if (auto r = reals.find(k); r != reals.end()) *r->second = kv.second.cast<double>();
                else if (auto i = ints.find(k); i != ints.end()) *i->second = kv.second.cast<int>();
                else if (k == "notes") c.notes = kv.second.cast<std::string>();
                else throw py::value_error("Unknown cycle field '" + k + "'");
            }
            return ProjectEditor(s.get()).add(c, "Add development cycle " + name).toString();
        }, py::arg("name"), py::arg("fields") = py::dict())
        .def("evaluate_cycle", [](PyProject& s, const std::string& cycle, double width, double height, const std::optional<std::string>& support) {
            const auto* c = s.get().model().cycles.find(uuidArg(cycle));
            if (!c) throw py::key_error("No development cycle '" + cycle + "'");
            const GroundSupportClass* gs = nullptr;
            if (support) {
                gs = s.get().model().supportClasses.find(uuidArg(*support));
                if (!gs) throw py::key_error("No ground support class '" + *support + "'");
            }
            const auto b = scheduling::evaluateCycle(*c, width, height, gs);
            py::dict d;
            for (const auto& [k, v] : b.items()) d[py::str(k)] = v;
            d["drilling_minutes"] = b.drillingMinutes;
            d["charging_minutes"] = b.chargingMinutes;
            d["blasting_minutes"] = b.blastingMinutes;
            d["mucking_minutes"] = b.muckingMinutes;
            d["support_minutes"] = b.supportMinutes;
            d["other_minutes"] = b.otherMinutes;
            d["total_minutes"] = b.totalMinutes;
            d["advance_per_cycle"] = b.advancePerCycle;
            d["muck_volume_loose"] = b.muckVolumeLoose;
            d["bucket_loads"] = b.bucketLoads;
            d["bolts"] = b.bolts;
            d["cycles_per_shift"] = b.cyclesPerShift;
            d["metres_per_shift"] = b.metresPerShift;
            d["metres_per_day"] = b.metresPerDay;
            d["metres_per_work_hour"] = b.metresPerWorkHour;
            return d;
        }, py::arg("cycle"), py::arg("width"), py::arg("height"), py::arg("support_class") = py::none())
        .def("use_cycle", [](PyProject& s, const std::string& activity, const std::optional<std::string>& cycle) {
            auto* a = s.get().model().activities.find(uuidArg(activity));
            if (!a) throw py::key_error("No activity '" + activity + "'");
            ScheduleActivity u = *a;
            if (cycle) {
                if (!s.get().model().cycles.find(uuidArg(*cycle))) throw py::key_error("No development cycle '" + *cycle + "'");
                u.productionModelId = uuidArg(*cycle);
                u.useCycleRate = true;
                u.durationFromRate = true;
            } else {
                u.productionModelId = {};
                u.useCycleRate = false;
            }
            ProjectEditor(s.get()).update(u, cycle ? "Use development cycle" : "Stop using the development cycle");
        }, py::arg("activity"), py::arg("cycle") = py::none())
        // targets and production by period
        .def("add_target", [](PyProject& s, const std::string& name, const std::string& from, const std::string& to, double quantity, const std::string& unit,
                              const std::string& zone, const std::string& level) {
            ProductionTarget t;
            t.name = name;
            t.periodStart = dateArg(from);
            t.periodEnd = dateArg(to);
            t.quantity = quantity;
            t.unit = unit;
            t.scopeZone = zone;
            t.scopeLevel = level;
            return ProjectEditor(s.get()).add(t, "Add target " + name).toString();
        }, py::arg("name"), py::arg("start"), py::arg("end"), py::arg("quantity"), py::arg("unit") = "m", py::arg("zone") = "", py::arg("level") = "")
        .def("production_by_period", [](PyProject& s, const std::string& kind, const std::optional<std::string>& scenario, const std::string& unit,
                                        const std::string& zone, const std::string& level) {
            const auto k = kind == "day" ? scheduling::PeriodKind::Day : kind == "week" ? scheduling::PeriodKind::Week : kind == "month" ? scheduling::PeriodKind::Month
                           : kind == "quarter" ? scheduling::PeriodKind::Quarter : kind == "year" ? scheduling::PeriodKind::Year
                                                                                  : throw py::value_error("period must be day, week, month, quarter or year");
            const auto calc = scheduling::calculateScenario(s.get(), scenarioArg(s.get(), scenario));
            if (!calc.result.ok) throw py::value_error("The schedule cannot be calculated: " + (calc.result.errors.empty() ? std::string() : calc.result.errors.front()));
            scheduling::QuantityFilter f;
            f.unit = unit;
            f.zone = zone;
            f.level = level;
            py::list out;
            for (const auto& q : scheduling::quantitiesByPeriod(calc.input, calc.result, k, f)) {
                py::dict d;
                d["period"] = q.period.label;
                d["start"] = q.period.start.toString();
                d["end"] = q.period.end.toString();
                d["scheduled"] = q.scheduled;
                d["actual"] = q.actual;
                d["target"] = q.target;
                out.append(d);
            }
            return out;
        }, py::arg("period") = "month", py::arg("scenario") = py::none(), py::arg("unit") = "m", py::arg("zone") = "", py::arg("level") = "")
        .def("resource_profiles", [](PyProject& s, const std::optional<std::string>& scenario) {
            const auto calc = scheduling::calculateScenario(s.get(), scenarioArg(s.get(), scenario));
            if (!calc.result.ok) throw py::value_error("The schedule cannot be calculated");
            py::list out;
            for (const auto& r : scheduling::resourceProfiles(calc.input, calc.result)) {
                py::dict d;
                d["resource"] = r.resourceId.toString();
                d["name"] = r.name;
                d["capacity"] = r.capacity;
                d["peak"] = r.peak;
                d["overallocated"] = r.overallocated;
                py::list steps;
                for (const auto& st : r.steps)
                    steps.append(py::make_tuple(scheduling::toDateTime(st.start).toString(), scheduling::toDateTime(st.end).toString(), st.units));
                d["usage"] = steps;
                py::list spans;
                for (const auto& [a, b] : r.overallocatedSpans) spans.append(py::make_tuple(scheduling::toDateTime(a).toString(), scheduling::toDateTime(b).toString()));
                d["overallocated_spans"] = spans;
                out.append(d);
            }
            return out;
        }, py::arg("scenario") = py::none())
        .def("export_gantt_pdf", [](PyProject& s, const fs::path& file, const std::optional<std::string>& scenario, const std::string& title,
                                    const std::optional<std::string>& baseline) {
            const auto calc = scheduling::calculateScenario(s.get(), scenarioArg(s.get(), scenario));
            const auto& in = calc.input;
            const auto& r = calc.result;
            if (!r.ok) throw py::value_error("The schedule cannot be calculated");
            reporting::GanttOptions o;
            o.title = title;
            const ScheduleBaseline* b = baseline ? s.get().model().baselines.find(uuidArg(*baseline)) : nullptr;
            return reporting::writeGanttPdf(s.get(), in, r, b, file, o);
        }, py::arg("file"), py::arg("scenario") = py::none(), py::arg("title") = "", py::arg("baseline") = py::none())
        .def("export_schedule_xlsx", [](PyProject& s, const fs::path& file, const std::optional<std::string>& scenario) {
            const auto calc = scheduling::calculateScenario(s.get(), scenarioArg(s.get(), scenario));
            reporting::writeScheduleXlsx(s.get(), scheduling::scheduleTable(s.get(), calc.input, calc.result), file);
        }, py::arg("file"), py::arg("scenario") = py::none())
        .def("plan_snapshot", [](PyProject& s, const std::string& at, const std::optional<std::string>& baseline, const std::optional<std::string>& scenario) {
            const auto calc = scheduling::calculateScenario(s.get(), scenarioArg(s.get(), scenario));
            const auto& in = calc.input;
            const auto& r = calc.result;
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
        .def("point_cloud_crs_warning", [](PyProject& s, const std::string& id) -> std::optional<std::string> {
            const auto* d = s.get().model().pointClouds.find(uuidArg(id));
            if (!d) throw py::key_error("No point cloud " + id);
            return survey::crsMismatch(s.get(), *d);
        })
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
        .def("conformance", [](PyProject& s, const std::string& asBuilt, const std::string& dev, double interval, bool commit, double maxGap) {
            survey::ConformanceOptions o;
            o.interval = interval;
            if (maxGap < 0) throw py::value_error("max_gap must not be negative");
            o.maxGap = maxGap;
            survey::ConformanceReport r;
            {
                py::gil_scoped_release rel;
                r = survey::analyseAsBuilt(s.get(), uuidArg(asBuilt), uuidArg(dev), o);
            }
            auto d = conformanceDict(r);
            if (commit) d["result"] = survey::commitConformance(s.get(), uuidArg(asBuilt), uuidArg(dev), r, o).toString();
            return d;
        }, py::arg("as_built"), py::arg("development"), py::arg("interval") = 1.0, py::arg("commit") = false, py::arg("max_gap") = 0.5)
        .def("derive_point_cloud", [progressOr](PyProject& s, const std::string& id, const std::string& op, const py::dict& prm, const std::string& engine,
                                                const std::string& name, jobs::Progress* progress) {
            const Uuid parent = uuidArg(id);
            if (!s.get().model().pointClouds.find(parent)) throw py::key_error("No point cloud '" + id + "'");
            const bool open3d = engine == "open3d" || engine == "Open3D";
            if (!open3d && engine != "openmine" && engine != "OpenMine") throw py::value_error("Unknown engine '" + engine + "' (use \"openmine\" or \"open3d\")");
#ifndef OPENMINE_PY_HAVE_OPEN3D
            if (open3d) throw py::value_error("This build has no Open3D engine");
#endif
            static const std::set<std::string> known{"k", "std_ratio", "radius", "min_neighbours", "voxel", "spacing", "count", "seed", "orientation",
                                                     "viewpoint", "box", "inside", "curvature", "tolerance", "min_size", "angle_deg", "drop_unclustered",
                                                     "polygon", "view_normal", "reference", "max_distance", "signed", "cell", "slope", "window", "threshold"};
            for (const auto& kv : prm)
                if (!known.count(kv.first.cast<std::string>())) throw py::value_error("Unknown parameter '" + kv.first.cast<std::string>() + "'");
            auto num = [&](const char* key, double def) { return prm.contains(key) ? prm[key].cast<double>() : def; };
            auto integer = [&](const char* key, int def) { return prm.contains(key) ? prm[key].cast<int>() : def; };
            auto flag = [&](const char* key, bool def) { return prm.contains(key) ? prm[key].cast<bool>() : def; };
            auto noEngine = [&] {
                if (open3d) throw py::value_error("Operation '" + op + "' has no Open3D implementation");
            };
            survey::Derivation d;
            std::shared_ptr<survey::DistanceStatisticsJson> stats;
            std::optional<std::pair<Uuid, bool>> distanceRef; // reference id, is a cloud
            if (op == "statistical_outliers") {
                survey::OutlierEngine e;
#ifdef OPENMINE_PY_HAVE_OPEN3D
                if (open3d) e = o3d::outlierEngine();
#endif
                d = survey::statisticalOutlierDerivation(integer("k", 16), num("std_ratio", 2.0), e);
            } else if (op == "radius_outliers") {
                survey::RadiusOutlierEngine e;
#ifdef OPENMINE_PY_HAVE_OPEN3D
                if (open3d) e = o3d::radiusOutlierEngine();
#endif
                d = survey::radiusOutlierDerivation(num("radius", 0.0), integer("min_neighbours", 4), e);
            } else if (op == "voxel" || op == "voxel_centroid") {
                survey::VoxelEngine e;
#ifdef OPENMINE_PY_HAVE_OPEN3D
                if (open3d) e = o3d::voxelEngine();
#endif
                d = survey::voxelDerivation(num("voxel", 0.0), op == "voxel" ? survey::VoxelMode::NearestToCentre : survey::VoxelMode::Centroid, e);
            } else if (op == "minimum_spacing") {
                noEngine();
                d = survey::minimumSpacingDerivation(num("spacing", 0.0));
            } else if (op == "random") {
                noEngine();
                const int count = integer("count", 0);
                if (count < 1) throw py::value_error("count must be at least 1");
                d = survey::randomSubsampleDerivation(static_cast<std::size_t>(count), static_cast<unsigned>(integer("seed", 1)));
            } else if (op == "normals") {
                survey::NormalOptions no;
                no.k = integer("k", 16);
                no.radius = num("radius", 0.0);
                no.storeCurvature = flag("curvature", true);
                const std::string orient = prm.contains("orientation") ? prm["orientation"].cast<std::string>() : "up";
                if (orient == "up") no.orientation = survey::NormalOptions::Orientation::Up;
                else if (orient == "towards_viewpoint") no.orientation = survey::NormalOptions::Orientation::TowardsViewpoint;
                else if (orient == "away_from_viewpoint") no.orientation = survey::NormalOptions::Orientation::AwayFromViewpoint;
                else throw py::value_error("orientation must be \"up\", \"towards_viewpoint\" or \"away_from_viewpoint\"");
                if (prm.contains("viewpoint")) no.viewpoint = vec(prm["viewpoint"].cast<std::array<double, 3>>());
                survey::NormalsEngine e;
#ifdef OPENMINE_PY_HAVE_OPEN3D
                if (open3d) e = o3d::normalsEngine();
#endif
                d = survey::normalsDerivation(no, e);
            } else if (op == "clusters") {
                noEngine();
                d = survey::euclideanClusterDerivation(num("tolerance", 0.0), static_cast<std::size_t>(integer("min_size", 1)), flag("drop_unclustered", false));
            } else if (op == "regions") {
                noEngine();
                d = survey::regionGrowingDerivation(num("angle_deg", 5.0), static_cast<std::size_t>(integer("min_size", 50)), flag("drop_unclustered", false));
            } else if (op == "crop_box") {
                noEngine();
                if (!prm.contains("box")) throw py::value_error("crop_box needs box=(xmin, ymin, zmin, xmax, ymax, zmax)");
                const auto a = prm["box"].cast<std::array<double, 6>>();
                d = survey::cropBoxDerivation(BoundingBox{{a[0], a[1], a[2]}, {a[3], a[4], a[5]}}, flag("inside", true));
            } else if (op == "crop_polygon") {
                noEngine();
                if (!prm.contains("polygon")) throw py::value_error("crop_polygon needs polygon=[(x, y, z), …]");
                d = survey::cropPolygonDerivation(toPoints(prm["polygon"].cast<Points>()),
                                                  prm.contains("view_normal") ? vec(prm["view_normal"].cast<std::array<double, 3>>()) : Vec3{0, 0, 1}, flag("inside", true));
            } else if (op == "ground") {
                noEngine();
#ifdef OPENMINE_PY_HAVE_PDAL
                pdalio::SmrfOptions so;
                so.cell = num("cell", so.cell);
                so.slope = num("slope", so.slope);
                so.window = num("window", so.window);
                so.threshold = num("threshold", so.threshold);
                d = pdalio::groundDerivation(so);
#else
                throw py::value_error("Ground classification needs PDAL, which this build does not include");
#endif
            } else if (op == "distance") {
                noEngine();
                if (!prm.contains("reference")) throw py::value_error("distance needs reference= (a point cloud or a mesh/solid id)");
                const Uuid ref = uuidArg(prm["reference"].cast<std::string>());
                stats = std::make_shared<survey::DistanceStatisticsJson>();
                const double maxD = num("max_distance", 0.0);
                const bool sign = flag("signed", false);
                if (const auto* rc = s.get().model().pointClouds.find(ref)) {
                    d = survey::cloudDistanceDerivation(survey::datasetPoints(s.get(), ref, true), rc->name, maxD, sign, stats);
                    distanceRef = {ref, true};
                } else {
                    const auto* g = s.get().model().geometry.find(ref);
                    if (!g || !g->mesh) throw py::key_error("The reference is neither a point cloud nor a mesh");
                    d = survey::meshDistanceDerivation(g->mesh, g->name, maxD, sign, stats);
                    distanceRef = {ref, false};
                }
            } else {
                throw py::value_error("Unknown operation '" + op +
                                      "' (statistical_outliers, radius_outliers, voxel, voxel_centroid, minimum_spacing, random, normals, clusters, regions, "
                                      "crop_box, crop_polygon, ground, distance)");
            }
            auto& p = s.get();
            CommandStack::MacroScope macro(p.commands(), "Derive point cloud");
            survey::DerivedDataset r;
            {
                py::gil_scoped_release rel;
                r = survey::deriveDataset(p, parent, d, name, progressOr(progress));
            }
            py::dict out;
            if (distanceRef && stats && !stats->json.empty()) {
                // The distance analysis is recorded like the desktop's (inputs, parameters, statistics).
                const nlohmann::json in{{"compared", parent.toString()}, {"reference", distanceRef->first.toString()}};
                const double maxD = num("max_distance", 0.0);
                const nlohmann::json pa{{"maxDistance", maxD > 0 ? nlohmann::json(maxD) : nlohmann::json(nullptr)}, {"signed", flag("signed", false)}};
                out["analysis"] = ProjectEditor(p).addAnalysis(distanceRef->second ? AnalysisKind::DistanceCloudToCloud : AnalysisKind::DistanceCloudToMesh,
                                                               "Distance to " + d.text.substr(12), in.dump(), pa.dump(), stats->json).toString();
                out["statistics"] = py::module_::import("json").attr("loads")(stats->json);
            }
            macro.commit();
            out["id"] = r.id.toString();
            out["input_count"] = r.inputCount;
            out["output_count"] = r.outputCount;
            out["derivation"] = r.derivation;
            out["message"] = r.message;
            return out;
        }, py::arg("id"), py::arg("operation"), py::arg("params") = py::dict(), py::arg("engine") = "openmine", py::arg("name") = "",
             py::arg("progress") = nullptr)
        .def("surface_tool", [progressOr](PyProject& s, const std::string& id, const std::string& op, const py::dict& a, const std::optional<std::string>& reference,
                                          const std::string& name, jobs::Progress* progress) {
            survey::SurfaceOperation so;
            if (op == "decimate") so = survey::SurfaceOperation::Decimate;
            else if (op == "smooth") so = survey::SurfaceOperation::Smooth;
            else if (op == "repair") so = survey::SurfaceOperation::Repair;
            else if (op == "compare") so = survey::SurfaceOperation::Compare;
            else throw py::value_error("operation must be decimate, smooth, repair or compare");
            survey::SurfaceToolOptions o;
            static const std::set<std::string> known{"target_triangles", "max_error", "preserve_boundary", "iterations", "lambda", "mu", "fix_boundary",
                                                     "merge_tolerance", "fill_holes_up_to", "orient_outward", "signed", "max_distance"};
            for (const auto& kv : a)
                if (!known.count(kv.first.cast<std::string>())) throw py::value_error("Unknown parameter '" + kv.first.cast<std::string>() + "'");
            if (a.contains("target_triangles")) o.targetTriangles = a["target_triangles"].cast<std::size_t>();
            if (a.contains("max_error")) o.maxError = a["max_error"].cast<double>();
            if (a.contains("preserve_boundary")) o.preserveBoundary = a["preserve_boundary"].cast<bool>();
            if (a.contains("iterations")) o.iterations = a["iterations"].cast<int>();
            if (a.contains("lambda")) o.lambda = a["lambda"].cast<double>();
            if (a.contains("mu")) o.mu = a["mu"].cast<double>();
            if (a.contains("fix_boundary")) o.fixBoundary = a["fix_boundary"].cast<bool>();
            if (a.contains("merge_tolerance")) o.mergeTolerance = a["merge_tolerance"].cast<double>();
            if (a.contains("fill_holes_up_to")) o.fillHolesUpTo = a["fill_holes_up_to"].cast<std::size_t>();
            if (a.contains("orient_outward")) o.orientOutward = a["orient_outward"].cast<bool>();
            if (a.contains("signed")) o.signedDistance = a["signed"].cast<bool>();
            if (a.contains("max_distance")) o.maxDistance = a["max_distance"].cast<double>();
            const auto src = survey::surfaceOf(s.get(), uuidArg(id));
            MeshPtr ref = reference ? survey::surfaceOf(s.get(), uuidArg(*reference)) : nullptr;
            survey::SurfaceToolResult r;
            {
                py::gil_scoped_release rel;
                r = survey::runSurfaceTool(*src, so, o, ref.get(), progressOr(progress));
            }
            py::dict d;
            d["id"] = survey::addDerivedSurface(s.get(), uuidArg(id), r, name).toString();
            d["method"] = r.method;
            d["summary"] = r.summary;
            d["triangles"] = r.mesh.triangles.size();
            if (so == survey::SurfaceOperation::Repair) {
                py::dict rep;
                rep["merged_vertices"] = r.repair.mergedVertices;
                rep["degenerate_removed"] = r.repair.degenerateRemoved;
                rep["duplicates_removed"] = r.repair.duplicatesRemoved;
                rep["flipped"] = r.repair.flipped;
                rep["holes_filled"] = r.repair.holesFilled;
                rep["non_manifold_edges"] = r.repair.nonManifoldEdges;
                rep["boundary_loops"] = r.repair.boundaryLoops;
                d["repair"] = rep;
            }
            if (so == survey::SurfaceOperation::Compare) {
                py::dict st;
                st["vertices"] = r.distances.vertices;
                st["measured"] = r.distances.measured;
                st["mean"] = r.distances.mean;
                st["rms"] = r.distances.rms;
                st["min"] = r.distances.min;
                st["median"] = r.distances.median;
                st["max"] = r.distances.max;
                d["distances"] = st;
            }
            return d;
        }, py::arg("id"), py::arg("operation"), py::arg("params") = py::dict(), py::arg("reference") = py::none(), py::arg("name") = "",
             py::arg("progress") = nullptr)
        .def("surface_from_cloud", [progressOr](PyProject& s, const std::string& dataset, const std::string& method, const py::dict& a, const std::string& engine,
                                                const std::string& name, const std::string& layer, jobs::Progress* progress) {
            survey::CloudSurfaceSpec spec;
            if (method == "delaunay") spec.method = survey::CloudSurfaceSpec::Method::Delaunay;
            else if (method == "poisson") spec.method = survey::CloudSurfaceSpec::Method::Poisson;
            else throw py::value_error("method must be 'delaunay' or 'poisson'");
            static const std::set<std::string> known{"voxel", "max_edge_length", "target_triangles", "depth", "orient_towards_centroid"};
            for (const auto& kv : a)
                if (!known.count(kv.first.cast<std::string>())) throw py::value_error("Unknown parameter '" + kv.first.cast<std::string>() + "'");
            if (a.contains("voxel")) spec.voxel = a["voxel"].cast<double>();
            if (a.contains("max_edge_length")) spec.maxEdgeLength = a["max_edge_length"].cast<double>();
            if (a.contains("target_triangles")) spec.targetTriangles = a["target_triangles"].cast<std::size_t>();
            if (a.contains("depth")) spec.depth = a["depth"].cast<int>();
            if (a.contains("orient_towards_centroid")) spec.orientTowardsCentroid = a["orient_towards_centroid"].cast<bool>();
            survey::PoissonEngine pe;
            if (spec.method == survey::CloudSurfaceSpec::Method::Poisson) {
                std::string eng = engine;
#if defined(OPENMINE_PY_HAVE_PDAL)
                if (eng.empty()) eng = "pdal";
#elif defined(OPENMINE_PY_HAVE_OPEN3D)
                if (eng.empty()) eng = "open3d";
#endif
                if (eng == "pdal") {
#ifdef OPENMINE_PY_HAVE_PDAL
                    pe = {[](const survey::PointBuffer& pts, int depth) {
                              pdalio::PoissonOptions po;
                              po.depth = depth;
                              return pdalio::poissonSurface(pts, po);
                          },
                          "PDAL " + pdalio::pdalVersion()};
#endif
                } else if (eng == "open3d") {
#ifdef OPENMINE_PY_HAVE_OPEN3D
                    pe = {[](const survey::PointBuffer& pts, int depth) {
                              o3d::PoissonOptions po;
                              po.depth = depth;
                              return o3d::poissonSurface(pts, po).mesh;
                          },
                          o3d::engineName()};
#endif
                } else if (!eng.empty()) {
                    throw py::value_error("engine must be 'pdal' or 'open3d'");
                }
                if (!pe.function) throw py::value_error("This build has no Poisson engine '" + eng + "'");
            }
            const Uuid ds = uuidArg(dataset);
            const auto* d = s.get().model().pointClouds.find(ds);
            if (!d) throw py::key_error("No point cloud '" + dataset + "'");
            survey::CloudSurface cs;
            {
                py::gil_scoped_release rel;
                auto& pr = progressOr(progress);
                pr.subRange(0, 0.3);
                auto pts = survey::datasetPoints(s.get(), ds, true, std::nullopt, pr);
                pr.subRange(0.3, 1.0);
                cs = survey::surfaceFromPoints(std::move(pts), spec, pe, pr);
            }
            const Uuid lay = ProjectEditor(s.get()).ensureLayer(layer, 0x9CCC65);
            py::dict out;
            out["triangles"] = cs.mesh.triangles.size();
            out["method"] = cs.method;
            out["id"] = survey::addSurfaceObject(s.get(), std::move(cs.mesh), name.empty() ? d->name + " surface" : name, lay, cs.method, ds).toString();
            return out;
        }, py::arg("dataset"), py::arg("method") = "delaunay", py::arg("params") = py::dict(), py::arg("engine") = "", py::arg("name") = "",
             py::arg("layer") = "Surfaces", py::arg("progress") = nullptr)
        .def("fit_planes", [](PyProject& s, const std::string& dataset, double threshold, int maxPlanes, std::size_t minInliers, bool store, unsigned seed) {
            const Uuid ds = uuidArg(dataset);
            const auto* d = s.get().model().pointClouds.find(ds);
            if (!d) throw py::key_error("No point cloud '" + dataset + "'");
            auto pts = survey::datasetPoints(s.get(), ds, true);
            if (pts.size() > 2'000'000) pts = survey::randomSubsample(pts, 2'000'000, seed);
            const auto planes = survey::ransacPlanes(pts, threshold, maxPlanes, minInliers, 1000, seed);
            py::list out;
            std::vector<StructuralMeasurement> ms;
            for (const auto& pl : planes) {
                const auto plane = geotechnical::planeOfPole(geotechnical::lineOf(pl.normal));
                py::dict x;
                x["normal"] = py::make_tuple(pl.normal.x, pl.normal.y, pl.normal.z);
                x["d"] = pl.d;
                x["centroid"] = py::make_tuple(pl.centroid.x, pl.centroid.y, pl.centroid.z);
                x["inliers"] = pl.inliers.size();
                x["rms"] = pl.rms;
                x["dip"] = plane.dip;
                x["dip_direction"] = plane.dipDirection;
                out.append(x);
                StructuralMeasurement m;
                m.setName = "Scan planes";
                m.type = StructureType::Joint;
                m.dip = plane.dip;
                m.dipDirection = plane.dipDirection;
                m.location = pl.centroid;
                m.developmentId = d->developmentId;
                ms.push_back(m);
            }
            if (store && !ms.empty()) {
                CommandStack::MacroScope macro(s.get().commands(), "Store scan planes as structures");
                ProjectEditor ed(s.get());
                for (auto& m : ms) ed.add(m);
                macro.commit();
            }
            return out;
        }, py::arg("dataset"), py::arg("threshold"), py::arg("max_planes") = 5, py::arg("min_inliers") = 100, py::arg("store") = false, py::arg("seed") = 1)
        .def("add_control_point", [](PyProject& s, const std::string& name, std::array<double, 3> xyz, const std::string& code, double hSigma, double vSigma,
                                     const std::string& role, const std::string& source) {
            SurveyControlPoint c;
            c.name = name;
            c.code = code;
            c.position = vec(xyz);
            c.horizontalSigma = hSigma;
            c.verticalSigma = vSigma;
            const auto r = enumFromString<ControlPointRole>(role);
            if (!r) throw py::value_error("role must be Control, Check, Temporary or ScanTarget");
            c.role = *r;
            c.source = source;
            return ProjectEditor(s.get()).add(c, "Add control point " + name).toString();
        }, py::arg("name"), py::arg("position"), py::arg("code") = "", py::arg("horizontal_sigma") = 0.005, py::arg("vertical_sigma") = 0.005,
             py::arg("role") = "Control", py::arg("source") = "Python")
        .def("pair_registration", [](PyProject& s, const std::string& dataset, const py::list& pairs, bool allowScale, bool apply) {
            std::vector<RegistrationPair> ps;
            bool control = false, plain = false;
            for (const auto& item : pairs) {
                const auto d = item.cast<py::dict>();
                RegistrationPair rp;
                rp.name = d.contains("name") ? d["name"].cast<std::string>() : "P" + std::to_string(ps.size() + 1);
                rp.source = vec(d["source"].cast<std::array<double, 3>>());
                if (d.contains("control")) {
                    const auto key = d["control"].cast<std::string>();
                    for (const auto& c : s.get().model().controlPoints.all())
                        if (c.name == key || c.id.toString() == key) rp.targetControlId = c.id;
                    if (rp.targetControlId.isNil()) throw py::key_error("No control point '" + key + "'");
                    control = true;
                } else {
                    rp.target = vec(d["target"].cast<std::array<double, 3>>());
                    plain = true;
                }
                if (d.contains("role")) {
                    const auto role = d["role"].cast<std::string>();
                    if (role == "check") rp.role = PairRole::Check;
                    else if (role != "fit") throw py::value_error("role must be 'fit' or 'check'");
                }
                ps.push_back(rp);
            }
            if (control && plain) throw py::value_error("Use either control points or explicit targets for all pairs");
            Registration r = control ? survey::computeControlRegistration(s.get(), uuidArg(dataset), ps, allowScale)
                                     : survey::computePairRegistration(s.get(), uuidArg(dataset), ps, allowScale);
            py::dict out;
            out["transform"] = r.transform.toString();
            out["fit_rms"] = r.fitRms;
            out["fit_max"] = r.fitMax;
            out["fit_count"] = r.fitCount;
            out["check_rms"] = r.checkCount ? py::object(py::float_(r.checkRms)) : py::none();
            out["check_max"] = r.checkCount ? py::object(py::float_(r.checkMax)) : py::none();
            py::list res;
            for (const auto& pr : r.pairs) {
                py::dict x;
                x["name"] = pr.name;
                x["role"] = pr.role == PairRole::Check ? "check" : "fit";
                x["residual"] = py::make_tuple(pr.residual.x, pr.residual.y, pr.residual.z);
                res.append(x);
            }
            out["pairs"] = res;
            out["registration"] = survey::commitRegistration(s.get(), r, apply).toString();
            return out;
        }, py::arg("dataset"), py::arg("pairs"), py::arg("allow_scale") = false, py::arg("apply") = false)
        .def("revert_registration", [](PyProject& s, const std::string& id) { survey::revertRegistration(s.get(), uuidArg(id)); }, py::arg("registration"))
        .def("build_as_built", [progressOr](PyProject& s, const std::string& dataset, const std::string& development, double corridor, double voxel, int depth,
                                            const std::string& engine, const std::string& name, const std::string& layer, std::size_t fillHoles,
                                            jobs::Progress* progress) {
            survey::AsBuiltOptions o;
            o.fillHolesUpTo = fillHoles;
            o.corridor = corridor;
            o.voxel = voxel;
            o.depth = depth;
            if (depth < 4 || depth > 14) throw py::value_error("depth must be between 4 and 14");
            if (voxel < 0) throw py::value_error("voxel must not be negative");
            std::string eng = engine;
            if (eng.empty()) {
#if defined(OPENMINE_PY_HAVE_PDAL)
                eng = "pdal";
#elif defined(OPENMINE_PY_HAVE_OPEN3D)
                eng = "open3d";
#else
                throw py::value_error("This build has no surface-reconstruction engine (PDAL or Open3D)");
#endif
            }
            survey::SurfaceEngine surface;
            std::string label;
            if (eng == "pdal") {
#ifdef OPENMINE_PY_HAVE_PDAL
                surface = [](const survey::PointBuffer& pts, int dep) {
                    pdalio::PoissonOptions po;
                    po.depth = dep;
                    return pdalio::poissonSurface(pts, po);
                };
                label = "PDAL " + pdalio::pdalVersion();
#endif
            } else if (eng == "open3d") {
#ifdef OPENMINE_PY_HAVE_OPEN3D
                surface = [](const survey::PointBuffer& pts, int dep) {
                    o3d::PoissonOptions po;
                    po.depth = dep;
                    return o3d::poissonSurface(pts, po).mesh;
                };
                label = o3d::engineName();
#endif
            } else {
                throw py::value_error("Unknown engine '" + eng + "' (use \"pdal\" or \"open3d\")");
            }
            if (!surface) throw py::value_error("This build has no '" + eng + "' engine");
            const Uuid ds = uuidArg(dataset), dev = uuidArg(development);
            const auto* d = s.get().model().developments.find(dev);
            if (!d) throw py::key_error("No development '" + development + "'");
            MeshData mesh;
            {
                py::gil_scoped_release rel;
                mesh = survey::buildAsBuiltSurface(s.get(), ds, dev, o, surface, progressOr(progress));
            }
            const std::string outName = name.empty() ? d->headingId + " as-built" : name;
            const std::string method = "Poisson depth=" + std::to_string(depth) + " (" + label + ")";
            Uuid id;
            {
                CommandStack::MacroScope macro(s.get().commands(), "Add as-built " + outName);
                const Uuid lay = ProjectEditor(s.get()).ensureLayer(layer, 0xFFB74D);
                id = survey::addAsBuiltModel(s.get(), ds, mesh, outName, lay, dev, method);
                macro.commit();
            }
            py::dict r;
            r["id"] = id.toString();
            r["vertices"] = mesh.vertices.size();
            r["triangles"] = mesh.triangles.size();
            r["method"] = method;
            r["engine"] = eng;
            return r;
        }, py::arg("dataset"), py::arg("development"), py::arg("corridor") = 6.0, py::arg("voxel") = 0.05, py::arg("depth") = 9, py::arg("engine") = "",
             py::arg("name") = "", py::arg("layer") = "As-built", py::arg("fill_holes_up_to") = 32, py::arg("progress") = nullptr);

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
    P.def("import_mesh", [](PyProject& s, const fs::path& file, const std::string& layer) {
         const auto mf = formats::readMesh(file);
         py::dict d;
         d["id"] = formats::addMeshObject(s.get(), mf, file.stem().string(), ProjectEditor(s.get()).ensureLayer(layer, 0xC8A165), file.filename().string()).toString();
         d["triangles"] = mf.mesh.triangles.size();
         d["warnings"] = mf.warnings;
         return d;
     }, py::arg("file"), py::arg("layer") = "Surfaces")
        .def("export_mesh", [](PyProject& s, const std::string& id, const fs::path& file) {
            const auto m = survey::surfaceOf(s.get(), uuidArg(id));
            return formats::writeMesh(file, *m, geometryOf(s.get(), id).name); // warnings (e.g. binary STL precision)
        }, py::arg("id"), py::arg("file"));
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
        .def("transform_points", [](PyProject& s, const Points& pts, const std::string& src, const std::string& tgt, int operation) {
            crs::TransformerOptions o;
            o.operationIndex = operation;
            return fromPoints(crs::transformPoints(s.get(), toPoints(pts), uuidArg(src), uuidArg(tgt), o));
        }, py::arg("points"), py::arg("source_crs"), py::arg("target_crs"), py::arg("operation") = 0)
        .def("set_project_crs", [](PyProject& s, const std::string& id) { crs::setProjectCrs(s.get(), uuidArg(id)); }, py::arg("crs"))
        .def("transform_objects", [](PyProject& s, const std::vector<std::string>& ids, const std::string& src, const std::string& tgt, const std::string& layer,
                                     int operation) {
            crs::TransformerOptions o;
            o.operationIndex = operation;
            std::vector<std::string> out;
            for (const auto& id : crs::transformObjects(s.get(), uuids(ids), uuidArg(src), uuidArg(tgt), layerArg(s.get(), layer), o)) out.push_back(id.toString());
            return out;
        }, py::arg("ids"), py::arg("source_crs"), py::arg("target_crs"), py::arg("layer") = "0", py::arg("operation") = 0)
        .def("define_local_grid", [](PyProject& s, const std::string& name, const std::string& base, const py::list& pairs, const std::string& method) {
            const auto m = enumFromString<LocalGridMethod>(method);
            if (!m) throw py::value_error("method must be Helmert2D, Helmert3D or Affine2D");
            std::vector<geometry::GridControlPair> ps;
            for (const auto& item : pairs) {
                const auto d = item.cast<py::dict>();
                geometry::GridControlPair gp;
                gp.name = d.contains("name") ? d["name"].cast<std::string>() : "P" + std::to_string(ps.size() + 1);
                gp.local = vec(d["local"].cast<std::array<double, 3>>());
                gp.base = vec(d["base"].cast<std::array<double, 3>>());
                gp.use = d.contains("use") ? d["use"].cast<bool>() : true;
                ps.push_back(gp);
            }
            geometry::LocalGridFit fit;
            try {
                fit = geometry::fitLocalGrid(ps, *m);
            } catch (const std::invalid_argument& e) {
                throw py::value_error(e.what());
            }
            py::dict out;
            out["crs"] = crs::defineLocalGrid(s.get(), name, uuidArg(base), fit).toString();
            out["transform"] = fit.localToBase.toString();
            out["parameters"] = fit.parameters;
            out["rms"] = fit.rms;
            out["max"] = fit.max;
            out["vertical_rms"] = fit.verticalRms;
            out["check_rms"] = fit.checkCount ? py::object(py::float_(fit.checkRms)) : py::none();
            py::list res;
            for (const auto& gp : fit.pairs) res.append(py::make_tuple(gp.name, gp.use, py::make_tuple(gp.residual.x, gp.residual.y, gp.residual.z)));
            out["residuals"] = res;
            return out;
        }, py::arg("name"), py::arg("base_crs"), py::arg("pairs"), py::arg("method") = "Helmert2D");
    m.def("search_crs", [](const std::string& fragment, std::size_t maxResults) {
        py::list out;
        for (const auto& c : crs::search(fragment, maxResults)) {
            py::dict d;
            d["name"] = c.name;
            d["code"] = c.authorityCode;
            d["area"] = c.areaName;
            d["deprecated"] = c.deprecated;
            out.append(d);
        }
        return out;
    }, py::arg("fragment"), py::arg("max_results") = 50);
    m.def("candidate_operations", [](const std::string& src, const std::string& tgt) {
        py::list out;
        for (const auto& o : crs::candidateOperations(src, tgt)) {
            py::dict d;
            d["name"] = o.name;
            d["pipeline"] = o.projPipeline;
            d["accuracy"] = o.accuracy >= 0 ? py::object(py::float_(o.accuracy)) : py::none();
            d["ballpark"] = o.ballpark;
            d["grids"] = o.grids;
            d["grids_available"] = o.gridsAvailable;
            out.append(d);
        }
        return out;
    }, py::arg("source"), py::arg("target"));
#endif
#ifdef OPENMINE_PY_HAVE_GIS
    m.def("import_dem", [](const fs::path& file, int step) {
        gis::DemImportOptions o;
        o.step = step;
        return fromMesh(gis::importDem(file, o));
    }, py::arg("file"), py::arg("step") = 1);
    auto projectCrsText = [](const Project& p) -> std::string {
        const auto* c = p.model().projectCrs();
        if (!c) return {};
        return c->authorityCode.empty() ? c->definition : c->authorityCode;
    };
    P.def("import_vector", [](PyProject& s, const fs::path& file, const std::string& sourceLayer, double z, const std::string& layer) {
         auto data = gis::importVector(file, sourceLayer, z);
         const Uuid lay = ProjectEditor(s.get()).ensureLayer(layer.empty() ? file.stem().string() : layer, 0xE0C060);
         std::vector<std::string> ids;
         for (const auto& id : gis::addVectorFeatures(s.get(), data, lay)) ids.push_back(id.toString());
         py::dict d;
         d["ids"] = ids;
         d["skipped"] = data.skipped;
         d["crs"] = data.crsAuthority;
         auto warnings = data.warnings;
         const auto* pc = s.get().model().projectCrs();
         if (!data.crsAuthority.empty()) {
             if (!pc || pc->authorityCode.empty())
                 warnings.push_back("The file declares " + data.crsAuthority + " but the project CRS is not declared; coordinates were imported as they are");
             else if (pc->authorityCode != data.crsAuthority)
                 warnings.push_back("The file's CRS (" + data.crsAuthority + ") differs from the project CRS (" + pc->authorityCode + "); coordinates were not transformed");
         }
         d["warnings"] = warnings;
         return d;
     }, py::arg("file"), py::arg("source_layer") = "", py::arg("default_z") = 0.0, py::arg("layer") = "")
        .def("export_vector", [projectCrsText](PyProject& s, const std::vector<std::string>& ids, const fs::path& file, bool polygons) {
            std::vector<const GeometryObject*> objs;
            for (const auto& id : ids) objs.push_back(&geometryOf(s.get(), id));
            gis::VectorExportOptions o;
            o.layerName = file.stem().string();
            o.crsWkt = projectCrsText(s.get());
            const auto rep = gis::exportVector(objs, file, o, polygons);
            py::dict d;
            d["written"] = rep.written;
            d["skipped"] = rep.skipped;
            return d;
        }, py::arg("ids"), py::arg("file"), py::arg("closed_as_polygons") = true)
        .def("import_dem_surface", [](PyProject& s, const fs::path& file, int step, double zScale, const std::string& name, const std::string& layer) {
            gis::DemImportOptions o;
            o.step = step;
            o.zScale = zScale;
            gis::RasterInfo info;
            auto mesh = gis::importDem(file, o, &info);
            const Uuid lay = ProjectEditor(s.get()).ensureLayer(layer, 0x9CCC65);
            const std::string method = "DEM " + info.driver + " step=" + std::to_string(step);
            py::dict d;
            d["triangles"] = mesh.triangles.size();
            d["crs"] = info.crsAuthority;
            d["id"] = survey::addSurfaceObject(s.get(), std::move(mesh), name.empty() ? file.stem().string() : name, lay, method, Uuid{}).toString();
            return d;
        }, py::arg("file"), py::arg("step") = 1, py::arg("z_scale") = 1.0, py::arg("name") = "", py::arg("layer") = "Surfaces")
        .def("export_dem", [projectCrsText](PyProject& s, const std::vector<std::string>& ids, const fs::path& file, double cell) {
            MeshData merged;
            for (const auto& id : ids) {
                const auto m = survey::surfaceOf(s.get(), uuidArg(id));
                const auto base = static_cast<std::uint32_t>(merged.vertices.size());
                merged.vertices.insert(merged.vertices.end(), m->vertices.begin(), m->vertices.end());
                for (auto t : m->triangles) merged.triangles.push_back({t[0] + base, t[1] + base, t[2] + base});
            }
            gis::DemExportOptions o;
            o.cellSize = cell;
            o.crsWkt = projectCrsText(s.get());
            std::string ext = file.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            o.driver = ext == ".asc" ? "AAIGrid" : "GTiff";
            const auto info = gis::exportDem(merged, file, o);
            py::dict d;
            d["width"] = info.width;
            d["height"] = info.height;
            d["driver"] = info.driver;
            return d;
        }, py::arg("ids"), py::arg("file"), py::arg("cell") = 1.0);
#endif
#ifdef OPENMINE_PY_HAVE_OCCT
    // ---- solids (exact B-rep through Open CASCADE) -----------------------------------------------------
    auto infoDict = [](const occt::ShapeInfo& i) {
        py::dict d;
        d["valid"] = i.valid;
        d["closed"] = i.closed;
        d["volume"] = i.volume;
        d["area"] = i.area;
        d["bounds"] = py::make_tuple(i.bounds.min.x, i.bounds.min.y, i.bounds.min.z, i.bounds.max.x, i.bounds.max.y, i.bounds.max.z);
        d["centre_of_mass"] = py::make_tuple(i.centreOfMass.x, i.centreOfMass.y, i.centreOfMass.z);
        d["solids"] = i.solids;
        d["shells"] = i.shells;
        d["faces"] = i.faces;
        d["edges"] = i.edges;
        d["vertices"] = i.vertices;
        d["problems"] = i.problems;
        return d;
    };
    auto strIds = [](const std::vector<Uuid>& v) {
        std::vector<std::string> o;
        for (const auto& u : v) o.push_back(u.toString());
        return o;
    };
    P.def("solid_from_mesh", [](PyProject& s, const std::string& meshId, bool requireClosed) {
         return occt::solidFromMeshObject(s.get(), uuidArg(meshId), requireClosed).toString();
     }, py::arg("mesh"), py::arg("require_closed") = true)
        .def("solid_primitive", [](PyProject& s, const std::string& kind, const py::dict& a, const std::string& name, const std::string& layer) {
            auto pt = [&](const char* k) {
                if (!a.contains(k)) throw py::value_error(kind + " needs " + k);
                return vec(a[k].cast<std::array<double, 3>>());
            };
            auto num = [&](const char* k) {
                if (!a.contains(k)) throw py::value_error(kind + " needs " + k);
                return a[k].cast<double>();
            };
            occt::Shape sh;
            if (kind == "box") sh = occt::makeBox(pt("corner1"), pt("corner2"));
            else if (kind == "cylinder") sh = occt::makeCylinder(pt("base"), pt("axis"), num("radius"), num("height"));
            else if (kind == "cone") sh = occt::makeCone(pt("base"), pt("axis"), num("radius_base"), num("radius_top"), num("height"));
            else if (kind == "sphere") sh = occt::makeSphere(pt("centre"), num("radius"));
            else throw py::value_error("Unknown primitive '" + kind + "' (box, cylinder, cone, sphere)");
            return occt::addSolidObject(s.get(), sh, name.empty() ? kind : name, layerArg(s.get(), layer), {}, "Add " + kind).toString();
        }, py::arg("kind"), py::arg("parameters"), py::arg("name") = "", py::arg("layer") = "0")
        .def("solid_extrude", [strIds](PyProject& s, const std::vector<std::string>& curves, const std::string& direction, double distance,
                                       std::array<double, 3> vector, bool symmetric, bool holes, const std::string& name, const std::string& layer) {
            occt::ExtrusionSpec spec;
            if (direction == "vertical") spec.direction = occt::ExtrusionSpec::Direction::Vertical;
            else if (direction == "normal") spec.direction = occt::ExtrusionSpec::Direction::Normal;
            else if (direction == "vector") spec.direction = occt::ExtrusionSpec::Direction::Vector;
            else throw py::value_error("direction must be 'vertical', 'normal' or 'vector'");
            spec.distance = distance;
            spec.vector = vec(vector);
            spec.symmetric = symmetric;
            spec.holes = holes;
            return strIds(occt::extrudeCurves(s.get(), uuids(curves), spec, name, layerArg(s.get(), layer)));
        }, py::arg("curves"), py::arg("direction") = "vertical", py::arg("distance") = 5.0, py::arg("vector") = std::array<double, 3>{0, 0, 5},
             py::arg("symmetric") = false, py::arg("holes") = true, py::arg("name") = "", py::arg("layer") = "0")
        .def("solid_revolve", [strIds](PyProject& s, const std::vector<std::string>& profiles, std::array<double, 3> axisPoint, std::array<double, 3> axisDir,
                                       double angleDeg, const std::string& name, const std::string& layer) {
            return strIds(occt::revolveCurves(s.get(), uuids(profiles), vec(axisPoint), vec(axisDir), angleDeg * std::numbers::pi / 180.0, name,
                                              layerArg(s.get(), layer)));
        }, py::arg("profiles"), py::arg("axis_point"), py::arg("axis_direction"), py::arg("angle_deg") = 360.0, py::arg("name") = "", py::arg("layer") = "0")
        .def("solid_sweep", [strIds](PyProject& s, const std::vector<std::string>& paths, const std::optional<std::string>& profile, double width, double height,
                                     const std::string& transition, const std::string& name, const std::string& layer) {
            geometry::ProfileCurve pc;
            if (profile) {
                const auto* tp = s.get().model().profiles.find(uuidArg(*profile));
                if (!tp) throw py::key_error("No tunnel profile '" + *profile + "'");
                pc = geometry::profileCurve(*tp);
            } else {
                if (!(width > 0) || !(height > 0)) throw py::value_error("width and height must be positive");
                pc.points = geometry::rectangleOutline(width, height);
                pc.bulges.assign(pc.points.size(), 0.0);
            }
            occt::SweepTransition t = occt::SweepTransition::RightCorner;
            if (transition == "transformed") t = occt::SweepTransition::Transformed;
            else if (transition == "round") t = occt::SweepTransition::RoundCorner;
            else if (transition != "right") throw py::value_error("transition must be 'right', 'round' or 'transformed'");
            return strIds(occt::sweepCurves(s.get(), uuids(paths), pc, t, name, layerArg(s.get(), layer)));
        }, py::arg("paths"), py::arg("profile") = py::none(), py::arg("width") = 5.0, py::arg("height") = 5.0, py::arg("transition") = "right",
             py::arg("name") = "", py::arg("layer") = "0")
        .def("solid_loft", [](PyProject& s, const std::vector<std::string>& sections, bool solid, bool ruled, const std::string& name, const std::string& layer) {
            return occt::loftCurves(s.get(), uuids(sections), solid, ruled, name, layerArg(s.get(), layer)).toString();
        }, py::arg("sections"), py::arg("solid") = true, py::arg("ruled") = false, py::arg("name") = "", py::arg("layer") = "0")
        .def("solid_boolean", [](PyProject& s, const std::string& a, const std::string& b, const std::string& op, bool removeInputs, const std::string& name,
                                 double fuzzy) {
            occt::BooleanOp o;
            if (op == "union") o = occt::BooleanOp::Union;
            else if (op == "difference" || op == "subtract") o = occt::BooleanOp::Difference;
            else if (op == "intersection" || op == "intersect") o = occt::BooleanOp::Intersection;
            else throw py::value_error("op must be 'union', 'difference' or 'intersection'");
            return occt::booleanObjects(s.get(), uuidArg(a), uuidArg(b), o, removeInputs, name, fuzzy).toString();
        }, py::arg("a"), py::arg("b"), py::arg("op"), py::arg("remove_inputs") = false, py::arg("name") = "", py::arg("fuzzy") = 0.0)
        .def("solid_fillet", [infoDict](PyProject& s, const std::string& id, double r) { return infoDict(occt::filletSolidEdges(s.get(), uuidArg(id), r)); },
             py::arg("id"), py::arg("radius"))
        .def("solid_offset", [infoDict](PyProject& s, const std::string& id, double d) { return infoDict(occt::offsetSolidObject(s.get(), uuidArg(id), d)); },
             py::arg("id"), py::arg("distance"))
        .def("solid_info", [infoDict](PyProject& s, const std::string& id) { return infoDict(occt::analyseSolidObject(s.get(), uuidArg(id))); }, py::arg("id"))
        .def("solid_heal", [infoDict](PyProject& s, const std::string& id) {
            const auto r = occt::healSolidObject(s.get(), uuidArg(id));
            py::dict d;
            d["before"] = infoDict(r.before);
            d["after"] = infoDict(r.after);
            d["fixes"] = r.fixes;
            return d;
        }, py::arg("id"))
        .def("section_objects", [strIds](PyProject& s, const std::vector<std::string>& ids, std::array<double, 3> point, std::array<double, 3> normal,
                                         const std::string& layer) {
            return strIds(occt::sectionObjects(s.get(), uuids(ids), vec(point), vec(normal), layerArg(s.get(), layer)));
        }, py::arg("ids"), py::arg("point"), py::arg("normal"), py::arg("layer") = "0")
        .def("development_solid", [](PyProject& s, const std::string& dev, const std::string& layer, const std::string& transition) {
            occt::SweepTransition t = transition == "round" ? occt::SweepTransition::RoundCorner
                                      : transition == "transformed" ? occt::SweepTransition::Transformed : occt::SweepTransition::RightCorner;
            return occt::developmentSolid(s.get(), uuidArg(dev), layerArg(s.get(), layer), t).toString();
        }, py::arg("development"), py::arg("layer") = "0", py::arg("transition") = "right")
        .def("solid_distance", [](PyProject& s, const std::string& a, const std::string& b) {
            const auto& p = s.get();
            const auto r = occt::minimumDistance(occt::shapeOfObject(p, geometryOf(p, a)), occt::shapeOfObject(p, geometryOf(p, b)));
            py::dict d;
            d["distance"] = r.distance;
            d["on_first"] = py::make_tuple(r.onFirst.x, r.onFirst.y, r.onFirst.z);
            d["on_second"] = py::make_tuple(r.onSecond.x, r.onSecond.y, r.onSecond.z);
            return d;
        }, py::arg("a"), py::arg("b"))
        .def("solid_classify", [](PyProject& s, const std::string& id, const Points& pts, double tol) {
            const auto& p = s.get();
            const auto sh = occt::loadSolid(p, geometryOf(p, id));
            std::vector<std::string> out;
            for (const auto& q : toPoints(pts)) {
                const auto st = occt::classify(sh, q, tol);
                out.push_back(st == occt::PointState::Inside ? "inside" : st == occt::PointState::On ? "on" : "outside");
            }
            return out;
        }, py::arg("id"), py::arg("points"), py::arg("tolerance") = 1e-7)
        .def("import_solids", [strIds](PyProject& s, const fs::path& file, const std::string& layer) {
            return strIds(occt::importSolids(s.get(), file, layerArg(s.get(), layer)));
        }, py::arg("file"), py::arg("layer") = "0")
        .def("export_solids", [](PyProject& s, const std::vector<std::string>& ids, const fs::path& file) { occt::exportSolids(s.get(), uuids(ids), file); },
             py::arg("ids"), py::arg("file"));
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
