// SPDX-License-Identifier: Apache-2.0
#include "SceneBuilder.h"

#include "openmine/cad/Annotation.h"
#include "openmine/geology/Desurvey.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/objects/EnumNames.h"

#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkVectorText.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace openmine::app::scene {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

const CadLayer* layerOf(const ProjectModel& m, const Uuid& id) { return m.layers.find(id); }

BoundingBox boundsOf(const std::vector<std::vector<Vec3>>& lines) {
    auto b = BoundingBox::empty();
    for (const auto& l : lines)
        for (const auto& p : l) b.expand(p);
    return b;
}

BoundingBox boundsOf(const MeshData& m) {
    auto b = BoundingBox::empty();
    for (const auto& p : m.vertices) b.expand(p);
    return b;
}

/// Cap height of the vector font relative to its unit size (measured once from "H").
double vectorFontCapHeight() {
    static const double h = [] {
        vtkNew<vtkVectorText> vt;
        vt->SetText("H");
        vt->Update();
        double b[6];
        vt->GetOutput()->GetBounds(b);
        return b[3] - b[2] > 0 ? b[3] - b[2] : 1.0;
    }();
    return h;
}

SceneItem baseItem(const SelectionItem& owner, const std::string& part, SceneItem::Prim prim) {
    SceneItem it;
    it.owner = owner;
    it.part = part;
    it.prim = prim;
    return it;
}

std::uint32_t displayColor(const ProjectModel& m, const GeometryObject& g) {
    if (g.colorOverride) return *g.colorOverride;
    if (const auto* l = layerOf(m, g.layerId)) return l->colorRgb;
    return 0xD0D0D0;
}

double displayOpacity(const ProjectModel& m, const GeometryObject& g) {
    double t = 0;
    if (g.transparency) t = *g.transparency;
    else if (const auto* l = layerOf(m, g.layerId)) t = l->transparency;
    return std::clamp(1.0 - t, 0.02, 1.0);
}

} // namespace

// ---- helpers ------------------------------------------------------------------------------------

vtkSmartPointer<vtkPolyData> polylinesData(const std::vector<std::vector<Vec3>>& lines, const coordinates::RenderOrigin& o) {
    auto pd = vtkSmartPointer<vtkPolyData>::New();
    vtkNew<vtkPoints> pts;
    vtkNew<vtkCellArray> cells;
    for (const auto& l : lines) {
        if (l.size() < 2) continue;
        const vtkIdType base = pts->GetNumberOfPoints();
        for (const auto& p : l) {
            const Vec3 q = o.toLocal(p);
            pts->InsertNextPoint(q.x, q.y, q.z);
        }
        cells->InsertNextCell(static_cast<vtkIdType>(l.size()));
        for (std::size_t i = 0; i < l.size(); ++i) cells->InsertCellPoint(base + static_cast<vtkIdType>(i));
    }
    pd->SetPoints(pts);
    pd->SetLines(cells);
    return pd;
}

vtkSmartPointer<vtkPolyData> segmentsData(const std::vector<std::pair<Vec3, Vec3>>& segments, const coordinates::RenderOrigin& o) {
    auto pd = vtkSmartPointer<vtkPolyData>::New();
    vtkNew<vtkPoints> pts;
    vtkNew<vtkCellArray> cells;
    pts->Allocate(static_cast<vtkIdType>(segments.size() * 2));
    for (const auto& [a, b] : segments) {
        const Vec3 qa = o.toLocal(a), qb = o.toLocal(b);
        const vtkIdType ia = pts->InsertNextPoint(qa.x, qa.y, qa.z);
        const vtkIdType ib = pts->InsertNextPoint(qb.x, qb.y, qb.z);
        const vtkIdType ids[2] = {ia, ib};
        cells->InsertNextCell(2, ids);
    }
    pd->SetPoints(pts);
    pd->SetLines(cells);
    return pd;
}

vtkSmartPointer<vtkPolyData> pointsData(const std::vector<Vec3>& pts, const coordinates::RenderOrigin& o) {
    auto pd = vtkSmartPointer<vtkPolyData>::New();
    vtkNew<vtkPoints> p;
    vtkNew<vtkCellArray> verts;
    for (const auto& v : pts) {
        const Vec3 q = o.toLocal(v);
        const vtkIdType id = p->InsertNextPoint(q.x, q.y, q.z);
        verts->InsertNextCell(1, &id);
    }
    pd->SetPoints(p);
    pd->SetVerts(verts);
    return pd;
}

vtkSmartPointer<vtkPolyData> meshPolyData(const MeshData& mesh, const coordinates::RenderOrigin& o, bool computeNormals, double featureAngleDeg) {
    auto pd = vtkSmartPointer<vtkPolyData>::New();
    vtkNew<vtkPoints> pts;
    pts->SetNumberOfPoints(static_cast<vtkIdType>(mesh.vertices.size()));
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Vec3 q = o.toLocal(mesh.vertices[i]);
        pts->SetPoint(static_cast<vtkIdType>(i), q.x, q.y, q.z);
    }
    vtkNew<vtkCellArray> polys;
    polys->AllocateExact(static_cast<vtkIdType>(mesh.triangles.size()), static_cast<vtkIdType>(mesh.triangles.size() * 3));
    for (const auto& t : mesh.triangles) {
        const vtkIdType ids[3] = {t[0], t[1], t[2]};
        polys->InsertNextCell(3, ids);
    }
    pd->SetPoints(pts);
    pd->SetPolys(polys);
    if (!mesh.vertexScalars.empty() && mesh.vertexScalars.size() == mesh.vertices.size()) {
        vtkNew<vtkFloatArray> s;
        s->SetName("scalar");
        s->SetNumberOfValues(static_cast<vtkIdType>(mesh.vertexScalars.size()));
        for (std::size_t i = 0; i < mesh.vertexScalars.size(); ++i) s->SetValue(static_cast<vtkIdType>(i), mesh.vertexScalars[i]);
        pd->GetPointData()->SetScalars(s);
    }
    if (!computeNormals || mesh.triangles.empty()) return pd;
    vtkNew<vtkPolyDataNormals> n;
    n->SetInputData(pd);
    n->ConsistencyOff(); // keep the stored orientation (outward for solids)
    n->AutoOrientNormalsOff();
    n->SetSplitting(featureAngleDeg < 180.0);
    n->SetFeatureAngle(featureAngleDeg);
    n->ComputeCellNormalsOff();
    n->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->ShallowCopy(n->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> textData(const std::string& text, Vec3 insertion, double height, double rotation, TextAlign align,
                                      const coordinates::RenderOrigin& o) {
    vtkNew<vtkVectorText> vt;
    vt->SetText(text.empty() ? " " : text.c_str());
    vt->Update();
    double b[6];
    vt->GetOutput()->GetBounds(b);
    const double scale = height / vectorFontCapHeight();
    double dx = -b[0];
    if (align == TextAlign::Center) dx = -(b[0] + b[1]) / 2;
    if (align == TextAlign::Right) dx = -b[1];
    const Vec3 q = o.toLocal(insertion);
    vtkNew<vtkTransform> t;
    t->PostMultiply();
    t->Translate(dx, 0, 0);
    t->Scale(scale, scale, scale);
    t->RotateZ(rotation * 180.0 / std::numbers::pi);
    t->Translate(q.x, q.y, q.z);
    vtkNew<vtkTransformPolyDataFilter> f;
    f->SetInputConnection(vt->GetOutputPort());
    f->SetTransform(t);
    f->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->ShallowCopy(f->GetOutput());
    return out;
}

std::vector<std::vector<Vec3>> applyLinetype(const std::vector<std::vector<Vec3>>& lines, const std::string& lineType, double scale) {
    std::vector<double> pattern; // alternating on/off lengths in pattern units
    if (lineType == "Dashed") pattern = {1.0, 0.5};
    else if (lineType == "Dotted") pattern = {0.1, 0.4};
    else if (lineType == "DashDot") pattern = {1.0, 0.3, 0.1, 0.3};
    else return lines;
    for (auto& v : pattern) v *= std::max(scale, 1e-6);
    double period = 0;
    for (double v : pattern) period += v;
    std::vector<std::vector<Vec3>> out;
    for (const auto& l : lines) {
        if (l.size() < 2) continue;
        // Too many dashes would swamp the renderer: fall back to a continuous line.
        const double len = geometry::length3d(l);
        if (len / period > 20000) {
            out.push_back(l);
            continue;
        }
        std::size_t pi = 0;      // pattern element
        double left = pattern[0];// length left in the current element
        std::vector<Vec3> cur{l.front()};
        for (std::size_t i = 1; i < l.size(); ++i) {
            Vec3 a = l[i - 1];
            const Vec3 b = l[i];
            double seg = distance(a, b);
            while (seg > 1e-12) {
                const bool on = pi % 2 == 0;
                if (seg < left) {
                    if (on) cur.push_back(b);
                    left -= seg;
                    seg = 0;
                } else {
                    const Vec3 p = lerp(a, b, left / seg);
                    if (on) {
                        cur.push_back(p);
                        if (cur.size() >= 2) out.push_back(cur);
                        cur.clear();
                    } else {
                        cur = {p};
                    }
                    seg -= left;
                    a = p;
                    pi = (pi + 1) % pattern.size();
                    left = pattern[pi];
                }
            }
        }
        if (pi % 2 == 0 && cur.size() >= 2) out.push_back(cur);
    }
    return out;
}

// ---- ownership ----------------------------------------------------------------------------------

SelectionItem ownerOfGeometry(const ProjectModel& m, const Uuid& gid) {
    if (const auto* d = m.developmentForGeometry(gid)) return {EntityKind::Development, d->id};
    for (const auto& s : m.stopes.all())
        if (s.geometryId == gid) return {EntityKind::Stope, s.id};
    for (const auto& b : m.miningBlocks.all())
        if (b.geometryId == gid) return {EntityKind::MiningBlock, b.id};
    for (const auto& g : m.geologicalObjects.all())
        if (g.geometryId == gid) return {EntityKind::GeologicalObject, g.id};
    for (const auto& a : m.asBuiltModels.all())
        if (a.geometryId == gid) return {EntityKind::AsBuilt, a.id};
    return {EntityKind::Geometry, gid};
}

// ---- geometry objects -----------------------------------------------------------------------------

std::vector<SceneItem> buildGeometry(const BuildContext& c, const GeometryObject& g) {
    const auto& m = c.project.model();
    std::vector<SceneItem> items;
    const auto* layer = layerOf(m, g.layerId);
    const SelectionItem owner = ownerOfGeometry(m, g.id);
    const std::uint32_t color = displayColor(m, g);
    const double opacity = displayOpacity(m, g);
    const double width = layer ? layer->lineWidth : 2.0;
    const std::string lineType = layer ? layer->lineType : "Continuous";

    auto finish = [&](SceneItem it) {
        it.geometryId = g.id;
        it.layerId = g.layerId;
        if (it.color == 0xD0D0D0) it.color = color;
        it.opacity = std::min(it.opacity, opacity);
        items.push_back(std::move(it));
    };

    switch (g.type) {
    case GeometryType::Point: {
        if (g.vertices.empty()) break;
        auto it = baseItem(owner, "point", SceneItem::Prim::Points);
        it.data = pointsData({g.vertices.front()}, c.origin);
        it.pointSize = std::max(6.0, width * 3);
        it.spheres = true;
        it.bounds.expand(g.vertices.front());
        finish(std::move(it));
        if (!g.name.empty()) {
            auto lb = baseItem(owner, "label", SceneItem::Prim::Label);
            lb.labelText = g.name;
            lb.labelAnchor = c.origin.toLocal(g.vertices.front());
            lb.pickable = false;
            lb.bounds.expand(g.vertices.front());
            finish(std::move(lb));
        }
        break;
    }
    case GeometryType::Line:
    case GeometryType::Polyline:
    case GeometryType::Arc:
    case GeometryType::Circle:
    case GeometryType::Spline: {
        const auto lines = geometry::tessellate(g, c.settings.chordTolerance);
        auto it = baseItem(owner, "curve", SceneItem::Prim::Lines);
        it.data = polylinesData(applyLinetype(lines, lineType, c.settings.linetypeScale), c.origin);
        it.lineWidth = width;
        it.lighting = false;
        it.bounds = boundsOf(lines);
        finish(std::move(it));
        break;
    }
    case GeometryType::Text: {
        if (g.vertices.empty()) break;
        auto it = baseItem(owner, "text", SceneItem::Prim::Surface);
        it.data = textData(g.text.text, g.vertices.front(), g.text.height, g.text.rotation, g.text.align, c.origin);
        it.lighting = false;
        double b[6];
        it.data->GetBounds(b);
        it.bounds.expand(c.origin.toWorld({b[0], b[2], b[4]}));
        it.bounds.expand(c.origin.toWorld({b[1], b[3], b[5]}));
        finish(std::move(it));
        break;
    }
    case GeometryType::Dimension: {
        cad::DimensionGraphics dg;
        try {
            dg = cad::dimensionGraphics(g);
        } catch (const std::exception&) {
            break;
        }
        auto lines = baseItem(owner, "dimension", SceneItem::Prim::Lines);
        lines.data = segmentsData(dg.lines, c.origin);
        lines.lineWidth = std::max(1.0, width * 0.6);
        lines.lighting = false;
        for (const auto& [a, b] : dg.lines) {
            lines.bounds.expand(a);
            lines.bounds.expand(b);
        }
        finish(std::move(lines));
        if (!dg.arrows.empty()) {
            MeshData arrows;
            for (const auto& tri : dg.arrows) {
                const auto base = static_cast<std::uint32_t>(arrows.vertices.size());
                arrows.vertices.insert(arrows.vertices.end(), tri.begin(), tri.end());
                arrows.triangles.push_back({base, base + 1, base + 2});
            }
            auto ar = baseItem(owner, "arrows", SceneItem::Prim::Surface);
            ar.data = meshPolyData(arrows, c.origin, false);
            ar.lighting = false;
            ar.bounds = boundsOf(arrows);
            finish(std::move(ar));
        }
        auto tx = baseItem(owner, "text", SceneItem::Prim::Surface);
        tx.data = textData(dg.text, dg.textPosition, g.dimension.textHeight, dg.textRotation, TextAlign::Center, c.origin);
        tx.lighting = false;
        tx.bounds.expand(dg.textPosition);
        finish(std::move(tx));
        break;
    }
    case GeometryType::Hatch: {
        const auto loops = geometry::tessellate(g, c.settings.chordTolerance);
        auto boundary = baseItem(owner, "boundary", SceneItem::Prim::Lines);
        boundary.data = polylinesData(loops, c.origin);
        boundary.lineWidth = width;
        boundary.lighting = false;
        boundary.bounds = boundsOf(loops);
        finish(std::move(boundary));
        try {
            if (g.hatch.pattern == "SOLID") {
                MeshData fill;
                for (const auto& tri : cad::hatchTriangles(g)) {
                    const auto base = static_cast<std::uint32_t>(fill.vertices.size());
                    fill.vertices.insert(fill.vertices.end(), tri.begin(), tri.end());
                    fill.triangles.push_back({base, base + 1, base + 2});
                }
                auto it = baseItem(owner, "fill", SceneItem::Prim::Surface);
                it.data = meshPolyData(fill, c.origin, false);
                it.lighting = false;
                it.opacity = 0.75;
                it.bounds = boundsOf(fill);
                finish(std::move(it));
            } else {
                auto it = baseItem(owner, "pattern", SceneItem::Prim::Lines);
                it.data = segmentsData(cad::hatchLines(g), c.origin);
                it.lineWidth = 1.0;
                it.lighting = false;
                it.bounds = boundsOf(loops);
                finish(std::move(it));
            }
        } catch (const std::exception&) {
            // invalid boundary: the boundary is still drawn
        }
        break;
    }
    case GeometryType::Mesh:
    case GeometryType::Solid: {
        if (!g.mesh || g.mesh->triangles.empty()) break;
        const bool solid = g.type == GeometryType::Solid;
        auto it = baseItem(owner, solid ? "solid" : "mesh", SceneItem::Prim::Surface);
        it.data = meshPolyData(*g.mesh, c.origin, true, solid ? 30.0 : 180.0);
        it.bounds = boundsOf(*g.mesh);
        if (!g.mesh->vertexScalars.empty()) {
            ScalarDisplay sd;
            sd.name = "scalar";
            sd.title = g.mesh->scalarName.empty() ? std::string("Value") : g.mesh->scalarName;
            float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
            for (float v : g.mesh->vertexScalars)
                if (std::isfinite(v)) {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            if (lo <= hi) {
                // Signed distances: symmetric diverging scale around zero.
                if (lo < 0 && hi > 0) {
                    const double r = std::max(-static_cast<double>(lo), static_cast<double>(hi));
                    sd.min = -r;
                    sd.max = r;
                    sd.lut = Lut::Diverging;
                } else {
                    sd.min = lo;
                    sd.max = hi > lo ? hi : lo + 1;
                }
                it.scalars = sd;
            }
        }
        finish(std::move(it));
        if (c.settings.meshEdges) {
            auto e = baseItem(owner, "edges", SceneItem::Prim::Lines);
            std::vector<std::pair<Vec3, Vec3>> segs;
            segs.reserve(g.mesh->triangles.size() * 3 / 2);
            for (const auto& t : g.mesh->triangles)
                for (int k = 0; k < 3; ++k) {
                    const auto a = t[static_cast<std::size_t>(k)], b = t[static_cast<std::size_t>((k + 1) % 3)];
                    if (a < b) segs.emplace_back(g.mesh->vertices[a], g.mesh->vertices[b]);
                }
            e.data = segmentsData(segs, c.origin);
            e.lineWidth = 1.0;
            e.color = 0x202020;
            e.lighting = false;
            e.pickable = false;
            e.bounds = boundsOf(*g.mesh);
            items.push_back(std::move(e));
        }
        break;
    }
    }
    return items;
}

// ---- developments --------------------------------------------------------------------------------

std::vector<SceneItem> buildDevelopment(const BuildContext& c, const MineDevelopment& d) {
    std::vector<SceneItem> items;
    const auto& m = c.project.model();
    const SelectionItem owner{EntityKind::Development, d.id};
    const auto* g = m.geometry.find(d.geometryId);
    const auto* layer = g ? layerOf(m, g->layerId) : nullptr;
    if (layer && !layer->visible) return items;
    const std::uint32_t base = g ? displayColor(m, *g) : 0x3FA9F5;
    bool flagged = false;
    for (const auto& aid : m.activitiesForDevelopment(d.id))
        if (const auto* a = m.activities.find(aid)) flagged = flagged || a->designImpactFlag;

    if (c.settings.developmentEnvelopes) {
        try {
            const MeshData env = mine_design::excavationEnvelope(c.project, d);
            if (!env.triangles.empty()) {
                auto it = baseItem(owner, "envelope", SceneItem::Prim::Surface);
                it.data = meshPolyData(env, c.origin, true, 40.0);
                it.color = flagged ? 0xE8B020 : (c.settings.colorDevelopmentsByStatus ? statusColor(d.status, base) : base);
                it.opacity = c.settings.envelopeOpacity;
                it.layerId = g ? g->layerId : Uuid{};
                it.geometryId = d.geometryId;
                it.bounds = boundsOf(env);
                items.push_back(std::move(it));
            }
        } catch (const std::exception&) {
            // Invalid centreline or profile: the centreline is still shown; Properties explain why.
        }
    }
    if (c.settings.developmentStations && c.settings.stationInterval > 0) {
        try {
            const auto st = mine_design::stations(c.project, d, c.settings.stationInterval);
            std::vector<std::pair<Vec3, Vec3>> ticks;
            const double half = std::max(d.designWidth, 1.0) * 0.6;
            for (const auto& s : st) {
                Vec3 side{-s.tangent.y, s.tangent.x, 0};
                if (side.length() < 1e-9) side = {1, 0, 0};
                side = side.normalized() * half;
                ticks.emplace_back(s.position - side, s.position + side);
                auto lb = baseItem(owner, "station", SceneItem::Prim::Label);
                lb.labelText = s.label;
                lb.labelAnchor = c.origin.toLocal(s.position + side);
                lb.labelSize = 10;
                lb.pickable = false;
                lb.bounds.expand(s.position);
                items.push_back(std::move(lb));
            }
            auto it = baseItem(owner, "stations", SceneItem::Prim::Lines);
            it.data = segmentsData(ticks, c.origin);
            it.color = 0xFFFFFF;
            it.lineWidth = 1.5;
            it.lighting = false;
            it.pickable = false;
            for (const auto& [a, b] : ticks) {
                it.bounds.expand(a);
                it.bounds.expand(b);
            }
            items.push_back(std::move(it));
        } catch (const std::exception&) {
        }
    }
    return items;
}

// ---- survey control ------------------------------------------------------------------------------

std::vector<SceneItem> buildControlPoint(const BuildContext& c, const SurveyControlPoint& p) {
    std::vector<SceneItem> items;
    const SelectionItem owner{EntityKind::ControlPoint, p.id};
    std::uint32_t color = 0xE53935;
    switch (p.role) {
    case ControlPointRole::Control: color = 0xE53935; break;
    case ControlPointRole::Check: color = 0x1E88E5; break;
    case ControlPointRole::Temporary: color = 0xFB8C00; break;
    case ControlPointRole::ScanTarget: color = 0xD81B60; break;
    }
    auto it = baseItem(owner, "marker", SceneItem::Prim::Points);
    it.data = pointsData({p.position}, c.origin);
    it.color = color;
    it.pointSize = 11;
    it.spheres = true;
    it.bounds.expand(p.position);
    items.push_back(std::move(it));
    if (c.settings.controlLabels) {
        auto lb = baseItem(owner, "label", SceneItem::Prim::Label);
        lb.labelText = p.name;
        lb.labelAnchor = c.origin.toLocal(p.position);
        lb.color = color;
        lb.pickable = false;
        lb.bounds.expand(p.position);
        items.push_back(std::move(lb));
    }
    return items;
}

// ---- drillholes ----------------------------------------------------------------------------------

std::vector<SceneItem> buildDrillhole(const BuildContext& c, const Drillhole& h) {
    std::vector<SceneItem> items;
    const SelectionItem owner{EntityKind::Drillhole, h.id};
    geology::DesurveyedHole dh;
    try {
        dh = geology::DesurveyedHole(h);
    } catch (const std::exception&) {
        return items;
    }
    const double len = dh.length();
    if (!(len > 0)) return items;
    // Sample depths: every metre, every survey and interval boundary, and the end of hole.
    std::vector<double> depths;
    for (double z = 0; z < len; z += 1.0) depths.push_back(z);
    depths.push_back(len);
    for (const auto& s : h.surveys)
        if (s.depth > 0 && s.depth < len) depths.push_back(s.depth);
    for (const auto& iv : h.intervals) {
        if (iv.from > 0 && iv.from < len) depths.push_back(iv.from);
        if (iv.to > 0 && iv.to < len) depths.push_back(iv.to);
    }
    std::sort(depths.begin(), depths.end());
    depths.erase(std::unique(depths.begin(), depths.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), depths.end());

    const std::string& field = c.settings.drillholeField;
    auto pd = vtkSmartPointer<vtkPolyData>::New();
    vtkNew<vtkPoints> pts;
    vtkNew<vtkCellArray> cells;
    vtkNew<vtkFloatArray> values;
    values->SetName("value");
    auto bounds = BoundingBox::empty();
    for (std::size_t i = 0; i < depths.size(); ++i) {
        const Vec3 w = dh.at(depths[i]);
        bounds.expand(w);
        const Vec3 q = c.origin.toLocal(w);
        pts->InsertNextPoint(q.x, q.y, q.z);
    }
    for (std::size_t i = 1; i < depths.size(); ++i) {
        const vtkIdType ids[2] = {static_cast<vtkIdType>(i - 1), static_cast<vtkIdType>(i)};
        cells->InsertNextCell(2, ids);
        double v = kNaN;
        if (!field.empty()) {
            const double mid = (depths[i - 1] + depths[i]) / 2;
            for (const auto& iv : h.intervals)
                if (mid >= iv.from && mid < iv.to) {
                    if (auto it = iv.values.find(field); it != iv.values.end()) v = it->second;
                    break;
                }
        }
        values->InsertNextValue(static_cast<float>(v));
    }
    pd->SetPoints(pts);
    pd->SetLines(cells);
    pd->GetCellData()->SetScalars(values);

    auto it = baseItem(owner, "trace", SceneItem::Prim::Lines);
    it.data = pd;
    it.lineWidth = c.settings.drillholeWidth;
    it.tubes = true;
    it.color = categoricalColor(h.holeType);
    it.bounds = bounds;
    if (!field.empty() && c.drillholeRange) {
        ScalarDisplay sd;
        sd.name = "value";
        sd.cellData = true;
        sd.min = c.drillholeRange->first;
        sd.max = c.drillholeRange->second > sd.min ? c.drillholeRange->second : sd.min + 1;
        sd.lut = Lut::Rainbow;
        sd.title = field;
        it.scalars = sd;
    }
    items.push_back(std::move(it));
    if (c.settings.drillholeLabels) {
        auto lb = baseItem(owner, "label", SceneItem::Prim::Label);
        lb.labelText = h.holeId;
        lb.labelAnchor = c.origin.toLocal(h.collar);
        lb.pickable = false;
        lb.color = 0xFFFFFF;
        lb.bounds.expand(h.collar);
        items.push_back(std::move(lb));
    }
    return items;
}

// ---- block models -------------------------------------------------------------------------------

std::vector<SceneItem> buildBlockModel(const BuildContext& c, const BlockModel& b) {
    std::vector<SceneItem> items;
    const SelectionItem owner{EntityKind::BlockModel, b.id};
    const geology::BlockGrid grid(b);
    BlockModelDisplay disp;
    if (auto it = c.settings.blockModels.find(b.id); it != c.settings.blockModels.end()) disp = it->second;

    // Extent outline (always available, even before values load).
    const auto corners = grid.corners();
    {
        // corners(): bit 0 = +x, bit 1 = +y, bit 2 = +z.
        static constexpr int e[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        std::vector<std::pair<Vec3, Vec3>> segs;
        for (const auto& ed : e) segs.emplace_back(corners[static_cast<std::size_t>(ed[0])], corners[static_cast<std::size_t>(ed[1])]);
        auto it = baseItem(owner, "outline", SceneItem::Prim::Lines);
        it.data = segmentsData(segs, c.origin);
        it.color = 0xB0B0B0;
        it.lineWidth = 1.5;
        it.lighting = false;
        for (const auto& p : corners) it.bounds.expand(p);
        if (disp.outline || !c.blockData) items.push_back(std::move(it));
    }
    if (!c.blockData) return items;
    const auto& data = *c.blockData;
    std::string attr = disp.attribute;
    if (attr.empty() && !b.attributes.empty()) attr = b.attributes.front();
    const auto* values = data.find(attr);
    if (!values || values->size() != data.count()) return items;

    const int nx = b.nx, ny = b.ny, nz = b.nz;
    const auto n = data.count();
    std::vector<char> vis(n, 0);
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    for (std::size_t i = 0; i < n; ++i) {
        const double v = (*values)[i];
        if (!std::isfinite(v)) continue;
        if (disp.filterMin && v < *disp.filterMin) continue;
        if (disp.filterMax && v > *disp.filterMax) continue;
        vis[i] = 1;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (lo > hi) return items;
    auto visible = [&](int i, int j, int k) {
        if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return false;
        return vis[grid.index(i, j, k)] != 0;
    };
    const Vec3 sz = b.blockSize;
    auto corner = [&](int i, int j, int k) { return c.origin.toLocal(grid.toWorld({i * sz.x, j * sz.y, k * sz.z})); };

    auto pd = vtkSmartPointer<vtkPolyData>::New();
    vtkNew<vtkPoints> pts;
    vtkNew<vtkCellArray> polys;
    vtkNew<vtkFloatArray> scal;
    scal->SetName("value");
    auto bounds = BoundingBox::empty();
    // Faces between a visible block and an invisible neighbour (or the model boundary).
    struct Face {
        int di, dj, dk;
        int c[4][3];
    };
    static constexpr Face faces[6] = {
        {-1, 0, 0, {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}, {1, 0, 0, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}},
        {0, -1, 0, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}}, {0, 1, 0, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}},
        {0, 0, -1, {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}}, {0, 0, 1, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}},
    };
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const auto idx = grid.index(i, j, k);
                if (!vis[idx]) continue;
                const float v = static_cast<float>((*values)[idx]);
                for (const auto& f : faces) {
                    if (visible(i + f.di, j + f.dj, k + f.dk)) continue;
                    vtkIdType ids[4];
                    for (int q = 0; q < 4; ++q) {
                        const Vec3 p = corner(i + f.c[q][0], j + f.c[q][1], k + f.c[q][2]);
                        ids[q] = pts->InsertNextPoint(p.x, p.y, p.z);
                    }
                    polys->InsertNextCell(4, ids);
                    scal->InsertNextValue(v);
                }
                bounds.expand(c.origin.toWorld(corner(i, j, k)));
                bounds.expand(c.origin.toWorld(corner(i + 1, j + 1, k + 1)));
            }
    pd->SetPoints(pts);
    pd->SetPolys(polys);
    pd->GetCellData()->SetScalars(scal);
    auto it = baseItem(owner, "blocks", SceneItem::Prim::Surface);
    it.data = pd;
    it.opacity = disp.opacity;
    it.bounds = bounds;
    ScalarDisplay sd;
    sd.name = "value";
    sd.cellData = true;
    sd.min = lo;
    sd.max = hi > lo ? hi : lo + 1;
    sd.lut = disp.lut;
    sd.title = b.name + ": " + attr;
    it.scalars = sd;
    items.push_back(std::move(it));
    return items;
}

// ---- geotechnical -------------------------------------------------------------------------------

std::vector<SceneItem> buildStructure(const BuildContext& c, const StructuralMeasurement& s) {
    std::vector<SceneItem> items;
    const double dip = s.dip * std::numbers::pi / 180.0, dd = s.dipDirection * std::numbers::pi / 180.0;
    // Upward pole of the plane (dip direction measured clockwise from grid north = +Y).
    const Vec3 n{std::sin(dip) * std::sin(dd), std::sin(dip) * std::cos(dd), std::cos(dip)};
    const Vec3 u = anyPerpendicular(n);
    const Vec3 v = cross(n, u);
    const double r = s.persistence > 0 ? s.persistence / 2 : c.settings.structureDiscRadius;
    MeshData disc;
    disc.vertices.push_back(s.location);
    const int segs = 32;
    for (int i = 0; i < segs; ++i) {
        const double a = 2 * std::numbers::pi * i / segs;
        disc.vertices.push_back(s.location + u * (r * std::cos(a)) + v * (r * std::sin(a)));
    }
    for (int i = 0; i < segs; ++i)
        disc.triangles.push_back({0u, static_cast<std::uint32_t>(1 + i), static_cast<std::uint32_t>(1 + (i + 1) % segs)});
    auto it = baseItem({EntityKind::Structure, s.id}, "disc", SceneItem::Prim::Surface);
    it.data = meshPolyData(disc, c.origin, false);
    it.color = categoricalColor(s.setName.empty() ? std::string(toString(s.type)) : s.setName);
    it.opacity = 0.85;
    it.bounds = boundsOf(disc);
    items.push_back(std::move(it));
    return items;
}

std::vector<SceneItem> buildClassification(const BuildContext& c, const GeotechClassification& g) {
    std::vector<SceneItem> items;
    if (g.location == Vec3{} && g.developmentId.isNil()) return items;
    Vec3 at = g.location;
    if (at == Vec3{}) {
        // Located by chainage on the heading.
        if (const auto* d = c.project.model().developments.find(g.developmentId)) {
            const auto cl = mine_design::centrelinePoints(c.project, *d);
            if (auto p = geometry::pointAtChainage(cl, g.chainage - d->chainageFrom)) at = *p;
            else return items;
        } else {
            return items;
        }
    }
    auto it = baseItem({EntityKind::GeotechClassification, g.id}, "marker", SceneItem::Prim::Points);
    it.data = pointsData({at}, c.origin);
    it.color = categoricalColor(g.rockClass);
    it.pointSize = 10;
    it.spheres = true;
    it.bounds.expand(at);
    items.push_back(std::move(it));
    auto lb = baseItem({EntityKind::GeotechClassification, g.id}, "label", SceneItem::Prim::Label);
    lb.labelText = std::string(g.method == GeotechMethod::QSystem ? "Q=" : "RMR=") + formatNumber(g.value, g.method == GeotechMethod::QSystem ? 2 : 0).toStdString();
    lb.labelAnchor = c.origin.toLocal(at);
    lb.labelSize = 10;
    lb.pickable = false;
    lb.bounds.expand(at);
    items.push_back(std::move(lb));
    return items;
}

std::vector<SceneItem> buildPointCloudBox(const BuildContext& c, const PointCloudDataset& d) {
    std::vector<SceneItem> items;
    if (d.storedBounds.isEmpty()) return items;
    const auto& sb = d.storedBounds;
    std::array<Vec3, 8> p;
    for (int i = 0; i < 8; ++i)
        p[static_cast<std::size_t>(i)] = d.transform.apply({(i & 1) ? sb.max.x : sb.min.x, (i & 2) ? sb.max.y : sb.min.y, (i & 4) ? sb.max.z : sb.min.z});
    static constexpr int e[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    std::vector<std::pair<Vec3, Vec3>> segs;
    for (const auto& ed : e) segs.emplace_back(p[static_cast<std::size_t>(ed[0])], p[static_cast<std::size_t>(ed[1])]);
    auto it = baseItem({EntityKind::PointCloud, d.id}, "box", SceneItem::Prim::Lines);
    it.data = segmentsData(segs, c.origin);
    it.color = 0x7FB3D5;
    it.lineWidth = 1.0;
    it.lighting = false;
    it.ghost = true;
    for (const auto& q : p) it.bounds.expand(q);
    items.push_back(std::move(it));
    return items;
}

} // namespace openmine::app::scene
