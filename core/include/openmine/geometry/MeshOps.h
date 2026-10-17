#pragma once
// SPDX-License-Identifier: Apache-2.0
// Triangle-mesh measurement and topology. Units: metres, m², m³.

#include "openmine/objects/CadEntities.h"

#include <optional>
#include <vector>

namespace openmine::geometry {

double triangleArea(Vec3 a, Vec3 b, Vec3 c);
Vec3 triangleNormal(Vec3 a, Vec3 b, Vec3 c); ///< unit; zero for degenerate triangles

double meshArea(const MeshData& m);

/// Signed volume by the divergence theorem (Σ a·(b×c)/6). Positive for a closed mesh
/// with outward-facing (counter-clockwise seen from outside) triangles. Only meaningful
/// for closed (watertight) meshes; see `topology`.
double meshSignedVolume(const MeshData& m);

struct MeshTopology {
    std::size_t vertexCount = 0;
    std::size_t triangleCount = 0;
    std::size_t degenerateTriangles = 0;
    std::size_t boundaryEdges = 0;     ///< edges used by one triangle
    std::size_t nonManifoldEdges = 0;  ///< edges used by more than two triangles
    std::size_t inconsistentEdges = 0; ///< shared edges traversed in the same direction twice
    std::size_t unreferencedVertices = 0;
    std::size_t components = 0;        ///< connected components (by shared vertices)
    bool closed() const { return boundaryEdges == 0 && nonManifoldEdges == 0 && triangleCount > 0; }
    bool orientable() const { return inconsistentEdges == 0; }
    bool watertight() const { return closed() && orientable() && degenerateTriangles == 0; }
};
MeshTopology topology(const MeshData& m);

std::vector<Vec3> faceNormals(const MeshData& m);
/// Area-weighted vertex normals.
std::vector<Vec3> vertexNormals(const MeshData& m);

/// Merges vertices closer than `tolerance` (grid hashing) and remaps triangles; drops
/// triangles that become degenerate. Returns the number of vertices removed.
std::size_t mergeCoincidentVertices(MeshData& m, double tolerance);

/// Intersection of the mesh with a plane: chained polylines (closed loops repeat the first point).
std::vector<std::vector<Vec3>> planeSection(const MeshData& m, Vec3 planePoint, Vec3 planeNormal,
                                            double chainTolerance = 1e-7);

Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c);

/// Axis-aligned bounding-volume hierarchy over triangles for distance and ray queries.
class MeshBvh {
public:
    explicit MeshBvh(const MeshData& m);
    struct Hit {
        double distance = 0;
        Vec3 point;
        std::uint32_t triangle = 0;
    };
    /// Closest point on the mesh (nullopt for an empty mesh or beyond maxDistance).
    std::optional<Hit> closest(Vec3 p, double maxDistance = 1e300) const;
    /// Signed distance: negative inside a closed, outward-oriented mesh (sign by ray parity).
    std::optional<double> signedDistance(Vec3 p) const;
    /// Point-in-closed-mesh test by ray parity along +X with jitter-robust majority of three rays.
    bool contains(Vec3 p) const;
    /// Nearest ray hit (Möller–Trumbore); t along the unit direction.
    std::optional<Hit> raycast(Vec3 origin, Vec3 dir, double maxT = 1e300) const;
    int countCrossings(Vec3 origin, Vec3 dir) const;

private:
    struct Node {
        BoundingBox box;
        std::uint32_t left = 0, right = 0; ///< children (internal) or [first, first+count) (leaf)
        std::uint32_t first = 0, count = 0;
    };
    std::uint32_t build(std::uint32_t first, std::uint32_t count, int depth);
    const MeshData& m_;
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> order_;
    std::vector<Vec3> centroids_;
};

} // namespace openmine::geometry
