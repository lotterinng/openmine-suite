#pragma once
// SPDX-License-Identifier: Apache-2.0
// Surface tools on project meshes, shared by the desktop dialogs and the Python API: decimation,
// smoothing, repair and comparison with a reference surface. The source surface is never changed:
// each result is a new mesh object recording its method and source (attributes "method" and
// "source_object"), added in one undo step.

#include "openmine/jobs/Job.h"
#include "openmine/project/Project.h"
#include "openmine/survey/Derivations.h"
#include "openmine/survey/Meshing.h"

#include <string>

namespace openmine::survey {

enum class SurfaceOperation { Decimate, Smooth, Repair, Compare };

struct SurfaceToolOptions {
    // Decimate
    std::size_t targetTriangles = 0; ///< 0: only maxError limits
    double maxError = 0;             ///< m; 0: only targetTriangles limits
    bool preserveBoundary = true;
    // Smooth (Taubin λ|μ)
    int iterations = 10;
    double lambda = 0.5, mu = -0.53;
    bool fixBoundary = true;
    // Repair
    double mergeTolerance = 1e-6;
    std::size_t fillHolesUpTo = 0;
    bool orientOutward = true;
    // Compare
    bool signedDistance = true;
    double maxDistance = 0;          ///< m; 0: unlimited
};

struct DistanceStatistics {
    std::size_t vertices = 0, measured = 0; ///< measured: within maxDistance
    double mean = 0, rms = 0, min = 0, median = 0, max = 0;
};

struct SurfaceToolResult {
    MeshData mesh;
    std::string method;   ///< recorded on the new object
    std::string summary;  ///< one line for the user
    RepairReport repair;  ///< Repair
    DistanceStatistics distances; ///< Compare (the per-vertex distances are mesh.vertexScalars, "distance")
};

/// Runs a tool on a surface (worker thread). `reference` is required for Compare. Throws
/// ValidationError for invalid options or an empty result.
SurfaceToolResult runSurfaceTool(const MeshData& surface, SurfaceOperation op, const SurfaceToolOptions& options, const MeshData* reference = nullptr,
                                 jobs::Progress& progress = jobs::nullProgress());

/// Adds a tool result as a new mesh object on the source's layer (one undo step).
Uuid addDerivedSurface(Project& project, const Uuid& sourceId, const SurfaceToolResult& result, const std::string& name);

// ---- surfaces from point clouds -----------------------------------------------------------------
using PoissonEngine = Engine<std::function<MeshData(const PointBuffer& pointsWithNormals, int depth)>>;

struct CloudSurfaceSpec {
    enum class Method { Delaunay, Poisson } method = Method::Delaunay;
    double voxel = 0;              ///< m; > 0: thin the points first (nearest-to-centre, no new coordinates)
    double maxEdgeLength = 0;      ///< Delaunay: drop plan edges longer than this (0 = keep all)
    std::size_t targetTriangles = 0; ///< Delaunay: decimate to this many (0 = no decimation)
    int depth = 9;                 ///< Poisson octree depth
    bool orientTowardsCentroid = false; ///< Poisson: normals (when missing) towards the centroid, else up
};
struct CloudSurface {
    MeshData mesh;
    std::string method; ///< "Delaunay 2.5D", "Poisson depth=9 (PDAL 2.8.4)"
};
/// A surface from points in project coordinates (worker thread). Poisson needs an engine.
CloudSurface surfaceFromPoints(PointBuffer points, const CloudSurfaceSpec& spec, const PoissonEngine& poisson = {},
                               jobs::Progress& progress = jobs::nullProgress());
/// Adds a surface derived from a dataset as a mesh object on `layerId` with its method and source
/// (one undo step).
Uuid addSurfaceObject(Project& project, MeshData mesh, const std::string& name, const Uuid& layerId, const std::string& method, const Uuid& sourceId);

/// The surface (mesh or solid tessellation) of an object; throws ValidationError otherwise.
MeshPtr surfaceOf(const Project& project, const Uuid& id);

} // namespace openmine::survey
