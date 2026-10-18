#pragma once
// SPDX-License-Identifier: Apache-2.0
// Open3D (MIT) engines for point-cloud processing, as alternatives to the built-in OpenMine
// implementations: ICP registration (point-to-point / point-to-plane, optional Tukey robust
// kernel), PCA normal estimation (with the same orientation rules and curvature definition),
// voxel-grid downsampling with attribute tracing, statistical / radius outlier removal and
// screened Poisson surface reconstruction with per-vertex density.
//
// Every function works in a local frame (coordinates minus a centre) so that mine coordinates
// of 10⁵–10⁷ m keep full precision, and returns results in the input coordinates. Result
// statistics use the same definitions as the built-in engines (see survey/Registration.h,
// survey/Processing.h) so that the two can be compared — tests/integration/test_open3d.cpp
// cross-validates them.

#include "openmine/jobs/Job.h"
#include "openmine/objects/CadEntities.h"
#include "openmine/survey/PointBuffer.h"
#include "openmine/survey/Processing.h"
#include "openmine/survey/Registration.h"

#include <string>
#include <vector>

namespace openmine::o3d {

/// Open3D library version, e.g. "0.18.0".
std::string open3dVersion();

/// Engine name used in the registries and in derivation / registration records.
std::string engineName(); ///< "Open3D <version>"

// ---- registration ---------------------------------------------------------------------------------

struct IcpExtras {
    /// > 0: Tukey biweight robust kernel with this scale (m), instead of trimming. Open3D has no
    /// trimmed ICP: options.trimFraction > 0 is refused unless a kernel is given.
    double tukeyK = 0.0;
};

/// ICP with Open3D's estimators, iterated one step at a time so that the convergence test on the
/// increment (translation/rotation tolerances), the RMS history, progress and cancellation behave
/// as in survey::icp. Moving-point sampling uses the same seed and rule; target normals are
/// estimated with options.normalNeighbours when the target has none. Final fitness and inlier
/// RMS use survey::icp's definitions (point-to-plane RMS for point-to-plane).
survey::IcpResult icp(const survey::PointBuffer& source, const survey::PointBuffer& target, const Transform3& initial = {},
                      const survey::IcpOptions& options = {}, const IcpExtras& extras = {}, jobs::Progress& progress = jobs::nullProgress());

// ---- normals --------------------------------------------------------------------------------------

/// PCA normals from Open3D's covariance estimation (k nearest or radius neighbourhood, as in
/// options), oriented by the same rules as survey::estimateNormals (Up, towards/away from a
/// viewpoint, towards the nearest scanner position). With storeCurvature a "curvature" scalar
/// λ0 / (λ0 + λ1 + λ2) is added.
void estimateNormals(survey::PointBuffer& points, const survey::NormalOptions& options = {}, jobs::Progress& progress = jobs::nullProgress());

// ---- downsampling and outliers ----------------------------------------------------------------------

/// Voxel-grid downsampling (Open3D VoxelDownSampleAndTrace). Voxels are anchored at the minimum
/// corner of the cloud's bounds (cell index floor((p − min) / size)), as in
/// survey::voxelDownsample, so both engines produce the same voxels; output order is by the first
/// point of each voxel, as there. Each output point is the mean of its voxel's points (positions, normals re-normalised,
/// colours, intensity, GPS time, scalar fields); classification and return numbers take the most
/// frequent value of the voxel.
survey::PointBuffer voxelDownsample(const survey::PointBuffer& points, double voxelSize);

/// Statistical outlier removal (Open3D: mean distance to the k nearest neighbours must not exceed
/// μ + stdRatio·σ). Returns a keep mask (1 = inlier).
std::vector<char> statisticalInliers(const survey::PointBuffer& points, int k = 16, double stdRatio = 2.0);
/// Radius outlier removal: at least `minNeighbours` other points within `radius`. Keep mask.
std::vector<char> radiusInliers(const survey::PointBuffer& points, double radius, int minNeighbours);

// ---- surfaces ------------------------------------------------------------------------------------------

struct PoissonOptions {
    int depth = 9;              ///< octree depth
    double width = 0.0;         ///< finest cell width (m); overrides depth when > 0
    double scale = 1.1;         ///< bounding-cube scale
    bool linearFit = false;
    /// Remove vertices whose sample density is below this quantile (0 = keep all; 0.02–0.1
    /// removes the extrapolated surface far from the data).
    double densityQuantile = 0.0;
    int threads = -1;
};
struct PoissonResult {
    MeshData mesh;                ///< mesh.vertexScalars = density, scalarName "density"
    std::size_t removedVertices = 0;
};
/// Screened Poisson reconstruction (Kazhdan & Hoppe 2013, Open3D implementation). The points need
/// normals (estimate them first).
PoissonResult poissonSurface(const survey::PointBuffer& points, const PoissonOptions& options = {});

/// Registers the Open3D ICP as engine "Open3D" with survey::registerIcpEngine.
void registerEngines();

} // namespace openmine::o3d
