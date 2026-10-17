// SPDX-License-Identifier: Apache-2.0
#include "openmine/planning/FourD.h"

#include "openmine/geometry/Polyline.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/mine_design/Development.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace openmine::planning {

namespace {
// Viridis colour map at 9 control points.
constexpr std::array<std::uint32_t, 9> kViridis = {0x440154, 0x472D7B, 0x3B528B, 0x2C728E, 0x21918C, 0x28AE80, 0x5EC962, 0xADDC30, 0xFDE725};
constexpr std::array<std::uint32_t, 10> kCategorical = {0x4E79A7, 0xF28E2B, 0xE15759, 0x76B7B2, 0x59A14F, 0xEDC948, 0xB07AA1, 0xFF9DA7, 0x9C755F, 0xBAB0AC};

std::uint32_t mix(std::uint32_t a, std::uint32_t b, double t) {
    auto ch = [&](int s) {
        const double x = ((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t;
        return static_cast<std::uint32_t>(std::lround(x)) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

/// Planned quantity fraction of one activity at time t (work-time interpolation, actuals first).
double activityFraction(const scheduling::InputActivity& ia, const scheduling::ActivityTiming& t, double time) {
    if (ia.completed) return 1.0;
    const double total = std::max(ia.totalMinutes, 1e-12);
    const double doneBefore = ia.started ? std::max(0.0, ia.totalMinutes - ia.remainingMinutes) : 0.0;
    const double start = t.remainingStart, finish = t.ef;
    double work = 0;
    if (time >= finish) work = ia.started ? ia.remainingMinutes : ia.totalMinutes;
    else if (time > start) work = ia.cal->workBetween(start, time);
    return std::clamp((doneBefore + work) / total, 0.0, 1.0);
}
} // namespace

const char* toString(ObjectState s) {
    switch (s) {
    case ObjectState::NotScheduled: return "Not scheduled";
    case ObjectState::Future: return "Future";
    case ObjectState::InProgress: return "In progress";
    case ObjectState::Complete: return "Complete";
    }
    return "?";
}

std::uint32_t timeColor(double t, double t0, double t1) {
    const double u = t1 > t0 ? std::clamp((t - t0) / (t1 - t0), 0.0, 1.0) : 0.0;
    const double x = u * (kViridis.size() - 1);
    const auto i = std::min(static_cast<std::size_t>(x), kViridis.size() - 2);
    return mix(kViridis[i], kViridis[i + 1], x - static_cast<double>(i));
}

std::uint32_t categoricalColor(const Uuid& id) {
    const auto s = id.toString();
    std::uint32_t h = 2166136261u;
    for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return kCategorical[h % kCategorical.size()];
}

PlanSnapshot snapshot(const Project& p, const scheduling::ScheduleInput& in, const scheduling::ScheduleResult& r, double time, ColorMode mode,
                      const ScheduleBaseline* baseline) {
    const auto& m = p.model();
    PlanSnapshot s;
    s.time = time;
    auto add = [&](const Uuid& id, const std::string& type, const std::string& name) {
        ObjectSnapshot o;
        o.designId = id;
        o.designType = type;
        o.name = name;
        o.activities = m.activitiesForDesign(id);
        double qTotal = 0, qPlanned = 0, qActual = 0, tTotal = 0, tPlanned = 0;
        bool anyActive = false;
        for (const auto& aid : o.activities) {
            const auto ii = in.index.find(aid);
            const auto ti = r.timings.find(aid);
            if (ii == in.index.end() || ti == r.timings.end()) continue;
            const auto& ia = in.activities[ii->second];
            const auto& t = ti->second;
            const double f = activityFraction(ia, t, time);
            const double q = ia.totalQuantity > 0 ? ia.totalQuantity : 0;
            qTotal += q;
            qPlanned += q * f;
            qActual += std::min(ia.actualQuantity, q);
            tTotal += ia.totalMinutes;
            tPlanned += ia.totalMinutes * f;
            o.plannedStart = o.plannedStart ? std::min(*o.plannedStart, t.es) : t.es;
            o.plannedFinish = o.plannedFinish ? std::max(*o.plannedFinish, t.ef) : t.ef;
            if (time >= t.remainingStart && time < t.ef && !ia.completed) {
                anyActive = true;
                for (const auto& [rid, units] : ia.resources) {
                    o.activeResources.push_back(rid);
                    s.resourceAssignments[rid].push_back(id);
                }
            }
        }
        if (o.activities.empty() || !o.plannedStart) o.state = ObjectState::NotScheduled;
        else {
            o.plannedFraction = qTotal > 0 ? qPlanned / qTotal : (tTotal > 0 ? tPlanned / tTotal : 0);
            o.actualFraction = qTotal > 0 ? qActual / qTotal : 0;
            o.state = o.plannedFraction >= 1 - 1e-12 ? ObjectState::Complete : (anyActive || o.plannedFraction > 0 ? ObjectState::InProgress : ObjectState::Future);
            if (baseline) {
                // Baseline progress (work-time interpolation on the activity calendar).
                auto baseFraction = [&](double at) {
                    double qb = 0, qt = 0;
                    for (const auto& aid : o.activities) {
                        const auto ii = in.index.find(aid);
                        if (ii == in.index.end()) continue;
                        const auto& ia = in.activities[ii->second];
                        const double w = ia.totalQuantity > 0 ? ia.totalQuantity : ia.totalMinutes;
                        qt += w;
                        for (const auto& b : baseline->activities) {
                            if (b.activityId != aid) continue;
                            const double bs = scheduling::toMinutes(b.start), bf = scheduling::toMinutes(b.finish);
                            const double whole = ia.cal->workBetween(bs, bf);
                            const double f = at >= bf ? 1.0 : (at <= bs ? 0.0 : (whole > 0 ? ia.cal->workBetween(bs, at) / whole : 1.0));
                            qb += w * std::clamp(f, 0.0, 1.0);
                        }
                    }
                    return qt > 0 ? qb / qt : 0.0;
                };
                o.baselineFraction = baseFraction(time);
                if (in.statusDate) o.behindPlan = o.actualFraction + 1e-9 < baseFraction(*in.statusDate);
            }
        }
        switch (mode) {
        case ColorMode::State:
            o.color = o.state == ObjectState::Complete ? 0x6A994E : o.state == ObjectState::InProgress ? 0xF4A261 : o.state == ObjectState::Future ? 0x8DA9C4 : 0x9E9E9E;
            break;
        case ColorMode::StartPeriod: o.color = o.plannedStart ? timeColor(*o.plannedStart, r.projectStart, r.projectFinish) : 0x9E9E9E; break;
        case ColorMode::Resource: o.color = o.activeResources.empty() ? 0xBDBDBD : categoricalColor(o.activeResources.front()); break;
        case ColorMode::Activity: o.color = o.activities.empty() ? 0xBDBDBD : categoricalColor(o.activities.front()); break;
        }
        return o;
    };
    for (const auto& d : m.developments.all()) {
        auto o = add(d.id, "MineDevelopment", d.headingId.empty() ? d.name : d.headingId);
        if (o.state != ObjectState::NotScheduled) {
            const auto cl = mine_design::centrelinePoints(p, d);
            const double L = geometry::length3d(cl);
            o.plannedFace = d.chainageFrom + o.plannedFraction * L;
            if (o.plannedFraction > 0) o.excavatedCentreline = geometry::slice(cl, 0, o.plannedFraction * L);
            s.plannedMetres += o.plannedFraction * L;
            s.actualMetres += o.actualFraction * L;
        }
        s.objects.push_back(std::move(o));
    }
    for (const auto& st : m.stopes.all()) s.objects.push_back(add(st.id, "Stope", st.name));
    for (const auto& b : m.miningBlocks.all()) s.objects.push_back(add(b.id, "MiningBlock", b.name));
    return s;
}

std::vector<double> playbackFrames(const scheduling::ScheduleResult& r, double step) {
    std::vector<double> f;
    if (!r.ok || !(step > 0)) return f;
    for (double t = r.projectStart; t < r.projectFinish; t += step) f.push_back(t);
    f.push_back(r.projectFinish);
    return f;
}

MeshData excavatedEnvelope(const Project& p, const Uuid& devId, double fraction) {
    const auto* d = p.model().developments.find(devId);
    if (!d) throw ValidationError("Development does not exist");
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (fraction <= 0) return {};
    const auto cl = mine_design::centrelinePoints(p, *d);
    const auto part = geometry::slice(cl, 0, fraction * geometry::length3d(cl));
    if (part.size() < 2 || geometry::length3d(part) < 1e-6) return {};
    return geometry::sweepProfile(part, mine_design::developmentProfile(p, *d));
}

std::vector<ObjectDifference> compareSnapshots(const PlanSnapshot& a, const PlanSnapshot& b) {
    std::map<Uuid, const ObjectSnapshot*> ib;
    for (const auto& o : b.objects) ib[o.designId] = &o;
    std::vector<ObjectDifference> out;
    for (const auto& o : a.objects) {
        const auto it = ib.find(o.designId);
        if (it == ib.end()) continue;
        ObjectDifference d;
        d.designId = o.designId;
        d.name = o.name;
        d.fractionA = o.plannedFraction;
        d.fractionB = it->second->plannedFraction;
        d.finishA = o.plannedFinish;
        d.finishB = it->second->plannedFinish;
        if (d.finishA && d.finishB) d.finishShiftHours = (*d.finishB - *d.finishA) / 60.0;
        if (std::abs(d.fractionA - d.fractionB) > 1e-9 || std::abs(d.finishShiftHours) > 1e-9) out.push_back(d);
    }
    return out;
}

} // namespace openmine::planning
