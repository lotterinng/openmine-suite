#pragma once
// SPDX-License-Identifier: Apache-2.0
// Solid-modelling kernel facade over Open CASCADE. No OCCT type appears in this header, so core,
// modules, the UI and the Python bindings stay independent of OCCT. Shapes are immutable values;
// every operation returns a new shape and throws SolidError with OCCT's diagnosis on failure.

#include "openmine/geometry/Transform.h"
#include "openmine/objects/CadEntities.h"
#include "openmine/objects/Common.h"

#include <memory>
#include <string>
#include <vector>

namespace openmine::occt {

class SolidError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Shape {
public:
    Shape();
    ~Shape();
    Shape(const Shape&);
    Shape& operator=(const Shape&);
    bool isNull() const;
    struct Impl;
    explicit Shape(std::shared_ptr<const Impl> impl);
    const Impl& impl() const;

private:
    std::shared_ptr<const Impl> impl_;
};

struct ShapeInfo {
    bool valid = false;         ///< BRepCheck_Analyzer verdict
    bool closed = false;        ///< every shell is closed
    double volume = 0;          ///< m³ (solids)
    double area = 0;            ///< m² (all faces)
    BoundingBox bounds = BoundingBox::empty();
    Vec3 centreOfMass;
    int solids = 0, shells = 0, faces = 0, edges = 0, vertices = 0;
    std::vector<std::string> problems; ///< validity problems found (empty when valid)
};

// ---- Primitives ----------------------------------------------------------------------------------
Shape makeBox(Vec3 corner1, Vec3 corner2);
Shape makeCylinder(Vec3 base, Vec3 axis, double radius, double height);
Shape makeCone(Vec3 base, Vec3 axis, double radiusBase, double radiusTop, double height);
Shape makeSphere(Vec3 centre, double radius);

// ---- Profiles, extrusion, revolution, sweep, loft --------------------------------------------------
/// Edge/wire from a curve object, exact for lines, plan arcs, circles and splines (Bézier spans).
Shape wireFromCurve(const GeometryObject& curve);
/// Planar face bounded by a closed curve, with optional holes (closed curves inside it).
Shape planarFace(const GeometryObject& outer, const std::vector<const GeometryObject*>& holes = {});
/// Planar face from a closed polygon.
Shape planarFace(const std::vector<Vec3>& ring);
/// Prism: a face swept along `vector` (a wire gives a shell).
Shape extrude(const Shape& profile, Vec3 vector);
/// Revolution of a face about an axis by `angleRad` (2π = full).
Shape revolve(const Shape& profile, Vec3 axisPoint, Vec3 axisDirection, double angleRad);

enum class SweepTransition { Transformed, RightCorner, RoundCorner };
/// Sweeps a 2D profile (section coordinates: u to the right of travel, v up) along a path curve.
/// The profile is placed perpendicular to the path at its start with its up axis in the vertical
/// plane (mine-development convention) and keeps a fixed binormal (+Z) along the path, so
/// sections stay upright through declines and bends. Result: a closed solid.
Shape sweepProfile(const std::vector<Vec2>& profile, const GeometryObject& path, SweepTransition t = SweepTransition::RightCorner);
/// As above with an exact profile: `bulges[i]` makes the edge i → i+1 a circular arc (DXF bulge,
/// in profile coordinates), so curved backs and shoulders are true cylinders in the solid.
Shape sweepProfile(const std::vector<Vec2>& profile, const std::vector<double>& bulges, const GeometryObject& path,
                   SweepTransition t = SweepTransition::RightCorner);
/// Loft through closed sections (polygons), solid or shell, smooth or ruled.
Shape loft(const std::vector<std::vector<Vec3>>& sections, bool solid = true, bool ruled = false);

// ---- Booleans and modifications ------------------------------------------------------------------
enum class BooleanOp { Union, Difference, Intersection };
/// Boolean operation; `fuzzy` (m) tolerates near-coincident faces. Coplanar faces of the result
/// are merged. Throws if OCCT reports an error or the result is empty for Union/Difference.
Shape boolean(const Shape& a, const Shape& b, BooleanOp op, double fuzzy = 0.0);
/// Rounds all edges with `radius`.
Shape filletAllEdges(const Shape& s, double radius);
/// Offset surface of a solid by `distance` (positive = outward), arcs at convex edges.
Shape offsetSolid(const Shape& s, double distance);
/// Rigid/similarity transforms keep exact geometry; general affine transforms are applied with
/// BRepBuilderAPI_GTransform (curves become B-splines).
Shape transformed(const Shape& s, const Transform3& t);
/// Shape healing (ShapeFix_Shape + solid orientation). `report` receives what was fixed.
Shape fix(const Shape& s, std::vector<std::string>* report = nullptr);
/// Merges coplanar/co-cylindrical faces (ShapeUpgrade_UnifySameDomain).
Shape unify(const Shape& s);

// ---- Mesh conversion ---------------------------------------------------------------------------------
/// Sews a triangle mesh into a shell, and into a solid when it is closed. `mergeFaces` unifies
/// coplanar triangles into larger planar faces.
Shape fromMesh(const MeshData& mesh, double sewingTolerance = 1e-6, bool mergeFaces = true);
/// Triangulation for display / mesh operations, outward-oriented, vertices merged.
MeshData tessellate(const Shape& s, double linearDeflection = 0.01, double angularDeflectionRad = 0.35);

// ---- Queries -------------------------------------------------------------------------------------------
ShapeInfo analyse(const Shape& s);
/// Intersection of the shape with a plane as polylines (closed loops where the section is closed).
std::vector<std::vector<Vec3>> section(const Shape& s, Vec3 planePoint, Vec3 planeNormal, double deflection = 0.001);
/// Minimum distance between two shapes and the closest points.
struct DistanceResult {
    double distance = 0;
    Vec3 onFirst, onSecond;
};
DistanceResult minimumDistance(const Shape& a, const Shape& b);
/// Point classification for solids: inside, on boundary (within tolerance) or outside.
enum class PointState { Inside, On, Outside };
PointState classify(const Shape& solid, Vec3 p, double tolerance = 1e-7);

// ---- Serialisation / exchange ---------------------------------------------------------------------
std::string toBrep(const Shape& s);
Shape fromBrep(const std::string& bytes);
void writeStep(const std::vector<Shape>& shapes, const std::string& file);
std::vector<Shape> readStep(const std::string& file);
void writeIges(const std::vector<Shape>& shapes, const std::string& file);
std::vector<Shape> readIges(const std::string& file);

} // namespace openmine::occt
