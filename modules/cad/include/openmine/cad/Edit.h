#pragma once
// SPDX-License-Identifier: Apache-2.0
// CAD editing algorithms. They are pure functions on geometry objects: callers (UI, console,
// Python) apply the results through ProjectEditor so every edit is validated, undoable and
// audited. All distances in metres; plan operations as described in Primitives.h.

#include "openmine/cad/Primitives.h"

#include <optional>
#include <string>
#include <vector>
#include <stdexcept>

namespace openmine::cad {

class EditError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---- Construction ------------------------------------------------------------------------------
GeometryObject makeLine(Vec3 a, Vec3 b);
GeometryObject makePolyline(std::vector<Vec3> pts, bool closed = false);
/// Axis-aligned rectangle from two opposite corners (elevation of the first corner).
GeometryObject makeRectangle(Vec3 c1, Vec3 c2);
/// Regular polygon. `inscribed`: vertices on the circle of `radius`; otherwise edges tangent to it.
GeometryObject makePolygon(Vec3 centre, double radius, int sides, double rotationRad = 0, bool inscribed = true);
GeometryObject makeCircle(Vec3 centre, double radius);
GeometryObject makeArc3(Vec3 p1, Vec3 p2, Vec3 p3);
GeometryObject makeArc(Vec3 centre, double radius, double startRad, double endRad);
GeometryObject makeSpline(std::vector<Vec3> fit, bool closed = false);
GeometryObject makeText(Vec3 insertion, const std::string& text, double height, double rotationRad = 0,
                        TextAlign align = TextAlign::Left);
GeometryObject makeHatch(const std::vector<std::vector<Vec3>>& loops, const std::string& pattern, double spacing, double angleRad = 0);
GeometryObject makeDimension(DimensionKind kind, std::vector<Vec3> definitionPoints, double textHeight = 1.0, int precision = 3);

// ---- Grips (direct editing of defining points) ------------------------------------------------------
enum class GripKind {
    Vertex,    ///< a vertex / fit point / definition point (index into `vertices`)
    Centre,    ///< arc or circle centre: moves the whole curve
    Radius,    ///< point on an arc (mid-arc) or circle (at the reference direction): sets the radius
    ArcStart,  ///< arc start point: sets the start angle (centre and radius kept)
    ArcEnd,    ///< arc end point: sets the end angle
};
struct Grip {
    GripKind kind = GripKind::Vertex;
    std::size_t vertex = 0; ///< for Vertex grips
    Vec3 point;
};
/// The editable defining points of an object: vertices of points, lines, polylines, splines (fit
/// points), text (insertion), dimensions (definition points) and hatch boundaries; centre, radius
/// and end grips of arcs and circles. Meshes and solids have none.
std::vector<Grip> grips(const GeometryObject& g);
/// The object with grip `index` (of grips(g)) moved to `to`. Arc/circle grips act in the curve's
/// plane (`to` is projected onto it). Throws EditError for an invalid index or a degenerate result
/// (zero radius, coincident line ends, zero-length arc).
GeometryObject moveGrip(const GeometryObject& g, std::size_t index, Vec3 to);

// ---- Transform-type edits (move, copy, rotate, scale, mirror) ---------------------------------
/// Throws EditError if the transform cannot be represented (e.g. non-uniform scale of an arc).
GeometryObject transformed(const GeometryObject& g, const Transform3& t);

// ---- Offset -----------------------------------------------------------------------------------------
enum class JoinStyle { Miter, Round };
/// Parallel copy at `distance` towards the side of `sidePoint` (plan). Lines → lines, arcs/circles →
/// concentric arcs, polylines → polylines with mitred or round joins and collapsed segments
/// removed; splines are offset as fine polylines. Throws EditError when the offset vanishes.
GeometryObject offset(const GeometryObject& g, double distance, Vec3 sidePoint, JoinStyle join = JoinStyle::Miter);

// ---- Trim / extend / split / join / explode -----------------------------------------------------
/// Removes the part of `target` between the cutting intersections that contains `pickPoint`.
/// Returns 0, 1 or 2 objects (the remaining pieces). Throws if no cutting edge crosses the target.
std::vector<GeometryObject> trim(const GeometryObject& target, const std::vector<const GeometryObject*>& cuttingEdges, Vec3 pickPoint);

/// Extends the end of `target` nearest to `pickPoint` to the nearest boundary intersection along
/// its extension (lines/polyline end segments straight, arcs along their circle).
GeometryObject extend(const GeometryObject& target, const std::vector<const GeometryObject*>& boundaries, Vec3 pickPoint);

/// Splits a curve at the point nearest to `at` (closed curves are opened there).
std::vector<GeometryObject> split(const GeometryObject& g, Vec3 at);

/// Joins curves whose end points coincide within `tolerance` into one polyline (closing it if the
/// chain is closed). Objects that cannot be chained are returned separately.
std::vector<GeometryObject> join(const std::vector<GeometryObject>& objects, double tolerance = 1e-4);

/// Explodes polylines into lines/arcs, hatches into boundary polylines, dimensions into lines and
/// text, splines into lines. Other types are returned unchanged.
std::vector<GeometryObject> explode(const GeometryObject& g);

// ---- Fillet / chamfer -------------------------------------------------------------------------------
struct CornerResult {
    GeometryObject first, second;       ///< trimmed/extended inputs
    std::optional<GeometryObject> join; ///< fillet arc or chamfer line (none for radius 0)
};
/// Fillets two lines (or line-like end segments of polylines) with a tangent arc of `radius`
/// in plan; the arc's elevation is interpolated. Radius 0 makes a sharp corner.
CornerResult fillet(const GeometryObject& a, Vec3 pickA, const GeometryObject& b, Vec3 pickB, double radius);
CornerResult chamfer(const GeometryObject& a, Vec3 pickA, const GeometryObject& b, Vec3 pickB, double d1, double d2);
/// Fillets the corner of a polyline nearest to `near` (replaces the vertex by a bulge arc).
GeometryObject filletPolylineVertex(const GeometryObject& poly, Vec3 near, double radius);

} // namespace openmine::cad
