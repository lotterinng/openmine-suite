#pragma once
// SPDX-License-Identifier: Apache-2.0
// A drawing that contains every OpenMine geometry type, shared by the DXF unit tests and the
// ezdxf validation tool.

#include "openmine/cad/Edit.h"

#include <memory>
#include <vector>

namespace openmine::testing {

struct Drawing {
    std::vector<CadLayer> layers;
    std::vector<GeometryObject> objects;
    std::vector<const GeometryObject*> ptrs() const {
        std::vector<const GeometryObject*> v;
        for (const auto& o : objects) v.push_back(&o);
        return v;
    }
};

inline Drawing sampleDrawing() {
    Drawing d;
    CadLayer a;
    a.id = Uuid::generate();
    a.name = "Design/Centrelines"; // '/' is not allowed in DXF names
    a.colorRgb = 0x12AB34;
    a.lineType = "Dashed";
    a.transparency = 0.4;
    a.lineWidth = 3.5;
    a.description = "Décline centrelines";
    CadLayer b;
    b.id = Uuid::generate();
    b.name = "Survey";
    b.colorRgb = 0xFF0000;
    b.visible = false;
    b.locked = true;
    d.layers = {a, b};
    auto add = [&](GeometryObject g, const CadLayer& l, std::string name) {
        g.layerId = l.id;
        g.name = std::move(name);
        d.objects.push_back(std::move(g));
    };
    add(cad::makeLine({1000.5, 2000.25, -350}, {1010, 2005, -351.5}), a, "line");
    GeometryObject pt;
    pt.type = GeometryType::Point;
    pt.vertices = {{5, 6, 7}};
    add(pt, b, "station");
    auto lw = cad::makePolyline({{0, 0, -100}, {10, 0, -100}, {20, 10, -100}, {20, 20, -100}});
    lw.bulges = {0.0, 0.41421356237, 0.0, 0.0};
    add(lw, a, "planar bulged");
    add(cad::makePolyline({{0, 0, 0}, {5, 0, -1}, {5, 5, -2}}, true), a, "3d closed");
    auto p3b = cad::makePolyline({{0, 0, 0}, {10, 0, -1}, {20, 0, -2}});
    p3b.bulges = {0.5, 0.0, 0.0};
    add(p3b, a, "3d bulged");
    add(cad::makeArc({100, 200, -50}, 7.5, 0.3, 2.2), a, "arc");
    add(cad::makeCircle({100, 200, -50}, 3), b, "circle");
    GeometryObject tilted;
    tilted.type = GeometryType::Arc;
    tilted.arc.center = {50, 50, 10};
    tilted.arc.radius = 4;
    tilted.arc.normal = Vec3{1, 1, 1}.normalized();
    tilted.arc.refDir = Vec3{1, -1, 0}.normalized();
    tilted.arc.startAngle = 0.2;
    tilted.arc.endAngle = 1.9;
    add(tilted, a, "tilted arc");
    add(cad::makeSpline({{0, 0, 0}, {10, 5, 1}, {20, -3, 2}, {35, 4, 2.5}}), a, "spline");
    add(cad::makeSpline({{0, 0, 5}, {10, 0, 5}, {10, 10, 5}, {0, 10, 5}}, true), a, "closed spline");
    add(cad::makeText({1, 2, 3}, "Level 1 — Décline", 2.5, 0.5, TextAlign::Center), b, "label");
    add(cad::makeText({4, 5, 6}, "Line one\nLine two", 1.5, 0.0, TextAlign::Left), b, "notes");
    add(cad::makeDimension(DimensionKind::Aligned, {{0, 0, 0}, {30, 40, 0}, {0, 0, 0}}, 2.0, 2), b, "aligned");
    add(cad::makeDimension(DimensionKind::Horizontal, {{0, 0, 0}, {30, 40, 0}, {0, -5, 0}}, 1.0, 3), b, "horizontal");
    add(cad::makeDimension(DimensionKind::Vertical, {{0, 0, 0}, {30, 40, 0}, {-5, 0, 0}}, 1.0, 1), b, "vertical");
    add(cad::makeDimension(DimensionKind::Angular, {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {7, 7, 0}}, 1.0, 1), b, "angular");
    add(cad::makeDimension(DimensionKind::Radius, {{0, 0, 0}, {0, 2.5, 0}}), b, "radius");
    add(cad::makeDimension(DimensionKind::Diameter, {{1, 1, 0}, {4, 5, 0}}), b, "diameter");
    auto elev = cad::makeDimension(DimensionKind::Elevation, {{10, 10, -812.25}}, 1.0, 2);
    add(elev, b, "rl");
    add(cad::makeHatch({{{0, 0, -5}, {10, 0, -5}, {10, 10, -5}, {0, 10, -5}}, {{4, 4, -5}, {6, 4, -5}, {6, 6, -5}, {4, 6, -5}}}, "ANSI31", 0.75, 0.2),
        a, "hatch");
    add(cad::makeHatch({{{0, 0, 0}, {3, 0, 0}, {0, 3, 0}}}, "SOLID", 1.0), a, "solid");
    add(cad::makeHatch({{{0, 0, 0}, {3, 0, 0}, {3, 3, 0}}}, "CROSS", 0.5, 0.1), a, "cross");
    add(cad::makeHatch({{{0, 0, 0}, {3, 0, 0}, {3, 3, 0}}}, "DOTS", 0.25, 0.0), a, "dots");
    auto m = std::make_shared<MeshData>();
    m->vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    m->triangles = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}};
    GeometryObject mesh;
    mesh.type = GeometryType::Mesh;
    mesh.mesh = m;
    mesh.colorOverride = 0x336699;
    mesh.transparency = 0.25;
    add(mesh, b, "tetra");
    return d;
}

} // namespace openmine::testing
