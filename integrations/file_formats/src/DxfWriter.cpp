// SPDX-License-Identifier: Apache-2.0
#include "DxfCommon.h"

#include "openmine/cad/Annotation.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polyline.h"

#include <cmath>
#include <fstream>
#include <map>
#include <numbers>
#include <set>

namespace openmine::formats {

using namespace dxf;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kRad2Deg = 180.0 / kPi;

std::string sanitizeName(std::string n) {
    for (auto& ch : n)
        if (std::string_view("<>/\\\":;?*|=`").find(ch) != std::string_view::npos) ch = '_';
    if (n.empty()) n = "_";
    return encodeText(n);
}

std::string ltypeName(const std::string& t) {
    if (t == "Dashed") return "DASHED";
    if (t == "Dotted") return "DOT";
    if (t == "DashDot") return "DASHDOT";
    return "Continuous";
}

int transparencyValue(double t) {
    const int alpha = static_cast<int>(std::lround(255.0 * (1.0 - std::clamp(t, 0.0, 1.0))));
    return static_cast<int>(0x02000000u | static_cast<unsigned>(alpha));
}

class Writer {
public:
    Writer(const std::vector<CadLayer>& layers, const DxfExportOptions& opt, DxfExportReport& rep) : opt_(opt), rep_(rep) {
        for (const auto& l : layers) layerById_[l.id] = &l;
    }

    std::string run(const std::vector<const GeometryObject*>& objects) {
        // Fixed handles for the structure.
        hBlockRecordTable_ = h();
        hModelBR_ = h();
        hPaperBR_ = h();
        hRootDict_ = h();
        hLayoutDict_ = h();
        hModelLayout_ = h();
        hPaperLayout_ = h();
        hPlotStyleDict_ = h();
        hPlotStyleNormal_ = h();

        // Layers used (plus "0").
        std::set<std::string> layerNames = {"0"};
        for (const auto* g : objects)
            if (g) layerNames.insert(layerName(*g));

        Out ents;
        for (const auto* g : objects) {
            if (!g) continue;
            try {
                entity(ents, *g, hModelBR_);
            } catch (const std::exception& ex) {
                ++rep_.skipped;
                rep_.warnings.push_back("Object '" + g->name + "' was not exported: " + ex.what());
            }
        }

        Out tables;
        writeTables(tables, layerNames);
        Out blocks;
        writeBlocks(blocks);
        Out objs;
        writeObjects(objs);

        Out o;
        writeHeader(o, objects);
        o.g(0, "SECTION");
        o.g(2, "CLASSES");
        o.g(0, "ENDSEC");
        o.g(0, "SECTION");
        o.g(2, "TABLES");
        o.str() += tables.str();
        o.g(0, "ENDSEC");
        o.g(0, "SECTION");
        o.g(2, "BLOCKS");
        o.str() += blocks.str();
        o.g(0, "ENDSEC");
        o.g(0, "SECTION");
        o.g(2, "ENTITIES");
        o.str() += ents.str();
        o.g(0, "ENDSEC");
        o.g(0, "SECTION");
        o.g(2, "OBJECTS");
        o.str() += objs.str();
        o.g(0, "ENDSEC");
        o.g(0, "EOF");
        return std::move(o.str());
    }

private:
    const DxfExportOptions& opt_;
    DxfExportReport& rep_;
    std::map<Uuid, const CadLayer*> layerById_;
    unsigned next_ = 0x20;
    unsigned hBlockRecordTable_ = 0, hModelBR_ = 0, hPaperBR_ = 0, hRootDict_ = 0, hLayoutDict_ = 0, hModelLayout_ = 0,
             hPaperLayout_ = 0, hPlotStyleDict_ = 0, hPlotStyleNormal_ = 0;
    struct AnonBlock {
        std::string name;
        unsigned record = 0;
        Out body; // entities (owner = record)
    };
    std::vector<AnonBlock> anon_;
    BoundingBox extents_ = BoundingBox::empty();

    unsigned h() { return next_++; }

    std::string layerName(const GeometryObject& g) const {
        const auto it = layerById_.find(g.layerId);
        return it == layerById_.end() ? std::string("0") : sanitizeName(it->second->name);
    }
    const CadLayer* layerByName(const std::string& n) const {
        for (const auto& [id, l] : layerById_)
            if (sanitizeName(l->name) == n) return l;
        return nullptr;
    }

    void count(const char* type) { ++rep_.written[type]; }

    void common(Out& o, const char* type, unsigned handle, unsigned owner, const GeometryObject& g, const std::string& layerOverride = {}) {
        o.g(0, type);
        o.handle(5, handle);
        o.handle(330, owner);
        o.g(100, "AcDbEntity");
        o.g(8, layerOverride.empty() ? layerName(g) : layerOverride);
        if (g.colorOverride) {
            o.g(62, rgbToAci(*g.colorOverride));
            o.g(420, static_cast<int>(*g.colorOverride & 0xFFFFFF));
        }
        if (g.transparency) o.g(440, transparencyValue(*g.transparency));
    }

    void xdata(Out& o, const GeometryObject& g, const std::vector<std::pair<std::string, std::string>>& strings = {},
               const std::vector<std::pair<std::string, double>>& reals = {}, const std::vector<std::pair<std::string, int>>& ints = {},
               const Vec3* point = nullptr) {
        o.g(1001, "OPENMINE");
        o.g(1000, encodeText(g.name));
        for (const auto& [k, v] : strings) {
            o.g(1000, k);
            o.g(1000, encodeText(v));
        }
        for (const auto& [k, v] : reals) {
            o.g(1000, k);
            o.g(1040, v);
        }
        for (const auto& [k, v] : ints) {
            o.g(1000, k);
            o.g(1070, v);
        }
        if (point) o.point(1010, *point);
    }

    void expand(const GeometryObject& g) {
        if (auto b = geometry::objectBounds(g)) {
            extents_.expand(b->min);
            extents_.expand(b->max);
        }
    }

    // ---- Entities ------------------------------------------------------------------------------
    void entity(Out& o, const GeometryObject& g, unsigned owner) {
        expand(g);
        switch (g.type) {
        case GeometryType::Point: {
            if (g.vertices.empty()) throw DxfError("point without coordinates");
            common(o, "POINT", h(), owner, g);
            o.g(100, "AcDbPoint");
            o.point(10, g.vertices[0]);
            xdata(o, g);
            count("POINT");
            break;
        }
        case GeometryType::Line: {
            if (g.vertices.size() < 2) throw DxfError("line without two points");
            common(o, "LINE", h(), owner, g);
            o.g(100, "AcDbLine");
            o.point(10, g.vertices[0]);
            o.point(11, g.vertices[1]);
            xdata(o, g);
            count("LINE");
            break;
        }
        case GeometryType::Polyline: polyline(o, g, owner); break;
        case GeometryType::Arc:
        case GeometryType::Circle: arc(o, g, owner); break;
        case GeometryType::Spline: spline(o, g, owner); break;
        case GeometryType::Text: text(o, g, owner, g.vertices.empty() ? Vec3{} : g.vertices[0], g.text, layerName(g)); xdataText(o, g); break;
        case GeometryType::Dimension: dimension(o, g, owner); break;
        case GeometryType::Hatch: hatch(o, g, owner); break;
        case GeometryType::Mesh:
        case GeometryType::Solid:
            if (!opt_.exportMeshes) {
                ++rep_.skipped;
                return;
            }
            if (g.type == GeometryType::Solid)
                rep_.warnings.push_back("Solid '" + g.name + "' was exported as its tessellation (polyface mesh); DXF has no open B-rep format");
            mesh(o, g, owner);
            break;
        }
    }

    void xdataText(Out& o, const GeometryObject& g) { xdata(o, g); }

    void polyline(Out& o, const GeometryObject& g, unsigned owner) {
        if (g.vertices.size() < 2) throw DxfError("polyline with fewer than two vertices");
        bool planar = true;
        for (const auto& v : g.vertices) planar = planar && std::abs(v.z - g.vertices[0].z) < 1e-9;
        bool anyBulge = false;
        for (double b : g.bulges) anyBulge = anyBulge || std::abs(b) > 1e-15;
        if (planar) {
            common(o, "LWPOLYLINE", h(), owner, g);
            o.g(100, "AcDbPolyline");
            o.g(90, static_cast<int>(g.vertices.size()));
            o.g(70, g.closed ? 1 : 0);
            o.g(43, 0.0);
            if (g.vertices[0].z != 0) o.g(38, g.vertices[0].z);
            for (std::size_t i = 0; i < g.vertices.size(); ++i) {
                o.g(10, g.vertices[i].x);
                o.g(20, g.vertices[i].y);
                if (anyBulge && i < g.bulges.size() && std::abs(g.bulges[i]) > 1e-15) o.g(42, g.bulges[i]);
            }
            xdata(o, g);
            count("LWPOLYLINE");
            return;
        }
        std::vector<Vec3> pts = g.vertices;
        if (anyBulge) {
            pts = geometry::expandBulges(g.vertices, g.bulges, g.closed, opt_.chordTolerance);
            if (g.closed && pts.size() > 2 && distance(pts.front(), pts.back()) < 1e-12) pts.pop_back();
            rep_.warnings.push_back("Polyline '" + g.name + "' has arcs and varying elevation: arcs were tessellated (3D polylines have no bulges)");
        }
        const unsigned ph = h();
        common(o, "POLYLINE", ph, owner, g);
        o.g(100, "AcDb3dPolyline");
        o.g(66, 1);
        o.point(10, {0, 0, 0});
        o.g(70, 8 | (g.closed ? 1 : 0));
        xdata(o, g);
        const std::string ln = layerName(g);
        for (const auto& p : pts) {
            o.g(0, "VERTEX");
            o.handle(5, h());
            o.handle(330, ph);
            o.g(100, "AcDbEntity");
            o.g(8, ln);
            o.g(100, "AcDbVertex");
            o.g(100, "AcDb3dPolylineVertex");
            o.point(10, p);
            o.g(70, 32);
        }
        o.g(0, "SEQEND");
        o.handle(5, h());
        o.handle(330, ph);
        o.g(100, "AcDbEntity");
        o.g(8, ln);
        count("POLYLINE");
    }

    void arc(Out& o, const GeometryObject& g, unsigned owner) {
        const ArcData& a = g.arc;
        if (!(a.radius > 0)) throw DxfError("arc without radius");
        const Vec3 n = a.normal.normalized();
        const auto W2O = wcsToOcs(n);
        const Vec3 c = W2O.apply(a.center);
        const Vec3 r = W2O.applyVector(a.refDir);
        const double off = std::atan2(r.y, r.x);
        const bool full = g.type == GeometryType::Circle || std::abs((a.endAngle - a.startAngle) - 2 * kPi) < 1e-12;
        common(o, full ? "CIRCLE" : "ARC", h(), owner, g);
        o.g(100, "AcDbCircle");
        o.point(10, c);
        o.g(40, a.radius);
        if (std::abs(n.z - 1) > 1e-15) o.point(210, n);
        if (!full) {
            o.g(100, "AcDbArc");
            o.g(50, std::fmod((a.startAngle + off) * kRad2Deg + 720.0, 360.0));
            o.g(51, std::fmod((a.endAngle + off) * kRad2Deg + 720.0, 360.0));
        }
        xdata(o, g);
        count(full ? "CIRCLE" : "ARC");
    }

    void spline(Out& o, const GeometryObject& g, unsigned owner) {
        if (g.vertices.size() < 2) throw DxfError("spline with fewer than two fit points");
        const auto spans = geometry::splineBezierSpans(g.vertices, g.closed);
        if (spans.empty()) throw DxfError("spline has no spans");
        std::vector<Vec3> ctrl = {spans[0][0]};
        for (const auto& s : spans) {
            ctrl.push_back(s[1]);
            ctrl.push_back(s[2]);
            ctrl.push_back(s[3]);
        }
        std::vector<double> knots = {0, 0, 0, 0};
        for (std::size_t k = 1; k < spans.size(); ++k)
            for (int m = 0; m < 3; ++m) knots.push_back(static_cast<double>(k));
        for (int m = 0; m < 4; ++m) knots.push_back(static_cast<double>(spans.size()));
        std::vector<Vec3> fit = g.vertices;
        if (g.closed) fit.push_back(g.vertices.front());
        bool planar = true;
        for (const auto& v : ctrl) planar = planar && std::abs(v.z - ctrl[0].z) < 1e-12;
        common(o, "SPLINE", h(), owner, g);
        o.g(100, "AcDbSpline");
        if (planar) o.point(210, {0, 0, 1});
        o.g(70, (g.closed ? 1 : 0) | (planar ? 8 : 0));
        o.g(71, 3);
        o.g(72, static_cast<int>(knots.size()));
        o.g(73, static_cast<int>(ctrl.size()));
        o.g(74, static_cast<int>(fit.size()));
        o.g(42, 1e-10);
        o.g(43, 1e-10);
        o.g(44, 1e-10);
        for (double k : knots) o.g(40, k);
        for (const auto& p : ctrl) o.point(10, p);
        for (const auto& p : fit) o.point(11, p);
        xdata(o, g);
        count("SPLINE");
    }

    void text(Out& o, const GeometryObject& g, unsigned owner, Vec3 p, const TextData& t, const std::string& layer) {
        const bool multi = t.text.find('\n') != std::string::npos || t.text.size() > 250;
        if (multi) {
            common(o, "MTEXT", h(), owner, g, layer);
            o.g(100, "AcDbMText");
            o.point(10, p);
            o.g(40, t.height);
            o.g(71, t.align == TextAlign::Center ? 8 : (t.align == TextAlign::Right ? 9 : 7));
            o.g(72, 1);
            std::string enc = encodeText(t.text);
            // Chunks of 250 characters in code 3, the remainder in code 1 (never split an escape).
            while (enc.size() > 250) {
                std::size_t cut = 250;
                const auto bs = enc.rfind('\\', cut);
                if (bs != std::string::npos && bs + 7 > cut) cut = bs;
                o.g(3, enc.substr(0, cut));
                enc.erase(0, cut);
            }
            o.g(1, enc);
            o.point(11, {std::cos(t.rotation), std::sin(t.rotation), 0});
            count("MTEXT");
            return;
        }
        common(o, "TEXT", h(), owner, g, layer);
        o.g(100, "AcDbText");
        o.point(10, p);
        o.g(40, t.height);
        o.g(1, encodeText(t.text));
        if (t.rotation != 0) o.g(50, std::fmod(t.rotation * kRad2Deg + 720.0, 360.0));
        o.g(7, "Standard");
        const int hj = t.align == TextAlign::Center ? 1 : (t.align == TextAlign::Right ? 2 : 0);
        if (hj) {
            o.g(72, hj);
            o.point(11, p);
        }
        o.g(100, "AcDbText");
        count("TEXT");
    }

    void dimension(Out& o, const GeometryObject& g, unsigned owner) {
        const auto& v = g.vertices;
        const auto kind = g.dimension.kind;
        const auto gr = cad::dimensionGraphics(g);
        std::vector<std::pair<std::string, double>> reals = {{"textHeight", g.dimension.textHeight}};
        std::vector<std::pair<std::string, int>> ints = {{"precision", g.dimension.precision}};
        if (kind == DimensionKind::Elevation) {
            // No DXF equivalent: written as the drawn text and leader with XDATA restoring the annotation.
            if (v.empty()) throw DxfError("elevation annotation without a point");
            TextData t;
            t.text = gr.text;
            t.height = g.dimension.textHeight;
            t.rotation = gr.textRotation;
            text(o, g, owner, gr.textPosition, t, layerName(g));
            std::vector<std::pair<std::string, std::string>> strs = {{"kind", "ElevationDimension"}};
            if (!g.dimension.overrideText.empty()) strs.push_back({"overrideText", g.dimension.overrideText});
            xdata(o, g, strs, reals, ints, &v[0]);
            for (const auto& [a, b] : gr.lines) {
                GeometryObject l = g;
                l.type = GeometryType::Line;
                l.name.clear();
                common(o, "LINE", h(), owner, l);
                o.g(100, "AcDbLine");
                o.point(10, a);
                o.point(11, b);
                o.g(1001, "OPENMINE");
                o.g(1000, "");
                o.g(1000, "kind");
                o.g(1000, "ElevationLeader");
                count("LINE");
            }
            return;
        }
        // Anonymous block with the dimension graphics.
        AnonBlock blk;
        blk.name = "*D" + std::to_string(anon_.size() + 1);
        blk.record = h();
        GeometryObject proto;
        proto.colorOverride = std::nullopt;
        for (const auto& [a, b] : gr.lines) {
            common(blk.body, "LINE", h(), blk.record, proto, "0");
            blk.body.g(100, "AcDbLine");
            blk.body.point(10, a);
            blk.body.point(11, b);
        }
        for (const auto& t : gr.arrows) {
            common(blk.body, "SOLID", h(), blk.record, proto, "0");
            blk.body.g(100, "AcDbTrace");
            blk.body.point(10, t[0]);
            blk.body.point(11, t[1]);
            blk.body.point(12, t[2]);
            blk.body.point(13, t[2]);
        }
        {
            TextData t;
            t.text = gr.text;
            t.height = g.dimension.textHeight;
            t.rotation = gr.textRotation;
            t.align = TextAlign::Center;
            common(blk.body, "MTEXT", h(), blk.record, proto, "0");
            blk.body.g(100, "AcDbMText");
            blk.body.point(10, gr.textPosition);
            blk.body.g(40, t.height);
            blk.body.g(71, 5);
            blk.body.g(1, encodeText(t.text));
            blk.body.point(11, {std::cos(t.rotation), std::sin(t.rotation), 0});
        }
        const std::string bname = blk.name;
        anon_.push_back(std::move(blk));

        int type = 0;
        Vec3 def = {}, p13 = {}, p14 = {}, p15 = {};
        double rot = 0;
        const char* sub = "AcDbAlignedDimension";
        switch (kind) {
        case DimensionKind::Aligned: type = 1; break;
        case DimensionKind::Horizontal: type = 0; rot = 0; break;
        case DimensionKind::Vertical: type = 0; rot = 90; break;
        case DimensionKind::Angular: type = 5; sub = "AcDb3PointAngularDimension"; break;
        case DimensionKind::Radius: type = 4; sub = "AcDbRadialDimension"; break;
        case DimensionKind::Diameter: type = 3; sub = "AcDbDiametricDimension"; break;
        default: break;
        }
        if (kind == DimensionKind::Aligned || kind == DimensionKind::Horizontal || kind == DimensionKind::Vertical) {
            if (v.size() < 3) throw DxfError("linear dimension needs three definition points");
            p13 = v[0];
            p14 = v[1];
            def = v[2];
        } else if (kind == DimensionKind::Angular) {
            if (v.size() < 4) throw DxfError("angular dimension needs four definition points");
            p15 = v[0];
            p13 = v[1];
            p14 = v[2];
            def = v[3];
        } else {
            if (v.size() < 2) throw DxfError("radial dimension needs two definition points");
            p15 = v[1];
            def = kind == DimensionKind::Radius ? v[0] : v[0] * 2.0 - v[1];
        }
        common(o, "DIMENSION", h(), owner, g);
        o.g(100, "AcDbDimension");
        o.g(2, bname);
        o.point(10, def);
        o.point(11, gr.textPosition);
        o.g(70, type | 32);
        if (!g.dimension.overrideText.empty()) o.g(1, encodeText(g.dimension.overrideText));
        o.g(71, 5);
        o.g(42, kind == DimensionKind::Angular ? gr.value * kPi / 180.0 : gr.value); // AutoCAD stores angles in radians
        o.g(3, "Standard");
        o.g(100, sub);
        if (type == 0 || type == 1) {
            o.point(13, p13);
            o.point(14, p14);
            if (type == 0) {
                o.g(50, rot);
                o.g(100, "AcDbRotatedDimension");
            }
        } else if (type == 5) {
            o.point(13, p13);
            o.point(14, p14);
            o.point(15, p15);
        } else {
            o.point(15, p15);
            o.g(40, 0.0);
        }
        xdata(o, g, {}, reals, ints);
        count("DIMENSION");
    }

    void hatch(Out& o, const GeometryObject& g, unsigned owner) {
        const auto& ls = g.hatch.loopStarts;
        if (g.vertices.size() < 3 || ls.empty()) throw DxfError("hatch without a boundary");
        const double elev = g.vertices[0].z;
        for (const auto& v : g.vertices)
            if (std::abs(v.z - elev) > 1e-6) {
                rep_.warnings.push_back("Hatch '" + g.name + "' is not horizontal; it was flattened to elevation " + formatReal(elev));
                break;
            }
        const std::string pat = g.hatch.pattern;
        const bool solid = pat == "SOLID";
        std::string dxfName = solid ? "SOLID" : (pat == "ANSI31" ? "ANSI31" : (pat == "DOTS" ? "DOTS" : "_USER"));
        common(o, "HATCH", h(), owner, g);
        o.g(100, "AcDbHatch");
        o.point(10, {0, 0, elev});
        o.point(210, {0, 0, 1});
        o.g(2, dxfName);
        o.g(70, solid ? 1 : 0);
        o.g(71, 0);
        o.g(91, static_cast<int>(ls.size()));
        for (std::size_t k = 0; k < ls.size(); ++k) {
            const std::size_t b = ls[k], e = k + 1 < ls.size() ? ls[k + 1] : g.vertices.size();
            o.g(92, k == 0 ? 3 : 2);
            o.g(72, 0);
            o.g(73, 1);
            o.g(93, static_cast<int>(e - b));
            for (std::size_t i = b; i < e; ++i) {
                o.g(10, g.vertices[i].x);
                o.g(20, g.vertices[i].y);
            }
            o.g(97, 0);
        }
        o.g(75, 0);
        o.g(76, solid ? 1 : (dxfName == "_USER" ? 0 : (dxfName == "ANSI31" ? 1 : 2)));
        if (!solid) {
            const double a = g.hatch.angle, s = g.hatch.spacing > 0 ? g.hatch.spacing : 1.0;
            struct L {
                double ang;
                std::vector<double> dashes;
            };
            std::vector<L> lines;
            if (pat == "ANSI31") lines = {{a + kPi / 4, {}}};
            else if (pat == "CROSS") lines = {{a, {}}, {a + kPi / 2, {}}};
            else if (pat == "DOTS") lines = {{a, {0.0, -s}}};
            else lines = {{a, {}}};
            o.g(52, std::fmod(a * kRad2Deg + 720.0, 360.0));
            o.g(41, pat == "ANSI31" ? s / 3.175 : 1.0);
            o.g(77, pat == "CROSS" ? 1 : 0);
            o.g(78, static_cast<int>(lines.size()));
            for (const auto& l : lines) {
                o.g(53, std::fmod(l.ang * kRad2Deg + 720.0, 360.0));
                o.g(43, 0.0);
                o.g(44, 0.0);
                o.g(45, -std::sin(l.ang) * s);
                o.g(46, std::cos(l.ang) * s);
                o.g(79, static_cast<int>(l.dashes.size()));
                for (double d : l.dashes) o.g(49, d);
            }
        }
        o.g(98, 0);
        xdata(o, g, {{"pattern", pat}});
        count("HATCH");
    }

    void mesh(Out& o, const GeometryObject& g, unsigned owner) {
        if (!g.mesh || g.mesh->triangles.empty()) throw DxfError("mesh without triangles");
        const auto& m = *g.mesh;
        if (m.vertices.size() <= 32767 && m.triangles.size() <= 32767) {
            const unsigned ph = h();
            common(o, "POLYLINE", ph, owner, g);
            o.g(100, "AcDbPolyFaceMesh");
            o.g(66, 1);
            o.point(10, {0, 0, 0});
            o.g(70, 64);
            o.g(71, static_cast<int>(m.vertices.size()));
            o.g(72, static_cast<int>(m.triangles.size()));
            xdata(o, g);
            const std::string ln = layerName(g);
            for (const auto& v : m.vertices) {
                o.g(0, "VERTEX");
                o.handle(5, h());
                o.handle(330, ph);
                o.g(100, "AcDbEntity");
                o.g(8, ln);
                o.g(100, "AcDbVertex");
                o.g(100, "AcDbPolyFaceMeshVertex");
                o.point(10, v);
                o.g(70, 192);
            }
            for (const auto& t : m.triangles) {
                o.g(0, "VERTEX");
                o.handle(5, h());
                o.handle(330, ph);
                o.g(100, "AcDbEntity");
                o.g(8, ln);
                o.g(100, "AcDbFaceRecord");
                o.point(10, {0, 0, 0});
                o.g(70, 128);
                o.g(71, static_cast<int>(t[0] + 1));
                o.g(72, static_cast<int>(t[1] + 1));
                o.g(73, static_cast<int>(t[2] + 1));
            }
            o.g(0, "SEQEND");
            o.handle(5, h());
            o.handle(330, ph);
            o.g(100, "AcDbEntity");
            o.g(8, ln);
            count("POLYLINE");
            return;
        }
        rep_.warnings.push_back("Mesh '" + g.name + "' exceeds the polyface limit (32767 vertices/faces) and was written as " +
                                std::to_string(m.triangles.size()) + " 3DFACE entities");
        for (const auto& t : m.triangles) {
            common(o, "3DFACE", h(), owner, g);
            o.g(100, "AcDbFace");
            o.point(10, m.vertices[t[0]]);
            o.point(11, m.vertices[t[1]]);
            o.point(12, m.vertices[t[2]]);
            o.point(13, m.vertices[t[2]]);
            count("3DFACE");
        }
    }

    // ---- Structure -------------------------------------------------------------------------------
    void writeHeader(Out& o, const std::vector<const GeometryObject*>&) {
        o.g(999, "Written by " + opt_.producer);
        o.g(0, "SECTION");
        o.g(2, "HEADER");
        auto var = [&](const char* n) { o.g(9, n); };
        var("$ACADVER");
        o.g(1, "AC1018");
        var("$DWGCODEPAGE");
        o.g(3, "ANSI_1252");
        var("$INSBASE");
        o.point(10, {0, 0, 0});
        const Vec3 mn = extents_.isEmpty() ? Vec3{} : extents_.min, mx = extents_.isEmpty() ? Vec3{} : extents_.max;
        var("$EXTMIN");
        o.point(10, mn);
        var("$EXTMAX");
        o.point(10, mx);
        var("$LIMMIN");
        o.point(10, mn, false);
        var("$LIMMAX");
        o.point(10, mx, false);
        var("$INSUNITS");
        o.g(70, 6);
        var("$MEASUREMENT");
        o.g(70, 1);
        var("$LUNITS");
        o.g(70, 2);
        var("$LUPREC");
        o.g(70, 4);
        var("$AUNITS");
        o.g(70, 0);
        var("$ANGBASE");
        o.g(50, 0.0);
        var("$ANGDIR");
        o.g(70, 0);
        var("$PDMODE");
        o.g(70, 34);
        var("$PDSIZE");
        o.g(40, 0.0);
        var("$TEXTSTYLE");
        o.g(7, "Standard");
        var("$CLAYER");
        o.g(8, "0");
        var("$DIMSTYLE");
        o.g(2, "Standard");
        var("$HANDSEED");
        o.g(5, hex(next_ + 1));
        o.g(0, "ENDSEC");
    }

    void tableHead(Out& o, const char* name, unsigned handle, int count) {
        o.g(0, "TABLE");
        o.g(2, name);
        o.handle(5, handle);
        o.handle(330, 0);
        o.g(100, "AcDbSymbolTable");
        o.g(70, count);
    }
    void record(Out& o, const char* type, unsigned handle, unsigned table, const char* sub) {
        o.g(0, type);
        o.handle(type == std::string("DIMSTYLE") ? 105 : 5, handle);
        o.handle(330, table);
        o.g(100, "AcDbSymbolTableRecord");
        o.g(100, sub);
    }

    void writeTables(Out& o, const std::set<std::string>& layerNames) {
        // VPORT
        {
            const unsigned t = h();
            tableHead(o, "VPORT", t, 1);
            record(o, "VPORT", h(), t, "AcDbViewportTableRecord");
            o.g(2, "*Active");
            o.g(70, 0);
            o.point(10, {0, 0, 0}, false);
            o.point(11, {1, 1, 0}, false);
            const Vec3 c = extents_.isEmpty() ? Vec3{} : (extents_.min + extents_.max) * 0.5;
            o.point(12, c, false);
            o.point(13, {0, 0, 0}, false);
            o.point(14, {0.5, 0.5, 0}, false);
            o.point(15, {0.5, 0.5, 0}, false);
            o.point(16, {0, 0, 1});
            o.point(17, {0, 0, 0});
            const double hgt = extents_.isEmpty() ? 100.0 : std::max(1.0, 1.1 * std::max(extents_.max.y - extents_.min.y, (extents_.max.x - extents_.min.x) / 1.6));
            o.g(40, hgt);
            o.g(41, 1.6);
            o.g(42, 50.0);
            o.g(43, 0.0);
            o.g(44, 0.0);
            o.g(50, 0.0);
            o.g(51, 0.0);
            o.g(71, 0);
            o.g(72, 1000);
            o.g(73, 1);
            o.g(74, 3);
            o.g(75, 0);
            o.g(76, 0);
            o.g(77, 0);
            o.g(78, 0);
            o.g(0, "ENDTAB");
        }
        // LTYPE
        {
            struct LT {
                const char* name;
                const char* desc;
                std::vector<double> dashes;
            };
            const std::vector<LT> lts = {{"ByBlock", "", {}},
                                         {"ByLayer", "", {}},
                                         {"Continuous", "Solid line", {}},
                                         {"DASHED", "Dashed __ __ __", {0.5, -0.25}},
                                         {"DOT", "Dot . . . .", {0.0, -0.25}},
                                         {"DASHDOT", "Dash dot __ . __ .", {0.5, -0.25, 0.0, -0.25}}};
            const unsigned t = h();
            tableHead(o, "LTYPE", t, static_cast<int>(lts.size()));
            for (const auto& l : lts) {
                record(o, "LTYPE", h(), t, "AcDbLinetypeTableRecord");
                o.g(2, l.name);
                o.g(70, 0);
                o.g(3, l.desc);
                o.g(72, 65);
                o.g(73, static_cast<int>(l.dashes.size()));
                double total = 0;
                for (double d : l.dashes) total += std::abs(d);
                o.g(40, total);
                for (double d : l.dashes) {
                    o.g(49, d);
                    o.g(74, 0);
                }
            }
            o.g(0, "ENDTAB");
        }
        // LAYER
        {
            const unsigned t = h();
            tableHead(o, "LAYER", t, static_cast<int>(layerNames.size()));
            for (const auto& n : layerNames) {
                const CadLayer* l = layerByName(n);
                record(o, "LAYER", h(), t, "AcDbLayerTableRecord");
                o.g(2, n);
                o.g(70, l && l->locked ? 4 : 0);
                const std::uint32_t rgb = l ? l->colorRgb : 0xFFFFFF;
                const int aci = l ? rgbToAci(rgb) : 7;
                o.g(62, l && !l->visible ? -aci : aci);
                if (l) o.g(420, static_cast<int>(rgb & 0xFFFFFF));
                o.g(6, l ? ltypeName(l->lineType) : "Continuous");
                o.g(370, -3);
                o.handle(390, hPlotStyleNormal_);
                if (l) {
                    if (l->transparency > 0) {
                        o.g(1001, "AcCmTransparency");
                        o.g(1071, transparencyValue(l->transparency));
                    }
                    o.g(1001, "OPENMINE");
                    o.g(1000, encodeText(l->name));
                    o.g(1000, "lineWidth");
                    o.g(1040, l->lineWidth);
                    if (!l->description.empty()) {
                        o.g(1000, "description");
                        o.g(1000, encodeText(l->description));
                    }
                }
            }
            o.g(0, "ENDTAB");
        }
        // STYLE
        {
            const unsigned t = h();
            tableHead(o, "STYLE", t, 1);
            record(o, "STYLE", h(), t, "AcDbTextStyleTableRecord");
            o.g(2, "Standard");
            o.g(70, 0);
            o.g(40, 0.0);
            o.g(41, 1.0);
            o.g(50, 0.0);
            o.g(71, 0);
            o.g(42, 2.5);
            o.g(3, "txt");
            o.g(4, "");
            o.g(0, "ENDTAB");
        }
        // VIEW, UCS
        for (const char* n : {"VIEW", "UCS"}) {
            tableHead(o, n, h(), 0);
            o.g(0, "ENDTAB");
        }
        // APPID
        {
            const unsigned t = h();
            tableHead(o, "APPID", t, 3);
            for (const char* n : {"ACAD", "OPENMINE", "AcCmTransparency"}) {
                record(o, "APPID", h(), t, "AcDbRegAppTableRecord");
                o.g(2, n);
                o.g(70, 0);
            }
            o.g(0, "ENDTAB");
        }
        // DIMSTYLE
        {
            const unsigned t = h();
            tableHead(o, "DIMSTYLE", t, 1);
            o.g(100, "AcDbDimStyleTable");
            record(o, "DIMSTYLE", h(), t, "AcDbDimStyleTableRecord");
            o.g(2, "Standard");
            o.g(70, 0);
            o.g(40, 1.0);   // DIMSCALE
            o.g(41, 1.0);   // DIMASZ
            o.g(42, 0.0);   // DIMEXO
            o.g(43, 0.0);   // DIMDLI
            o.g(44, 0.5);   // DIMEXE
            o.g(140, 1.0);  // DIMTXT
            o.g(147, 0.25); // DIMGAP
            o.g(77, 1);     // DIMTAD
            o.g(271, 3);    // DIMDEC
            o.g(0, "ENDTAB");
        }
        // BLOCK_RECORD
        {
            tableHead(o, "BLOCK_RECORD", hBlockRecordTable_, static_cast<int>(2 + anon_.size()));
            record(o, "BLOCK_RECORD", hModelBR_, hBlockRecordTable_, "AcDbBlockTableRecord");
            o.g(2, "*Model_Space");
            o.handle(340, hModelLayout_);
            o.g(70, 6);
            o.g(280, 1);
            o.g(281, 0);
            record(o, "BLOCK_RECORD", hPaperBR_, hBlockRecordTable_, "AcDbBlockTableRecord");
            o.g(2, "*Paper_Space");
            o.handle(340, hPaperLayout_);
            o.g(70, 6);
            o.g(280, 1);
            o.g(281, 0);
            for (const auto& b : anon_) {
                record(o, "BLOCK_RECORD", b.record, hBlockRecordTable_, "AcDbBlockTableRecord");
                o.g(2, b.name);
                o.handle(340, 0);
                o.g(70, 6);
                o.g(280, 1);
                o.g(281, 0);
            }
            o.g(0, "ENDTAB");
        }
    }

    void block(Out& o, const std::string& name, unsigned record, bool paper, int flags, const std::string& body) {
        o.g(0, "BLOCK");
        o.handle(5, h());
        o.handle(330, record);
        o.g(100, "AcDbEntity");
        if (paper) o.g(67, 1);
        o.g(8, "0");
        o.g(100, "AcDbBlockBegin");
        o.g(2, name);
        o.g(70, flags);
        o.point(10, {0, 0, 0});
        o.g(3, name);
        o.g(1, "");
        o.str() += body;
        o.g(0, "ENDBLK");
        o.handle(5, h());
        o.handle(330, record);
        o.g(100, "AcDbEntity");
        if (paper) o.g(67, 1);
        o.g(8, "0");
        o.g(100, "AcDbBlockEnd");
    }

    void writeBlocks(Out& o) {
        block(o, "*Model_Space", hModelBR_, false, 0, {});
        block(o, "*Paper_Space", hPaperBR_, true, 0, {});
        for (auto& b : anon_) block(o, b.name, b.record, false, 1, b.body.str());
    }

    void dictionary(Out& o, unsigned handle, unsigned owner, const std::vector<std::pair<std::string, unsigned>>& entries) {
        o.g(0, "DICTIONARY");
        o.handle(5, handle);
        o.handle(330, owner);
        o.g(100, "AcDbDictionary");
        o.g(281, 1);
        for (const auto& [k, v] : entries) {
            o.g(3, k);
            o.handle(350, v);
        }
    }

    void layout(Out& o, unsigned handle, const char* name, int tabOrder, unsigned blockRecord, bool model) {
        o.g(0, "LAYOUT");
        o.handle(5, handle);
        o.handle(330, hLayoutDict_);
        o.g(100, "AcDbPlotSettings");
        o.g(1, "");
        o.g(4, "A3");
        o.g(6, "");
        o.g(40, 7.5);
        o.g(41, 20.0);
        o.g(42, 7.5);
        o.g(43, 20.0);
        o.g(44, 420.0);
        o.g(45, 297.0);
        o.g(46, 0.0);
        o.g(47, 0.0);
        o.g(48, 0.0);
        o.g(49, 0.0);
        o.g(140, 0.0);
        o.g(141, 0.0);
        o.g(142, 1.0);
        o.g(143, 1.0);
        o.g(70, model ? 1024 : 0);
        o.g(72, 1);
        o.g(73, 0);
        o.g(74, 5);
        o.g(7, "");
        o.g(75, 16);
        o.g(76, 0);
        o.g(77, 2);
        o.g(78, 300);
        o.g(147, 1.0);
        o.g(148, 0.0);
        o.g(149, 0.0);
        o.g(100, "AcDbLayout");
        o.g(1, name);
        o.g(70, 1);
        o.g(71, tabOrder);
        o.point(10, {0, 0, 0}, false);
        o.point(11, {420, 297, 0}, false);
        o.point(12, {0, 0, 0});
        o.point(14, {1e20, 1e20, 1e20});
        o.point(15, {-1e20, -1e20, -1e20});
        o.g(146, 0.0);
        o.point(13, {0, 0, 0});
        o.point(16, {1, 0, 0});
        o.point(17, {0, 1, 0});
        o.g(76, 1);
        o.handle(330, blockRecord);
    }

    void writeObjects(Out& o) {
        const unsigned hGroup = h(), hMlineDict = h(), hMlineStd = h();
        dictionary(o, hRootDict_, 0,
                   {{"ACAD_GROUP", hGroup}, {"ACAD_LAYOUT", hLayoutDict_}, {"ACAD_MLINESTYLE", hMlineDict}, {"ACAD_PLOTSTYLENAME", hPlotStyleDict_}});
        dictionary(o, hGroup, hRootDict_, {});
        dictionary(o, hLayoutDict_, hRootDict_, {{"Layout1", hPaperLayout_}, {"Model", hModelLayout_}});
        dictionary(o, hMlineDict, hRootDict_, {{"Standard", hMlineStd}});
        o.g(0, "ACDBDICTIONARYWDFLT");
        o.handle(5, hPlotStyleDict_);
        o.handle(330, hRootDict_);
        o.g(100, "AcDbDictionary");
        o.g(281, 1);
        o.g(3, "Normal");
        o.handle(350, hPlotStyleNormal_);
        o.g(100, "AcDbDictionaryWithDefault");
        o.handle(340, hPlotStyleNormal_);
        o.g(0, "ACDBPLACEHOLDER");
        o.handle(5, hPlotStyleNormal_);
        o.handle(330, hPlotStyleDict_);
        o.g(0, "MLINESTYLE");
        o.handle(5, hMlineStd);
        o.handle(330, hMlineDict);
        o.g(100, "AcDbMlineStyle");
        o.g(2, "Standard");
        o.g(70, 0);
        o.g(3, "");
        o.g(62, 256);
        o.g(51, 90.0);
        o.g(52, 90.0);
        o.g(71, 2);
        o.g(49, 0.5);
        o.g(62, 256);
        o.g(6, "BYLAYER");
        o.g(49, -0.5);
        o.g(62, 256);
        o.g(6, "BYLAYER");
        layout(o, hModelLayout_, "Model", 0, hModelBR_, true);
        layout(o, hPaperLayout_, "Layout1", 1, hPaperBR_, false);
    }
};

} // namespace

std::string writeDxfString(const std::vector<CadLayer>& layers, const std::vector<const GeometryObject*>& objects,
                           const DxfExportOptions& options, DxfExportReport* report) {
    DxfExportReport local;
    DxfExportReport& rep = report ? *report : local;
    Writer w(layers, options, rep);
    return w.run(objects);
}

DxfExportReport writeDxf(const std::filesystem::path& file, const std::vector<CadLayer>& layers,
                         const std::vector<const GeometryObject*>& objects, const DxfExportOptions& options) {
    DxfExportReport rep;
    const std::string text = writeDxfString(layers, objects, options, &rep);
    const auto tmp = file.parent_path() / (file.filename().string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw DxfError("Cannot write " + file.string());
        out << text;
        out.close();
        if (!out) throw DxfError("Writing " + file.string() + " failed");
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        throw DxfError("Cannot replace " + file.string() + ": " + ec.message());
    }
    return rep;
}

} // namespace openmine::formats
