// SPDX-License-Identifier: Apache-2.0
#include "openmine/geotechnical/RockMass.h"

#include "openmine/project/ProjectEditor.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <iomanip>
#include <numbers>
#include <sstream>

namespace openmine::geotechnical {

namespace {
constexpr double kKgfCm2ToMPa = 0.0980665;

std::string fmt(double v, int prec = 3) {
    std::ostringstream ss;
    ss << std::setprecision(prec) << v;
    return ss.str();
}
void requireRange(double v, double lo, double hi, const char* name) {
    if (!std::isfinite(v) || v < lo - 1e-12 || v > hi + 1e-12)
        throw ClassificationError(std::string(name) + " = " + fmt(v) + " is outside the published range " + fmt(lo) + "–" + fmt(hi));
}
} // namespace

// ---- tables ---------------------------------------------------------------------------------------
const std::vector<TableEntry>& jnTable() {
    static const std::vector<TableEntry> t = {
        {"A", "Massive, no or few joints", 0.5, 1.0},
        {"B", "One joint set", 2, 2},
        {"C", "One joint set plus random joints", 3, 3},
        {"D", "Two joint sets", 4, 4},
        {"E", "Two joint sets plus random joints", 6, 6},
        {"F", "Three joint sets", 9, 9},
        {"G", "Three joint sets plus random joints", 12, 12},
        {"H", "Four or more joint sets, random, heavily jointed (sugar cube)", 15, 15},
        {"J", "Crushed rock, earthlike", 20, 20},
    };
    return t;
}
const std::vector<TableEntry>& jrTable() {
    static const std::vector<TableEntry> t = {
        {"A", "Discontinuous joints", 4, 4},
        {"B", "Rough or irregular, undulating", 3, 3},
        {"C", "Smooth, undulating", 2, 2},
        {"D", "Slickensided, undulating", 1.5, 1.5},
        {"E", "Rough or irregular, planar", 1.5, 1.5},
        {"F", "Smooth, planar", 1.0, 1.0},
        {"G", "Slickensided, planar", 0.5, 0.5},
        {"H", "No rock-wall contact when sheared (clay zone or sandy/gravelly zone)", 1.0, 1.0},
    };
    return t;
}
const std::vector<TableEntry>& jaTable() {
    static const std::vector<TableEntry> t = {
        {"A", "Tightly healed, hard, non-softening, impermeable filling", 0.75, 0.75},
        {"B", "Unaltered joint walls, surface staining only", 1, 1},
        {"C", "Slightly altered joint walls, non-softening coatings", 2, 2},
        {"D", "Silty or sandy clay coatings, small clay fraction", 3, 3},
        {"E", "Softening or low-friction clay mineral coatings", 4, 4},
        {"F", "Sandy particles, clay-free disintegrated rock (contact before 10 cm shear)", 4, 4},
        {"G", "Strongly over-consolidated non-softening clay fillings (< 5 mm)", 6, 6},
        {"H", "Medium or low over-consolidation softening clay fillings (< 5 mm)", 8, 8},
        {"J", "Swelling clay fillings (< 5 mm)", 8, 12},
        {"K", "Zones of disintegrated rock and clay, strongly over-consolidated (no contact)", 6, 6},
        {"L", "Zones of disintegrated rock and clay, medium or low over-consolidation (no contact)", 8, 8},
        {"M", "Zones of disintegrated rock and swelling clay (no contact)", 8, 12},
        {"N", "Thick continuous zones of clay, strongly over-consolidated", 10, 10},
        {"O", "Thick continuous zones of clay, medium or low over-consolidation", 13, 13},
        {"P", "Thick continuous zones of swelling clay", 13, 20},
    };
    return t;
}
const std::vector<TableEntry>& jwTable() {
    static const std::vector<TableEntry> t = {
        {"A", "Dry excavations or minor inflow (humid or a few drips)", 1.0, 1.0},
        {"B", "Medium inflow, occasional outwash of joint fillings (many drips, rain)", 0.66, 0.66},
        {"C", "Jet inflow or high pressure in competent rock with unfilled joints", 0.5, 0.5},
        {"D", "Large inflow or high pressure, considerable outwash of joint fillings", 0.33, 0.33},
        {"E", "Exceptionally high inflow or water pressure decaying with time", 0.1, 0.2},
        {"F", "Exceptionally high inflow or water pressure continuing without noticeable decay", 0.05, 0.1},
    };
    return t;
}
const std::vector<TableEntry>& srfTable() {
    static const std::vector<TableEntry> t = {
        {"A", "Multiple weakness zones with clay or chemically disintegrated rock (any depth)", 10, 10},
        {"B", "Single weakness zone with clay or disintegrated rock (depth ≤ 50 m)", 5, 5},
        {"C", "Single weakness zone with clay or disintegrated rock (depth > 50 m)", 2.5, 2.5},
        {"D", "Multiple shear zones in competent clay-free rock, loose surrounding rock (any depth)", 7.5, 7.5},
        {"E", "Single shear zone in competent clay-free rock (depth ≤ 50 m)", 5, 5},
        {"F", "Single shear zone in competent clay-free rock (depth > 50 m)", 2.5, 2.5},
        {"G", "Loose, open joints, heavily jointed (any depth)", 5, 5},
        {"H", "Competent rock, low stress, near surface, open joints (σc/σ1 > 200)", 2.5, 2.5},
        {"J", "Competent rock, medium stress, favourable stress condition (σc/σ1 200–10)", 1, 1},
        {"K", "Competent rock, high stress, very tight structure (σc/σ1 10–5)", 0.5, 2},
        {"L", "Moderate slabbing after > 1 hour in massive rock (σc/σ1 5–3)", 5, 50},
        {"M", "Slabbing and rock burst after a few minutes in massive rock (σc/σ1 3–2)", 50, 200},
        {"N", "Heavy rock burst and immediate dynamic deformation in massive rock (σc/σ1 < 2)", 200, 400},
        {"O", "Squeezing rock: mild squeezing pressure (σθ/σc 1–5)", 5, 10},
        {"P", "Squeezing rock: heavy squeezing pressure (σθ/σc > 5)", 10, 20},
        {"R", "Swelling rock: mild swelling pressure", 5, 10},
        {"S", "Swelling rock: heavy swelling pressure", 10, 15},
    };
    return t;
}
const std::vector<TableEntry>& esrTable() {
    static const std::vector<TableEntry> t = {
        {"A", "Temporary mine openings", 3, 5},
        {"B", "Vertical shafts: circular / rectangular or square section", 2.0, 2.5},
        {"C", "Permanent mine openings, water tunnels for hydropower, pilot tunnels, drifts and headings for large openings", 1.6, 1.6},
        {"D", "Minor road and railway tunnels, surge chambers, access tunnels, sewage tunnels", 1.3, 1.3},
        {"E", "Power stations, storage rooms, water treatment plants, major road and railway tunnels, civil defence chambers, portals, intersections", 1.0, 1.0},
        {"F", "Underground nuclear power stations, railway stations, sports and public facilities, factories, major gas pipeline tunnels", 0.8, 0.8},
        {"G", "Very important caverns and tunnels with a long lifetime (≈ 100 years) or without access for maintenance", 0.5, 0.5},
    };
    return t;
}

// ---- Q --------------------------------------------------------------------------------------------
std::string qClass(double q) {
    if (q < 0.01) return "Exceptionally poor";
    if (q < 0.1) return "Extremely poor";
    if (q < 1) return "Very poor";
    if (q < 4) return "Poor";
    if (q < 10) return "Fair";
    if (q < 40) return "Good";
    if (q < 100) return "Very good";
    if (q < 400) return "Extremely good";
    return "Exceptionally good";
}

QResult computeQ(const QInputs& in) {
    requireRange(in.rqd, 0, 100, "RQD");
    requireRange(in.jn, 0.5, 60, "Jn");
    requireRange(in.jr, 0.5, 4, "Jr");
    requireRange(in.ja, 0.75, 20, "Ja");
    requireRange(in.jw, 0.05, 1, "Jw");
    requireRange(in.srf, 0.5, 400, "SRF");
    QResult r;
    double rqd = in.rqd;
    if (rqd < 10) {
        r.warnings.push_back("RQD " + fmt(in.rqd) + " < 10: the nominal value 10 is used (Barton et al. 1974)");
        rqd = 10;
    }
    if (in.jn > 20) r.warnings.push_back("Jn > 20: includes the intersection (×3) or portal (×2) factor");
    r.blockSize = rqd / in.jn;
    r.shearStrength = in.jr / in.ja;
    r.activeStress = in.jw / in.srf;
    r.q = r.blockSize * r.shearStrength * r.activeStress;
    r.rockClass = qClass(r.q);
    return r;
}

QSupport qSupport(const QInputs& in, double span, double height, double esr) {
    if (!(esr > 0)) throw ClassificationError("ESR must be positive");
    if (!(span > 0)) throw ClassificationError("Span must be positive");
    const auto q = computeQ(in).q;
    QSupport s;
    s.esr = esr;
    s.span = span;
    s.height = height;
    s.equivalentDimension = span / esr;
    s.maxUnsupportedSpan = 2 * esr * std::pow(q, 0.4);
    s.unsupportedSpanExceeded = span > s.maxUnsupportedSpan;
    s.roofBoltLength = 2 + 0.15 * span / esr;
    s.wallBoltLength = height > 0 ? 2 + 0.15 * height / esr : 0;
    s.qWall = q > 10 ? 5 * q : (q > 0.1 ? 2.5 * q : q);
    auto pressure = [&](double qq) { return 2 * std::sqrt(in.jn) * std::pow(qq, -1.0 / 3.0) / (3 * in.jr) * kKgfCm2ToMPa; };
    s.roofPressureMPa = pressure(q);
    s.wallPressureMPa = pressure(s.qWall);
    return s;
}

double qc(double q, double sigmaC) { return q * sigmaC / 100.0; }

SrfStress srfFromStress(double sc, double s1, double st) {
    if (!(sc > 0) || !(s1 > 0)) throw ClassificationError("σc and σ1 must be positive");
    const double ratio = sc / s1, tang = st / sc;
    auto byRatio = [&] { return ratio > 200 ? 0 : ratio > 10 ? 1 : ratio > 5 ? 2 : ratio > 3 ? 3 : ratio > 2 ? 4 : 5; };
    auto byTang = [&] { return tang < 0.01 ? 0 : tang < 0.3 ? 1 : tang < 0.5 ? 2 : tang < 0.65 ? 3 : tang < 1.0 ? 4 : 5; };
    const int row = std::max(byRatio(), st > 0 ? byTang() : 0); // the more severe indicator governs
    const auto& t = srfTable();
    const auto& e = t[static_cast<std::size_t>(7 + row)];
    return {e.code, e.description, e.value, e.valueMax};
}

// ---- RMR89 ----------------------------------------------------------------------------------------
const char* toString(OrientationClass c) {
    switch (c) {
    case OrientationClass::VeryFavourable: return "Very favourable";
    case OrientationClass::Favourable: return "Favourable";
    case OrientationClass::Fair: return "Fair";
    case OrientationClass::Unfavourable: return "Unfavourable";
    case OrientationClass::VeryUnfavourable: return "Very unfavourable";
    }
    return "?";
}

RmrResult computeRmr89(const RmrInputs& in) {
    RmrResult r;
    if (in.ucsMPa) {
        const double u = *in.ucsMPa;
        if (!(u >= 0)) throw ClassificationError("UCS must be ≥ 0");
        r.ucsRating = u > 250 ? 15 : u >= 100 ? 12 : u >= 50 ? 7 : u >= 25 ? 4 : u >= 5 ? 2 : u >= 1 ? 1 : 0;
    } else if (in.pointLoadMPa) {
        const double i = *in.pointLoadMPa;
        if (i < 1) throw ClassificationError("Point-load index < 1 MPa: the uniaxial compressive strength is required (Bieniawski 1989)");
        r.ucsRating = i > 10 ? 15 : i >= 4 ? 12 : i >= 2 ? 7 : 4;
    } else throw ClassificationError("Intact strength (UCS or point-load index) is required");
    if (!(in.rqd >= 0 && in.rqd <= 100)) throw ClassificationError("RQD must be 0–100");
    r.rqdRating = in.rqd >= 90 ? 20 : in.rqd >= 75 ? 17 : in.rqd >= 50 ? 13 : in.rqd >= 25 ? 8 : 3;
    if (!(in.spacingM > 0)) throw ClassificationError("Discontinuity spacing must be positive");
    r.spacingRating = in.spacingM > 2 ? 20 : in.spacingM >= 0.6 ? 15 : in.spacingM >= 0.2 ? 10 : in.spacingM >= 0.06 ? 8 : 5;
    if (in.conditionDetail) {
        const auto& c = *in.conditionDetail;
        const double p = c.persistence;
        const double rp = p < 1 ? 6 : p <= 3 ? 4 : p <= 10 ? 2 : p <= 20 ? 1 : 0;
        const double a = c.aperture;
        const double ra = a <= 0 ? 6 : a < 0.1 ? 5 : a <= 1 ? 4 : a <= 5 ? 1 : 0;
        const double rr = c.roughness == Roughness::VeryRough ? 6 : c.roughness == Roughness::Rough ? 5 : c.roughness == Roughness::SlightlyRough ? 3 : c.roughness == Roughness::Smooth ? 1 : 0;
        const double ri = c.infilling == Infilling::None ? 6 : c.infilling == Infilling::HardUnder5mm ? 4 : c.infilling == Infilling::HardOver5mm ? 2 : c.infilling == Infilling::SoftUnder5mm ? 2 : 0;
        const double rw = c.weathering == Weathering::Unweathered ? 6 : c.weathering == Weathering::Slightly ? 5 : c.weathering == Weathering::Moderately ? 3 : c.weathering == Weathering::Highly ? 1 : 0;
        r.conditionRating = rp + ra + rr + ri + rw;
    } else if (in.condition) {
        switch (*in.condition) {
        case JointCondition::VeryRough: r.conditionRating = 30; break;
        case JointCondition::SlightlyRough: r.conditionRating = 25; break;
        case JointCondition::SlightlyRoughHighlyWeathered: r.conditionRating = 20; break;
        case JointCondition::Slickensided: r.conditionRating = 10; break;
        case JointCondition::SoftGouge: r.conditionRating = 0; break;
        }
    } else throw ClassificationError("Discontinuity condition is required");
    if (in.inflowLitresPerMinPer10m) {
        const double q = *in.inflowLitresPerMinPer10m;
        r.groundwaterRating = q <= 0 ? 15 : q < 10 ? 10 : q <= 25 ? 7 : q <= 125 ? 4 : 0;
    } else if (in.waterPressureRatio) {
        const double w = *in.waterPressureRatio;
        r.groundwaterRating = w <= 0 ? 15 : w < 0.1 ? 10 : w <= 0.2 ? 7 : w <= 0.5 ? 4 : 0;
    } else if (in.groundwater) {
        switch (*in.groundwater) {
        case GroundwaterCondition::CompletelyDry: r.groundwaterRating = 15; break;
        case GroundwaterCondition::Damp: r.groundwaterRating = 10; break;
        case GroundwaterCondition::Wet: r.groundwaterRating = 7; break;
        case GroundwaterCondition::Dripping: r.groundwaterRating = 4; break;
        case GroundwaterCondition::Flowing: r.groundwaterRating = 0; break;
        }
    } else throw ClassificationError("Groundwater condition is required");
    r.basic = r.ucsRating + r.rqdRating + r.spacingRating + r.conditionRating + r.groundwaterRating;
    if (in.orientation) {
        switch (*in.orientation) {
        case OrientationClass::VeryFavourable: r.adjustment = 0; break;
        case OrientationClass::Favourable: r.adjustment = -2; break;
        case OrientationClass::Fair: r.adjustment = -5; break;
        case OrientationClass::Unfavourable: r.adjustment = -10; break;
        case OrientationClass::VeryUnfavourable: r.adjustment = -12; break;
        }
    }
    r.rmr = r.basic + r.adjustment;
    static const struct {
        const char* name;
        const char* standUp;
        const char* c;
        const char* phi;
    } cls[5] = {{"I — Very good rock", "20 years for a 15 m span", "> 400 kPa", "> 45°"},
                {"II — Good rock", "1 year for a 10 m span", "300–400 kPa", "35–45°"},
                {"III — Fair rock", "1 week for a 5 m span", "200–300 kPa", "25–35°"},
                {"IV — Poor rock", "10 hours for a 2.5 m span", "100–200 kPa", "15–25°"},
                {"V — Very poor rock", "30 minutes for a 1 m span", "< 100 kPa", "< 15°"}};
    r.classNumber = r.rmr > 80 ? 1 : r.rmr > 60 ? 2 : r.rmr > 40 ? 3 : r.rmr > 20 ? 4 : 5;
    const auto& k = cls[r.classNumber - 1];
    r.rockClass = k.name;
    r.standUpTime = k.standUp;
    r.cohesion = k.c;
    r.frictionAngle = k.phi;
    return r;
}

OrientationClass tunnelOrientationClass(double dip, double dipDir, double drive) {
    if (dip < 20) return OrientationClass::Fair;
    const double strike = dipDir - 90;
    double d = std::fmod(std::abs(drive - strike), 180.0);
    if (d > 90) d = 180 - d; // 0 = parallel, 90 = perpendicular
    if (d < 45) return dip >= 45 ? OrientationClass::VeryUnfavourable : OrientationClass::Fair;
    const bool withDip = std::cos((drive - dipDir) * std::numbers::pi / 180.0) > 0;
    if (withDip) return dip >= 45 ? OrientationClass::VeryFavourable : OrientationClass::Favourable;
    return dip >= 45 ? OrientationClass::Fair : OrientationClass::Unfavourable;
}

// ---- GSI / Hoek–Brown -----------------------------------------------------------------------------
double gsiFromRmr89(const RmrResult& r) {
    const double rmrPrime = r.basic - r.groundwaterRating + 15;
    if (rmrPrime <= 23)
        throw ClassificationError("RMR89′ = " + fmt(rmrPrime) + " ≤ 23: GSI cannot be estimated from RMR89 (use Q′ or direct GSI observation)");
    return rmrPrime - 5;
}

HoekBrown hoekBrown(double gsi, double mi, double sci, double D, double Ei) {
    if (!(gsi >= 0 && gsi <= 100)) throw ClassificationError("GSI must be 0–100");
    if (!(mi > 0) || !(sci > 0)) throw ClassificationError("mi and σci must be positive");
    if (!(D >= 0 && D <= 1)) throw ClassificationError("Disturbance factor D must be 0–1");
    HoekBrown h;
    h.mb = mi * std::exp((gsi - 100) / (28 - 14 * D));
    h.s = std::exp((gsi - 100) / (9 - 3 * D));
    h.a = 0.5 + (std::exp(-gsi / 15) - std::exp(-20.0 / 3)) / 6;
    h.sigmaC = sci * std::pow(h.s, h.a);
    h.sigmaT = -h.s * sci / h.mb;
    const double a = h.a;
    h.sigmaCm = sci * (h.mb + 4 * h.s - a * (h.mb - 8 * h.s)) * std::pow(h.mb / 4 + h.s, a - 1) / (2 * (1 + a) * (2 + a));
    if (Ei > 0) h.modulusMPa = Ei * (0.02 + (1 - D / 2) / (1 + std::exp((60 + 15 * D - gsi) / 11)));
    else h.modulusMPa = 100000 * (1 - D / 2) / (1 + std::exp((75 + 25 * D - gsi) / 11));
    return h;
}

double hoekBrownSigma1(const HoekBrown& hb, double sci, double s3) {
    const double base = hb.mb * s3 / sci + hb.s;
    if (base < 0) throw ClassificationError("σ3 below the tensile strength");
    return s3 + sci * std::pow(base, hb.a);
}

double rmrFromQBieniawski1976(double q) { return 9 * std::log(q) + 44; }
double rmrFromQBarton1995(double q) { return 15 * std::log10(q) + 50; }
double qFromRmrBarton1995(double rmr) { return std::pow(10.0, (rmr - 50) / 15); }

// ---- records ----------------------------------------------------------------------------------------
Uuid recordQ(Project& p, const Uuid& dev, double chainage, Vec3 loc, const QInputs& in, const std::string& by, std::optional<Date> date,
             const std::string& notes) {
    const auto r = computeQ(in);
    GeotechClassification g;
    g.method = GeotechMethod::QSystem;
    g.developmentId = dev;
    g.chainage = chainage;
    g.location = loc;
    g.inputsJson = nlohmann::json{{"RQD", in.rqd}, {"Jn", in.jn}, {"Jr", in.jr}, {"Ja", in.ja}, {"Jw", in.jw}, {"SRF", in.srf}}.dump();
    g.value = r.q;
    g.rockClass = r.rockClass;
    g.notes = notes;
    for (const auto& w : r.warnings) g.notes += (g.notes.empty() ? "" : "\n") + w;
    g.mappedDate = date;
    g.mappedBy = by;
    return ProjectEditor(p).add(std::move(g), "Record Q classification");
}

Uuid recordRmr(Project& p, const Uuid& dev, double chainage, Vec3 loc, const RmrInputs& in, const std::string& by, std::optional<Date> date,
               const std::string& notes) {
    const auto r = computeRmr89(in);
    nlohmann::json j{{"ratings",
                      {{"strength", r.ucsRating}, {"RQD", r.rqdRating}, {"spacing", r.spacingRating}, {"condition", r.conditionRating},
                       {"groundwater", r.groundwaterRating}, {"orientation", r.adjustment}}},
                     {"RQD", in.rqd},
                     {"spacing_m", in.spacingM}};
    if (in.ucsMPa) j["UCS_MPa"] = *in.ucsMPa;
    if (in.pointLoadMPa) j["Is50_MPa"] = *in.pointLoadMPa;
    if (in.orientation) j["orientation"] = toString(*in.orientation);
    GeotechClassification g;
    g.method = GeotechMethod::Rmr89;
    g.developmentId = dev;
    g.chainage = chainage;
    g.location = loc;
    g.inputsJson = j.dump();
    g.value = r.rmr;
    g.rockClass = r.rockClass;
    g.notes = notes;
    g.mappedDate = date;
    g.mappedBy = by;
    return ProjectEditor(p).add(std::move(g), "Record RMR89 classification");
}

} // namespace openmine::geotechnical
