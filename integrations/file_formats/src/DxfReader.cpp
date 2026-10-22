// SPDX-License-Identifier: Apache-2.0
#include "DxfCommon.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <tuple>
#include <limits>
#include <cctype>

namespace openmine::formats {

using namespace dxf;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

/// One DXF entity (or table record): its type and groups, with POLYLINE/INSERT children attached.
struct Entity {
    std::string type;
    std::vector<Group> g;        ///< groups before the first 1001 (XDATA)
    std::vector<Group> xdata;    ///< groups from the first 1001 on
    std::vector<Entity> children; ///< VERTEX (POLYLINE) or ATTRIB (INSERT)

    const Group* find(int code) const {
        for (const auto& x : g)
            if (x.code == code) return &x;
        return nullptr;
    }
    bool has(int code) const { return find(code) != nullptr; }
    std::string str(int code, std::string def = {}) const {
        const auto* x = find(code);
        return x ? x->value : def;
    }
    double num(int code, double def = 0) const {
        const auto* x = find(code);
        return x ? x->num() : def;
    }
    int integer(int code, int def = 0) const {
        const auto* x = find(code);
        return x ? x->integer() : def;
    }
    Vec3 point(int code, Vec3 def = {}) const {
        for (std::size_t i = 0; i < g.size(); ++i) {
            if (g[i].code != code) continue;
            Vec3 p{g[i].num(), def.y, def.z};
            for (std::size_t j = i + 1; j < g.size() && g[j].code != code; ++j) {
                if (g[j].code == code + 10) p.y = g[j].num();
                else if (g[j].code == code + 20) {
                    p.z = g[j].num();
                    break;
                }
            }
            return p;
        }
        return def;
    }
    Vec3 extrusion() const { return has(210) ? point(210, {0, 0, 1}) : Vec3{0, 0, 1}; }
    /// Value following `key` in OPENMINE xdata (1000 key, then one value group).
    const Group* xkey(std::string_view key) const {
        bool ours = false;
        for (std::size_t i = 0; i < xdata.size(); ++i) {
            if (xdata[i].code == 1001) ours = xdata[i].value == "OPENMINE";
            else if (ours && xdata[i].code == 1000 && xdata[i].value == key && i + 1 < xdata.size()) return &xdata[i + 1];
        }
        return nullptr;
    }
    std::string xname() const {
        bool ours = false;
        for (const auto& x : xdata) {
            if (x.code == 1001) ours = x.value == "OPENMINE";
            else if (ours && x.code == 1000) return x.value; // first string is the object name
        }
        return {};
    }
};

struct Block {
    std::string name;
    Vec3 base;
    std::vector<Entity> entities;
};

struct LayerInfo {
    CadLayer layer;
    bool frozen = false, off = false;
    bool used = false;
};

struct DimStyle {
    double textHeight = 2.5;
    int decimals = 4;
    double scale = 1.0;
};

/// Groups a flat list into entities, attaching VERTEX/ATTRIB children up to SEQEND.
std::vector<Entity> collectEntities(const std::vector<Group>& gs, std::size_t& i, const std::set<std::string>& stops) {
    std::vector<Entity> out;
    while (i < gs.size()) {
        if (gs[i].code != 0) { ++i; continue; }
        if (stops.count(gs[i].value)) break;
        Entity e;
        e.type = gs[i].value;
        ++i;
        bool inX = false;
        while (i < gs.size() && gs[i].code != 0) {
            if (gs[i].code == 1001) inX = true;
            (inX ? e.xdata : e.g).push_back(gs[i]);
            ++i;
        }
        if (e.type == "VERTEX" || e.type == "ATTRIB") {
            if (!out.empty() && (out.back().type == "POLYLINE" || out.back().type == "INSERT")) out.back().children.push_back(std::move(e));
            continue;
        }
        if (e.type == "SEQEND") continue;
        out.push_back(std::move(e));
    }
    return out;
}

/// Uniform cubic B-spline / NURBS evaluation (de Boor).
struct Nurbs {
    int degree = 3;
    std::vector<double> knots, weights;
    std::vector<Vec3> ctrl;

    Vec3 eval(double u) const {
        const int p = degree;
        const int n = static_cast<int>(ctrl.size()) - 1;
        int k = p;
        while (k < n && knots[static_cast<std::size_t>(k + 1)] <= u) ++k;
        std::vector<Vec3> d(static_cast<std::size_t>(p + 1));
        std::vector<double> w(static_cast<std::size_t>(p + 1));
        for (int j = 0; j <= p; ++j) {
            const auto idx = static_cast<std::size_t>(j + k - p);
            w[static_cast<std::size_t>(j)] = weights.empty() ? 1.0 : weights[idx];
            d[static_cast<std::size_t>(j)] = ctrl[idx] * w[static_cast<std::size_t>(j)];
        }
        for (int r = 1; r <= p; ++r)
            for (int j = p; j >= r; --j) {
                const double kl = knots[static_cast<std::size_t>(j + k - p)], kr = knots[static_cast<std::size_t>(j + 1 + k - r)];
                const double a = kr - kl > 0 ? (u - kl) / (kr - kl) : 0.0;
                const auto J = static_cast<std::size_t>(j);
                d[J] = d[J - 1] * (1 - a) + d[J] * a;
                w[J] = w[J - 1] * (1 - a) + w[J] * a;
            }
        return d[static_cast<std::size_t>(p)] / w[static_cast<std::size_t>(p)];
    }
    double uMin() const { return knots[static_cast<std::size_t>(degree)]; }
    double uMax() const { return knots[knots.size() - 1 - static_cast<std::size_t>(degree)]; }

    std::vector<Vec3> sample(double tol) const {
        std::vector<Vec3> pts;
        std::vector<double> breaks;
        for (std::size_t i = static_cast<std::size_t>(degree); i + static_cast<std::size_t>(degree) < knots.size(); ++i)
            if (breaks.empty() || knots[i] > breaks.back() + 1e-15) breaks.push_back(knots[i]);
        std::function<void(double, Vec3, double, Vec3, int)> rec = [&](double u0, Vec3 p0, double u1, Vec3 p1, int depth) {
            const double um = 0.5 * (u0 + u1);
            const Vec3 pm = eval(um);
            // deviation of the midpoint and quarter points from the chord
            const Vec3 q1 = eval(0.75 * u0 + 0.25 * u1), q3 = eval(0.25 * u0 + 0.75 * u1);
            auto dev = [&](Vec3 q) {
                const Vec3 c = p1 - p0;
                const double l2 = c.lengthSquared();
                const double t = l2 > 0 ? std::clamp(dot(q - p0, c) / l2, 0.0, 1.0) : 0.0;
                return distance(q, p0 + c * t);
            };
            if (depth >= 14 || (std::max({dev(pm), dev(q1), dev(q3)}) <= tol && depth >= 1)) {
                pts.push_back(p1);
                return;
            }
            rec(u0, p0, um, pm, depth + 1);
            rec(um, pm, u1, p1, depth + 1);
        };
        pts.push_back(eval(breaks.front()));
        for (std::size_t i = 0; i + 1 < breaks.size(); ++i) rec(breaks[i], eval(breaks[i]), breaks[i + 1], eval(breaks[i + 1] - 1e-12 * (breaks[i + 1] - breaks[i])), 0);
        return pts;
    }
};

double pointPolylineDistance(Vec3 p, const std::vector<Vec3>& pl) {
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i + 1 < pl.size(); ++i) {
        const Vec3 c = pl[i + 1] - pl[i];
        const double l2 = c.lengthSquared();
        const double t = l2 > 0 ? std::clamp(dot(p - pl[i], c) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, distance(p, pl[i] + c * t));
    }
    if (pl.size() == 1) best = distance(p, pl[0]);
    return best;
}

class Reader {
public:
    Reader(const DxfImportOptions& o) : opt_(o) {}

    DxfImportResult run(std::string_view text) {
        const auto gs = tokenize(text);
        std::size_t i = 0;
        std::vector<Entity> entities;
        while (i < gs.size()) {
            if (gs[i].code == 0 && gs[i].value == "SECTION" && i + 1 < gs.size() && gs[i + 1].code == 2) {
                const std::string name = gs[i + 1].value;
                i += 2;
                if (name == "HEADER") readHeader(gs, i);
                else if (name == "TABLES") readTables(gs, i);
                else if (name == "BLOCKS") readBlocks(gs, i);
                else if (name == "ENTITIES") entities = collectEntities(gs, i, {"ENDSEC"});
                while (i < gs.size() && !(gs[i].code == 0 && gs[i].value == "ENDSEC")) ++i;
                ++i;
                continue;
            }
            if (gs[i].code == 0 && gs[i].value == "EOF") break;
            ++i;
        }
        // Units.
        double scale = 1.0;
        if (opt_.unitScaleOverride > 0) scale = opt_.unitScaleOverride;
        else if (opt_.scaleToMetres) {
            const double m = metresPerInsUnit(res_.insUnits);
            if (m > 0) scale = m;
            else if (res_.insUnits != 0) warn("Unknown $INSUNITS " + std::to_string(res_.insUnits) + "; coordinates taken as metres");
        }
        res_.unitScale = scale;
        Ctx root;
        root.T = Transform3::scaling(scale);
        root.scale = scale;
        for (const auto& e : entities) convert(e, root);
        flushFaces();
        for (auto& [name, li] : layers_)
            if (li.used) res_.layers.push_back(li.layer);
        std::sort(res_.layers.begin(), res_.layers.end(), [](const CadLayer& a, const CadLayer& b) { return a.name < b.name; });
        return std::move(res_);
    }

private:
    struct Ctx {
        Transform3 T;
        double scale = 1.0;            ///< uniform part of T (for tolerances and heights)
        std::string insertLayer;       ///< layer of the INSERT (for entities on layer 0)
        std::optional<std::uint32_t> insertColor;
        int depth = 0;
        bool inBlock = false;
    };

    const DxfImportOptions& opt_;
    DxfImportResult res_;
    std::map<std::string, LayerInfo> layers_;
    std::map<std::string, DimStyle> dimStyles_;
    std::map<std::string, Block> blocks_;
    bool cp1252_ = true;
    struct FaceKey {
        std::string layer;
        std::optional<std::uint32_t> color;
        friend bool operator<(const FaceKey& a, const FaceKey& b) { return std::tie(a.layer, a.color) < std::tie(b.layer, b.color); }
    };
    std::map<FaceKey, std::vector<std::array<Vec3, 3>>> faces_;

    void warn(std::string w) {
        if (std::find(res_.warnings.begin(), res_.warnings.end(), w) == res_.warnings.end()) res_.warnings.push_back(std::move(w));
    }
    void skip(const std::string& type, const std::string& why = {}) {
        ++res_.skipped[type];
        if (!why.empty()) warn(type + ": " + why);
    }

    // ---- Sections -------------------------------------------------------------------------------
    void readHeader(const std::vector<Group>& gs, std::size_t& i) {
        std::string var;
        for (; i < gs.size() && !(gs[i].code == 0 && gs[i].value == "ENDSEC"); ++i) {
            if (gs[i].code == 9) var = gs[i].value;
            else if (var == "$ACADVER" && gs[i].code == 1) res_.acadVersion = gs[i].value;
            else if (var == "$INSUNITS" && gs[i].code == 70) res_.insUnits = gs[i].integer();
            else if (var == "$DWGCODEPAGE" && gs[i].code == 3) cp1252_ = gs[i].value == "ANSI_1252" || gs[i].value.empty();
        }
        // R2007+ (AC1021 and later) files are UTF-8.
        if (res_.acadVersion >= "AC1021") cp1252_ = false;
    }

    void readTables(const std::vector<Group>& gs, std::size_t& i) {
        while (i < gs.size() && !(gs[i].code == 0 && gs[i].value == "ENDSEC")) {
            if (gs[i].code == 0 && gs[i].value == "TABLE") {
                ++i;
                auto recs = collectEntities(gs, i, {"ENDTAB", "ENDSEC"});
                for (const auto& r : recs) {
                    if (r.type == "LAYER") readLayer(r);
                    else if (r.type == "DIMSTYLE") {
                        DimStyle s;
                        s.textHeight = r.num(140, 2.5);
                        s.decimals = r.integer(271, 4);
                        s.scale = r.num(40, 1.0);
                        if (s.scale <= 0) s.scale = 1.0;
                        dimStyles_[r.str(2)] = s;
                    }
                }
                if (i < gs.size() && gs[i].value == "ENDTAB") ++i;
                continue;
            }
            ++i;
        }
    }

    void readLayer(const Entity& r) {
        LayerInfo li;
        li.layer.id = Uuid::generate();
        li.layer.name = opt_.layerPrefix + decodeText(r.str(2, "0"), cp1252_);
        const int flags = r.integer(70, 0);
        const int aci = r.integer(62, 7);
        li.off = aci < 0;
        li.frozen = (flags & 1) != 0;
        li.layer.locked = (flags & 4) != 0;
        li.layer.visible = !li.off && !li.frozen;
        li.layer.colorRgb = r.has(420) ? static_cast<std::uint32_t>(r.integer(420)) & 0xFFFFFF : aciToRgb(std::abs(aci));
        const std::string lt = r.str(6, "Continuous");
        std::string up = lt;
        std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (up.find("DASHDOT") != std::string::npos || up.find("CENTER") != std::string::npos) li.layer.lineType = "DashDot";
        else if (up.find("DASH") != std::string::npos || up.find("HIDDEN") != std::string::npos) li.layer.lineType = "Dashed";
        else if (up.find("DOT") != std::string::npos) li.layer.lineType = "Dotted";
        else li.layer.lineType = "Continuous";
        // Transparency (XDATA AcCmTransparency 1071 = 0x02000000 | alpha).
        bool tr = false;
        bool ours = false;
        for (std::size_t k = 0; k < r.xdata.size(); ++k) {
            const auto& x = r.xdata[k];
            if (x.code == 1001) {
                tr = x.value == "AcCmTransparency";
                ours = x.value == "OPENMINE";
            } else if (tr && x.code == 1071) {
                const auto v = static_cast<std::uint32_t>(static_cast<long long>(x.num()));
                if (v & 0x02000000u) li.layer.transparency = 1.0 - static_cast<double>(v & 0xFF) / 255.0;
            } else if (ours && x.code == 1000 && x.value == "lineWidth" && k + 1 < r.xdata.size()) li.layer.lineWidth = r.xdata[k + 1].num();
            else if (ours && x.code == 1000 && x.value == "description" && k + 1 < r.xdata.size())
                li.layer.description = decodeText(r.xdata[k + 1].value, cp1252_);
        }
        layers_[decodeText(r.str(2, "0"), cp1252_)] = std::move(li);
    }

    void readBlocks(const std::vector<Group>& gs, std::size_t& i) {
        while (i < gs.size() && !(gs[i].code == 0 && gs[i].value == "ENDSEC")) {
            if (gs[i].code == 0 && gs[i].value == "BLOCK") {
                Entity head;
                ++i;
                while (i < gs.size() && gs[i].code != 0) head.g.push_back(gs[i++]);
                Block b;
                b.name = head.str(2);
                b.base = head.point(10);
                b.entities = collectEntities(gs, i, {"ENDBLK", "ENDSEC"});
                if (i < gs.size() && gs[i].value == "ENDBLK") {
                    ++i;
                    while (i < gs.size() && gs[i].code != 0) ++i;
                }
                blocks_[b.name] = std::move(b);
                continue;
            }
            ++i;
        }
    }

    // ---- Attributes ---------------------------------------------------------------------------
    std::string layerFor(const Entity& e, const Ctx& c) {
        std::string name = decodeText(e.str(8, "0"), cp1252_);
        if (c.inBlock && name == "0" && !c.insertLayer.empty()) name = c.insertLayer;
        return name;
    }
    LayerInfo& layerInfo(const std::string& name) {
        auto it = layers_.find(name);
        if (it == layers_.end()) {
            LayerInfo li;
            li.layer.id = Uuid::generate();
            li.layer.name = opt_.layerPrefix + name;
            li.layer.colorRgb = 0xFFFFFF;
            it = layers_.emplace(name, std::move(li)).first;
        }
        return it->second;
    }
    std::optional<std::uint32_t> colorFor(const Entity& e, const Ctx& c) {
        if (e.has(420)) return static_cast<std::uint32_t>(e.integer(420)) & 0xFFFFFF;
        const int aci = e.integer(62, 256);
        if (aci == 0) return c.insertColor; // BYBLOCK
        if (aci == 256 || aci == 257) {
            // BYLAYER of an entity on layer 0 inside a block takes the insert's colour if it has one.
            return std::nullopt;
        }
        return aciToRgb(std::abs(aci));
    }
    std::optional<double> transparencyFor(const Entity& e) {
        if (!e.has(440)) return std::nullopt;
        const auto v = static_cast<std::uint32_t>(static_cast<long long>(e.num(440)));
        if (!(v & 0x02000000u)) return std::nullopt; // BYLAYER / BYBLOCK
        return 1.0 - static_cast<double>(v & 0xFF) / 255.0;
    }

    /// Applies the context transform; converts curves to polylines if the transform would distort them.
    void emit(GeometryObject g, const Entity& e, const Ctx& c) {
        const std::string lname = layerFor(e, c);
        auto& li = layerInfo(lname);
        if (!opt_.includeHiddenLayers && (li.frozen || li.off)) {
            skip(e.type);
            return;
        }
        GeometryObject t = g;
        if (!geometry::transformGeometry(t, c.T)) {
            if (geometry::isCurve(g.type)) {
                GeometryObject pl;
                pl.type = GeometryType::Polyline;
                // Tolerance in block units so that the deviation after the largest axis scale is within tolerance.
                const double maxScale = std::max({c.T.applyVector({1, 0, 0}).length(), c.T.applyVector({0, 1, 0}).length(),
                                                  c.T.applyVector({0, 0, 1}).length(), 1e-12});
                pl.vertices = geometry::curvePolyline(g, opt_.chordTolerance / maxScale);
                pl.closed = g.type == GeometryType::Circle || g.closed;
                if (pl.closed && pl.vertices.size() > 2 && distance(pl.vertices.front(), pl.vertices.back()) < 1e-12) pl.vertices.pop_back();
                t = pl;
                if (!geometry::transformGeometry(t, c.T)) {
                    skip(e.type, "could not be transformed");
                    return;
                }
                warn(e.type + " in a non-uniformly scaled block reference was converted to a polyline");
            } else if (g.type == GeometryType::Hatch && !g.bulges.empty()) {
                skip(e.type, "could not be transformed");
                return;
            } else {
                // Text under non-uniform scale: place it, keep its height (average scale).
                for (auto& v : t.vertices) v = c.T.apply(v);
                warn(e.type + " in a non-uniformly scaled block reference keeps an averaged height");
            }
        }
        t.id = Uuid{};
        t.layerId = li.layer.id;
        t.colorOverride = colorFor(e, c);
        if (auto tr = transparencyFor(e)) t.transparency = tr;
        if (t.name.empty()) t.name = decodeText(e.xname(), cp1252_);
        li.used = true;
        ++res_.imported[e.type];
        res_.objects.push_back(std::move(t));
    }

    // ---- Entities ------------------------------------------------------------------------------
    void convert(const Entity& e, const Ctx& c) {
        if (e.integer(67, 0) == 1) { // paper space
            skip(e.type, "paper-space entities are not imported");
            return;
        }
        try {
            const auto& t = e.type;
            if (t == "LINE") line(e, c);
            else if (t == "POINT") point(e, c);
            else if (t == "CIRCLE" || t == "ARC") arc(e, c);
            else if (t == "ELLIPSE") ellipse(e, c);
            else if (t == "LWPOLYLINE") lwpolyline(e, c);
            else if (t == "POLYLINE") polyline(e, c);
            else if (t == "SPLINE") spline(e, c);
            else if (t == "TEXT" || t == "ATTRIB") text(e, c);
            else if (t == "ATTDEF") {
                // Attribute definitions are templates: inside a block the INSERT's ATTRIBs carry the values.
                if (!c.inBlock) text(e, c);
            }
            else if (t == "MTEXT") mtext(e, c);
            else if (t == "INSERT") insert(e, c);
            else if (t == "3DFACE") face3d(e, c);
            else if (t == "SOLID" || t == "TRACE") solid2d(e, c);
            else if (t == "HATCH") hatch(e, c);
            else if (t == "DIMENSION") dimension(e, c);
            else if (t == "MESH") subdMesh(e, c);
            else skip(t, "entity type is not supported by the importer");
        } catch (const DxfError& ex) {
            skip(e.type, ex.what());
        }
    }

    void line(const Entity& e, const Ctx& c) {
        if (const auto* k = e.xkey("kind"); k && k->value == "ElevationLeader") return; // regenerated from the annotation
        GeometryObject g;
        g.type = GeometryType::Line;
        g.vertices = {e.point(10), e.point(11)};
        if (distance(g.vertices[0], g.vertices[1]) < 1e-12) {
            skip("LINE", "zero-length lines are skipped");
            return;
        }
        emit(std::move(g), e, c);
    }

    void point(const Entity& e, const Ctx& c) {
        GeometryObject g;
        g.type = GeometryType::Point;
        g.vertices = {e.point(10)};
        emit(std::move(g), e, c);
    }

    void arc(const Entity& e, const Ctx& c) {
        const Vec3 n = e.extrusion();
        const auto T = ocsToWcs(n);
        GeometryObject g;
        g.type = e.type == "CIRCLE" ? GeometryType::Circle : GeometryType::Arc;
        g.arc.center = T.apply(e.point(10));
        g.arc.radius = e.num(40);
        g.arc.normal = n.normalized();
        g.arc.refDir = ocsXAxis(n);
        if (!(g.arc.radius > 0)) throw DxfError("radius must be positive");
        if (g.type == GeometryType::Circle) {
            g.arc.startAngle = 0;
            g.arc.endAngle = 2 * kPi;
        } else {
            double s = e.num(50) * kDeg, en = e.num(51) * kDeg;
            while (en <= s) en += 2 * kPi;
            while (en - s > 2 * kPi + 1e-12) en -= 2 * kPi;
            g.arc.startAngle = s;
            g.arc.endAngle = en;
        }
        emit(std::move(g), e, c);
    }

    std::vector<Vec3> ellipsePoints(Vec3 ctr, Vec3 major, Vec3 n, double ratio, double t0, double t1, double tol) const {
        const Vec3 minor = cross(n.normalized(), major) * ratio;
        const double a = major.length();
        while (t1 <= t0) t1 += 2 * kPi;
        const double step = a > tol ? 2 * std::acos(std::max(0.0, 1 - tol / a)) : kPi / 4;
        const int segs = std::clamp(static_cast<int>(std::ceil((t1 - t0) / std::max(step, 1e-4))), 4, 20000);
        std::vector<Vec3> pts;
        for (int k = 0; k <= segs; ++k) {
            const double t = t0 + (t1 - t0) * k / segs;
            pts.push_back(ctr + major * std::cos(t) + minor * std::sin(t));
        }
        return pts;
    }

    void ellipse(const Entity& e, const Ctx& c) {
        const Vec3 ctr = e.point(10), major = e.point(11), n = e.extrusion();
        const double ratio = e.num(40, 1.0);
        double t0 = e.num(41, 0.0), t1 = e.num(42, 2 * kPi);
        if (major.length() <= 0 || ratio <= 0) throw DxfError("invalid ellipse axes");
        const bool full = std::abs(std::abs(t1 - t0) - 2 * kPi) < 1e-9;
        if (std::abs(ratio - 1.0) < 1e-12) { // a circle or circular arc
            GeometryObject g;
            g.type = full ? GeometryType::Circle : GeometryType::Arc;
            g.arc.center = ctr;
            g.arc.radius = major.length();
            g.arc.normal = n.normalized();
            g.arc.refDir = major.normalized();
            while (t1 <= t0) t1 += 2 * kPi;
            g.arc.startAngle = full ? 0 : t0;
            g.arc.endAngle = full ? 2 * kPi : t1;
            emit(std::move(g), e, c);
            return;
        }
        GeometryObject g;
        g.type = GeometryType::Polyline;
        g.vertices = ellipsePoints(ctr, major, n, ratio, t0, t1, opt_.chordTolerance / std::max(c.scale, 1e-12));
        if (full) {
            g.vertices.pop_back();
            g.closed = true;
        }
        warn("ELLIPSE entities are imported as polylines (chord tolerance " + formatReal(opt_.chordTolerance) + " m)");
        emit(std::move(g), e, c);
    }

    /// Builds a polyline object from OCS vertices with bulges.
    GeometryObject ocsPolyline(std::vector<Vec3> pts, std::vector<double> bulges, bool closed, Vec3 n, double tolUnits) {
        GeometryObject g;
        g.type = GeometryType::Polyline;
        g.closed = closed;
        bool anyBulge = false;
        for (double b : bulges) anyBulge = anyBulge || std::abs(b) > 1e-15;
        const Vec3 nn = n.normalized();
        if (nn.z > 1 - 1e-12) {
            g.vertices = std::move(pts);
            if (anyBulge) g.bulges = std::move(bulges);
        } else if (nn.z < -1 + 1e-12) {
            const auto T = ocsToWcs(nn);
            for (auto& p : pts) p = T.apply(p);
            g.vertices = std::move(pts);
            if (anyBulge) {
                for (auto& b : bulges) b = -b;
                g.bulges = std::move(bulges);
            }
        } else {
            if (anyBulge) {
                pts = geometry::expandBulges(pts, bulges, closed, tolUnits);
                if (closed && pts.size() > 2 && distance(pts.front(), pts.back()) < 1e-12) pts.pop_back();
                warn("Bulged polylines with a tilted extrusion direction are imported with arcs tessellated");
            }
            const auto T = ocsToWcs(nn);
            for (auto& p : pts) p = T.apply(p);
            g.vertices = std::move(pts);
        }
        return g;
    }

    void lwpolyline(const Entity& e, const Ctx& c) {
        std::vector<Vec3> pts;
        std::vector<double> bulges;
        const double elev = e.num(38, 0.0);
        for (const auto& x : e.g) {
            if (x.code == 10) {
                pts.push_back({x.num(), 0, elev});
                bulges.push_back(0);
            } else if (x.code == 20 && !pts.empty()) pts.back().y = x.num();
            else if (x.code == 42 && !bulges.empty()) bulges.back() = x.num();
        }
        if (pts.size() < 2) throw DxfError("polyline with fewer than two vertices");
        const bool closed = (e.integer(70, 0) & 1) != 0;
        auto g = ocsPolyline(std::move(pts), std::move(bulges), closed, e.extrusion(), opt_.chordTolerance / std::max(c.scale, 1e-12));
        emit(std::move(g), e, c);
    }

    void polyline(const Entity& e, const Ctx& c) {
        const int flags = e.integer(70, 0);
        if (flags & 64) return polyface(e, c);
        if (flags & 16) return polygonMesh(e, c);
        std::vector<Vec3> pts;
        std::vector<double> bulges;
        const double elev = e.point(10).z;
        for (const auto& v : e.children) {
            const int vf = v.integer(70, 0);
            if (vf & 16) continue; // spline frame control point
            Vec3 p = v.point(10);
            if (!(flags & 8)) p.z = elev;
            pts.push_back(p);
            bulges.push_back(v.num(42, 0.0));
        }
        if (pts.size() < 2) throw DxfError("polyline with fewer than two vertices");
        const bool closed = (flags & 1) != 0;
        if (flags & 8) {
            GeometryObject g;
            g.type = GeometryType::Polyline;
            g.vertices = std::move(pts);
            g.closed = closed;
            emit(std::move(g), e, c);
            return;
        }
        auto g = ocsPolyline(std::move(pts), std::move(bulges), closed, e.extrusion(), opt_.chordTolerance / std::max(c.scale, 1e-12));
        emit(std::move(g), e, c);
    }

    void emitMesh(std::vector<Vec3> verts, std::vector<std::array<std::uint32_t, 3>> tris, const Entity& e, const Ctx& c) {
        if (tris.empty()) throw DxfError("mesh without faces");
        auto m = std::make_shared<MeshData>();
        m->vertices = std::move(verts);
        m->triangles = std::move(tris);
        GeometryObject g;
        g.type = GeometryType::Mesh;
        g.mesh = std::move(m);
        emit(std::move(g), e, c);
    }

    void polyface(const Entity& e, const Ctx& c) {
        std::vector<Vec3> verts;
        std::vector<std::array<std::uint32_t, 3>> tris;
        std::vector<std::array<int, 4>> faces;
        for (const auto& v : e.children) {
            const int vf = v.integer(70, 0);
            if ((vf & 128) && (vf & 64)) verts.push_back(v.point(10));
            else if (vf & 128) faces.push_back({std::abs(v.integer(71, 0)), std::abs(v.integer(72, 0)), std::abs(v.integer(73, 0)), std::abs(v.integer(74, 0))});
        }
        const auto nv = static_cast<int>(verts.size());
        for (const auto& f : faces) {
            auto ok = [&](int k) { return k >= 1 && k <= nv; };
            if (!ok(f[0]) || !ok(f[1]) || !ok(f[2])) continue;
            tris.push_back({static_cast<std::uint32_t>(f[0] - 1), static_cast<std::uint32_t>(f[1] - 1), static_cast<std::uint32_t>(f[2] - 1)});
            if (ok(f[3]) && f[3] != f[2])
                tris.push_back({static_cast<std::uint32_t>(f[0] - 1), static_cast<std::uint32_t>(f[2] - 1), static_cast<std::uint32_t>(f[3] - 1)});
        }
        emitMesh(std::move(verts), std::move(tris), e, c);
    }

    void polygonMesh(const Entity& e, const Ctx& c) {
        const int flags = e.integer(70, 0);
        std::vector<Vec3> verts;
        for (const auto& v : e.children)
            if (!(v.integer(70, 0) & 16)) verts.push_back(v.point(10));
        int M = e.integer(71, 0), N = e.integer(72, 0);
        if (static_cast<int>(verts.size()) != M * N) {
            const int M2 = e.integer(73, 0), N2 = e.integer(74, 0);
            if (static_cast<int>(verts.size()) == M2 * N2) {
                M = M2;
                N = N2;
            } else throw DxfError("polygon mesh vertex count does not match its M × N size");
        }
        const bool closedM = (flags & 1) != 0, closedN = (flags & 32) != 0;
        std::vector<std::array<std::uint32_t, 3>> tris;
        auto id = [&](int m, int n) { return static_cast<std::uint32_t>((m % M) * N + (n % N)); };
        for (int m = 0; m < (closedM ? M : M - 1); ++m)
            for (int n = 0; n < (closedN ? N : N - 1); ++n) {
                tris.push_back({id(m, n), id(m + 1, n), id(m + 1, n + 1)});
                tris.push_back({id(m, n), id(m + 1, n + 1), id(m, n + 1)});
            }
        emitMesh(std::move(verts), std::move(tris), e, c);
    }

    void subdMesh(const Entity& e, const Ctx& c) {
        // AcDbSubDMesh: 91 subdivision level, 92 vertex count, 10 vertices, 93 face-list size, 90 entries.
        std::vector<Vec3> verts;
        std::vector<int> faceList;
        int stage = 0;
        for (const auto& x : e.g) {
            if (x.code == 92) stage = 1;
            else if (x.code == 93) stage = 2;
            else if (x.code == 94) stage = 3;
            else if (stage == 1 && x.code == 10) verts.push_back({x.num(), 0, 0});
            else if (stage == 1 && x.code == 20 && !verts.empty()) verts.back().y = x.num();
            else if (stage == 1 && x.code == 30 && !verts.empty()) verts.back().z = x.num();
            else if (stage == 2 && x.code == 90) faceList.push_back(x.integer());
        }
        std::vector<std::array<std::uint32_t, 3>> tris;
        for (std::size_t k = 0; k < faceList.size();) {
            const int n = faceList[k++];
            if (n < 3 || k + static_cast<std::size_t>(n) > faceList.size()) break;
            for (int j = 1; j + 1 < n; ++j)
                tris.push_back({static_cast<std::uint32_t>(faceList[k]), static_cast<std::uint32_t>(faceList[k + static_cast<std::size_t>(j)]),
                                static_cast<std::uint32_t>(faceList[k + static_cast<std::size_t>(j) + 1])});
            k += static_cast<std::size_t>(n);
        }
        for (const auto& t : tris)
            for (auto v : t)
                if (v >= verts.size()) throw DxfError("mesh face index out of range");
        if (e.integer(91, 0) > 0) warn("MESH subdivision levels are ignored; the control cage is imported");
        emitMesh(std::move(verts), std::move(tris), e, c);
    }

    void spline(const Entity& e, const Ctx& c) {
        const int flags = e.integer(70, 0);
        Nurbs nb;
        nb.degree = e.integer(71, 3);
        std::vector<Vec3> fit;
        for (const auto& x : e.g) {
            if (x.code == 40) nb.knots.push_back(x.num());
            else if (x.code == 41) nb.weights.push_back(x.num());
            else if (x.code == 10) nb.ctrl.push_back({x.num(), 0, 0});
            else if (x.code == 20 && !nb.ctrl.empty()) nb.ctrl.back().y = x.num();
            else if (x.code == 30 && !nb.ctrl.empty()) nb.ctrl.back().z = x.num();
            else if (x.code == 11) fit.push_back({x.num(), 0, 0});
            else if (x.code == 21 && !fit.empty()) fit.back().y = x.num();
            else if (x.code == 31 && !fit.empty()) fit.back().z = x.num();
        }
        if (!nb.weights.empty() && nb.weights.size() != nb.ctrl.size()) nb.weights.clear();
        const bool closed = (flags & 1) != 0;
        const double tolU = opt_.chordTolerance / std::max(c.scale, 1e-12);
        const bool haveNurbs = nb.degree >= 1 && nb.ctrl.size() > static_cast<std::size_t>(nb.degree) &&
                               nb.knots.size() == nb.ctrl.size() + static_cast<std::size_t>(nb.degree) + 1;
        if (!haveNurbs) {
            if (fit.size() < 2) throw DxfError("spline has neither a valid control net nor fit points");
            GeometryObject g;
            g.type = GeometryType::Spline;
            g.vertices = fit;
            g.closed = closed && fit.size() > 2;
            if (g.closed && distance(g.vertices.front(), g.vertices.back()) < 1e-12) g.vertices.pop_back();
            warn("SPLINE defined only by fit points: imported as an interpolating spline through them (end tangents not preserved)");
            emit(std::move(g), e, c);
            return;
        }
        const auto curve = nb.sample(tolU * 0.5);
        if (fit.size() >= 2) {
            GeometryObject s;
            s.type = GeometryType::Spline;
            s.vertices = fit;
            s.closed = closed && fit.size() > 2;
            if (s.closed && distance(s.vertices.front(), s.vertices.back()) < 1e-12) s.vertices.pop_back();
            const auto ours = geometry::splinePoints(s.vertices, s.closed, tolU * 0.5);
            double dev = 0;
            for (const auto& p : ours) dev = std::max(dev, pointPolylineDistance(p, curve));
            for (const auto& p : curve) dev = std::max(dev, pointPolylineDistance(p, ours));
            if (dev <= std::max(tolU, 1e-9)) {
                emit(std::move(s), e, c);
                return;
            }
        }
        GeometryObject g;
        g.type = GeometryType::Polyline;
        g.vertices = curve;
        g.closed = closed;
        if (g.closed && g.vertices.size() > 2 && distance(g.vertices.front(), g.vertices.back()) < 1e-9) g.vertices.pop_back();
        warn("SPLINE entities that are not interpolating Catmull–Rom curves are imported as polylines (chord tolerance " +
             formatReal(opt_.chordTolerance) + " m)");
        emit(std::move(g), e, c);
    }

    static TextAlign alignFrom72(int h) { return h == 1 || h == 4 ? TextAlign::Center : (h == 2 ? TextAlign::Right : TextAlign::Left); }

    void text(const Entity& e, const Ctx& c) {
        if ((e.type == "ATTRIB" || e.type == "ATTDEF") && (e.integer(70, 0) & 1)) return; // invisible attribute
        // OpenMine elevation annotations are written as TEXT with XDATA.
        if (const auto* k = e.xkey("kind"); k && k->value == "ElevationDimension") {
            GeometryObject d;
            d.type = GeometryType::Dimension;
            d.dimension.kind = DimensionKind::Elevation;
            Vec3 p = e.point(10);
            for (std::size_t i = 0; i + 2 < e.xdata.size(); ++i)
                if (e.xdata[i].code == 1010) p = {e.xdata[i].num(), e.xdata[i + 1].num(), e.xdata[i + 2].num()};
            d.vertices = {p};
            if (const auto* pr = e.xkey("precision")) d.dimension.precision = pr->integer();
            if (const auto* th = e.xkey("textHeight")) d.dimension.textHeight = th->num();
            if (const auto* ov = e.xkey("overrideText")) d.dimension.overrideText = decodeText(ov->value, cp1252_);
            emit(std::move(d), e, c);
            return;
        }
        const Vec3 n = e.extrusion();
        const auto T = ocsToWcs(n);
        const int h = e.integer(72, 0), v = e.integer(73, 0);
        const bool useSecond = (h != 0 && h != 3 && h != 5) || v != 0;
        Vec3 p = useSecond && e.has(11) ? e.point(11) : e.point(10);
        if (e.type == "ATTRIB" && !e.has(30)) p.z = 0;
        double rot = e.num(50, 0.0) * kDeg;
        if (n.normalized().z < 1 - 1e-12) {
            p = T.apply(p);
            const Vec3 dir = T.applyVector({std::cos(rot), std::sin(rot), 0});
            rot = std::atan2(dir.y, dir.x);
        }
        GeometryObject g;
        g.type = GeometryType::Text;
        g.vertices = {p};
        g.text.text = stripMText(decodeText(e.type == "ATTDEF" ? e.str(2) : e.str(1), cp1252_));
        g.text.height = e.num(40, 1.0);
        g.text.rotation = rot;
        g.text.align = alignFrom72(h);
        if (g.text.text.empty()) return;
        emit(std::move(g), e, c);
    }

    void mtext(const Entity& e, const Ctx& c) {
        std::string s;
        for (const auto& x : e.g)
            if (x.code == 3) s += x.value;
        s += e.str(1);
        GeometryObject g;
        g.type = GeometryType::Text;
        g.vertices = {e.point(10)};
        g.text.text = stripMText(decodeText(s, cp1252_));
        g.text.height = e.num(40, 1.0);
        double rot = e.num(50, 0.0) * kDeg; // AutoCAD writes MTEXT rotation in degrees
        if (e.has(11)) {
            const Vec3 d = e.point(11);
            rot = std::atan2(d.y, d.x);
        }
        g.text.rotation = rot;
        const int att = e.integer(71, 1);
        const int col = (att - 1) % 3;
        g.text.align = col == 1 ? TextAlign::Center : (col == 2 ? TextAlign::Right : TextAlign::Left);
        if (g.text.text.empty()) return;
        emit(std::move(g), e, c);
    }

    void insert(const Entity& e, const Ctx& c) {
        if (!opt_.expandInserts) {
            skip("INSERT", "block references are not expanded (option)");
            return;
        }
        if (c.depth >= 32) throw DxfError("block nesting deeper than 32 levels (recursive block?)");
        const std::string bname = e.str(2);
        const auto it = blocks_.find(bname);
        if (it == blocks_.end()) throw DxfError("block '" + bname + "' is not defined");
        const Block& b = it->second;
        const Vec3 n = e.extrusion();
        const Vec3 ins = e.point(10);
        const double sx = e.num(41, 1.0), sy = e.num(42, 1.0), sz = e.num(43, 1.0);
        const double rot = e.num(50, 0.0) * kDeg;
        const int cols = std::max(1, e.integer(70, 1)), rows = std::max(1, e.integer(71, 1));
        const double cs = e.num(44, 0.0), rs = e.num(45, 0.0);
        Ctx sub;
        sub.depth = c.depth + 1;
        sub.inBlock = true;
        sub.insertLayer = layerFor(e, c);
        sub.insertColor = colorFor(e, c);
        if (!sub.insertColor) sub.insertColor = layerInfo(sub.insertLayer).layer.colorRgb;
        const double uni = std::cbrt(std::abs(sx * sy * sz));
        for (int r = 0; r < rows; ++r)
            for (int k = 0; k < cols; ++k) {
                const Transform3 local = ocsToWcs(n) * Transform3::translation(ins) * Transform3::rotation({0, 0, 1}, rot) *
                                         Transform3::translation({k * cs, r * rs, 0}) * Transform3::scaling({sx, sy, sz}) *
                                         Transform3::translation(-b.base);
                sub.T = c.T * local;
                sub.scale = c.scale * uni;
                for (const auto& be : b.entities) convert(be, sub);
            }
        ++res_.imported["INSERT"];
        // Attributes are already placed in world (OCS) coordinates; like block entities, those on layer 0
        // and with BYBLOCK colour take the insert's layer and colour.
        Ctx attrCtx = c;
        attrCtx.inBlock = true;
        attrCtx.insertLayer = sub.insertLayer;
        attrCtx.insertColor = sub.insertColor;
        for (const auto& a : e.children) convert(a, attrCtx);
    }

    void face3d(const Entity& e, const Ctx& c) {
        const Vec3 p0 = e.point(10), p1 = e.point(11), p2 = e.point(12), p3 = e.point(13, p2);
        auto addTri = [&](Vec3 a, Vec3 b, Vec3 d) {
            const Vec3 A = c.T.apply(a), B = c.T.apply(b), D = c.T.apply(d);
            if (cross(B - A, D - A).length() < 1e-14) return;
            faces_[{layerFor(e, c), colorFor(e, c)}].push_back({A, B, D});
        };
        addTri(p0, p1, p2);
        if (distance(p3, p2) > 1e-12) addTri(p0, p2, p3);
        ++res_.imported["3DFACE"];
        layerInfo(layerFor(e, c)).used = true;
    }

    /// 3DFACEs are merged per layer and colour into one mesh object each (typical for exported DTMs).
    void flushFaces() {
        for (auto& [key, tris] : faces_) {
            if (tris.empty()) continue;
            auto m = std::make_shared<MeshData>();
            std::map<std::array<double, 3>, std::uint32_t> index;
            for (const auto& t : tris) {
                std::array<std::uint32_t, 3> f{};
                for (int k = 0; k < 3; ++k) {
                    const std::array<double, 3> kk{t[static_cast<std::size_t>(k)].x, t[static_cast<std::size_t>(k)].y, t[static_cast<std::size_t>(k)].z};
                    auto [it, inserted] = index.emplace(kk, static_cast<std::uint32_t>(m->vertices.size()));
                    if (inserted) m->vertices.push_back(t[static_cast<std::size_t>(k)]);
                    f[static_cast<std::size_t>(k)] = it->second;
                }
                m->triangles.push_back(f);
            }
            GeometryObject g;
            g.type = GeometryType::Mesh;
            g.name = "3DFACE surface (" + key.layer + ")";
            g.mesh = std::move(m);
            auto& li = layerInfo(key.layer);
            g.layerId = li.layer.id;
            g.colorOverride = key.color;
            li.used = true;
            res_.objects.push_back(std::move(g));
        }
        faces_.clear();
    }

    void solid2d(const Entity& e, const Ctx& c) {
        const auto T = ocsToWcs(e.extrusion());
        Vec3 p[4] = {e.point(10), e.point(11), e.point(12), e.point(13, e.point(12))};
        // SOLID vertex order is 1-2-4-3.
        std::vector<Vec3> loop = {p[0], p[1], p[3], p[2]};
        if (distance(p[3], p[2]) < 1e-12) loop = {p[0], p[1], p[2]};
        for (auto& v : loop) v = T.apply(v);
        GeometryObject g;
        g.type = GeometryType::Hatch;
        g.vertices = loop;
        g.hatch.loopStarts = {0};
        g.hatch.pattern = "SOLID";
        emit(std::move(g), e, c);
    }

    // HATCH: sequential parse of boundary paths and pattern definition lines.
    void hatch(const Entity& e, const Ctx& c) {
        const auto& G = e.g;
        std::size_t i = 0;
        auto at = [&](int code) { return i < G.size() && G[i].code == code; };
        auto seek = [&](int code) {
            while (i < G.size() && G[i].code != code) ++i;
            return i < G.size();
        };
        const Vec3 n = e.extrusion();
        const double elev = e.point(10).z;
        const double tolU = opt_.chordTolerance / std::max(c.scale, 1e-12);
        std::vector<std::vector<Vec3>> loops;
        if (!seek(91)) throw DxfError("hatch without boundary paths");
        const int paths = G[i++].integer();
        for (int pth = 0; pth < paths; ++pth) {
            if (!seek(92)) throw DxfError("truncated hatch boundary");
            const int type = G[i++].integer();
            std::vector<Vec3> loop;
            if (type & 2) { // polyline path
                bool hasBulge = false, closed = true;
                int count = 0;
                while (i < G.size() && G[i].code != 10) {
                    if (G[i].code == 72) hasBulge = G[i].integer() != 0;
                    else if (G[i].code == 73) closed = G[i].integer() != 0;
                    else if (G[i].code == 93) count = G[i].integer();
                    ++i;
                }
                (void)closed;
                std::vector<Vec3> pts;
                std::vector<double> bul;
                for (int k = 0; k < count && i < G.size(); ++k) {
                    Vec3 p{G[i].num(), 0, elev};
                    ++i;
                    if (at(20)) p.y = G[i++].num();
                    double b = 0;
                    if (hasBulge && at(42)) b = G[i++].num();
                    pts.push_back(p);
                    bul.push_back(b);
                }
                loop = geometry::expandBulges(pts, bul, true, tolU);
            } else { // edge path
                if (!seek(93)) throw DxfError("truncated hatch edge path");
                const int edges = G[i++].integer();
                for (int k = 0; k < edges; ++k) {
                    if (!seek(72)) throw DxfError("truncated hatch edge");
                    const int et = G[i++].integer();
                    std::vector<Vec3> seg;
                    if (et == 1) {
                        Vec3 a, b;
                        a.x = G[i++].num(); a.y = G[i++].num();
                        b.x = G[i++].num(); b.y = G[i++].num();
                        seg = {a, b};
                    } else if (et == 2) {
                        Vec3 ctr;
                        ctr.x = G[i++].num(); ctr.y = G[i++].num();
                        const double r = G[i++].num();
                        // Clockwise arcs store mirrored angles: the point at stored angle a is at -a.
                        const double a0 = G[i++].num() * kDeg;
                        double a1 = G[i++].num() * kDeg;
                        const bool ccw = at(73) ? G[i++].integer() != 0 : true;
                        while (a1 <= a0) a1 += 2 * kPi;
                        const int segs = std::max(2, static_cast<int>(geometry::arcSegmentCount(r, a1 - a0, tolU)));
                        for (int s = 0; s <= segs; ++s) {
                            const double a = a0 + (a1 - a0) * s / segs;
                            seg.push_back({ctr.x + r * std::cos(a), ctr.y + r * std::sin(a) * (ccw ? 1 : -1), 0});
                        }
                    } else if (et == 3) {
                        Vec3 ctr, maj;
                        ctr.x = G[i++].num(); ctr.y = G[i++].num();
                        maj.x = G[i++].num(); maj.y = G[i++].num();
                        const double ratio = G[i++].num();
                        const double a0 = G[i++].num() * kDeg, a1 = G[i++].num() * kDeg;
                        const bool ccw = at(73) ? G[i++].integer() != 0 : true;
                        auto pts = ellipsePoints(ctr, maj, {0, 0, ccw ? 1.0 : -1.0}, ratio, a0, a1, tolU);
                        seg = pts;
                    } else if (et == 4) {
                        Nurbs nb;
                        nb.degree = at(94) ? G[i++].integer() : 3;
                        while (i < G.size() && G[i].code != 40) ++i;
                        while (at(40)) nb.knots.push_back(G[i++].num());
                        while (at(10)) {
                            Vec3 p{G[i++].num(), 0, 0};
                            if (at(20)) p.y = G[i++].num();
                            nb.ctrl.push_back(p);
                            if (at(42)) nb.weights.push_back(G[i++].num());
                        }
                        if (nb.weights.size() != nb.ctrl.size()) nb.weights.clear();
                        if (nb.knots.size() != nb.ctrl.size() + static_cast<std::size_t>(nb.degree) + 1) throw DxfError("invalid hatch spline edge");
                        seg = nb.sample(tolU);
                        // skip fit data
                        while (i < G.size() && (G[i].code == 97 || G[i].code == 11 || G[i].code == 21 || G[i].code == 12 || G[i].code == 22 ||
                                                G[i].code == 13 || G[i].code == 23))
                            ++i;
                    } else throw DxfError("unknown hatch edge type " + std::to_string(et));
                    for (auto& p : seg) p.z = elev;
                    if (!loop.empty() && !seg.empty() && horizontalDistance(loop.back(), seg.front()) < 1e-9) seg.erase(seg.begin());
                    loop.insert(loop.end(), seg.begin(), seg.end());
                }
            }
            if (loop.size() > 2 && horizontalDistance(loop.front(), loop.back()) < 1e-9) loop.pop_back();
            if (loop.size() >= 3) loops.push_back(std::move(loop));
            // skip source boundary handles
            if (at(97)) {
                const int nh = G[i++].integer();
                for (int k = 0; k < nh && at(330); ++k) ++i;
            }
        }
        if (loops.empty()) throw DxfError("hatch has no usable boundary");
        GeometryObject g;
        g.type = GeometryType::Hatch;
        for (auto& l : loops) {
            g.hatch.loopStarts.push_back(static_cast<std::uint32_t>(g.vertices.size()));
            g.vertices.insert(g.vertices.end(), l.begin(), l.end());
        }
        const bool solid = e.integer(70, 0) == 1;
        std::string name = e.str(2, "SOLID");
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        // Pattern definition lines (after code 78).
        struct PLine { double angle; Vec3 off; std::vector<double> dashes; };
        std::vector<PLine> plines;
        if (seek(78)) {
            const int nl = G[i++].integer();
            for (int k = 0; k < nl; ++k) {
                if (!seek(53)) break;
                PLine pl{G[i++].num() * kDeg, {}, {}};
                if (at(43)) ++i;
                if (at(44)) ++i;
                if (at(45)) pl.off.x = G[i++].num();
                if (at(46)) pl.off.y = G[i++].num();
                const int nd = at(79) ? G[i++].integer() : 0;
                for (int d = 0; d < nd && at(49); ++d) pl.dashes.push_back(G[i++].num());
                plines.push_back(pl);
            }
        }
        if (const auto* xp = e.xkey("pattern")) name = xp->value; // our own pattern name round-trips exactly
        if (solid || name == "SOLID") g.hatch.pattern = "SOLID";
        else {
            double spacing = 1.0, angle = 0.0;
            if (!plines.empty()) {
                const auto& pl = plines.front();
                const Vec3 dir{std::cos(pl.angle), std::sin(pl.angle), 0};
                spacing = std::abs(dir.x * pl.off.y - dir.y * pl.off.x);
                angle = pl.angle;
            }
            if (!(spacing > 0)) spacing = 1.0;
            if (name == "ANSI31") {
                g.hatch.pattern = "ANSI31";
                angle -= kPi / 4;
            } else if (name == "DOTS" || (plines.size() == 1 && plines[0].dashes.size() == 2 && plines[0].dashes[0] == 0)) {
                g.hatch.pattern = "DOTS";
            } else if (name == "CROSS" || plines.size() == 2) {
                g.hatch.pattern = "CROSS";
            } else {
                g.hatch.pattern = "LINES";
                if (name != "LINES" && name != "_USER") warn("Hatch pattern " + name + " imported as LINES with the spacing and angle of its first pattern line");
            }
            g.hatch.spacing = spacing;
            g.hatch.angle = angle;
        }
        // Boundary is in OCS.
        if (n.normalized().z < 1 - 1e-12) {
            const auto T = ocsToWcs(n);
            for (auto& v : g.vertices) v = T.apply(v);
        }
        emit(std::move(g), e, c);
    }

    void dimension(const Entity& e, const Ctx& c) {
        const int type = e.integer(70, 0) & 7;
        const double rotDeg = e.num(50, 0.0);
        const std::string style = e.str(3, "Standard");
        const DimStyle ds = dimStyles_.count(style) ? dimStyles_[style] : DimStyle{};
        GeometryObject g;
        g.type = GeometryType::Dimension;
        g.dimension.textHeight = ds.textHeight * ds.scale;
        g.dimension.precision = std::clamp(ds.decimals, 0, 8);
        std::string txt = decodeText(e.str(1), cp1252_);
        if (txt != "<>" && !txt.empty()) g.dimension.overrideText = stripMText(txt);
        bool mapped = true;
        const Vec3 p10 = e.point(10), p13 = e.point(13), p14 = e.point(14), p15 = e.point(15), p16 = e.point(16);
        switch (type) {
        case 0: { // rotated / horizontal / vertical
            const double r = std::fmod(std::fmod(rotDeg, 180.0) + 180.0, 180.0);
            if (r < 1e-9 || r > 180 - 1e-9) g.dimension.kind = DimensionKind::Horizontal;
            else if (std::abs(r - 90) < 1e-9) g.dimension.kind = DimensionKind::Vertical;
            else {
                const Vec3 d = p14 - p13;
                const double a = std::fmod(std::fmod(std::atan2(d.y, d.x) / kDeg, 180.0) + 180.0, 180.0);
                if (std::abs(a - r) < 1e-6 && std::abs(d.z) < 1e-9) g.dimension.kind = DimensionKind::Aligned;
                else mapped = false;
            }
            g.vertices = {p13, p14, p10};
            break;
        }
        case 1: g.dimension.kind = DimensionKind::Aligned; g.vertices = {p13, p14, p10}; break;
        case 2: { // two-line angular: lines 13–14 and 10–15, arc through 16
            const Vec3 a = p13, b = p14, cc = p15, d = p10;
            const Vec2 r1 = xy(b - a), r2 = xy(d - cc);
            const double den = cross(r1, r2);
            if (std::abs(den) < 1e-12) { mapped = false; break; }
            const double t = cross(xy(cc - a), r2) / den;
            const Vec3 v = a + (b - a) * t;
            g.dimension.kind = DimensionKind::Angular;
            const Vec3 q1 = horizontalDistance(b, v) > horizontalDistance(a, v) ? b : a;
            const Vec3 q2 = horizontalDistance(d, v) > horizontalDistance(cc, v) ? d : cc;
            g.vertices = {v, q1, q2, p16};
            break;
        }
        case 5: g.dimension.kind = DimensionKind::Angular; g.vertices = {p15, p13, p14, p10}; break;
        case 3: g.dimension.kind = DimensionKind::Diameter; g.vertices = {(p15 + p10) * 0.5, p15}; break;
        case 4: g.dimension.kind = DimensionKind::Radius; g.vertices = {p10, p15}; break;
        default: mapped = false;
        }
        if (const auto* pr = e.xkey("precision")) g.dimension.precision = pr->integer();
        if (const auto* th = e.xkey("textHeight")) g.dimension.textHeight = th->num();
        if (mapped) {
            emit(std::move(g), e, c);
            return;
        }
        // Expand the dimension's anonymous block (its graphics are in WCS).
        const auto it = blocks_.find(e.str(2));
        if (it == blocks_.end()) throw DxfError("dimension type is not supported and its block is missing");
        Ctx sub = c;
        sub.inBlock = true;
        sub.insertLayer = layerFor(e, c);
        sub.insertColor = colorFor(e, c);
        sub.depth = c.depth + 1;
        sub.T = c.T * Transform3::translation(e.point(12));
        for (const auto& be : it->second.entities) convert(be, sub);
        warn("Ordinate and oblique-rotated dimensions are imported as their drawn lines and text");
        ++res_.imported["DIMENSION"];
    }
};

} // namespace

DxfImportResult readDxfString(std::string_view content, const DxfImportOptions& options) {
    Reader r(options);
    return r.run(content);
}

DxfImportResult readDxf(const std::filesystem::path& file, const DxfImportOptions& options) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw DxfError("Cannot open " + file.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return readDxfString(ss.str(), options);
}

} // namespace openmine::formats
