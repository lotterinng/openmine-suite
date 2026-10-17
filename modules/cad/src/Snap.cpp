// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Snap.h"

#include "openmine/geometry/Curves.h"

#include <cmath>
#include <numbers>

namespace openmine::cad {

const char* toString(SnapMode m) {
    switch (m) {
    case SnapMode::Endpoint: return "Endpoint";
    case SnapMode::Midpoint: return "Midpoint";
    case SnapMode::Center: return "Center";
    case SnapMode::Intersection: return "Intersection";
    case SnapMode::Perpendicular: return "Perpendicular";
    case SnapMode::Tangent: return "Tangent";
    case SnapMode::Nearest: return "Nearest";
    case SnapMode::Grid: return "Grid";
    case SnapMode::Node: return "Node";
    }
    return "?";
}

std::vector<Vec3> snapPoints(const GeometryObject& g, SnapMode mode) {
    std::vector<Vec3> out;
    switch (mode) {
    case SnapMode::Endpoint:
        if (g.type == GeometryType::Line || g.type == GeometryType::Polyline || g.type == GeometryType::Spline)
            out = g.vertices;
        else if (g.type == GeometryType::Arc) out = {geometry::arcPoint(g.arc, g.arc.startAngle), geometry::arcPoint(g.arc, g.arc.endAngle)};
        else if (g.type == GeometryType::Hatch || g.type == GeometryType::Dimension) out = g.vertices;
        break;
    case SnapMode::Midpoint:
        if (geometry::isCurve(g.type))
            for (const auto& p : decompose(g)) out.push_back(p.at(0.5));
        break;
    case SnapMode::Center:
        if (g.type == GeometryType::Arc || g.type == GeometryType::Circle) out.push_back(g.arc.center);
        else if (g.type == GeometryType::Polyline)
            for (const auto& p : decompose(g))
                if (p.kind == Prim::Arc) out.push_back({p.c.x, p.c.y, (p.z0 + p.z1) / 2});
        break;
    case SnapMode::Node:
        if (g.type == GeometryType::Point || g.type == GeometryType::Text) out = g.vertices;
        if ((g.type == GeometryType::Mesh || g.type == GeometryType::Solid) && g.mesh && g.mesh->vertices.size() <= 200000)
            out = g.mesh->vertices;
        break;
    case SnapMode::Intersection:
        if (geometry::isCurve(g.type))
            for (const auto& x : intersectObjects(g, g)) out.push_back(x.pointOnFirst);
        break;
    default: break;
    }
    return out;
}

std::optional<SnapResult> findSnap(const std::vector<const GeometryObject*>& cands, Vec2 cursor, Vec3 cursorWorld,
                                   const ProjectFn& project, const SnapSettings& s, std::optional<Vec3> from) {
    struct Hit {
        SnapResult r;
        int priority;
    };
    std::optional<Hit> best;
    auto consider = [&](SnapMode mode, int priority, Vec3 p, const Uuid& id) {
        const double d = distance(project(p), cursor);
        if (d > s.aperturePixels) return;
        if (!best || priority < best->priority || (priority == best->priority && d < best->r.screenDistance))
            best = Hit{{mode, p, id, d}, priority};
    };
    for (const auto* g : cands) {
        if (!g) continue;
        for (SnapMode m : {SnapMode::Endpoint, SnapMode::Node, SnapMode::Midpoint, SnapMode::Center})
            if (has(s.modes, m))
                for (auto p : snapPoints(*g, m)) consider(m, m == SnapMode::Endpoint || m == SnapMode::Node ? 0 : (m == SnapMode::Midpoint ? 2 : 3), p, g->id);
    }
    if (has(s.modes, SnapMode::Intersection))
        for (std::size_t i = 0; i < cands.size(); ++i)
            for (std::size_t j = i; j < cands.size(); ++j) {
                if (!cands[i] || !cands[j] || !geometry::isCurve(cands[i]->type) || !geometry::isCurve(cands[j]->type)) continue;
                for (const auto& x : intersectObjects(*cands[i], *cands[j])) consider(SnapMode::Intersection, 1, x.pointOnFirst, cands[i]->id);
            }
    if (from && (has(s.modes, SnapMode::Perpendicular) || has(s.modes, SnapMode::Tangent)))
        for (const auto* g : cands) {
            if (!g || !geometry::isCurve(g->type)) continue;
            for (const auto& p : decompose(*g)) {
                if (has(s.modes, SnapMode::Perpendicular)) {
                    const double t = p.closestParam(xy(*from), true);
                    if (t >= -1e-9 && t <= 1 + 1e-9) consider(SnapMode::Perpendicular, 4, p.at(std::clamp(t, 0.0, 1.0)), g->id);
                }
                if (has(s.modes, SnapMode::Tangent) && p.kind == Prim::Arc) {
                    const Vec2 f = xy(*from);
                    const double dist = distance(f, p.c);
                    if (dist <= p.r) continue;
                    const double base = std::atan2(f.y - p.c.y, f.x - p.c.x), off = std::acos(p.r / dist);
                    for (double ang : {base + off, base - off}) {
                        // Is the tangent point on this arc?
                        const Prim full = p;
                        const double tParam = full.closestParam({p.c.x + p.r * std::cos(ang), p.c.y + p.r * std::sin(ang)});
                        const Vec3 tp = p.at(tParam);
                        if (horizontalDistance(tp, {p.c.x + p.r * std::cos(ang), p.c.y + p.r * std::sin(ang), 0}) < 1e-6)
                            consider(SnapMode::Tangent, 5, tp, g->id);
                    }
                }
            }
        }
    if (has(s.modes, SnapMode::Nearest))
        for (const auto* g : cands) {
            if (!g || !geometry::isCurve(g->type)) continue;
            for (const auto& p : decompose(*g)) consider(SnapMode::Nearest, 6, p.at(p.closestParam(xy(cursorWorld))), g->id);
        }
    if (best) return best->r;
    if (has(s.modes, SnapMode::Grid) && s.gridSpacing > 0) {
        SnapResult r;
        r.mode = SnapMode::Grid;
        r.point = {s.gridOriginX + std::round((cursorWorld.x - s.gridOriginX) / s.gridSpacing) * s.gridSpacing,
                   s.gridOriginY + std::round((cursorWorld.y - s.gridOriginY) / s.gridSpacing) * s.gridSpacing, cursorWorld.z};
        r.screenDistance = distance(project(r.point), cursor);
        return r;
    }
    return std::nullopt;
}

} // namespace openmine::cad
