#pragma once
// SPDX-License-Identifier: Apache-2.0
// As-built surfaces of development headings from registered scans:
//  1. the scan points within a corridor around the heading's centreline (project coordinates),
//     optionally voxel-thinned, with normals oriented towards the inside of the excavation (the
//     design profile's centroid carried along the centreline — not the centreline itself, which is
//     usually the floor or grade line), so the surface faces into the excavation;
//  2. a watertight implicit reconstruction by a surface engine (Poisson in PDAL or Open3D —
//     injected, the survey module depends on neither);
//  3. trimming of the triangles farther from the scan than twice its measured point spacing
//     (Poisson closes unscanned ends and shadows), and mesh repair with consistent orientation.
// The same pipeline serves the desktop application and the Python API.

#include "openmine/jobs/Job.h"
#include "openmine/project/Project.h"
#include "openmine/survey/PointBuffer.h"

#include <functional>
#include <string>

namespace openmine::survey {

struct AsBuiltOptions {
    double corridor = 6.0; ///< m, half-width around the centreline
    double voxel = 0.05;   ///< m; 0 = no thinning
    int depth = 9;         ///< reconstruction depth passed to the engine
    /// Holes left by trimming (scan gaps a little wider than the trimming distance) with at most
    /// this many boundary edges are filled; the open ends of the heading are far larger. 0 = none.
    std::size_t fillHolesUpTo = 32;
};

/// Points of `datasetId` within the corridor of `developmentId`, with oriented normals.
PointBuffer corridorPoints(const Project& project, const Uuid& datasetId, const Uuid& developmentId, const AsBuiltOptions& options,
                           jobs::Progress& progress = jobs::nullProgress());

/// Removes triangles whose centroid lies farther from the points than max(3 × voxel, 2 × median
/// spacing, 5 cm), then repairs the mesh (consistent orientation, not forced outward; holes with at
/// most `fillHolesUpTo` edges filled). Throws when nothing remains.
MeshData trimToScan(const MeshData& surface, const PointBuffer& points, double voxel, std::size_t fillHolesUpTo = 0);

/// Surface engine: reconstructs a surface from points with normals at the given depth.
using SurfaceEngine = std::function<MeshData(const PointBuffer& points, int depth)>;

/// The full pipeline (corridor points → engine → trim and repair).
MeshData buildAsBuiltSurface(const Project& project, const Uuid& datasetId, const Uuid& developmentId, const AsBuiltOptions& options,
                             const SurfaceEngine& engine, jobs::Progress& progress = jobs::nullProgress());

} // namespace openmine::survey
