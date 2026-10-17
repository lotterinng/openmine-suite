// SPDX-License-Identifier: Apache-2.0
#include "openmine/crs/Crs.h"

#include "openmine/commands/CommandStack.h"
#include "openmine/geometry/Curves.h"
#include "openmine/project/ProjectEditor.h"

#include <proj.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <regex>

namespace openmine::crs {

namespace {

/// RAII PROJ context (one per object; PROJ contexts are not thread-safe).
struct Ctx {
    PJ_CONTEXT* c = proj_context_create();
    Ctx() {
        proj_log_level(c, PJ_LOG_NONE);
    }
    ~Ctx() { proj_context_destroy(c); }
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;
    std::string error() const { return proj_context_errno_string(c, proj_context_errno(c)); }
};

struct Obj {
    PJ* p = nullptr;
    Obj() = default;
    explicit Obj(PJ* x) : p(x) {}
    ~Obj() {
        if (p) proj_destroy(p);
    }
    Obj(Obj&& o) noexcept : p(o.p) { o.p = nullptr; }
    Obj& operator=(Obj&& o) noexcept {
        if (this != &o) {
            if (p) proj_destroy(p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    explicit operator bool() const { return p != nullptr; }
};

std::string str(const char* s) { return s ? s : ""; }

Obj create(Ctx& ctx, const std::string& def) {
    std::string d = def;
    // Normalise "epsg:1234" and bare numbers.
    if (std::all_of(d.begin(), d.end(), [](unsigned char ch) { return std::isdigit(ch); }) && !d.empty()) d = "EPSG:" + d;
    Obj o(proj_create(ctx.c, d.c_str()));
    if (!o) throw CrsError("Unknown coordinate reference system '" + def + "': " + ctx.error());
    const auto t = proj_get_type(o.p);
    const bool isCrs = t == PJ_TYPE_GEOGRAPHIC_2D_CRS || t == PJ_TYPE_GEOGRAPHIC_3D_CRS || t == PJ_TYPE_GEOCENTRIC_CRS || t == PJ_TYPE_PROJECTED_CRS ||
                       t == PJ_TYPE_COMPOUND_CRS || t == PJ_TYPE_ENGINEERING_CRS || t == PJ_TYPE_BOUND_CRS || t == PJ_TYPE_VERTICAL_CRS ||
                       t == PJ_TYPE_GEOGRAPHIC_CRS || t == PJ_TYPE_OTHER_CRS || t == PJ_TYPE_DERIVED_PROJECTED_CRS;
    if (!isCrs) throw CrsError("'" + def + "' is not a coordinate reference system");
    return o;
}

CrsKind kindOf(PJ_TYPE t) {
    switch (t) {
    case PJ_TYPE_PROJECTED_CRS:
    case PJ_TYPE_DERIVED_PROJECTED_CRS: return CrsKind::Projected;
    case PJ_TYPE_GEOGRAPHIC_2D_CRS:
    case PJ_TYPE_GEOGRAPHIC_3D_CRS:
    case PJ_TYPE_GEOGRAPHIC_CRS: return CrsKind::Geographic;
    case PJ_TYPE_GEOCENTRIC_CRS: return CrsKind::Geocentric;
    case PJ_TYPE_ENGINEERING_CRS: return CrsKind::Engineering;
    default: return CrsKind::Unknown;
    }
}

std::string datumName(Ctx& ctx, PJ* crs) {
    Obj d(proj_crs_get_datum(ctx.c, crs));
    if (d) return str(proj_get_name(d.p));
    Obj e(proj_crs_get_datum_ensemble(ctx.c, crs));
    if (e) return str(proj_get_name(e.p));
    return {};
}

OperationInfo describeOperation(Ctx& ctx, PJ* op) {
    OperationInfo o;
    o.name = str(proj_get_name(op));
    o.projPipeline = str(proj_as_proj_string(ctx.c, op, PJ_PROJ_5, nullptr));
    o.accuracy = proj_coordoperation_get_accuracy(ctx.c, op);
    o.ballpark = proj_coordoperation_has_ballpark_transformation(ctx.c, op) != 0;
    const int n = proj_coordoperation_get_grid_used_count(ctx.c, op);
    for (int i = 0; i < n; ++i) {
        const char *shortName = nullptr, *fullName = nullptr, *pkg = nullptr, *url = nullptr;
        int direct = 0, open = 0, available = 0;
        if (proj_coordoperation_get_grid_used(ctx.c, op, i, &shortName, &fullName, &pkg, &url, &direct, &open, &available)) {
            o.grids.push_back(str(shortName));
            if (!available) o.gridsAvailable = false;
        }
    }
    return o;
}

struct OpList {
    PJ_OBJ_LIST* list = nullptr;
    ~OpList() {
        if (list) proj_list_destroy(list);
    }
};

PJ_OBJ_LIST* operations(Ctx& ctx, PJ* src, PJ* dst) {
    PJ_OPERATION_FACTORY_CONTEXT* f = proj_create_operation_factory_context(ctx.c, nullptr);
    proj_operation_factory_context_set_grid_availability_use(ctx.c, f, PROJ_GRID_AVAILABILITY_KNOWN_AVAILABLE);
    proj_operation_factory_context_set_spatial_criterion(ctx.c, f, PROJ_SPATIAL_CRITERION_PARTIAL_INTERSECTION);
    PJ_OBJ_LIST* l = proj_create_operations(ctx.c, src, dst, f);
    proj_operation_factory_context_destroy(f);
    return l;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::string projVersion() { return proj_info().release; }

std::string CrsInfo::axisDescription() const {
    std::string s;
    for (const auto& a : axes) {
        if (!s.empty()) s += ", ";
        s += (a.abbreviation.empty() ? a.name : a.abbreviation) + " " + a.direction;
    }
    return s;
}

CrsInfo describe(const std::string& def) {
    Ctx ctx;
    Obj o = create(ctx, def);
    CrsInfo i;
    i.name = str(proj_get_name(o.p));
    if (const char* a = proj_get_id_auth_name(o.p, 0)) i.authorityCode = std::string(a) + ":" + str(proj_get_id_code(o.p, 0));
    i.projString = str(proj_as_proj_string(ctx.c, o.p, PJ_PROJ_5, nullptr));
    i.wkt = str(proj_as_wkt(ctx.c, o.p, PJ_WKT2_2019, nullptr));
    i.kind = kindOf(proj_get_type(o.p));
    i.deprecated = proj_is_deprecated(o.p) != 0;
    // Horizontal part of compound CRSs.
    Obj horiz;
    PJ* h = o.p;
    if (proj_get_type(o.p) == PJ_TYPE_COMPOUND_CRS) {
        horiz = Obj(proj_crs_get_sub_crs(ctx.c, o.p, 0));
        Obj vert(proj_crs_get_sub_crs(ctx.c, o.p, 1));
        if (horiz) {
            h = horiz.p;
            i.kind = kindOf(proj_get_type(h));
        }
        if (vert) i.verticalDatum = datumName(ctx, vert.p);
    }
    i.horizontalDatum = datumName(ctx, h);
    Obj cs(proj_crs_get_coordinate_system(ctx.c, h));
    if (cs) {
        const int n = proj_cs_get_axis_count(ctx.c, cs.p);
        for (int k = 0; k < n; ++k) {
            const char *name = nullptr, *abbr = nullptr, *dir = nullptr, *unit = nullptr, *ua = nullptr, *uc = nullptr;
            double factor = 1;
            proj_cs_get_axis_info(ctx.c, cs.p, k, &name, &abbr, &dir, &factor, &unit, &ua, &uc);
            i.axes.push_back({str(name), str(abbr), str(dir), str(unit)});
            if (k == 0 && i.kind != CrsKind::Geographic) {
                i.linearUnit = str(unit);
                i.unitToMetre = factor;
            }
        }
    }
    double w, s, e, n;
    const char* area = nullptr;
    if (proj_get_area_of_use(ctx.c, o.p, &w, &s, &e, &n, &area) && w > -1000) {
        i.areaOfUse = std::array<double, 4>{w, s, e, n};
        i.areaName = str(area);
    }
    return i;
}

CoordinateReferenceSystem toEntity(const CrsInfo& i) {
    CoordinateReferenceSystem c;
    c.name = i.name;
    c.authorityCode = i.authorityCode;
    c.definition = i.wkt;
    c.kind = i.kind;
    c.horizontalDatum = i.horizontalDatum;
    if (!i.verticalDatum.empty()) c.verticalDatum = i.verticalDatum;
    c.linearUnit = i.kind == CrsKind::Geographic ? "degree" : (i.linearUnit.empty() ? "metre" : i.linearUnit);
    c.axisDescription = i.axisDescription();
    return c;
}

std::vector<CrsInfo> search(const std::string& frag, std::size_t maxResults) {
    Ctx ctx;
    PROJ_CRS_LIST_PARAMETERS* params = proj_get_crs_list_parameters_create();
    params->allow_deprecated = 0;
    int count = 0;
    PROJ_CRS_INFO** list = proj_get_crs_info_list_from_database(ctx.c, "EPSG", params, &count);
    proj_get_crs_list_parameters_destroy(params);
    std::vector<std::string> codes;
    const std::string f = lower(frag);
    for (int k = 0; k < count && codes.size() < maxResults; ++k) {
        const auto* ci = list[k];
        if (ci->type != PJ_TYPE_PROJECTED_CRS && ci->type != PJ_TYPE_GEOGRAPHIC_2D_CRS && ci->type != PJ_TYPE_GEOGRAPHIC_3D_CRS) continue;
        if (lower(str(ci->name)).find(f) == std::string::npos && std::string(ci->code) != frag) continue;
        codes.push_back(std::string(ci->auth_name) + ":" + ci->code);
    }
    proj_crs_info_list_destroy(list);
    std::vector<CrsInfo> out;
    for (const auto& c : codes) out.push_back(describe(c));
    return out;
}

std::vector<OperationInfo> candidateOperations(const std::string& source, const std::string& target) {
    Ctx ctx;
    Obj s = create(ctx, source), t = create(ctx, target);
    OpList l{operations(ctx, s.p, t.p)};
    if (!l.list) throw CrsError("No transformation between " + source + " and " + target + ": " + ctx.error());
    std::vector<OperationInfo> out;
    const int n = proj_list_get_count(l.list);
    for (int i = 0; i < n; ++i) {
        Obj op(proj_list_get(ctx.c, l.list, i));
        out.push_back(describeOperation(ctx, op.p));
    }
    if (out.empty()) throw CrsError("No transformation between " + source + " and " + target);
    return out;
}

struct Transformer::Impl {
    Ctx ctx;
    Obj op;
    std::mutex m;
};

Transformer::Transformer(const std::string& source, const std::string& target, const TransformerOptions& o)
    : impl_(std::make_unique<Impl>()), src_(source), dst_(target) {
    Obj s = create(impl_->ctx, source), t = create(impl_->ctx, target);
    OpList l{operations(impl_->ctx, s.p, t.p)};
    const int n = l.list ? proj_list_get_count(l.list) : 0;
    if (n == 0) throw CrsError("No transformation between " + source + " and " + target);
    if (o.operationIndex < 0 || o.operationIndex >= n) throw CrsError("Operation index out of range (" + std::to_string(n) + " candidates)");
    impl_->op = Obj(proj_list_get(impl_->ctx.c, l.list, o.operationIndex));
    op_ = describeOperation(impl_->ctx, impl_->op.p);
    if (op_.ballpark && !o.allowBallpark)
        throw CrsError("The only available operation '" + op_.name + "' is a ballpark transformation (datum shift unknown); allow it explicitly");
    if (!op_.gridsAvailable) throw CrsError("Operation '" + op_.name + "' needs grid files that are not installed");
}

Transformer::~Transformer() = default;

Vec3 Transformer::forward(Vec3 p) const {
    std::lock_guard lk(impl_->m);
    PJ_COORD c = proj_coord(p.x, p.y, p.z, HUGE_VAL);
    const PJ_COORD r = proj_trans(impl_->op.p, PJ_FWD, c);
    if (!std::isfinite(r.xyz.x) || !std::isfinite(r.xyz.y)) throw CrsError("Coordinate cannot be transformed: " + impl_->ctx.error());
    return {r.xyz.x, r.xyz.y, std::isfinite(r.xyz.z) ? r.xyz.z : p.z};
}

Vec3 Transformer::inverse(Vec3 p) const {
    std::lock_guard lk(impl_->m);
    PJ_COORD c = proj_coord(p.x, p.y, p.z, HUGE_VAL);
    const PJ_COORD r = proj_trans(impl_->op.p, PJ_INV, c);
    if (!std::isfinite(r.xyz.x) || !std::isfinite(r.xyz.y)) throw CrsError("Coordinate cannot be transformed: " + impl_->ctx.error());
    return {r.xyz.x, r.xyz.y, std::isfinite(r.xyz.z) ? r.xyz.z : p.z};
}

std::vector<Vec3> Transformer::forward(const std::vector<Vec3>& pts) const {
    std::vector<Vec3> out;
    out.reserve(pts.size());
    for (const auto& p : pts) out.push_back(forward(p));
    return out;
}

std::vector<std::string> checkAreaOfUse(const std::string& crs, const std::vector<Vec3>& pts) {
    const auto info = describe(crs);
    std::vector<std::string> w;
    if (!info.areaOfUse || pts.empty()) return w;
    const auto [west, south, east, north] = *info.areaOfUse;
    const double mx = std::max(0.5, 0.1 * (east - west)), my = std::max(0.5, 0.1 * (north - south));
    // Geographic coordinates in degrees, (lat, lon) order of EPSG:4326 → normalise.
    Transformer t(crs, "EPSG:4326", {0, true});
    std::size_t outside = 0;
    Vec3 first;
    for (const auto& p : pts) {
        Vec3 g;
        try {
            g = t.forward(p);
        } catch (const CrsError&) {
            ++outside;
            continue;
        }
        const double lat = g.x, lon = g.y;
        const bool in = lat >= south - my && lat <= north + my && ((west <= east) ? (lon >= west - mx && lon <= east + mx) : (lon >= west - mx || lon <= east + mx));
        if (!in) {
            if (outside == 0) first = p;
            ++outside;
        }
    }
    if (outside > 0) {
        char buf[300];
        std::snprintf(buf, sizeof buf, "%zu of %zu points lie outside the area of use of %s (%s); first: %.3f %.3f. Check the declared CRS and axis order.",
                      outside, pts.size(), info.name.c_str(), info.areaName.c_str(), first.x, first.y);
        w.push_back(buf);
    }
    return w;
}

// ---- Project integration ----------------------------------------------------------------------------
Uuid declareCrs(Project& p, const std::string& def) {
    const auto info = describe(def);
    for (const auto& c : p.model().crs.all())
        if (!info.authorityCode.empty() && c.authorityCode == info.authorityCode) return c.id;
    auto e = toEntity(info);
    if (e.kind == CrsKind::Geographic)
        throw CrsError(info.name + " is geographic (degrees): project data must use a projected or local metre-based CRS");
    if (info.unitToMetre != 1.0) throw CrsError(info.name + " uses " + info.linearUnit + ": project data must be in metres");
    return ProjectEditor(p).add(e, "Declare CRS " + info.name);
}

void setProjectCrs(Project& p, const Uuid& id) {
    if (!p.model().crs.find(id)) throw CrsError("CRS does not exist");
    ProjectInfo i = p.model().info;
    i.projectCrsId = id;
    ProjectEditor(p).setProjectInfo(i, "Set project CRS");
}

Uuid defineLocalGrid(Project& p, const std::string& name, const Uuid& baseCrsId, const geometry::LocalGridFit& fit) {
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Define local grid " + name);
    LocalGridDefinition g;
    g.name = name;
    g.baseCrsId = baseCrsId;
    g.method = fit.method;
    g.localToBase = fit.localToBase;
    g.parameters = fit.parameters;
    g.rmsResidual = fit.rms;
    g.maxResidual = fit.max;
    g.controlPairCount = fit.fitCount;
    g.controlPairsJson = geometry::pairsJson(fit.pairs);
    const Uuid gid = ed.add(g);
    CoordinateReferenceSystem c;
    c.name = name;
    c.kind = CrsKind::LocalGrid;
    c.localGridId = gid;
    const auto* base = p.model().crs.find(baseCrsId);
    c.horizontalDatum = base ? base->horizontalDatum : "";
    c.definition = "Local grid fitted to " + (base ? base->name : std::string("base CRS")) + " (" + std::to_string(fit.fitCount) + " control pairs)";
    c.axisDescription = "x, y (local grid)";
    const Uuid cid = ed.add(c);
    macro.commit();
    return cid;
}

namespace {
/// Resolves a project CRS into (authority/definition string of the PROJ CRS, local → that CRS transform).
std::pair<std::string, Transform3> resolve(const Project& p, const Uuid& id) {
    const auto* c = p.model().crs.find(id);
    if (!c) throw CrsError("CRS does not exist in the project");
    if (c->kind == CrsKind::LocalGrid) {
        const auto* g = p.model().localGrids.find(c->localGridId);
        if (!g) throw CrsError("Local grid definition of " + c->name + " is missing");
        auto [base, t] = resolve(p, g->baseCrsId);
        return {base, t * g->localToBase};
    }
    if (!c->authorityCode.empty()) return {c->authorityCode, Transform3{}};
    if (!c->definition.empty()) return {c->definition, Transform3{}};
    throw CrsError(c->name + " has no definition that PROJ can use (declare it from EPSG or WKT)");
}
} // namespace

std::vector<Vec3> transformPoints(const Project& p, const std::vector<Vec3>& pts, const Uuid& s, const Uuid& t, const TransformerOptions& o,
                                  TransformationRecord* rec) {
    const auto [sb, sl] = resolve(p, s);
    const auto [tb, tl] = resolve(p, t);
    const auto tlInv = tl.inverse();
    if (!tlInv) throw CrsError("Target local grid transformation is not invertible");
    std::vector<Vec3> out;
    out.reserve(pts.size());
    std::unique_ptr<Transformer> tr;
    if (sb != tb) tr = std::make_unique<Transformer>(sb, tb, o);
    for (const auto& q : pts) {
        Vec3 v = sl.apply(q);
        if (tr) v = tr->forward(v);
        out.push_back(tlInv->apply(v));
    }
    if (rec) {
        const auto* sc = p.model().crs.find(s);
        const auto* tc = p.model().crs.find(t);
        rec->sourceCrs = sc->authorityCode.empty() ? sc->name : sc->authorityCode;
        rec->targetCrs = tc->authorityCode.empty() ? tc->name : tc->authorityCode;
        double acc2 = 0;
        bool known = true;
        std::string pipe;
        auto addGrid = [&](const Uuid& id, const char* dir) {
            const auto* c = p.model().crs.find(id);
            if (c->kind != CrsKind::LocalGrid) return;
            const auto* g = p.model().localGrids.find(c->localGridId);
            acc2 += g->rmsResidual * g->rmsResidual;
            pipe += std::string(dir) + " local grid " + c->name + " (" + g->localToBase.toString() + "); ";
        };
        addGrid(s, "from");
        if (tr) {
            rec->operation = tr->operation().name;
            pipe += tr->operation().projPipeline + "; ";
            if (tr->operation().accuracy < 0) known = false;
            else acc2 += tr->operation().accuracy * tr->operation().accuracy;
            std::string grids;
            for (const auto& g : tr->operation().grids) grids += (grids.empty() ? "" : ",") + g;
            rec->gridsUsed = grids;
        } else rec->operation = "Local grid transformation";
        addGrid(t, "to");
        rec->pipeline = pipe;
        rec->accuracyMetres = known ? std::sqrt(acc2) : -1;
        rec->createdAt = utcTimestampNow();
        rec->user = p.userName();
    }
    return out;
}

std::vector<Uuid> transformObjects(Project& p, const std::vector<Uuid>& ids, const Uuid& s, const Uuid& t, const Uuid& layer, const TransformerOptions& o) {
    ProjectEditor ed(p);
    std::vector<GeometryObject> copies;
    TransformationRecord rec;
    for (const auto& id : ids) {
        const auto* g = p.model().geometry.find(id);
        if (!g) throw CrsError("Geometry object does not exist");
        GeometryObject c = *g;
        c.id = Uuid{};
        c.meta = {};
        c.layerId = layer.isNil() ? g->layerId : layer;
        if (c.type == GeometryType::Solid) throw CrsError("Solids are transformed as meshes: convert '" + g->name + "' first");
        // Curves with exact arcs: transform their defining points only when the transformation
        // is (locally) a similarity; otherwise densify to polylines first.
        if ((c.type == GeometryType::Arc || c.type == GeometryType::Circle || c.type == GeometryType::Spline || !c.bulges.empty())) {
            c.vertices = geometry::curvePolyline(c, 0.001);
            c.bulges.clear();
            if (c.type == GeometryType::Circle) {
                c.closed = true;
                c.vertices.pop_back();
            }
            c.type = GeometryType::Polyline;
        }
        c.vertices = transformPoints(p, c.vertices, s, t, o, &rec);
        if (c.mesh) {
            auto m = std::make_shared<MeshData>(*c.mesh);
            m->vertices = transformPoints(p, m->vertices, s, t, o, &rec);
            c.mesh = m;
        }
        c.name = g->name.empty() ? "" : g->name + " (" + rec.targetCrs + ")";
        copies.push_back(std::move(c));
    }
    if (copies.empty()) return {};
    CommandStack::MacroScope macro(p.commands(), "Transform " + std::to_string(copies.size()) + " object(s) to " + rec.targetCrs);
    std::vector<Uuid> out;
    for (auto& c : copies) out.push_back(ed.add(std::move(c)));
    rec.appliedTo = std::to_string(out.size()) + " geometry object copies";
    ed.add(rec);
    macro.commit();
    return out;
}

// ---- Units ----------------------------------------------------------------------------------------
double toMetres(double v, LengthUnit u) {
    switch (u) {
    case LengthUnit::Metre: return v;
    case LengthUnit::Millimetre: return v / 1000;
    case LengthUnit::Centimetre: return v / 100;
    case LengthUnit::Kilometre: return v * 1000;
    case LengthUnit::Foot: return v * 0.3048;
    case LengthUnit::UsSurveyFoot: return v * 1200.0 / 3937.0;
    case LengthUnit::Inch: return v * 0.0254;
    case LengthUnit::Yard: return v * 0.9144;
    }
    return v;
}
double fromMetres(double m, LengthUnit u) { return m / toMetres(1.0, u); }

std::optional<LengthUnit> unitFromName(const std::string& n0) {
    const auto n = lower(n0);
    static const std::map<std::string, LengthUnit> names = {
        {"m", LengthUnit::Metre}, {"metre", LengthUnit::Metre}, {"meter", LengthUnit::Metre}, {"metres", LengthUnit::Metre}, {"meters", LengthUnit::Metre},
        {"mm", LengthUnit::Millimetre}, {"millimetre", LengthUnit::Millimetre}, {"cm", LengthUnit::Centimetre}, {"km", LengthUnit::Kilometre},
        {"ft", LengthUnit::Foot}, {"foot", LengthUnit::Foot}, {"feet", LengthUnit::Foot}, {"'", LengthUnit::Foot}, {"usft", LengthUnit::UsSurveyFoot},
        {"us-ft", LengthUnit::UsSurveyFoot}, {"in", LengthUnit::Inch}, {"inch", LengthUnit::Inch}, {"\"", LengthUnit::Inch}, {"yd", LengthUnit::Yard}};
    const auto it = names.find(n);
    if (it == names.end()) return std::nullopt;
    return it->second;
}

double parseLength(const std::string& text) {
    static const std::regex re(R"(^\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*([A-Za-z'"\-]*)\s*$)");
    std::smatch m;
    if (!std::regex_match(text, m, re)) throw CrsError("Cannot read a length from '" + text + "'");
    const double v = std::stod(m[1].str());
    if (m[2].str().empty()) return v;
    const auto u = unitFromName(m[2].str());
    if (!u) throw CrsError("Unknown length unit '" + m[2].str() + "'");
    return toMetres(v, *u);
}

} // namespace openmine::crs
