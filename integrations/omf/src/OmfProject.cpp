// SPDX-License-Identifier: Apache-2.0
#include "openmine/omf/OmfProject.h"

#include "openmine/cad/Primitives.h"
#include "openmine/geology/BlockModel.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/PointCloudProject.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>

namespace openmine::omf {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::array<int, 3> rgb(std::uint32_t c) { return {static_cast<int>((c >> 16) & 0xFF), static_cast<int>((c >> 8) & 0xFF), static_cast<int>(c & 0xFF)}; }
std::uint32_t packRgb(const std::array<int, 3>& c) {
    auto cl = [](int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); };
    return (cl(c[0]) << 16) | (cl(c[1]) << 8) | cl(c[2]);
}

std::uint32_t objectColor(const Project& p, const GeometryObject& g) {
    if (g.colorOverride) return *g.colorOverride;
    if (const auto* l = p.model().layers.find(g.layerId)) return l->colorRgb;
    return 0x808080;
}

/// Polyline points of a curve object (arcs tessellated to the chord tolerance).
std::vector<Vec3> curvePoints(const GeometryObject& g, double tol) {
    const auto prims = cad::decompose(g);
    std::vector<Vec3> pts;
    for (const auto& pr : prims) {
        int n = 1;
        if (pr.kind == cad::Prim::Arc) n = std::max(1, geometry::arcSegmentCount(pr.r, std::abs(pr.sweep), tol));
        if (pts.empty()) pts.push_back(pr.start());
        for (int k = 1; k <= n; ++k) pts.push_back(pr.at(static_cast<double>(k) / n));
    }
    return pts;
}

Data scalarData(std::string name, std::string loc, std::vector<double> v) {
    Data d;
    d.kind = Data::Kind::Scalar;
    d.name = std::move(name);
    d.location = std::move(loc);
    d.scalars = std::move(v);
    return d;
}

/// Mapped data with a names legend (sorted unique codes) and a generated colours legend.
Data mappedData(std::string name, std::string loc, const std::vector<std::optional<std::string>>& codes) {
    std::set<std::string> unique;
    for (const auto& c : codes)
        if (c) unique.insert(*c);
    std::vector<std::string> names(unique.begin(), unique.end());
    std::map<std::string, std::int64_t> index;
    for (std::size_t i = 0; i < names.size(); ++i) index[names[i]] = static_cast<std::int64_t>(i);
    Data d;
    d.kind = Data::Kind::Mapped;
    d.name = std::move(name);
    d.location = std::move(loc);
    for (const auto& c : codes) d.indices.push_back(c ? index.at(*c) : -1);
    Legend ln;
    ln.name = "names";
    ln.kind = Legend::Kind::String;
    ln.strings = names;
    Legend lc;
    lc.name = "colors";
    lc.kind = Legend::Kind::Color;
    static const std::uint32_t palette[] = {0x4E79A7, 0xF28E2B, 0xE15759, 0x76B7B2, 0x59A14F, 0xEDC948, 0xB07AA1, 0xFF9DA7, 0x9C755F, 0xBAB0AC};
    for (std::size_t i = 0; i < names.size(); ++i) lc.colors.push_back(rgb(palette[i % 10]));
    d.legends = {ln, lc};
    return d;
}

bool uniform(const std::vector<double>& t) {
    if (t.empty()) return false;
    for (double x : t)
        if (std::abs(x - t.front()) > 1e-9 * std::max(1.0, t.front())) return false;
    return true;
}
} // namespace

std::vector<std::vector<std::int64_t>> chainSegments(const std::vector<std::array<std::int64_t, 2>>& segs, std::size_t nv) {
    std::vector<std::vector<std::size_t>> adj(nv);
    for (std::size_t i = 0; i < segs.size(); ++i) {
        adj[static_cast<std::size_t>(segs[i][0])].push_back(i);
        if (segs[i][1] != segs[i][0]) adj[static_cast<std::size_t>(segs[i][1])].push_back(i);
    }
    std::vector<char> used(segs.size(), 0);
    std::vector<std::vector<std::int64_t>> out;
    auto walk = [&](std::int64_t v, std::size_t s) {
        std::vector<std::int64_t> chain{v};
        while (true) {
            used[s] = 1;
            const std::int64_t w = segs[s][0] == v ? segs[s][1] : segs[s][0];
            chain.push_back(w);
            v = w;
            const auto& a = adj[static_cast<std::size_t>(v)];
            if (a.size() != 2) break;
            const std::size_t next = a[0] == s ? a[1] : a[0];
            if (used[next]) break;
            s = next;
        }
        out.push_back(std::move(chain));
    };
    for (std::size_t v = 0; v < nv; ++v)
        if (adj[v].size() != 2)
            for (auto s : adj[v])
                if (!used[s]) walk(static_cast<std::int64_t>(v), s);
    for (std::size_t s = 0; s < segs.size(); ++s)
        if (!used[s]) walk(segs[s][0], s); // closed loops
    return out;
}

Document exportProjectOmf(const Project& p, const OmfExportOptions& o, std::vector<std::string>* notes, jobs::Progress& progress) {
    const auto& m = p.model();
    Document doc;
    doc.name = m.info.name;
    doc.description = m.info.description;
    doc.author = o.author;
    doc.units = "m";
    auto note = [&](std::string s) {
        if (notes) notes->push_back(std::move(s));
    };
    progress.update(0, "Geometry");
    // Point objects are collected into one point set.
    Element points;
    points.kind = Element::Kind::PointSet;
    points.name = "Points";
    Data pointNames;
    pointNames.kind = Data::Kind::String;
    pointNames.name = "name";
    pointNames.location = "vertices";
    std::set<Uuid> wanted(o.geometry.begin(), o.geometry.end());
    for (const auto& g : m.geometry.all()) {
        if (!o.allGeometry && !wanted.count(g.id)) continue;
        progress.checkCancelled();
        switch (g.type) {
        case GeometryType::Point:
            if (!g.vertices.empty()) {
                points.vertices.push_back(g.vertices.front());
                pointNames.strings.push_back(g.name);
            }
            break;
        case GeometryType::Line:
        case GeometryType::Polyline:
        case GeometryType::Arc:
        case GeometryType::Circle:
        case GeometryType::Spline: {
            const auto pts = curvePoints(g, o.chordTolerance);
            if (pts.size() < 2) {
                note("'" + g.name + "': curve with fewer than two points skipped");
                break;
            }
            Element e;
            e.kind = Element::Kind::LineSet;
            e.name = g.name;
            e.color = rgb(objectColor(p, g));
            e.vertices = pts;
            const bool closed = distance(pts.front(), pts.back()) < 1e-9 && pts.size() > 2;
            if (closed) e.vertices.pop_back();
            const auto n = static_cast<std::int64_t>(e.vertices.size());
            for (std::int64_t i = 0; i + 1 < n; ++i) e.segments.push_back({i, i + 1});
            if (closed) e.segments.push_back({n - 1, 0});
            doc.elements.push_back(std::move(e));
            break;
        }
        case GeometryType::Mesh:
        case GeometryType::Solid: {
            if (!g.mesh || g.mesh->triangles.empty()) {
                note("'" + g.name + "': no triangles; skipped");
                break;
            }
            Element e;
            e.kind = Element::Kind::Surface;
            e.name = g.name;
            e.color = rgb(objectColor(p, g));
            e.vertices = g.mesh->vertices;
            for (const auto& t : g.mesh->triangles) e.triangles.push_back({t[0], t[1], t[2]});
            if (!g.mesh->vertexScalars.empty() && g.mesh->vertexScalars.size() == g.mesh->vertices.size())
                e.data.push_back(scalarData(g.mesh->scalarName.empty() ? "scalar" : g.mesh->scalarName, "vertices",
                                            std::vector<double>(g.mesh->vertexScalars.begin(), g.mesh->vertexScalars.end())));
            if (g.type == GeometryType::Solid) e.description = "Tessellated solid (the exact B-rep is not part of OMF)";
            doc.elements.push_back(std::move(e));
            break;
        }
        default: note("'" + g.name + "': " + std::string(toString(g.type)) + " objects have no OMF equivalent; skipped"); break;
        }
    }
    if (!points.vertices.empty()) {
        points.data.push_back(std::move(pointNames));
        doc.elements.push_back(std::move(points));
    }

    if (o.drillholes && !m.drillholes.empty()) {
        progress.update(0.3, "Drillholes");
        Element dh, col;
        dh.kind = Element::Kind::LineSet;
        dh.name = "Drillhole intervals";
        dh.subtype = "borehole";
        dh.description = std::string("Desurveyed by ") + geology::toString(o.method);
        col.kind = Element::Kind::PointSet;
        col.name = "Drillhole collars";
        col.subtype = "collar";
        std::set<std::string> vfields, cfields;
        for (const auto& h : m.drillholes.all())
            for (const auto& i : h.intervals) {
                for (const auto& [k, v] : i.values) vfields.insert(k);
                for (const auto& [k, v] : i.codes) cfields.insert(k);
            }
        std::map<std::string, std::vector<double>> vals;
        std::map<std::string, std::vector<std::optional<std::string>>> codes;
        std::vector<std::optional<std::string>> holeOfSeg;
        Data colNames;
        colNames.kind = Data::Kind::String;
        colNames.name = "holeid";
        colNames.location = "vertices";
        std::vector<double> colLen;
        for (const auto& h : m.drillholes.all()) {
            col.vertices.push_back(h.collar);
            colNames.strings.push_back(h.holeId);
            colLen.push_back(h.length);
            const geology::DesurveyedHole d(h, o.method);
            auto iv = h.intervals;
            std::sort(iv.begin(), iv.end(), [](const auto& a, const auto& b) { return a.from < b.from; });
            for (const auto& i : iv) {
                const auto a = static_cast<std::int64_t>(dh.vertices.size());
                dh.vertices.push_back(d.at(i.from));
                dh.vertices.push_back(d.at(i.to));
                dh.segments.push_back({a, a + 1});
                holeOfSeg.emplace_back(h.holeId);
                for (const auto& f : vfields) {
                    auto it = i.values.find(f);
                    vals[f].push_back(it == i.values.end() ? kNaN : it->second);
                }
                for (const auto& f : cfields) {
                    auto it = i.codes.find(f);
                    codes[f].push_back(it == i.codes.end() ? std::nullopt : std::optional<std::string>(it->second));
                }
            }
        }
        if (!dh.segments.empty()) {
            dh.data.push_back(mappedData("holeid", "segments", holeOfSeg));
            for (auto& [f, v] : vals) dh.data.push_back(scalarData(f, "segments", std::move(v)));
            for (auto& [f, c] : codes) dh.data.push_back(mappedData(f, "segments", c));
            doc.elements.push_back(std::move(dh));
        }
        col.data.push_back(std::move(colNames));
        col.data.push_back(scalarData("length", "vertices", std::move(colLen)));
        doc.elements.push_back(std::move(col));
    }

    progress.update(0.5, "Block models");
    std::set<Uuid> bms(o.blockModels.begin(), o.blockModels.end());
    for (const auto& b : m.blockModels.all()) {
        if (!o.allBlockModels && !bms.count(b.id)) continue;
        progress.checkCancelled();
        const auto data = geology::loadBlockModel(p, b.id);
        const geology::BlockGrid g(b);
        Element e;
        e.kind = Element::Kind::Volume;
        e.name = b.name;
        e.description = b.description;
        e.origin = b.origin;
        e.axisU = g.axisU();
        e.axisV = g.axisV();
        e.axisW = {0, 0, 1};
        e.tensorU.assign(static_cast<std::size_t>(b.nx), b.blockSize.x);
        e.tensorV.assign(static_cast<std::size_t>(b.ny), b.blockSize.y);
        e.tensorW.assign(static_cast<std::size_t>(b.nz), b.blockSize.z);
        for (const auto& a : b.attributes) e.data.push_back(scalarData(a, "cells", data.values.at(a)));
        doc.elements.push_back(std::move(e));
    }

    for (const auto& pcId : o.pointClouds) {
        progress.update(0.7, "Point clouds");
        const auto& ds = survey::dataset(p, pcId);
        const auto buf = survey::datasetPoints(p, pcId, true, std::nullopt, progress);
        Element e;
        e.kind = Element::Kind::PointSet;
        e.name = ds.name;
        e.description = "Point cloud (project coordinates)";
        e.vertices = buf.xyz;
        if (!buf.intensity.empty()) e.data.push_back(scalarData("intensity", "vertices", std::vector<double>(buf.intensity.begin(), buf.intensity.end())));
        if (!buf.classification.empty())
            e.data.push_back(scalarData("classification", "vertices", std::vector<double>(buf.classification.begin(), buf.classification.end())));
        if (!buf.rgb.empty()) {
            Data c;
            c.kind = Data::Kind::Color;
            c.name = "rgb";
            c.location = "vertices";
            for (const auto& x : buf.rgb) c.colors.push_back({x.r >> 8, x.g >> 8, x.b >> 8});
            e.data.push_back(std::move(c));
        }
        for (std::size_t s = 0; s < buf.scalars.size(); ++s)
            e.data.push_back(scalarData(buf.schema().scalarFields[s], "vertices", std::vector<double>(buf.scalars[s].begin(), buf.scalars[s].end())));
        doc.elements.push_back(std::move(e));
    }
    progress.update(1.0);
    return doc;
}

OmfImportSummary importOmf(Project& p, const Document& doc, const OmfImportOptions& o, jobs::Progress& progress) {
    validate(doc);
    OmfImportSummary s;
    if (!doc.units.empty() && doc.units != "m" && doc.units != "metres" && doc.units != "meters" && doc.units != "metre")
        s.notes.push_back("Document units are '" + doc.units + "'; coordinates were imported unchanged (no unit conversion is applied implicitly)");
    // Point clouds are prepared (octree building) before the model is touched.
    std::vector<std::pair<std::size_t, survey::PreparedDataset>> clouds;
    for (std::size_t ei = 0; ei < doc.elements.size(); ++ei) {
        const auto& e = doc.elements[ei];
        if (e.kind != Element::Kind::PointSet || !o.pointSetsAsPointClouds || e.vertices.empty()) continue;
        progress.checkCancelled();
        progress.update(static_cast<double>(ei) / doc.elements.size(), "Point sets");
        survey::PointSchema schema;
        for (const auto& d : e.data)
            if (d.kind == Data::Kind::Scalar && d.location == "vertices") schema.scalarFields.push_back(d.name);
            else if (d.kind == Data::Kind::Color && d.location == "vertices") schema.rgb = true;
        survey::PointBuffer buf(schema);
        buf.resize(e.vertices.size());
        for (std::size_t i = 0; i < e.vertices.size(); ++i) buf.xyz[i] = doc.origin + e.origin + e.vertices[i];
        std::size_t sf = 0;
        for (const auto& d : e.data) {
            if (d.location != "vertices") {
                s.notes.push_back("'" + e.name + "': data '" + d.name + "' on " + d.location + " not imported");
                continue;
            }
            if (d.kind == Data::Kind::Scalar) {
                for (std::size_t i = 0; i < d.scalars.size(); ++i) buf.scalars[sf][i] = static_cast<float>(d.scalars[i]);
                ++sf;
            } else if (d.kind == Data::Kind::Color) {
                for (std::size_t i = 0; i < d.colors.size(); ++i)
                    buf.rgb[i] = {static_cast<std::uint16_t>(d.colors[i][0] * 257), static_cast<std::uint16_t>(d.colors[i][1] * 257),
                                  static_cast<std::uint16_t>(d.colors[i][2] * 257)};
            } else s.notes.push_back("'" + e.name + "': data '" + d.name + "' (non-numeric) not imported into the point cloud");
        }
        if (!schema.scalarFields.empty()) s.notes.push_back("'" + e.name + "': point scalar fields are stored as float32 in point clouds");
        clouds.emplace_back(ei, survey::prepareFromPoints(p, buf, e.name.empty() ? "OMF points" : e.name, o.sourceFile, "OMF", progress));
    }

    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Import OMF" + (o.sourceFile.empty() ? std::string{} : " " + o.sourceFile));
    Uuid layer = o.layerId;
    if (layer.isNil()) layer = ed.addLayer(o.layerName, 0xD0D0D0);
    for (auto& [ei, prep] : clouds) s.pointClouds.push_back([&] {
        ed.add(prep.asset);
        return ed.add(std::move(prep.dataset));
    }());
    for (const auto& e : doc.elements) {
        progress.checkCancelled();
        const Vec3 base = doc.origin + e.origin;
        for (const auto& n : e.notes) s.notes.push_back("'" + e.name + "': " + n);
        switch (e.kind) {
        case Element::Kind::PointSet: {
            if (o.pointSetsAsPointClouds) break;
            const Data* names = nullptr;
            for (const auto& d : e.data)
                if (d.kind == Data::Kind::String && d.location == "vertices") names = &d;
            for (std::size_t i = 0; i < e.vertices.size(); ++i) {
                GeometryObject g;
                g.type = GeometryType::Point;
                g.layerId = layer;
                g.name = names ? names->strings[i] : e.name + " " + std::to_string(i + 1);
                g.vertices = {base + e.vertices[i]};
                g.colorOverride = packRgb(e.color);
                s.geometry.push_back(ed.addGeometry(std::move(g)));
            }
            break;
        }
        case Element::Kind::LineSet: {
            const auto chains = chainSegments(e.segments, e.vertices.size());
            int k = 0;
            for (const auto& c : chains) {
                GeometryObject g;
                g.type = GeometryType::Polyline;
                g.layerId = layer;
                g.name = chains.size() == 1 ? e.name : e.name + " " + std::to_string(++k);
                const bool closed = c.size() > 3 && c.front() == c.back();
                for (std::size_t i = 0; i + (closed ? 1 : 0) < c.size(); ++i) g.vertices.push_back(base + e.vertices[static_cast<std::size_t>(c[i])]);
                g.closed = closed;
                g.colorOverride = packRgb(e.color);
                s.geometry.push_back(ed.addGeometry(std::move(g)));
            }
            for (const auto& d : e.data) s.notes.push_back("'" + e.name + "': line data '" + d.name + "' not imported (polylines carry no per-segment values)");
            break;
        }
        case Element::Kind::Surface:
        case Element::Kind::SurfaceGrid: {
            auto mesh = std::make_shared<MeshData>();
            if (e.kind == Element::Kind::Surface) {
                for (const auto& v : e.vertices) mesh->vertices.push_back(base + v);
                for (const auto& t : e.triangles)
                    mesh->triangles.push_back({static_cast<std::uint32_t>(t[0]), static_cast<std::uint32_t>(t[1]), static_cast<std::uint32_t>(t[2])});
            } else {
                const Vec3 w = cross(e.axisU, e.axisV).normalized();
                std::vector<double> us{0}, vs{0};
                for (double x : e.tensorU) us.push_back(us.back() + x);
                for (double x : e.tensorV) vs.push_back(vs.back() + x);
                const std::size_t nu = us.size(), nv = vs.size();
                for (std::size_t j = 0; j < nv; ++j)
                    for (std::size_t i = 0; i < nu; ++i) {
                        const double off = e.offsetW.empty() ? 0 : e.offsetW[i + nu * j];
                        mesh->vertices.push_back(base + e.axisU.normalized() * us[i] + e.axisV.normalized() * vs[j] + w * off);
                    }
                for (std::size_t j = 0; j + 1 < nv; ++j)
                    for (std::size_t i = 0; i + 1 < nu; ++i) {
                        const auto a = static_cast<std::uint32_t>(i + nu * j), b = a + 1, c = static_cast<std::uint32_t>(a + nu), d = c + 1;
                        mesh->triangles.push_back({a, b, d});
                        mesh->triangles.push_back({a, d, c});
                    }
            }
            bool scalarUsed = false;
            for (const auto& d : e.data) {
                if (!scalarUsed && d.kind == Data::Kind::Scalar && d.location == "vertices") {
                    mesh->vertexScalars.assign(d.scalars.begin(), d.scalars.end());
                    mesh->scalarName = d.name;
                    scalarUsed = true;
                } else s.notes.push_back("'" + e.name + "': surface data '" + d.name + "' not imported");
            }
            GeometryObject g;
            g.type = GeometryType::Mesh;
            g.layerId = layer;
            g.name = e.name;
            g.mesh = mesh;
            g.colorOverride = packRgb(e.color);
            s.geometry.push_back(ed.addGeometry(std::move(g)));
            break;
        }
        case Element::Kind::Volume: {
            const Vec3 u = e.axisU.normalized(), v = e.axisV.normalized(), w = e.axisW.normalized();
            const bool vertical = std::abs(w.z - 1) < 1e-9 && std::abs(u.z) < 1e-9 && std::abs(dot(u, v)) < 1e-9;
            if (!uniform(e.tensorU) || !uniform(e.tensorV) || !uniform(e.tensorW) || !vertical) {
                s.notes.push_back("'" + e.name + "': volume skipped — only regular grids (uniform block sizes) rotated about Z are supported as block models");
                break;
            }
            BlockModel b;
            b.name = e.name.empty() ? "OMF block model" : e.name;
            b.description = e.description;
            b.origin = base;
            b.blockSize = {e.tensorU.front(), e.tensorV.front(), e.tensorW.front()};
            b.nx = static_cast<int>(e.tensorU.size());
            b.ny = static_cast<int>(e.tensorV.size());
            b.nz = static_cast<int>(e.tensorW.size());
            b.rotationDeg = std::atan2(u.y, u.x) * 180.0 / std::numbers::pi;
            if (std::abs(cross(u, v).z - 1) > 1e-9) {
                s.notes.push_back("'" + e.name + "': volume skipped — left-handed axes are not supported");
                break;
            }
            geology::BlockModelData data;
            data.header = b;
            for (const auto& d : e.data) {
                if (d.location != "cells") {
                    s.notes.push_back("'" + e.name + "': data '" + d.name + "' on " + d.location + " not imported");
                    continue;
                }
                if (!isValidAttributeKey(d.name)) {
                    s.notes.push_back("'" + e.name + "': data '" + d.name + "' is not a valid attribute name; skipped");
                    continue;
                }
                if (d.kind == Data::Kind::Scalar) data.attribute(d.name) = d.scalars;
                else if (d.kind == Data::Kind::Mapped) {
                    auto& a = data.attribute(d.name);
                    for (std::size_t i = 0; i < d.indices.size(); ++i) a[i] = d.indices[i] < 0 ? kNaN : static_cast<double>(d.indices[i]);
                    std::string legend;
                    for (const auto& l : d.legends)
                        if (l.kind == Legend::Kind::String) {
                            for (std::size_t i = 0; i < l.strings.size(); ++i) legend += (i ? ", " : "") + std::to_string(i) + "=" + l.strings[i];
                            break;
                        }
                    data.header.description += (data.header.description.empty() ? "" : "\n") + d.name + " codes: " + legend;
                    s.notes.push_back("'" + e.name + "': mapped data '" + d.name + "' stored as legend indices (legend in the description)");
                } else s.notes.push_back("'" + e.name + "': data '" + d.name + "' (non-numeric) not imported");
            }
            BlockModel h = data.header;
            h.attributes.clear();
            data.header.id = ed.add(h);
            const Uuid bid = data.header.id;
            geology::saveBlockModel(p, data, "Import block model");
            s.blockModels.push_back(bid);
            break;
        }
        }
    }
    macro.commit();
    progress.update(1.0);
    return s;
}

} // namespace openmine::omf
