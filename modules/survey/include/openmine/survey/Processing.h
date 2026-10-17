#pragma once
// SPDX-License-Identifier: Apache-2.0
// Point-cloud processing on in-memory buffers. Functions never modify their input (except the
// explicitly in-place attribute estimators) and return masks, labels or new buffers; persisting a
// result is done by creating a derived dataset (PointCloudProject.h).

#include "openmine/objects/CadEntities.h"
#include "openmine/survey/PointBuffer.h"

#include <random>

namespace openmine::survey {

// ---- Selection masks (1 = selected) -------------------------------------------------------------
std::vector<char> selectBox(const PointBuffer& b, const BoundingBox& box);
/// Inside a plan polygon and between two elevations.
std::vector<char> selectPolygon(const PointBuffer& b, const std::vector<Vec2>& polygon, double zMin = -1e300, double zMax = 1e300);
std::vector<char> selectSphere(const PointBuffer& b, Vec3 centre, double radius);
/// On the side of the plane its normal points to (signed distance ≥ 0).
std::vector<char> selectPlaneSide(const PointBuffer& b, Vec3 planePoint, Vec3 planeNormal);
/// Within ±halfWidth of a plane (a section slab).
std::vector<char> selectSlab(const PointBuffer& b, Vec3 planePoint, Vec3 planeNormal, double halfWidth);
std::vector<char> invert(std::vector<char> mask);

// ---- Subsampling --------------------------------------------------------------------------------
enum class VoxelMode {
    NearestToCentre, ///< keeps original points (preferred: no coordinate is invented)
    Centroid,        ///< replaces each voxel's points by their mean (positions and attributes)
};
PointBuffer voxelDownsample(const PointBuffer& b, double voxelSize, VoxelMode mode = VoxelMode::NearestToCentre);
PointBuffer randomSubsample(const PointBuffer& b, std::size_t targetCount, unsigned seed = 1);
/// Keeps points so that no two kept points are closer than `spacing` (greedy, input order).
PointBuffer minimumSpacing(const PointBuffer& b, double spacing);

// ---- Outliers (returned masks: 1 = inlier to keep) -----------------------------------------------
/// Statistical outlier removal: mean distance to the k nearest neighbours must not exceed
/// μ + stdRatio·σ of all points' mean distances.
std::vector<char> statisticalInliers(const PointBuffer& b, int k = 16, double stdRatio = 2.0, jobs::Progress& progress = jobs::nullProgress());
/// Radius outlier removal: at least `minNeighbours` other points within `radius`.
std::vector<char> radiusInliers(const PointBuffer& b, double radius, int minNeighbours, jobs::Progress& progress = jobs::nullProgress());

// ---- Normals ----------------------------------------------------------------------------------------
struct NormalOptions {
    int k = 16;                          ///< neighbours for the PCA fit
    double radius = 0;                   ///< > 0: use a radius neighbourhood instead of k
    enum class Orientation { Up, TowardsViewpoint, AwayFromViewpoint, TowardsNearestScanner } orientation = Orientation::Up;
    Vec3 viewpoint;                      ///< for the viewpoint orientations
    std::vector<Vec3> scanners;          ///< for TowardsNearestScanner
    bool storeCurvature = true;          ///< adds a "curvature" scalar (λ0 / Σλ)
};
/// Estimates normals by PCA of each neighbourhood (smallest eigenvector) and orients them.
void estimateNormals(PointBuffer& b, const NormalOptions& options = {}, jobs::Progress& progress = jobs::nullProgress());

// ---- Segmentation --------------------------------------------------------------------------------
/// Euclidean clustering: connected components of the "within tolerance" graph. Returns a label
/// per point (clusters numbered by decreasing size from 0; -1 for points in clusters outside
/// [minSize, maxSize]).
std::vector<int> euclideanClusters(const PointBuffer& b, double tolerance, std::size_t minSize = 1, std::size_t maxSize = SIZE_MAX,
                                   jobs::Progress& progress = jobs::nullProgress());
/// Region growing on normals (needs normals): neighbours join a region when the angle between
/// normals is below `angleDeg`; seeds are taken by increasing curvature.
std::vector<int> regionGrowing(const PointBuffer& b, int k = 16, double angleDeg = 5.0, std::size_t minSize = 50,
                               jobs::Progress& progress = jobs::nullProgress());

struct PlaneFit {
    Vec3 normal;       ///< unit
    double d = 0;      ///< plane: dot(normal, p) + d = 0
    std::vector<std::size_t> inliers;
    double rms = 0;    ///< of the inlier distances after least-squares refinement
    Vec3 centroid;
};
/// Least-squares plane through points (PCA).
PlaneFit fitPlane(const std::vector<Vec3>& points);
/// Sequential RANSAC: extracts up to `maxPlanes` planes with at least `minInliers` points each.
std::vector<PlaneFit> ransacPlanes(const PointBuffer& b, double distanceThreshold, int maxPlanes = 5, std::size_t minInliers = 100,
                                   int iterations = 1000, unsigned seed = 1);

// ---- Distances ----------------------------------------------------------------------------------
/// Nearest-neighbour distance from each point of `compared` to `reference` (C2C). With
/// `signedByNormals` the sign follows the reference point's normal. Points farther than
/// `maxDistance` get NaN.
std::vector<float> cloudToCloud(const PointBuffer& compared, const PointBuffer& reference, double maxDistance = 1e300,
                                bool signedByNormals = false, jobs::Progress& progress = jobs::nullProgress());
/// Distance from each point to a mesh; signed (negative inside / behind) for closed or
/// consistently oriented meshes when `signedDistance`.
std::vector<float> cloudToMesh(const PointBuffer& b, const MeshData& mesh, bool signedDistance = true, double maxDistance = 1e300,
                               jobs::Progress& progress = jobs::nullProgress());

struct CloudStatistics {
    std::size_t count = 0;
    BoundingBox bounds = BoundingBox::empty();
    Vec3 centroid;
    double meanSpacing = 0;   ///< mean nearest-neighbour distance (sampled)
    double medianSpacing = 0;
};
CloudStatistics statistics(const PointBuffer& b, std::size_t spacingSamples = 10000);

} // namespace openmine::survey
