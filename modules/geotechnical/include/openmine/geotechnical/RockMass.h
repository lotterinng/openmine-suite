#pragma once
// SPDX-License-Identifier: Apache-2.0
// Rock-mass classification and derived parameters.
//
//  * Q-system (Barton, Lien & Lunde 1974; Grimstad & Barton 1993; NGI handbook 2015):
//    Q = (RQD/Jn)·(Jr/Ja)·(Jw/SRF), RQD < 10 taken as 10. Parameter values are validated against
//    the published ranges and the standard descriptive tables are provided for selection. Derived:
//    equivalent dimension, maximum unsupported span 2·ESR·Q^0.4, rock-bolt length 2 + 0.15·B/ESR,
//    permanent roof and wall support pressure (Barton et al. 1974; Grimstad & Barton 1993), Qwall,
//    Qc = Q·σc/100. The support *category* is read from the NGI chart by the engineer; it is not
//    computed here (the chart boundaries are graphical).
//  * RMR89 (Bieniawski 1989): tabulated ratings for intact strength, RQD, spacing, discontinuity
//    condition (overall or the five guideline sub-ratings), groundwater (inflow, pressure ratio or
//    general condition) and the orientation adjustment for tunnels and mines, determined from the
//    discontinuity orientation and the drive direction. Rock class, stand-up time and indicative
//    cohesion/friction from the class table.
//  * GSI ≈ RMR89′ − 5 (dry, no orientation adjustment; valid for RMR89′ > 23, Hoek et al. 1995),
//    generalised Hoek–Brown (Hoek, Carranza-Torres & Corkum 2002) and rock-mass modulus
//    (Hoek & Diederichs 2006). Q↔RMR correlations (Bieniawski 1976; Barton 1995) are labelled as
//    empirical correlations.

#include "openmine/project/Project.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openmine::geotechnical {

class ClassificationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---- Q-system -------------------------------------------------------------------------------------
struct QInputs {
    double rqd = 100, jn = 1, jr = 1, ja = 1, jw = 1, srf = 1;
};
struct TableEntry {
    std::string code, description;
    double value = 0, valueMax = 0; ///< valueMax > value for ranges
};
/// Standard descriptive tables (NGI 2015) for selection in the UI.
const std::vector<TableEntry>& jnTable();
const std::vector<TableEntry>& jrTable();
const std::vector<TableEntry>& jaTable();
const std::vector<TableEntry>& jwTable();
const std::vector<TableEntry>& srfTable();
const std::vector<TableEntry>& esrTable();

struct QResult {
    double q = 0;
    double blockSize = 0, shearStrength = 0, activeStress = 0; ///< RQD/Jn, Jr/Ja, Jw/SRF
    std::string rockClass;              ///< "Exceptionally poor" … "Exceptionally good"
    std::vector<std::string> warnings;  ///< e.g. RQD < 10 raised to 10
};
/// Throws ClassificationError for values outside the published ranges.
QResult computeQ(const QInputs& in);
std::string qClass(double q);

struct QSupport {
    double esr = 1, span = 0, height = 0;
    double equivalentDimension = 0;     ///< span / ESR
    double maxUnsupportedSpan = 0;      ///< 2·ESR·Q^0.4
    double roofBoltLength = 0, wallBoltLength = 0; ///< 2 + 0.15·B/ESR (B = span, H = height)
    double qWall = 0;                   ///< 5Q (Q > 10), 2.5Q (0.1 < Q < 10), Q (Q < 0.1)
    double roofPressureMPa = 0, wallPressureMPa = 0; ///< 2·√Jn·Q^(−1/3) / (3·Jr) kgf/cm² → MPa
    bool unsupportedSpanExceeded = false;
};
QSupport qSupport(const QInputs& in, double span, double height, double esr);
double qc(double q, double sigmaCMPa);  ///< Q·σc/100

/// SRF range for competent rock from the stress state (Grimstad & Barton 1993, table 6b): from
/// σc/σ1 and σθ/σc. Returns the table row and range; the engineer selects the value.
struct SrfStress {
    std::string code, description;
    double srfMin = 0, srfMax = 0;
};
SrfStress srfFromStress(double sigmaCMPa, double sigma1MPa, double sigmaThetaMPa);

// ---- RMR89 ----------------------------------------------------------------------------------------
enum class JointCondition { VeryRough, SlightlyRough, SlightlyRoughHighlyWeathered, Slickensided, SoftGouge };
enum class Roughness { VeryRough, Rough, SlightlyRough, Smooth, Slickensided };
enum class Infilling { None, HardUnder5mm, HardOver5mm, SoftUnder5mm, SoftOver5mm };
enum class Weathering { Unweathered, Slightly, Moderately, Highly, Decomposed };
enum class GroundwaterCondition { CompletelyDry, Damp, Wet, Dripping, Flowing };
enum class OrientationClass { VeryFavourable, Favourable, Fair, Unfavourable, VeryUnfavourable };
const char* toString(OrientationClass c);

struct RmrConditionDetail {
    double persistence = 1;    ///< m
    double aperture = 0;       ///< mm
    Roughness roughness = Roughness::Rough;
    Infilling infilling = Infilling::None;
    Weathering weathering = Weathering::Unweathered;
};

struct RmrInputs {
    std::optional<double> ucsMPa;          ///< intact uniaxial strength, or
    std::optional<double> pointLoadMPa;    ///< point-load index Is(50) (rating table; UCS preferred below 1 MPa)
    double rqd = 100;
    double spacingM = 1;                   ///< discontinuity spacing
    std::optional<JointCondition> condition;          ///< overall condition, or
    std::optional<RmrConditionDetail> conditionDetail; ///< guideline E sub-ratings (preferred)
    std::optional<double> inflowLitresPerMinPer10m;   ///< groundwater: inflow, or
    std::optional<double> waterPressureRatio;         ///< joint water pressure / major principal stress, or
    std::optional<GroundwaterCondition> groundwater;  ///< general condition
    std::optional<OrientationClass> orientation;      ///< for the adjustment (nullopt = none)
};

struct RmrResult {
    double ucsRating = 0, rqdRating = 0, spacingRating = 0, conditionRating = 0, groundwaterRating = 0;
    double basic = 0;          ///< RMR89 basic (sum of the five)
    double adjustment = 0;     ///< orientation adjustment (≤ 0)
    double rmr = 0;            ///< basic + adjustment
    int classNumber = 0;       ///< 1 … 5
    std::string rockClass;     ///< e.g. "II — Good rock"
    std::string standUpTime, cohesion, frictionAngle;
};
RmrResult computeRmr89(const RmrInputs& in);

/// Orientation class of a discontinuity for a tunnel driven in `driveTrendDeg` (Bieniawski 1989,
/// table "effect of discontinuity strike and dip in tunnelling"). Strike within 45° of the axis is
/// treated as parallel, otherwise perpendicular.
OrientationClass tunnelOrientationClass(double dipDeg, double dipDirectionDeg, double driveTrendDeg);

// ---- GSI, Hoek–Brown, correlations ----------------------------------------------------------------
/// GSI from RMR89′ (groundwater rating 15, no orientation adjustment). Throws for RMR89′ ≤ 23.
double gsiFromRmr89(const RmrResult& r);

struct HoekBrown {
    double mb = 0, s = 0, a = 0;
    double sigmaC = 0;          ///< rock-mass uniaxial strength σci·s^a (MPa)
    double sigmaT = 0;          ///< tensile strength −s·σci/mb (MPa)
    double sigmaCm = 0;         ///< global rock-mass strength (MPa)
    double modulusMPa = 0;      ///< Hoek–Diederichs (2006) generalised, from Ei when given, else simplified
};
/// D = disturbance factor (0 undisturbed … 1 very disturbed); Ei = intact modulus (MPa, 0 = use
/// the simplified equation).
HoekBrown hoekBrown(double gsi, double mi, double sigmaCiMPa, double disturbance = 0, double intactModulusMPa = 0);
/// Major principal stress at failure for a minor principal stress (generalised criterion).
double hoekBrownSigma1(const HoekBrown& hb, double sigmaCiMPa, double sigma3MPa);

double rmrFromQBieniawski1976(double q); ///< 9 ln Q + 44
double rmrFromQBarton1995(double q);     ///< 15 log10 Q + 50
double qFromRmrBarton1995(double rmr);   ///< 10^((RMR − 50)/15)

// ---- project records ------------------------------------------------------------------------------
/// Stores a Q classification (inputs, Q, class) at a development chainage. One undo step.
Uuid recordQ(Project& project, const Uuid& developmentId, double chainage, Vec3 location, const QInputs& in, const std::string& mappedBy = {},
             std::optional<Date> date = {}, const std::string& notes = {});
Uuid recordRmr(Project& project, const Uuid& developmentId, double chainage, Vec3 location, const RmrInputs& in, const std::string& mappedBy = {},
               std::optional<Date> date = {}, const std::string& notes = {});

} // namespace openmine::geotechnical
