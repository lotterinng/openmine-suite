// SPDX-License-Identifier: Apache-2.0
#include "openmine/reporting/SurveyReports.h"

#include "openmine/mine_design/Development.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/Processing.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>

namespace openmine::reporting {

namespace {
using nlohmann::json;
constexpr double kDeg = 180.0 / std::numbers::pi;

std::string f(double v, int d = 3) { return fmtNumber(v, d); }
std::string vec(Vec3 v, int d = 3) { return f(v.x, d) + ", " + f(v.y, d) + ", " + f(v.z, d); }
std::string dateOr(const std::optional<Date>& d) { return d ? d->toString() : "—"; }
std::string orDash(const std::string& s) { return s.empty() ? "—" : s; }

std::string devLabel(const Project& p, const Uuid& id) {
    const auto* d = p.model().developments.find(id);
    if (!d) return "(missing development)";
    return d->headingId.empty() ? d->name : d->headingId + (d->name.empty() ? "" : " — " + d->name);
}
std::string datasetLabel(const Project& p, const Uuid& id) {
    const auto* d = p.model().pointClouds.find(id);
    return d ? d->name : (id.isNil() ? "—" : "(missing dataset)");
}
const char* methodName(RegistrationMethod m) {
    switch (m) {
    case RegistrationMethod::PointPair: return "Point pairs";
    case RegistrationMethod::ControlPoints: return "Survey control points";
    case RegistrationMethod::IcpPointToPoint: return "ICP (point-to-point)";
    case RegistrationMethod::IcpPointToPlane: return "ICP (point-to-plane)";
    case RegistrationMethod::Manual: return "Manual transform";
    }
    return "?";
}
} // namespace

// ---- conformance ------------------------------------------------------------------------------------
ReportDocument conformanceReport(const Project& p, const Uuid& rid, const survey::ConformanceReport* full, const ConformanceReportOptions& o) {
    const auto& m = p.model();
    const auto* c = m.conformanceResults.find(rid);
    if (!c) throw ValidationError("Conformance result does not exist");
    const auto* a = m.analysisResults.find(c->analysisResultId);
    const auto* ab = m.asBuiltModels.find(c->asBuiltId);
    const auto* dev = m.developments.find(c->developmentId);
    ReportDocument d;
    d.title = "Design conformance — " + devLabel(p, c->developmentId);
    d.subtitle = "Overbreak, underbreak and clearance of the as-built excavation against the design";
    d.meta = standardMeta(p);

    d.heading("Inputs");
    std::vector<std::pair<std::string, std::string>> in{{"Development", devLabel(p, c->developmentId)}};
    if (dev) {
        const auto mt = mine_design::computeMetrics(p, *dev);
        in.push_back({"Design section", f(dev->designWidth, 2) + " m × " + f(dev->designHeight, 2) + " m, area " + f(mt.sectionArea, 2) + " m²"});
        in.push_back({"Design length", f(mt.length3d, 2) + " m (chainage " + f(dev->chainageFrom, 2) + " – " + f(mt.chainageTo, 2) + ")"});
    }
    if (ab) {
        in.push_back({"As-built model", ab->name + " (" + orDash(ab->method) + ")"});
        in.push_back({"Survey date", dateOr(ab->surveyDate)});
        in.push_back({"Source scan", datasetLabel(p, ab->sourceDatasetId)});
        if (const auto* reg = m.registrations.find(ab->registrationId))
            in.push_back({"Registration in force", std::string(methodName(reg->method)) + ", fit RMS " + f(reg->fitRms, 3) + " m" +
                                                       (reg->checkRms >= 0 ? ", check RMS " + f(reg->checkRms, 3) + " m" : ", no independent check points")});
        else in.push_back({"Registration in force", "none recorded (scan used in its stored coordinates)"});
    }
    in.push_back({"Station interval", f(c->stationInterval, 2) + " m"});
    in.push_back({"Chainage analysed", f(c->chainageFrom, 2) + " – " + f(c->chainageTo, 2)});
    if (a) {
        const auto params = json::parse(a->parametersJson.empty() ? "{}" : a->parametersJson);
        in.push_back({"Section orientation", params.value("orientation", "perpendicular") == "vertical" ? "Vertical" : "Perpendicular to the centreline"});
        if (params.contains("clearanceEnvelope")) in.push_back({"Clearance envelope", std::to_string(params["clearanceEnvelope"].size()) + " vertices"});
    }
    d.keyValues(in);

    d.heading("Summary");
    const double obPct = c->designVolume > 0 ? 100.0 * c->overbreakVolume / c->designVolume : 0.0;
    std::vector<std::pair<std::string, std::string>> sum{{"Design volume", fmtNumber(c->designVolume, 1, true) + " m³"},
                                                         {"As-built volume", fmtNumber(c->asBuiltVolume, 1, true) + " m³"},
                                                         {"Overbreak volume", fmtNumber(c->overbreakVolume, 1, true) + " m³ (" + f(obPct, 1) + " % of design)"},
                                                         {"Underbreak volume", fmtNumber(c->underbreakVolume, 1, true) + " m³"},
                                                         {"Mean overbreak depth", f(c->meanOverbreakDepth, 3) + " m"},
                                                         {"Maximum underbreak depth", f(c->maxUnderbreakDepth, 3) + " m"},
                                                         {"Minimum clearance", c->minClearance >= 0 ? f(c->minClearance, 3) + " m" : "not assessed"},
                                                         {"Stations", std::to_string(c->stationCount)}};
    d.keyValues(sum);

    if (!a || a->payloadJson.empty()) {
        d.note("The per-station table of this result is not available.");
        return d;
    }
    const json rows = json::parse(a->payloadJson);
    int valid = 0;
    LineChart areas{"Section area along the heading", "Chainage (m)", "Area (m²)", {}, std::nullopt};
    LineChart breaks{"Overbreak and underbreak area", "Chainage (m)", "Area (m²)", {}, 0.0};
    ChartSeries sd{"Design", {}, 0x1F77B4, true}, sa{"As-built", {}, 0xD62728, false}, so{"Overbreak", {}, 0xFF7F0E, false}, su{"Underbreak (negative)", {}, 0x9467BD, false};
    ReportTable t;
    t.caption = "Stations (highlighted: invalid, underbreak deeper than " + f(o.underbreakTolerance, 2) + " m, or overbreak deeper than " + f(o.overbreakTolerance, 2) + " m)";
    t.header = {"Chainage", "Design m²", "As-built m²", "Over m²", "Under m²", "Max over m", "Max under m", "Clearance m", "Issue"};
    t.numeric = {true, true, true, true, true, true, true, true, false};
    for (const auto& r : rows) {
        const double ch = r.value("chainage", 0.0);
        const bool ok = r.value("valid", false);
        if (ok) {
            ++valid;
            sd.points.emplace_back(ch, r.value("designArea", 0.0));
            sa.points.emplace_back(ch, r.value("asBuiltArea", 0.0));
            so.points.emplace_back(ch, r.value("overbreakArea", 0.0));
            su.points.emplace_back(ch, -r.value("underbreakArea", 0.0));
        } else {
            const double nan = std::numeric_limits<double>::quiet_NaN();
            sa.points.emplace_back(ch, nan);
            so.points.emplace_back(ch, nan);
            su.points.emplace_back(ch, nan);
        }
        const bool flagged = !ok || r.value("maxUnderbreak", 0.0) > o.underbreakTolerance || r.value("maxOverbreak", 0.0) > o.overbreakTolerance;
        if (flagged) t.highlightRows.push_back(static_cast<int>(t.rows.size()));
        auto num = [&](const char* k, int dec) { return ok ? f(r.value(k, 0.0), dec) : std::string("–"); };
        t.rows.push_back({f(ch, 2), f(r.value("designArea", 0.0), 2), num("asBuiltArea", 2), num("overbreakArea", 2), num("underbreakArea", 2), num("maxOverbreak", 3),
                          num("maxUnderbreak", 3), r.contains("clearance") && !r["clearance"].is_null() ? f(r["clearance"].get<double>(), 3) : "–", r.value("issue", "")});
    }
    areas.series = {sd, sa};
    breaks.series = {so, su};
    d.heading("Along the heading");
    d.chart(areas).chart(breaks);
    if (valid < static_cast<int>(rows.size()))
        d.note(std::to_string(rows.size() - static_cast<std::size_t>(valid)) +
               " station(s) could not be evaluated (no closed as-built section — e.g. a hole in the scan or the face); they are excluded from the "
               "volumes and listed with their reason.");

    if (full && dev && o.worstSections > 0) {
        const auto ring = mine_design::developmentProfile(p, *dev);
        std::vector<std::pair<double, double>> design;
        for (const auto& v : ring) design.emplace_back(v.x, v.y);
        // Half of the drawings for the largest overbreak, half for the largest underbreak.
        std::vector<const survey::StationConformance*> byOver, byUnder;
        for (const auto& s : full->stations)
            if (s.valid) {
                if (s.overbreakArea > 1e-6) byOver.push_back(&s);
                if (s.underbreakArea > 1e-6) byUnder.push_back(&s);
            }
        std::sort(byOver.begin(), byOver.end(), [](auto* x, auto* y) { return x->overbreakArea > y->overbreakArea; });
        std::sort(byUnder.begin(), byUnder.end(), [](auto* x, auto* y) { return x->underbreakArea > y->underbreakArea; });
        std::vector<const survey::StationConformance*> st;
        const std::size_t half = static_cast<std::size_t>((o.worstSections + 1) / 2);
        for (std::size_t k = 0; k < byUnder.size() && k < half; ++k) st.push_back(byUnder[k]);
        for (std::size_t k = 0; k < byOver.size() && st.size() < static_cast<std::size_t>(o.worstSections); ++k)
            if (std::find(st.begin(), st.end(), byOver[k]) == st.end()) st.push_back(byOver[k]);
        for (std::size_t k = half; k < byUnder.size() && st.size() < static_cast<std::size_t>(o.worstSections); ++k) st.push_back(byUnder[k]);
        std::sort(st.begin(), st.end(), [](auto* x, auto* y) { return x->chainage < y->chainage; });
        std::vector<SectionPlot> plots;
        for (const auto* s : st) {
            SectionPlot sp;
            sp.title = "Ch " + f(s->chainage, 1) + ": over " + f(s->overbreakArea, 2) + " m², under " + f(s->underbreakArea, 2) + " m²";
            sp.design = design;
            for (const auto& v : s->asBuilt) sp.asBuilt.emplace_back(v.x, v.y);
            plots.push_back(std::move(sp));
        }
        if (!plots.empty()) {
            d.heading("Largest deviations");
            d.paragraph("Cross-sections looking up-chainage at the stations with the largest underbreak and overbreak areas. Blue: design; red: as-built.");
            d.sections(std::move(plots));
        }
    }
    d.heading("Station table");
    d.table(std::move(t));
    d.heading("Method", 2);
    d.paragraph("At each station the as-built surface is cut by the section plane; the design profile and the as-built section are compared with exact polygon "
                "booleans (overbreak = as-built outside design, underbreak = design not excavated). Volumes integrate the section areas along the slope "
                "length between stations (trapezoidal rule). Depths are measured normal to the design outline.");
    return d;
}

// ---- registration ------------------------------------------------------------------------------------------
ReportDocument registrationReport(const Project& p, const Uuid& id) {
    const auto& m = p.model();
    const auto* r = m.registrations.find(id);
    if (!r) throw ValidationError("Registration does not exist");
    ReportDocument d;
    d.title = "Registration — " + datasetLabel(p, r->datasetId);
    d.subtitle = std::string(methodName(r->method)) + (r->applied ? " (applied)" : " (not applied)");
    d.meta = standardMeta(p);
    d.heading("Registration");
    std::vector<std::pair<std::string, std::string>> kv{{"Moving dataset", datasetLabel(p, r->datasetId)}, {"Method", methodName(r->method)}};
    if (!r->targetDatasetId.isNil()) kv.push_back({"Target dataset", datasetLabel(p, r->targetDatasetId)});
    kv.push_back({"Scale", r->allowScale ? "estimated: " + fmtNumber(r->transform.uniformScale(), 8) : "fixed at 1 (rigid)"});
    const Vec3 e = r->transform.eulerZYX(); // {x: roll about X, y: pitch about Y, z: yaw about Z}
    kv.push_back({"Rotation Z / Y / X", f(e.z * kDeg, 6) + "° / " + f(e.y * kDeg, 6) + "° / " + f(e.x * kDeg, 6) + "°"});
    kv.push_back({"Translation", vec(r->transform.translationPart(), 4) + " m"});
    kv.push_back({"Status", r->applied ? "Applied to the dataset transform" : "Stored only (dataset unchanged)"});
    if (!r->notes.empty()) kv.push_back({"Notes", r->notes});
    d.keyValues(kv);
    ReportTable mt;
    mt.caption = "Transform increment (row-major, stored → project coordinates)";
    mt.header = {"", "c1", "c2", "c3", "c4"};
    for (int i = 0; i < 4; ++i) mt.rows.push_back({"r" + std::to_string(i + 1), fmtNumber(r->transform(i, 0), 9), fmtNumber(r->transform(i, 1), 9),
                                                   fmtNumber(r->transform(i, 2), 9), fmtNumber(r->transform(i, 3), 4)});
    d.table(std::move(mt));

    d.heading("Quality");
    std::vector<std::pair<std::string, std::string>> q;
    if (r->fitCount > 0) q.push_back({"Fit residuals", std::to_string(r->fitCount) + " points, RMS " + f(r->fitRms, 4) + " m, max " + f(r->fitMax, 4) + " m"});
    if (r->checkCount > 0) q.push_back({"Independent check points", std::to_string(r->checkCount) + " points, RMS " + f(r->checkRms, 4) + " m, max " + f(r->checkMax, 4) + " m"});
    else q.push_back({"Independent check points", "none"});
    if (r->icpFitness >= 0) {
        q.push_back({"ICP overlap (fitness)", f(100 * r->icpFitness, 1) + " % of sampled points within " + f(r->icpMaxCorrespondence, 3) + " m"});
        q.push_back({"ICP inlier RMS", f(r->icpInlierRms, 4) + " m after " + std::to_string(r->icpIterations) + " iterations" + (r->icpConverged ? " (converged)" : " (NOT converged)")});
    }
    d.keyValues(q);
    if (r->checkCount == 0)
        d.note("Fit residuals measure only how well the transform reproduces the points used to compute it and are optimistic. Without independent check "
               "points the absolute accuracy of the registration has not been verified.");
    if (!r->pairs.empty()) {
        ReportTable t;
        t.caption = "Point pairs (residual = target − transformed source)";
        t.header = {"Point", "Role", "Target E", "Target N", "Target Z", "dE", "dN", "dZ", "|d|"};
        for (const auto& pr : r->pairs) {
            const double len = pr.residual.length();
            if (pr.role == PairRole::Check && len > std::max(3 * r->fitRms, 0.05)) t.highlightRows.push_back(static_cast<int>(t.rows.size()));
            t.rows.push_back({pr.name, pr.role == PairRole::Fit ? "Fit" : "Check", f(pr.target.x, 3), f(pr.target.y, 3), f(pr.target.z, 3), f(pr.residual.x, 4),
                              f(pr.residual.y, 4), f(pr.residual.z, 4), f(len, 4)});
        }
        d.table(std::move(t));
    }
    d.heading("Previous transform", 2);
    d.paragraph("The dataset transform before this registration was " + std::string(r->previousTransform.isIdentity() ? "the identity" : "not the identity") +
                "; the stored coordinates of the scan are never changed, only its transform, so the registration can be reverted.");
    return d;
}

// ---- point cloud ---------------------------------------------------------------------------------------------
ReportDocument pointCloudReport(const Project& p, const Uuid& id, bool spacing) {
    const auto& m = p.model();
    const auto* ds = m.pointClouds.find(id);
    if (!ds) throw ValidationError("Point-cloud dataset does not exist");
    ReportDocument d;
    d.title = "Point cloud — " + ds->name;
    d.subtitle = "Provenance, georeferencing and content of a survey dataset";
    d.meta = standardMeta(p);
    d.heading("Provenance");
    std::vector<std::pair<std::string, std::string>> pv{{"Source file", orDash(ds->sourceFile)}, {"Format", orDash(ds->sourceFormat)},
                                                        {"Declared CRS of the source", orDash(ds->sourceCrs)}, {"Scanner", orDash(ds->scanner)},
                                                        {"Scan date", dateOr(ds->scanDate)}};
    if (const auto* crs = m.projectCrs()) pv.push_back({"Project CRS", crs->name + (crs->authorityCode.empty() ? "" : " (" + crs->authorityCode + ")")});
    if (!ds->developmentId.isNil()) pv.push_back({"Heading", devLabel(p, ds->developmentId)});
    // Derivation chain.
    std::string chain;
    const PointCloudDataset* cur = ds;
    for (int guard = 0; cur && !cur->parentDatasetId.isNil() && guard < 32; ++guard) {
        chain += (chain.empty() ? "" : " ← ") + cur->derivation;
        cur = m.pointClouds.find(cur->parentDatasetId);
        if (cur) chain += " ← " + cur->name;
    }
    pv.push_back({"Derivation", chain.empty() ? "original import" : chain});
    d.keyValues(pv);

    d.heading("Content");
    std::string attrs = "XYZ (float64)";
    if (ds->attributes.intensity) attrs += ", intensity";
    if (ds->attributes.rgb) attrs += ", RGB";
    if (ds->attributes.classification) attrs += ", classification";
    for (const auto& s : ds->attributes.scalarFields) attrs += ", " + s;
    const auto& b = ds->storedBounds;
    BoundingBox pb = BoundingBox::empty();
    for (int n = 0; n < 8; ++n) pb.expand(ds->transform.apply({(n & 1) ? b.max.x : b.min.x, (n & 2) ? b.max.y : b.min.y, (n & 4) ? b.max.z : b.min.z}));
    std::vector<std::pair<std::string, std::string>> ct{{"Points", fmtNumber(static_cast<double>(ds->pointCount), 0, true)},
                                                        {"Attributes", attrs},
                                                        {"Stored bounds min", vec(b.min)},
                                                        {"Stored bounds max", vec(b.max)},
                                                        {"Project bounds min", vec(pb.min)},
                                                        {"Project bounds max", vec(pb.max)},
                                                        {"Transform", ds->transform.isIdentity() ? "identity (stored = project coordinates)" : ds->transform.toString()}};
    if (spacing) {
        const auto pts = survey::datasetPoints(p, id);
        const auto st = survey::statistics(pts);
        ct.push_back({"Mean point spacing", f(st.meanSpacing, 4) + " m (sampled nearest-neighbour distance)"});
        ct.push_back({"Median point spacing", f(st.medianSpacing, 4) + " m"});
    }
    d.keyValues(ct);

    ReportTable regs;
    regs.caption = "Registrations of this dataset";
    regs.header = {"Date", "Method", "Applied", "Fit RMS m", "Check RMS m", "ICP inlier RMS m"};
    for (const auto& r : m.registrations.all())
        if (r.datasetId == id)
            regs.rows.push_back({r.meta.createdAt.substr(0, 10), methodName(r.method), r.applied ? "yes" : "no", r.fitCount ? f(r.fitRms, 4) : "–",
                                 r.checkCount ? f(r.checkRms, 4) : "–", r.icpFitness >= 0 ? f(r.icpInlierRms, 4) : "–"});
    d.heading("Registrations");
    if (regs.rows.empty()) d.paragraph("No registrations recorded.");
    else d.table(std::move(regs));
    ReportTable abt;
    abt.caption = "As-built models derived from this dataset";
    abt.header = {"Model", "Method", "Heading", "Survey date"};
    for (const auto& a : m.asBuiltModels.all())
        if (a.sourceDatasetId == id) abt.rows.push_back({a.name, orDash(a.method), a.developmentId.isNil() ? "—" : devLabel(p, a.developmentId), dateOr(a.surveyDate)});
    if (!abt.rows.empty()) {
        d.heading("Derived as-built models");
        d.table(std::move(abt));
    }
    return d;
}

} // namespace openmine::reporting
