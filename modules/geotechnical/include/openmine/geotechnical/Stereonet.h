#pragma once
// SPDX-License-Identifier: Apache-2.0
// Structural orientation analysis on the lower-hemisphere stereonet.
//
// Conventions: planes by dip (0–90°) and dip direction (0–360°, clockwise from grid north);
// lines by trend and plunge (positive down). A plane's pole has trend = dip direction + 180° and
// plunge = 90° − dip. World axes: x east, y north, z up.
//
//  * Projections: equal-area (Schmidt/Lambert, r = √2·sin(θ/2)) and equal-angle (Wulff,
//    r = tan(θ/2)), θ = angle from the downward vertical, unit primitive circle.
//  * Fisher statistics for axial data (vectors aligned to the principal axis first): resultant
//    length, mean, concentration K = (N − 1)/(N − R), 95% cone α95, spherical variance; Terzaghi
//    weights for sampling-line bias (capped); orientation tensor eigenvalues with Woodcock shape and
//    strength parameters.
//  * Pole density: Schmidt 1% counting area (density in % of poles per 1% of the hemisphere) or Kamb
//    counting (cone with expected count 3σ under uniformity, density in σ), on an equal-area grid;
//    contours by marching squares, joined into polylines in net coordinates.
//  * Joint sets: k-means on the sphere for axial data (k-means++ seeding with a fixed seed, mean by
//    the principal eigenvector), optional outlier cut-off angle; or assignment to given set means
//    within a cone.
//  * Kinematic analysis (Markland/Hocking) for a face: planar sliding, wedge sliding on
//    intersections of set pairs, flexural toppling; percentages of poles/intersections that are
//    kinematically admissible. Direct toppling is not evaluated.
//  * SVG rendering of the net with poles, set planes, contours and the friction circle.

#include "openmine/objects/Entities.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace openmine::geotechnical {

struct Plane {
    double dip = 0, dipDirection = 0;
};
struct Line {
    double trend = 0, plunge = 0;
};

Vec3 lineVector(Line l);                   ///< unit, lower hemisphere for plunge ≥ 0
Line lineOf(Vec3 v);                       ///< folded to the lower hemisphere
Line poleOf(Plane p);
Plane planeOfPole(Line pole);
Vec3 poleVector(Plane p);
/// Intersection line of two planes (nullopt when parallel within 1e-9).
std::optional<Line> intersection(Plane a, Plane b);
/// Apparent dip of a plane in a given direction (degrees, ≥ 0 when the plane dips that way).
double apparentDip(Plane p, double directionDeg);
/// Angle between two lines as axes (0–90°).
double axialAngle(Vec3 a, Vec3 b);

enum class Projection { EqualArea, EqualAngle };
struct NetPoint {
    double x = 0, y = 0; ///< unit primitive circle; +y = north
};
NetPoint project(Line l, Projection p = Projection::EqualArea);
Line unproject(NetPoint q, Projection p = Projection::EqualArea); ///< inverse (inside the primitive)
/// Points of the great circle of a plane (n + 1 points from strike to strike).
std::vector<NetPoint> greatCircle(Plane p, int n = 90, Projection proj = Projection::EqualArea);
/// Small circle (cone of half-angle `angleDeg` about `axis`); parts above the horizontal are
/// returned as their antipodes, so the result can contain several runs.
std::vector<std::vector<NetPoint>> smallCircle(Line axis, double angleDeg, int n = 180, Projection proj = Projection::EqualArea);

// ---- statistics ---------------------------------------------------------------------------------
struct FisherStats {
    std::size_t n = 0;
    double resultant = 0;       ///< R (weighted)
    Line mean;                  ///< mean axis (lower hemisphere)
    double k = 0;               ///< concentration; infinite for identical vectors (reported as 1e12)
    double alpha95 = 0;         ///< degrees (NaN for n < 2)
    double sphericalVariance = 0; ///< 1 − R/N
};
FisherStats fisher(const std::vector<Vec3>& axes, const std::vector<double>& weights = {});

struct OrientationTensor {
    std::array<double, 3> eigenvalues{};   ///< λ1 ≥ λ2 ≥ λ3 (normalised to sum 1)
    std::array<Vec3, 3> eigenvectors{};
    double shapeK = 0;                     ///< ln(λ1/λ2) / ln(λ2/λ3) (Woodcock 1977)
    double strengthC = 0;                  ///< ln(λ1/λ3)
};
OrientationTensor orientationTensor(const std::vector<Vec3>& axes, const std::vector<double>& weights = {});

/// Terzaghi weight 1/|cos δ| for poles sampled along a scanline or borehole, capped at `maxWeight`.
double terzaghiWeight(Vec3 pole, Vec3 samplingLine, double maxWeight = 10);

// ---- density & contours ----------------------------------------------------------------------------
enum class DensityMethod { Schmidt1Percent, Kamb };
struct DensityGrid {
    int n = 0;                       ///< grid points per side over [−1, 1]
    std::vector<double> values;      ///< row-major (y outer), NaN outside the primitive
    DensityMethod method = DensityMethod::Schmidt1Percent;
    double coneAngleDeg = 0;
    double maxValue = 0;
    NetPoint maxAt;
    double value(int i, int j) const { return values[static_cast<std::size_t>(j * n + i)]; }
    NetPoint point(int i, int j) const { return {-1 + 2.0 * i / (n - 1), -1 + 2.0 * j / (n - 1)}; }
};
/// Density at given axes (same units as the grid: % per 1% area, or σ for Kamb).
std::vector<double> densityAt(const std::vector<Vec3>& poles, const std::vector<double>& weights, DensityMethod m, const std::vector<Vec3>& at,
                              double* coneAngleDeg = nullptr);
DensityGrid poleDensity(const std::vector<Vec3>& poles, const std::vector<double>& weights = {}, DensityMethod m = DensityMethod::Schmidt1Percent,
                        int gridSize = 61);
/// Iso-lines of the grid at `level` as polylines in net coordinates.
std::vector<std::vector<NetPoint>> contour(const DensityGrid& g, double level);

// ---- joint sets ------------------------------------------------------------------------------------
struct JointSet {
    std::string name;
    Line meanPole;
    Plane meanPlane;
    FisherStats stats;
    std::vector<std::size_t> members;
};
struct JointSetResult {
    std::vector<JointSet> sets;
    std::vector<int> assignment;     ///< set index per pole, −1 = unassigned (outlier)
    int iterations = 0;
};
/// k-means for axial data. `maxAngleDeg` > 0 leaves poles further than that from every set mean
/// unassigned. Deterministic for a given seed.
JointSetResult identifyJointSets(const std::vector<Vec3>& poles, int k, double maxAngleDeg = 0, unsigned seed = 1,
                                 const std::vector<double>& weights = {});
/// Assigns each pole to the nearest given set mean within `coneDeg`.
JointSetResult assignToSets(const std::vector<Vec3>& poles, const std::vector<Line>& meanPoles, double coneDeg,
                            const std::vector<double>& weights = {});
/// Poles of the structural measurements in a project (optionally of one set name).
std::vector<Vec3> measurementPoles(const std::vector<StructuralMeasurement>& m, const std::string& setName = {});

// ---- kinematics --------------------------------------------------------------------------------------
struct KinematicOptions {
    Plane face;                       ///< slope face or excavation wall
    double frictionDeg = 30;
    double planarLateralLimitDeg = 20;
    double topplingLateralLimitDeg = 10;
};
struct PlanarResult {
    std::vector<std::size_t> critical; ///< pole indices
    double percent = 0;
};
struct WedgeResult {
    std::size_t setA = 0, setB = 0;
    Line intersection;
    bool critical = false;
    bool singlePlaneSliding = false;   ///< Hocking: sliding along one plane instead of the line
    std::size_t slidingPlane = 0;
};
struct KinematicResult {
    PlanarResult planar, toppling;
    std::vector<WedgeResult> wedges;   ///< for every pair of the given set planes
    double wedgePercent = 0;           ///< of all pole-pair intersections (when computed from poles)
};
/// Planar and toppling from all poles; wedges from set mean planes (pairs).
KinematicResult kinematicAnalysis(const std::vector<Vec3>& poles, const std::vector<Plane>& setPlanes, const KinematicOptions& o);
/// Percentage of all pole-pair intersections that are critical for wedge sliding (O(n²)).
double wedgeIntersectionPercent(const std::vector<Vec3>& poles, const KinematicOptions& o);
/// The two walls of a drive with the given trend, as vertical faces dipping into the drive.
std::array<Plane, 2> driveWalls(double driveTrendDeg);

// ---- rendering -------------------------------------------------------------------------------------
struct StereonetSvgOptions {
    double size = 480;
    Projection projection = Projection::EqualArea;
    bool grid = true;                  ///< 10° net
    std::optional<double> frictionDeg; ///< friction circle for pole plots
    std::vector<double> contourLevels;
    std::string title;
};
std::string stereonetSvg(const std::vector<Vec3>& poles, const std::vector<JointSet>& sets, const DensityGrid* density,
                         const StereonetSvgOptions& o);

} // namespace openmine::geotechnical
