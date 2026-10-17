#pragma once
// SPDX-License-Identifier: Apache-2.0
// Registration of point clouds: closed-form point-pair / control fits (Umeyama 1991, optional
// scale), ICP (point-to-point and point-to-plane, trimmed), and their project integration.
//
// Accuracy statistics are kept distinct (see Registration in SurveyEntities.h): fit residuals
// of the pairs used, ICP inlier RMS, and independent check-point residuals. Applying a
// registration changes the dataset's transform only — stored coordinates never change.

#include "openmine/project/Project.h"
#include "openmine/survey/PointBuffer.h"

namespace openmine::survey {

struct RigidFit {
    Transform3 transform; ///< source → target
    double scale = 1.0;
    double rms = 0, max = 0;
    std::vector<Vec3> residuals; ///< target − T(source)
};

/// Least-squares similarity (rotation + translation [+ uniform scale]) mapping `source` onto
/// `target`. Needs ≥ 3 non-collinear pairs. Optional per-pair weights.
RigidFit fitTransform(const std::vector<Vec3>& source, const std::vector<Vec3>& target, bool allowScale = false,
                      const std::vector<double>* weights = nullptr);

enum class IcpMode { PointToPoint, PointToPlane };

struct IcpOptions {
    IcpMode mode = IcpMode::PointToPlane;
    double maxCorrespondence = 0.5;    ///< m; pairs farther apart are ignored
    int maxIterations = 50;
    double translationTolerance = 1e-6;///< m, convergence on the increment
    double rotationTolerance = 1e-7;   ///< rad
    double trimFraction = 0.0;         ///< drop this fraction of the worst pairs each iteration
    std::size_t sampleSize = 50000;    ///< moving points used (random subset; 0 = all)
    unsigned seed = 1;
    int normalNeighbours = 12;         ///< for target normals when the target has none
};

struct IcpResult {
    Transform3 transform;              ///< full source → target (initial guess included)
    double fitness = 0;                ///< fraction of sampled source points with a correspondence
    double inlierRms = 0;              ///< m
    int iterations = 0;
    bool converged = false;
    std::vector<double> rmsHistory;
};

IcpResult icp(const PointBuffer& source, const PointBuffer& target, const Transform3& initial = {}, const IcpOptions& options = {},
              jobs::Progress& progress = jobs::nullProgress());

// ---- Project integration ------------------------------------------------------------------------------

/// Point-pair registration. Pairs with role Fit determine the transform; Check pairs are only
/// evaluated. Pair sources are in the dataset's stored coordinates. Nothing is changed.
Registration computePairRegistration(const Project& project, const Uuid& datasetId, std::vector<RegistrationPair> pairs, bool allowScale,
                                     RegistrationMethod method = RegistrationMethod::PointPair);
/// Pairs whose target is a survey control point (targetControlId set; target filled in).
Registration computeControlRegistration(const Project& project, const Uuid& datasetId, std::vector<RegistrationPair> pairs, bool allowScale);
/// ICP of a dataset onto a target dataset (both in project coordinates). Optional check pairs.
Registration computeIcpRegistration(const Project& project, const Uuid& movingId, const Uuid& targetId, const IcpOptions& options,
                                    std::vector<RegistrationPair> checkPairs = {}, jobs::Progress& progress = jobs::nullProgress());

/// Stores the registration and, when `apply`, sets the dataset transform to
/// registration.transform ∘ current transform (one undo step). Returns the registration id.
Uuid commitRegistration(Project& project, Registration r, bool apply);
/// Reverts an applied registration (only the most recent one applied to its dataset).
void revertRegistration(Project& project, const Uuid& registrationId);

} // namespace openmine::survey
