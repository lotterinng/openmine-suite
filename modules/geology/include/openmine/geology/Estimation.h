#pragma once
// SPDX-License-Identifier: Apache-2.0
// Variography and grade estimation into block models.
//
// Orientation of anisotropy ellipsoids (variograms and search): the major axis has `azimuth`
// (degrees clockwise from grid north) and `dip` (plunge below horizontal, positive down); the
// semi-major axis is horizontal at azimuth + 90° before `tilt` rotates it about the major axis
// (positive tilt raises the semi-major axis); the minor axis completes the frame.
//
// Variogram structures use practical ranges: spherical (exact range), exponential
// 1 − exp(−3h/a), Gaussian 1 − exp(−3(h/a)²). Covariance C(h) = total sill − γ(h).
//
// Estimators: nearest neighbour, inverse distance to a power (anisotropic distance), ordinary and
// simple kriging (point or block, with block discretisation and GSLib-style block covariance),
// with search ellipsoid passes, minimum/maximum samples, maximum per drillhole and per octant.
// Diagnostics per block: samples used, average anisotropic distance, pass, kriging variance,
// slope of regression (OK), sum of negative weights. Leave-one-out cross-validation checks the
// parameters against the data.

#include "openmine/geology/BlockModel.h"
#include "openmine/geology/Compositing.h"

#include <optional>
#include <string>
#include <vector>

namespace openmine::geology {

struct Anisotropy {
    double major = 1, semi = 1, minor = 1; ///< ranges (m)
    double azimuth = 0, dip = 0, tilt = 0; ///< degrees
    /// Unit axes (major, semi-major, minor) in world coordinates.
    std::array<Vec3, 3> axes() const;
    /// Distance normalised so the ellipsoid surface is 1.
    double normalised(Vec3 h) const;
    static Anisotropy isotropic(double range) { return {range, range, range, 0, 0, 0}; }
};

enum class VariogramType { Spherical, Exponential, Gaussian };
const char* toString(VariogramType t);

struct VariogramStructure {
    VariogramType type = VariogramType::Spherical;
    double sill = 1;     ///< contribution
    Anisotropy ranges;
};

struct VariogramModel {
    double nugget = 0;
    std::vector<VariogramStructure> structures;
    double totalSill() const;
    double gamma(Vec3 h) const;
    double covariance(Vec3 h) const { return totalSill() - gamma(h); }
    /// Value of a unit structure at normalised distance r.
    static double unitGamma(VariogramType t, double r);
};

// ---- samples ------------------------------------------------------------------------------------
struct SampleSet {
    std::vector<Vec3> points;
    std::vector<double> values;
    std::vector<int> group;               ///< drillhole index (for per-hole limits)
    std::vector<std::string> groupNames;
    std::size_t mergedDuplicates = 0;     ///< colocated samples averaged into one
};
/// Composites with a value for `field`, optionally only where `codeField` == `code`. Colocated
/// samples (within `tolerance`) are averaged so kriging systems stay non-singular.
SampleSet samplesFromComposites(const std::vector<Composite>& composites, const std::string& field, const std::string& codeField = {},
                                const std::string& code = {}, double tolerance = 1e-6);

/// Cell-declustering weights (sum = number of samples) for a cubic cell size, averaged over
/// `offsets` grid origins.
std::vector<double> cellDeclusterWeights(const std::vector<Vec3>& points, double cellSize, int offsets = 8);

// ---- experimental variogram ---------------------------------------------------------------------
enum class VariogramKind { Semivariogram, PairwiseRelative };

struct ExperimentalOptions {
    VariogramKind kind = VariogramKind::Semivariogram;
    double lag = 10;
    int lags = 10;
    double lagTolerance = -1;      ///< default lag/2
    double azimuth = 0, dip = 0;   ///< direction (dip positive down)
    double angleTolerance = 90;    ///< degrees; ≥ 90 = omnidirectional
    double bandwidth = 1e300;      ///< max distance from the direction line
};
struct LagPoint {
    double distance = 0; ///< mean pair distance
    double gamma = 0;
    std::size_t pairs = 0;
};
std::vector<LagPoint> experimentalVariogram(const SampleSet& s, const ExperimentalOptions& o);

struct VariogramFit {
    VariogramModel model;
    double weightedSse = 0;
    int iterations = 0;
    bool converged = false;
};
/// Fits nugget, sills and ranges along one direction (the ranges of every axis are scaled by the
/// same factor as the fitted one, so the template's anisotropy ratios are kept). Weighted least
/// squares (weights N/γ_model², Cressie 1985) with Levenberg–Marquardt; `fixNugget` keeps the
/// template nugget.
VariogramFit fitVariogram(const std::vector<LagPoint>& lags, const VariogramModel& initial, Vec3 direction, bool fixNugget = false);

// ---- estimation ---------------------------------------------------------------------------------
enum class EstimationMethod { NearestNeighbour, InverseDistance, OrdinaryKriging, SimpleKriging };
const char* toString(EstimationMethod m);

struct SearchPass {
    Anisotropy ellipse;      ///< search radii and orientation
    int minSamples = 1, maxSamples = 24;
    int maxPerHole = 0;      ///< 0 = no limit
    int maxPerOctant = 0;    ///< 0 = no limit
};

struct EstimationOptions {
    EstimationMethod method = EstimationMethod::OrdinaryKriging;
    std::string outputField = "est";
    std::vector<SearchPass> passes{SearchPass{}};
    double power = 2;                         ///< IDW
    VariogramModel variogram;                 ///< kriging
    double simpleMean = 0;                    ///< SK
    std::array<int, 3> discretisation{1, 1, 1}; ///< points per block (1,1,1 = point at centroid)
    std::string maskField;                    ///< estimate only blocks where mask > 0 (empty = all)
    bool diagnostics = true;                  ///< writes `<out>_NS`, `_DIST`, `_PASS`, and for kriging `_KV`, `_SLOPE` (OK), `_NEGW`
    int threads = 0;                          ///< 0 = hardware concurrency
};

struct EstimationSummary {
    std::size_t estimated = 0, skippedMask = 0, notEstimated = 0;
    std::vector<std::size_t> perPass;
    std::size_t singularSystems = 0;          ///< kriging systems that could not be solved (block left empty)
    std::size_t negativeWeightBlocks = 0;
};

EstimationSummary estimate(BlockModelData& model, const SampleSet& samples, const EstimationOptions& options,
                           jobs::Progress& progress = jobs::nullProgress());

/// Estimate at arbitrary points (point support) with the same options; NaN where not estimated.
std::vector<double> estimateAt(const std::vector<Vec3>& points, const SampleSet& samples, const EstimationOptions& options,
                               std::vector<double>* krigingVariance = nullptr);

struct CrossValidation {
    std::vector<double> estimates, errors; ///< NaN where not estimated
    double meanError = 0, meanSquaredError = 0, correlation = 0, slope = 0; ///< slope of true on estimate
    std::size_t estimated = 0;
};
/// Leave-one-out (excluding the whole drillhole when `excludeHole`, which avoids optimistic
/// results from neighbouring composites).
CrossValidation crossValidate(const SampleSet& samples, const EstimationOptions& options, bool excludeHole = true);

} // namespace openmine::geology
