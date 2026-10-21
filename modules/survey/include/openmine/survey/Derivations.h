#pragma once
// SPDX-License-Identifier: Apache-2.0
// Derived point-cloud datasets. A survey dataset is never modified: filters, subsampling, normals,
// segmentation, crops and distance computations write a new dataset recording its parent and a
// derivation text. The operations and their recorded texts are defined here once, so the desktop
// dialogs, the command console and the Python API record identical derivations.
//
// Engines: some operations have alternative implementations (Open3D, PDAL) in integrations; they are
// injected as functions together with an engine label, which is appended to the derivation
// ("statistical outlier removal k=16 n=2.000000 (Open3D 0.18.0)").

#include "openmine/jobs/Job.h"
#include "openmine/project/Project.h"
#include "openmine/survey/PointBuffer.h"
#include "openmine/survey/Processing.h"

#include <functional>
#include <optional>
#include <string>

namespace openmine::survey {

struct Derivation {
    std::string text; ///< recorded on the derived dataset
    /// Processes the parent's points (project coordinates) into the derived points; may set a
    /// one-line message for the user.
    std::function<PointBuffer(PointBuffer& points, jobs::Progress& progress, std::string& message)> process;
};

/// Optional alternative engine.
template <class F> struct Engine {
    F function;          ///< empty = built-in
    std::string label;   ///< e.g. "Open3D 0.18.0"
};

using OutlierEngine = Engine<std::function<std::vector<char>(const PointBuffer&, int k, double stdRatio)>>;
using RadiusOutlierEngine = Engine<std::function<std::vector<char>(const PointBuffer&, double radius, int minNeighbours)>>;
using VoxelEngine = Engine<std::function<PointBuffer(const PointBuffer&, double voxel)>>; ///< centroid mode
using NormalsEngine = Engine<std::function<void(PointBuffer&, const NormalOptions&, jobs::Progress&)>>;

// ---- subsampling --------------------------------------------------------------------------------
Derivation voxelDerivation(double voxel, VoxelMode mode, const VoxelEngine& engine = {});
Derivation minimumSpacingDerivation(double spacing);
Derivation randomSubsampleDerivation(std::size_t count, unsigned seed = 1);
// ---- outliers -----------------------------------------------------------------------------------
Derivation statisticalOutlierDerivation(int k, double stdRatio, const OutlierEngine& engine = {});
Derivation radiusOutlierDerivation(double radius, int minNeighbours, const RadiusOutlierEngine& engine = {});
// ---- attributes ---------------------------------------------------------------------------------
/// PCA normals. With a viewpoint orientation and no viewpoint, the cloud's centroid is used.
Derivation normalsDerivation(const NormalOptions& options, const NormalsEngine& engine = {});
// ---- segmentation -------------------------------------------------------------------------------
/// Labels points with a "cluster" scalar (−1 = in no cluster); unclustered points are dropped
/// with `dropUnclustered`.
Derivation euclideanClusterDerivation(double tolerance, std::size_t minSize, bool dropUnclustered);
/// Region growing on normals (estimated when missing).
Derivation regionGrowingDerivation(double angleDeg, std::size_t minSize, bool dropUnclustered);
// ---- crops --------------------------------------------------------------------------------------
Derivation cropBoxDerivation(const BoundingBox& box, bool keepInside);
/// Polygon in the plane perpendicular to `viewNormal` (plan for +Z).
Derivation cropPolygonDerivation(const std::vector<Vec3>& polygon, Vec3 viewNormal, bool keepInside);
// ---- distances ----------------------------------------------------------------------------------
struct DistanceStatisticsJson {
    std::string json; ///< {"count","mean","std","min","max","p05","median","p95"}; empty when nothing was measured
};
/// Adds a "distance" scalar: nearest distance to a reference cloud (signed by the reference's
/// normals, estimated when missing) or to a mesh (signed by its orientation). Points farther than
/// `maxDistance` get NaN. Statistics are written to `*stats` when given.
Derivation cloudDistanceDerivation(PointBuffer reference, const std::string& referenceName, double maxDistance, bool signedDistance,
                                   std::shared_ptr<DistanceStatisticsJson> stats = {});
Derivation meshDistanceDerivation(MeshPtr reference, const std::string& referenceName, double maxDistance, bool signedDistance,
                                  std::shared_ptr<DistanceStatisticsJson> stats = {});

// ---- running ------------------------------------------------------------------------------------
struct DerivedDataset {
    Uuid id;
    std::size_t inputCount = 0, outputCount = 0;
    std::string derivation, message;
};
/// Reads the parent in project coordinates, applies the derivation and adds the derived dataset
/// (one undo step). Synchronous: the desktop splits the same steps over a background job.
DerivedDataset deriveDataset(Project& project, const Uuid& parentId, const Derivation& derivation, const std::string& name,
                             jobs::Progress& progress = jobs::nullProgress());

} // namespace openmine::survey
