// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/Conformance.h"

#include "openmine/commands/CommandStack.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polygon2D.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"

#include <clipper2/clipper.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <cstdio>
#include <sstream>

namespace openmine::survey {

namespace {
constexpr double kPi = std::numbers::pi;

Clipper2Lib::PathsD toPaths(const std::vector<Vec2>& ring) {
    Clipper2Lib::PathD p;
    p.reserve(ring.size());
    for (const auto& v : ring) p.emplace_back(v.x, v.y);
    return {p};
}

double area(const Clipper2Lib::PathsD& p) { return std::abs(Clipper2Lib::Area(p)); }

/// Distance from p to the polygon boundary, positive inside, negative outside.
double signedInside(const std::vector<Vec2>& ring, Vec2 p) {
    const double d = geometry::distanceToBoundary(ring, p);
    return geometry::contains(ring, p) ? d : -d;
}

std::vector<Vec2> densify(const std::vector<Vec2>& ring, double step) {
    std::vector<Vec2> out;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Vec2 a = ring[i], b = ring[(i + 1) % ring.size()];
        const int n = std::max(1, static_cast<int>(std::ceil(distance(a, b) / step)));
        for (int k = 0; k < n; ++k) out.push_back(a + (b - a) * (static_cast<double>(k) / n));
    }
    return out;
}

std::vector<double> stationChainages(const std::vector<Vec3>& cl, const ConformanceOptions& o) {
    const double len = geometry::length3d(cl);
    const double from = std::clamp(o.fromChainage.value_or(0.0), 0.0, len), to = std::clamp(o.toChainage.value_or(len), 0.0, len);
    if (!(o.interval > 0)) throw PointCloudError("Station interval must be positive");
    if (to <= from) throw PointCloudError("The chainage range is empty");
    std::vector<double> ch;
    for (double c = from; c < to - 1e-9; c += o.interval) ch.push_back(c);
    ch.push_back(to);
    return ch;
}

ConformanceReport summarise(std::vector<StationConformance> st) {
    ConformanceReport r;
    r.stations = std::move(st);
    double depthSum = 0;
    for (std::size_t i = 0; i < r.stations.size(); ++i) {
        const auto& s = r.stations[i];
        if (!s.valid) continue;
        ++r.validStations;
        depthSum += s.maxOverbreakDepth;
        r.maxOverbreakDepth = std::max(r.maxOverbreakDepth, s.maxOverbreakDepth);
        r.maxUnderbreakDepth = std::max(r.maxUnderbreakDepth, s.maxUnderbreakDepth);
        if (s.clearance) r.minClearance = r.minClearance ? std::min(*r.minClearance, *s.clearance) : *s.clearance;
        // Average end areas between consecutive valid stations.
        if (i + 1 < r.stations.size() && r.stations[i + 1].valid) {
            const auto& n = r.stations[i + 1];
            const double L = n.chainage - s.chainage;
            r.designVolume += L * (s.designArea + n.designArea) / 2;
            r.asBuiltVolume += L * (s.asBuiltArea + n.asBuiltArea) / 2;
            r.overbreakVolume += L * (s.overbreakArea + n.overbreakArea) / 2;
            r.underbreakVolume += L * (s.underbreakArea + n.underbreakArea) / 2;
        }
    }
    if (r.validStations) r.meanOverbreakDepth = depthSum / r.validStations;
    if (r.designVolume > 0) r.overbreakPercent = 100.0 * r.overbreakVolume / r.designVolume;
    return r;
}
} // namespace

StationConformance compareSection(const std::vector<Vec2>& design, const std::vector<Vec2>& asBuilt, const std::vector<Vec2>& envelope) {
    StationConformance s;
    if (design.size() < 3) throw PointCloudError("The design profile needs at least three points");
    if (asBuilt.size() < 3) {
        s.issue = "no as-built section";
        return s;
    }
    s.asBuilt = asBuilt;
    const auto D = toPaths(design), A = toPaths(asBuilt);
    s.designArea = area(Clipper2Lib::Union(D, Clipper2Lib::FillRule::NonZero, 8));
    s.asBuiltArea = area(Clipper2Lib::Union(A, Clipper2Lib::FillRule::NonZero, 8));
    s.overbreakArea = area(Clipper2Lib::Difference(A, D, Clipper2Lib::FillRule::NonZero, 8));
    s.underbreakArea = area(Clipper2Lib::Difference(D, A, Clipper2Lib::FillRule::NonZero, 8));
    // Depths: as-built vertices outside (overbreak) or inside (underbreak) the design, and design
    // vertices outside the as-built (underbreak at sparse sampling).
    for (const auto& p : densify(asBuilt, 0.02)) {
        const double d = signedInside(design, p);
        if (d < 0) s.maxOverbreakDepth = std::max(s.maxOverbreakDepth, -d);
        else s.maxUnderbreakDepth = std::max(s.maxUnderbreakDepth, d);
    }
    if (s.underbreakArea <= 1e-12) s.maxUnderbreakDepth = 0;
    for (const auto& p : densify(design, 0.02)) {
        const double d = signedInside(asBuilt, p);
        if (d < 0) s.maxUnderbreakDepth = std::max(s.maxUnderbreakDepth, -d);
    }
    if (s.overbreakArea <= 1e-12) s.maxOverbreakDepth = 0;
    if (envelope.size() >= 3) {
        double c = 1e300;
        for (const auto& p : densify(envelope, 0.02)) c = std::min(c, signedInside(asBuilt, p));
        for (const auto& p : asBuilt)
            if (geometry::contains(envelope, p)) c = std::min(c, -geometry::distanceToBoundary(envelope, p));
        s.clearance = c;
    }
    s.valid = true;
    return s;
}

ConformanceReport analyseConformance(const std::vector<Vec3>& cl, const std::vector<Vec2>& design, const MeshData& mesh, const ConformanceOptions& o,
                                     jobs::Progress& progress) {
    if (cl.size() < 2) throw PointCloudError("The centreline needs at least two points");
    if (mesh.triangles.empty()) throw PointCloudError("The as-built mesh has no triangles");
    const auto chs = stationChainages(cl, o);
    const Vec2 dc = geometry::centroid(design);
    std::vector<StationConformance> out(chs.size());
    jobs::parallelFor(chs.size(), [&](std::size_t lo, std::size_t hi) {
        for (std::size_t k = lo; k < hi; ++k) {
            const auto f = geometry::sectionFrameAt(cl, chs[k], o.orientation);
            const Vec3 normal = o.orientation == geometry::SectionOrientation::PerpendicularToAxis ? f.tangent : cross(f.right, f.up).normalized();
            const auto loops = geometry::planeSection(mesh, f.origin, normal);
            std::vector<Vec2> best;
            bool bestClosed = true, bestContains = false;
            double bestArea = 0;
            for (const auto& l : loops) {
                if (l.size() < 3) continue;
                std::vector<Vec2> ring;
                for (const auto& p : l) ring.push_back(f.toProfile(p));
                const bool closed = distance(ring.front(), ring.back()) < 1e-6;
                if (closed) ring.pop_back();
                ring = geometry::normalizeRing(ring);
                if (ring.size() < 3) continue;
                const bool contains = geometry::contains(ring, dc);
                const double a = geometry::area(ring);
                if ((contains && !bestContains) || (contains == bestContains && a > bestArea)) {
                    best = ring;
                    bestClosed = closed;
                    bestContains = contains;
                    bestArea = a;
                }
            }
            StationConformance s;
            if (best.empty()) s.issue = "the as-built does not cross this station";
            else {
                s = compareSection(design, best, o.clearanceEnvelope);
                s.closedByChord = !bestClosed;
                if (!bestClosed) s.issue = "open as-built section closed by its chord";
                if (!bestContains) s.issue += std::string(s.issue.empty() ? "" : "; ") + "as-built section does not enclose the design centroid";
            }
            s.chainage = chs[k];
            out[k] = std::move(s);
        }
    });
    progress.update(1.0, "Conformance");
    return summarise(std::move(out));
}

ConformanceReport analyseConformance(const std::vector<Vec3>& cl, const std::vector<Vec2>& design, const PointBuffer& pts, const ConformanceOptions& o,
                                     jobs::Progress& progress) {
    if (cl.size() < 2) throw PointCloudError("The centreline needs at least two points");
    const auto chs = stationChainages(cl, o);
    const Vec2 dc = geometry::centroid(design);
    const int bins = std::max(36, o.angularBins);
    std::vector<StationConformance> out(chs.size());
    jobs::parallelFor(chs.size(), [&](std::size_t lo, std::size_t hi) {
        for (std::size_t k = lo; k < hi; ++k) {
            const auto f = geometry::sectionFrameAt(cl, chs[k], o.orientation);
            const Vec3 normal = o.orientation == geometry::SectionOrientation::PerpendicularToAxis ? f.tangent : cross(f.right, f.up).normalized();
            std::vector<std::pair<double, Vec2>> best(static_cast<std::size_t>(bins), {-1.0, Vec2{}});
            std::size_t n = 0;
            for (const auto& p : pts.xyz) {
                const double s = dot(p - f.origin, normal);
                if (std::abs(s) > o.slabHalfWidth) continue;
                const Vec2 uv = f.toProfile(p);
                const Vec2 d = uv - dc;
                const double r = d.length();
                if (r > 50) continue; // not this heading
                ++n;
                double a = std::atan2(d.y, d.x);
                if (a < 0) a += 2 * kPi;
                const auto bi = std::min(static_cast<std::size_t>(a / (2 * kPi) * bins), static_cast<std::size_t>(bins - 1));
                if (r > best[bi].first) best[bi] = {r, uv};
            }
            StationConformance s;
            s.chainage = chs[k];
            if (n < o.minSlabPoints) {
                s.issue = "too few scan points at this station (" + std::to_string(n) + ")";
                out[k] = std::move(s);
                continue;
            }
            std::vector<Vec2> ring;
            std::size_t empty = 0;
            for (const auto& b : best) {
                if (b.first < 0) ++empty;
                else ring.push_back(b.second);
            }
            s = compareSection(design, ring, o.clearanceEnvelope);
            s.chainage = chs[k];
            if (empty > static_cast<std::size_t>(bins) / 10) s.issue = std::to_string(empty) + " of " + std::to_string(bins) + " angular bins have no points (shadowed)";
            out[k] = std::move(s);
        }
    });
    progress.update(1.0, "Conformance");
    return summarise(std::move(out));
}

FacePosition facePosition(const std::vector<Vec3>& cl, const PointBuffer& pts, double corridor, double bin, std::size_t minPts, double maxGap) {
    if (cl.size() < 2) throw PointCloudError("The centreline needs at least two points");
    const double len = geometry::length3d(cl);
    const auto nb = static_cast<std::size_t>(std::ceil(len / bin)) + 1;
    std::vector<std::size_t> count(nb, 0);
    FacePosition f;
    for (const auto& p : pts.xyz) {
        const auto pr = geometry::projectOntoPolyline(p, cl);
        if (!pr || pr->distance > corridor) continue;
        ++count[std::min(static_cast<std::size_t>(pr->chainage / bin), nb - 1)];
        ++f.pointsUsed;
    }
    std::optional<std::size_t> first, last;
    for (std::size_t i = 0; i < nb; ++i) {
        if (count[i] < minPts) continue;
        if (!first) first = last = i;
        else if (static_cast<double>(i - *last) * bin <= maxGap + 1e-9) last = i;
        else break; // coverage interrupted: a separate stretch beyond the face
    }
    if (!first) throw PointCloudError("No scan points lie along the centreline corridor");
    f.coveredFrom = static_cast<double>(*first) * bin;
    f.faceChainage = std::min(len, static_cast<double>(*last + 1) * bin);
    // Refine to the furthest point in the last covered bin.
    double furthest = 0;
    for (const auto& p : pts.xyz) {
        const auto pr = geometry::projectOntoPolyline(p, cl);
        if (pr && pr->distance <= corridor && static_cast<std::size_t>(pr->chainage / bin) == *last) furthest = std::max(furthest, pr->chainage);
    }
    if (furthest > 0) f.faceChainage = furthest;
    return f;
}

// ---- Project integration --------------------------------------------------------------------------------
Uuid addAsBuiltModel(Project& p, const Uuid& datasetId, MeshData mesh, const std::string& name, const Uuid& layerId, const Uuid& devId,
                     const std::string& method) {
    ProjectEditor ed(p);
    const auto* d = p.model().pointClouds.find(datasetId);
    if (!d) throw PointCloudError("Point-cloud dataset does not exist");
    // Registration in force: the latest applied registration that produced the current transform.
    Uuid regId;
    for (const auto& r : p.model().registrations.all())
        if (r.datasetId == datasetId && r.applied && r.transform * r.previousTransform == d->transform) regId = r.id;
    CommandStack::MacroScope macro(p.commands(), "Create as-built model " + name);
    GeometryObject g;
    g.type = GeometryType::Mesh;
    g.name = name;
    g.layerId = layerId;
    g.mesh = std::make_shared<MeshData>(std::move(mesh));
    const Uuid gid = ed.addGeometry(g);
    AsBuiltModel a;
    a.name = name;
    a.sourceDatasetId = datasetId;
    a.registrationId = regId;
    a.geometryId = gid;
    a.developmentId = devId;
    a.method = method;
    a.surveyDate = d->scanDate;
    const Uuid id = ed.add(a);
    macro.commit();
    return id;
}

ConformanceReport analyseAsBuilt(const Project& p, const Uuid& abId, const Uuid& devId, const ConformanceOptions& o, jobs::Progress& progress) {
    const auto* ab = p.model().asBuiltModels.find(abId);
    if (!ab) throw PointCloudError("As-built model does not exist");
    const auto* g = p.model().geometry.find(ab->geometryId);
    if (!g || !g->mesh) throw PointCloudError("The as-built model has no mesh");
    const auto* d = p.model().developments.find(devId);
    if (!d) throw PointCloudError("Development does not exist");
    const auto cl = mine_design::centrelinePoints(p, *d);
    // Exact design outline, finely tessellated (64 chords per quarter circle).
    const auto pc = mine_design::developmentProfileCurve(p, *d);
    std::vector<Vec2> design;
    {
        const std::size_t n = pc.points.size();
        for (std::size_t i = 0; i < n; ++i) {
            design.push_back(pc.points[i]);
            const double bl = pc.bulges[i];
            if (std::abs(bl) < 1e-15) continue;
            std::vector<Vec3> seg = geometry::expandBulges(std::vector<Vec3>{{pc.points[i].x, pc.points[i].y, 0}, {pc.points[(i + 1) % n].x, pc.points[(i + 1) % n].y, 0}},
                                                           std::vector<double>{bl, 0.0}, false, 1e-5);
            for (std::size_t k = 1; k + 1 < seg.size(); ++k) design.push_back({seg[k].x, seg[k].y});
        }
    }
    return analyseConformance(cl, design, *g->mesh, o, progress);
}

AdvanceReconciliation reconcileAdvance(const Project& p, const Uuid& devId, const Uuid& actId, double face, const Date& date) {
    const auto* d = p.model().developments.find(devId);
    if (!d) throw PointCloudError("Development does not exist");
    if (!p.model().activities.find(actId)) throw PointCloudError("Activity does not exist");
    AdvanceReconciliation r;
    r.previousFace = d->chainageFrom;
    for (const auto* a : p.model().actualsForActivity(actId))
        if (a->source == ActualSource::Survey && a->faceChainage && a->date < date && (!r.previousDate || a->date >= *r.previousDate)) {
            r.previousDate = a->date;
            r.previousFace = *a->faceChainage;
        }
    r.currentFace = face;
    r.surveyedAdvance = face - r.previousFace;
    for (const auto* a : p.model().actualsForActivity(actId))
        if (a->source != ActualSource::Survey && a->date <= date && (!r.previousDate || a->date > *r.previousDate)) r.reportedAdvance += a->quantity;
    r.correction = r.surveyedAdvance - r.reportedAdvance;
    return r;
}

Uuid commitSurveyActual(Project& p, const AdvanceReconciliation& r, const Uuid& devId, const Uuid& actId, const Uuid& datasetId, const Date& date) {
    ProductionActual a;
    a.activityId = actId;
    a.developmentId = devId;
    a.date = date;
    a.quantity = r.correction;
    a.unit = "m";
    a.source = ActualSource::Survey;
    a.surveyDatasetId = datasetId;
    a.faceChainage = r.currentFace;
    char buf[200];
    std::snprintf(buf, sizeof buf, "Survey reconciliation: surveyed advance %.2f m, reported %.2f m, correction %+.2f m", r.surveyedAdvance, r.reportedAdvance,
                  r.correction);
    a.note = buf;
    a.enteredBy = p.userName();
    return ProjectEditor(p).add(a, "Survey actual");
}

std::string conformanceCsv(const ConformanceReport& r) {
    std::ostringstream s;
    s << "chainage,valid,design_area_m2,asbuilt_area_m2,overbreak_area_m2,underbreak_area_m2,max_overbreak_m,max_underbreak_m,clearance_m,issue\n";
    s.setf(std::ios::fixed);
    s.precision(4);
    for (const auto& st : r.stations) {
        std::string issue = st.issue;
        std::replace(issue.begin(), issue.end(), ',', ';');
        s << mine_design::formatChainage(st.chainage) << ',' << (st.valid ? "yes" : "no") << ',' << st.designArea << ',' << st.asBuiltArea << ','
          << st.overbreakArea << ',' << st.underbreakArea << ',' << st.maxOverbreakDepth << ',' << st.maxUnderbreakDepth << ',';
        if (st.clearance) s << *st.clearance;
        s << ',' << issue << '\n';
    }
    return s.str();
}

Uuid commitConformance(Project& p, const Uuid& abId, const Uuid& devId, const ConformanceReport& r, const ConformanceOptions& o) {
    using json = nlohmann::json;
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Store conformance analysis");
    AnalysisResult a;
    a.kind = AnalysisKind::DesignConformance;
    const auto* dev = p.model().developments.find(devId);
    a.name = "Conformance " + (dev ? dev->name : std::string("development"));
    a.inputsJson = json{{"asBuilt", abId.toString()}, {"development", devId.toString()}}.dump();
    json params{{"interval", o.interval}, {"orientation", o.orientation == geometry::SectionOrientation::PerpendicularToAxis ? "perpendicular" : "vertical"}};
    if (o.fromChainage) params["from"] = *o.fromChainage;
    if (o.toChainage) params["to"] = *o.toChainage;
    if (!o.clearanceEnvelope.empty()) {
        json env = json::array();
        for (const auto& v : o.clearanceEnvelope) env.push_back({v.x, v.y});
        params["clearanceEnvelope"] = env;
    }
    a.parametersJson = params.dump();
    json summary{{"designVolume", r.designVolume}, {"asBuiltVolume", r.asBuiltVolume}, {"overbreakVolume", r.overbreakVolume},
                 {"underbreakVolume", r.underbreakVolume}, {"overbreakPercent", r.overbreakPercent}, {"validStations", r.validStations},
                 {"stations", r.stations.size()}};
    if (r.minClearance) summary["minClearance"] = *r.minClearance;
    a.summaryJson = summary.dump();
    json rows = json::array();
    for (const auto& s : r.stations)
        rows.push_back({{"chainage", s.chainage}, {"valid", s.valid}, {"designArea", s.designArea}, {"asBuiltArea", s.asBuiltArea},
                        {"overbreakArea", s.overbreakArea}, {"underbreakArea", s.underbreakArea}, {"maxOverbreak", s.maxOverbreakDepth},
                        {"maxUnderbreak", s.maxUnderbreakDepth}, {"clearance", s.clearance ? json(*s.clearance) : json(nullptr)}, {"issue", s.issue}});
    a.payloadJson = rows.dump();
    const Uuid aid = ed.add(a);
    DesignConformanceResult c;
    c.asBuiltId = abId;
    c.developmentId = devId;
    c.stationInterval = o.interval;
    if (!r.stations.empty()) {
        c.chainageFrom = r.stations.front().chainage;
        c.chainageTo = r.stations.back().chainage;
    }
    c.designVolume = r.designVolume;
    c.asBuiltVolume = r.asBuiltVolume;
    c.overbreakVolume = r.overbreakVolume;
    c.underbreakVolume = r.underbreakVolume;
    c.meanOverbreakDepth = r.meanOverbreakDepth;
    c.maxUnderbreakDepth = r.maxUnderbreakDepth;
    c.minClearance = r.minClearance.value_or(-1);
    c.stationCount = static_cast<int>(r.stations.size());
    c.analysisResultId = aid;
    const Uuid id = ed.add(c);
    macro.commit();
    return id;
}

} // namespace openmine::survey
