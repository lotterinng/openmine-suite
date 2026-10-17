#pragma once
// SPDX-License-Identifier: Apache-2.0
// Surface generation and mesh processing for survey data: 2.5D Delaunay triangulation (sweep-hull
// with Lawson flips), quadric-error decimation, Taubin smoothing, repair and hole filling, and
// volume computations (exact surface-to-plane, grid surface-to-surface cut/fill).

#include "openmine/objects/CadEntities.h"
#include "openmine/jobs/Job.h"

#include <vector>

namespace openmine::survey {

struct DelaunayOptions {
    double maxEdgeLength = 0;     ///< > 0: remove triangles with a plan edge longer than this
    double duplicateTolerance = 1e-9; ///< points closer than this in plan are merged (first kept)
};
struct DelaunayReport {
    std::size_t inputPoints = 0, usedPoints = 0, duplicates = 0, removedLongTriangles = 0;
};
/// Delaunay triangulation in plan; vertex elevations are the point elevations. Triangles are
/// counter-clockwise in plan (upward normals). Throws if all points are collinear.
MeshData delaunay25D(const std::vector<Vec3>& points, const DelaunayOptions& options = {}, DelaunayReport* report = nullptr,
                     jobs::Progress& progress = jobs::nullProgress());

struct DecimateOptions {
    std::size_t targetTriangles = 0;  ///< stop when reached (0 = only maxError limits)
    double maxError = 1e300;          ///< stop when the next collapse would exceed this (m, √quadric error)
    bool preserveBoundary = true;
    double boundaryWeight = 1000.0;
};
/// Garland–Heckbert quadric edge-collapse decimation with manifold (link-condition) and
/// normal-flip checks.
MeshData decimate(const MeshData& mesh, const DecimateOptions& options, jobs::Progress& progress = jobs::nullProgress());

/// Taubin λ|μ smoothing (volume-preserving low-pass); boundary vertices fixed when requested.
MeshData smoothTaubin(const MeshData& mesh, int iterations = 10, double lambda = 0.5, double mu = -0.53, bool fixBoundary = true);

struct RepairOptions {
    double mergeTolerance = 1e-6;
    bool orientConsistently = true;
    bool orientOutward = true;        ///< for closed meshes
    std::size_t fillHolesUpTo = 0;    ///< fill boundary loops with at most this many edges
};
struct RepairReport {
    std::size_t mergedVertices = 0, degenerateRemoved = 0, duplicatesRemoved = 0, flipped = 0, holesFilled = 0;
    std::size_t nonManifoldEdges = 0, boundaryLoops = 0;
};
MeshData repairMesh(const MeshData& mesh, const RepairOptions& options = {}, RepairReport* report = nullptr);

/// Boundary loops (ordered vertex indices) of a mesh.
std::vector<std::vector<std::uint32_t>> boundaryLoops(const MeshData& mesh);

struct VolumeResult {
    double above = 0;    ///< m³ of surface above the reference (fill relative to the reference)
    double below = 0;    ///< m³ of surface below the reference (cut)
    double planArea = 0; ///< m² compared
};
/// Exact volume between a surface (TIN, triangles projected in plan) and a horizontal plane.
VolumeResult volumeToPlane(const MeshData& surface, double elevation);
/// Volume between two surfaces on a plan grid of `cell` metres (cell-centre vertical sampling of
/// the topmost surface hit). `above`: later above earlier (fill); `below`: later below (cut).
VolumeResult volumeBetween(const MeshData& earlier, const MeshData& later, double cell, jobs::Progress& progress = jobs::nullProgress());

} // namespace openmine::survey
