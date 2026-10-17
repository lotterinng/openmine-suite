#pragma once
// SPDX-License-Identifier: Apache-2.0
// CAD entities: layers and the typed geometry object.

#include "openmine/objects/Common.h"

#include <array>
#include <memory>

namespace openmine {

struct CadLayer {
    Uuid id;
    std::string name;
    std::uint32_t colorRgb = 0xD0D0D0; ///< 0xRRGGBB
    bool visible = true;
    bool locked = false;
    double transparency = 0.0; ///< 0 = opaque … 1 = invisible
    double lineWidth = 2.0;    ///< screen pixels
    std::string lineType = "Continuous"; ///< Continuous | Dashed | Dotted | DashDot
    std::string description;
    EntityMetadata meta;
};

enum class GeometryType {
    Point,
    Line,
    Polyline,  ///< open/closed; optional per-vertex bulges (plan arcs, DXF convention)
    Arc,       ///< circular arc in an arbitrary plane
    Circle,
    Spline,    ///< centripetal Catmull–Rom through fit points (exactly convertible to cubic B-spline)
    Text,
    Dimension,
    Hatch,
    Mesh,      ///< triangulated surface or closed wireframe
    Solid,     ///< B-rep solid (Open CASCADE), stored as an asset; tessellation cached in `mesh`
};

/// Circular arc/circle definition. The arc lies in the plane through `center` with
/// normal `normal`; angles are measured counter-clockwise about `normal` from `refDir`.
struct ArcData {
    Vec3 center;
    double radius = 0.0;
    Vec3 normal{0, 0, 1};
    Vec3 refDir{1, 0, 0};
    double startAngle = 0.0; ///< radians
    double endAngle = 0.0;   ///< radians; end > start (sweep = end − start)
    friend bool operator==(const ArcData&, const ArcData&) = default;
};

enum class TextAlign { Left, Center, Right };

struct TextData {
    std::string text;
    double height = 1.0;   ///< metres
    double rotation = 0.0; ///< radians about +Z, CCW from +X
    TextAlign align = TextAlign::Left;
    friend bool operator==(const TextData&, const TextData&) = default;
};

enum class DimensionKind { Aligned, Horizontal, Vertical, Angular, Radius, Diameter, Elevation };

/// Associative-free dimension annotation. Definition points in `vertices`:
///  Aligned/Horizontal/Vertical: [p1, p2, dimLinePoint]
///  Angular: [vertex, p1, p2, arcPoint]   Radius/Diameter: [center, pointOnCircle]
///  Elevation: [point]
struct DimensionData {
    DimensionKind kind = DimensionKind::Aligned;
    std::string overrideText; ///< empty = measured value
    int precision = 3;
    double textHeight = 1.0;
    friend bool operator==(const DimensionData&, const DimensionData&) = default;
};

struct HatchData {
    std::string pattern = "ANSI31"; ///< SOLID | ANSI31 (45° lines) | LINES | CROSS | DOTS
    double angle = 0.0;     ///< radians, added to the pattern angle
    double spacing = 1.0;   ///< metres between pattern lines
    /// Start index (into `vertices`) of each boundary loop; loop i spans [loopStarts[i], loopStarts[i+1]).
    std::vector<std::uint32_t> loopStarts;
    friend bool operator==(const HatchData&, const HatchData&) = default;
};

/// Immutable triangle mesh (shared between undo snapshots to keep commands cheap).
struct MeshData {
    std::vector<Vec3> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<float> vertexScalars; ///< optional per-vertex scalar (e.g. distance), empty if none
    std::string scalarName;
};
using MeshPtr = std::shared_ptr<const MeshData>;

/// A CAD/design geometry object. Which fields are meaningful depends on `type`:
///  Point: vertices[0]            Line: vertices[0..1]
///  Polyline: vertices, closed, bulges (optional, size == vertices.size())
///  Arc/Circle: arc               Spline: vertices = fit points, closed
///  Text: vertices[0] = insertion, text     Dimension: vertices = definition points, dimension
///  Hatch: vertices = boundary loops, hatch   Mesh: mesh   Solid: brepAsset + mesh (tessellation)
struct GeometryObject {
    Uuid id;
    std::string name;
    GeometryType type = GeometryType::Polyline;
    Uuid layerId;
    std::vector<Vec3> vertices;
    std::vector<double> bulges; ///< DXF bulge = tan(θ/4) for the arc from vertex i to i+1 (plan)
    bool closed = false;
    ArcData arc;
    TextData text;
    DimensionData dimension;
    HatchData hatch;
    MeshPtr mesh;          ///< Mesh/Solid
    AssetRef brepAsset;    ///< Solid
    Uuid materialId;
    std::optional<std::uint32_t> colorOverride; ///< 0xRRGGBB; nullopt = by layer
    std::optional<double> transparency;         ///< nullopt = by layer
    AttributeMap attributes;
    EntityMetadata meta;
};

} // namespace openmine
