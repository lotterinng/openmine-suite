// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/Levelling.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <tuple>

namespace openmine::scheduling {

namespace {
constexpr double kEps = 1e-6;
constexpr double kHorizonMinutes = 50.0 * 365 * 1440;

struct Booking {
    double start, end, units;
};

double peakUsage(const std::vector<Booking>& b, double s, double e) {
    // Max concurrent usage over [s, e) by sweeping booking boundaries inside the interval.
    std::vector<std::pair<double, double>> ev;
    for (const auto& x : b)
        if (x.start < e - kEps && x.end > s + kEps) {
            ev.emplace_back(std::max(x.start, s), x.units);
            ev.emplace_back(std::min(x.end, e), -x.units);
        }
    std::sort(ev.begin(), ev.end(), [](auto& a, auto& c) { return a.first < c.first || (a.first == c.first && a.second < c.second); });
    double cur = 0, peak = 0;
    for (const auto& [t, u] : ev) {
        cur += u;
        peak = std::max(peak, cur);
    }
    return peak;
}
} // namespace

std::vector<ResourceProfile> resourceProfiles(const ScheduleInput& in, const ScheduleResult& r) {
    std::vector<ResourceProfile> out;
    for (const auto& res : in.resources) {
        ResourceProfile p;
        p.resourceId = res.id;
        p.name = res.name;
        p.capacity = res.capacity;
        std::vector<std::pair<double, double>> ev;
        for (const auto& ia : in.activities)
            for (const auto& [rid, units] : ia.resources) {
                if (rid != res.id) continue;
                auto it = r.timings.find(ia.a.id);
                if (it == r.timings.end() || it->second.ef <= it->second.es + kEps) continue;
                ev.emplace_back(it->second.es, units);
                ev.emplace_back(it->second.ef, -units);
            }
        std::sort(ev.begin(), ev.end());
        double cur = 0, last = 0;
        for (std::size_t i = 0; i < ev.size(); ++i) {
            if (i > 0 && ev[i].first > last + kEps && cur > kEps) p.steps.push_back({last, ev[i].first, cur});
            cur += ev[i].second;
            last = ev[i].first;
        }
        for (const auto& s : p.steps) {
            p.peak = std::max(p.peak, s.units);
            if (s.units > p.capacity + 1e-9) {
                p.overallocated = true;
                p.overallocatedSpans.emplace_back(s.start, s.end);
            }
        }
        out.push_back(std::move(p));
    }
    return out;
}

ScheduleResult levelResources(const ScheduleInput& in, const ScheduleResult& cpm, LevellingReport* report) {
    ScheduleResult r = cpm;
    r.levelling = LevellingMode::Heuristic;
    if (!cpm.ok) return r;
    const std::size_t n = in.activities.size();
    std::vector<std::vector<const ActivityDependency*>> preds(n);
    std::vector<int> remainingPreds(n, 0);
    for (const auto& d : in.dependencies) {
        preds[in.index.at(d.successorId)].push_back(&d);
        ++remainingPreds[in.index.at(d.successorId)];
    }
    std::vector<std::vector<std::size_t>> succs(n);
    for (const auto& d : in.dependencies) succs[in.index.at(d.predecessorId)].push_back(in.index.at(d.successorId));

    std::vector<std::vector<Booking>> bookings(in.resources.size());
    std::map<std::string, std::vector<Booking>> areaBookings;
    struct Times {
        double es = 0, ef = 0, rs = 0, d = 0;
    };
    std::vector<Times> t(n);
    std::vector<char> done(n, 0);

    auto key = [&](std::size_t i) {
        const auto& ia = in.activities[i];
        const auto& ct = cpm.timings.at(ia.a.id);
        return std::make_tuple(-ia.a.priority, ct.totalFloatHours, ct.es, i);
    };
    std::set<std::tuple<int, double, double, std::size_t>> eligible;
    for (std::size_t i = 0; i < n; ++i)
        if (remainingPreds[i] == 0) eligible.insert(key(i));

    while (!eligible.empty()) {
        const std::size_t i = std::get<3>(*eligible.begin());
        eligible.erase(eligible.begin());
        const auto& ia = in.activities[i];
        const auto& cal = *ia.cal;
        const auto& ct = cpm.timings.at(ia.a.id);
        auto& x = t[i];
        x.d = ct.durationHours * 60.0;
        if (ia.completed && in.statusDate) {
            x.es = ct.es;
            x.ef = ct.ef;
            x.rs = ct.ef;
        } else {
            double logic = cal.nextWorkStart(in.projectStart);
            for (const auto* dep : preds[i]) {
                const auto& y = t[in.index.at(dep->predecessorId)];
                const double lag = dep->lagHours * 60.0;
                switch (dep->type) {
                case DependencyType::FinishToStart: logic = std::max(logic, cal.shift(y.ef, lag)); break;
                case DependencyType::StartToStart: logic = std::max(logic, cal.shift(y.es, lag)); break;
                case DependencyType::FinishToFinish: logic = std::max(logic, cal.subtractWork(cal.shift(y.ef, lag), x.d)); break;
                case DependencyType::StartToFinish: logic = std::max(logic, cal.subtractWork(cal.shift(y.es, lag), x.d)); break;
                }
            }
            if (ia.a.constraint == ConstraintType::StartNoEarlierThan && ia.a.constraintDate) logic = std::max(logic, toMinutes(*ia.a.constraintDate));
            if (ia.a.constraint == ConstraintType::MustStartOn && ia.a.constraintDate) logic = toMinutes(*ia.a.constraintDate);
            if (in.statusDate) logic = std::max(logic, *in.statusDate);
            double start = x.d > 0 ? cal.nextWorkStart(logic) : logic;
            const bool needsCheck = x.d > 0 && (!ia.resources.empty() || (!ia.a.workArea.empty() && in.areaLimits.count(ia.a.workArea)));
            int guard = 0;
            while (needsCheck) {
                if (start - in.projectStart > kHorizonMinutes || ++guard > 100000) {
                    r.ok = false;
                    r.errors.push_back("Resource levelling could not place '" + ia.a.name + "' within 50 years (insufficient resource availability)");
                    return r;
                }
                const double finish = cal.addWork(start, x.d);
                double nextCandidate = 1e300;
                bool feasible = true;
                for (const auto& [rid, units] : ia.resources) {
                    const auto ri = in.resourceIndex.at(rid);
                    const auto& bk = bookings[ri];
                    if (peakUsage(bk, start, finish) + units > in.resources[ri].capacity + 1e-9) {
                        feasible = false;
                        for (const auto& b : bk)
                            if (b.end > start + kEps && b.start < finish - kEps) nextCandidate = std::min(nextCandidate, b.end);
                    }
                }
                if (!ia.a.workArea.empty()) {
                    auto lim = in.areaLimits.find(ia.a.workArea);
                    if (lim != in.areaLimits.end()) {
                        const auto& bk = areaBookings[ia.a.workArea];
                        if (peakUsage(bk, start, finish) + 1 > lim->second + 1e-9) {
                            feasible = false;
                            for (const auto& b : bk)
                                if (b.end > start + kEps && b.start < finish - kEps) nextCandidate = std::min(nextCandidate, b.end);
                        }
                    }
                }
                if (feasible) break;
                start = cal.nextWorkStart(std::max(nextCandidate, start + 1.0));
            }
            if (ia.started && in.statusDate) {
                x.rs = start;
                x.es = ct.es; // actual start
            } else {
                x.es = x.rs = start;
            }
            x.ef = cal.addWork(start, x.d);
        }
        done[i] = 1;
        if (x.ef > x.rs + kEps) {
            for (const auto& [rid, units] : ia.resources) bookings[in.resourceIndex.at(rid)].push_back({x.rs, x.ef, units});
            if (!ia.a.workArea.empty()) areaBookings[ia.a.workArea].push_back({x.rs, x.ef, 1.0});
        }
        for (auto s : succs[i])
            if (--remainingPreds[s] == 0) eligible.insert(key(s));
    }

    LevellingReport rep;
    for (std::size_t i = 0; i < n; ++i) {
        auto& tm = r.timings[in.activities[i].a.id];
        const double delay = (t[i].es - tm.es) / 60.0;
        if (delay > 1e-6) {
            ++rep.delayedActivities;
            rep.totalDelayHours += delay;
            rep.delays.emplace_back(in.activities[i].a.id, delay);
        }
        tm.es = t[i].es;
        tm.ef = t[i].ef;
        tm.remainingStart = t[i].rs;
    }
    recomputeFloats(in, r);
    rep.makespanIncreaseHours = (r.projectFinish - cpm.projectFinish) / 60.0;
    char buf[200];
    std::snprintf(buf, sizeof buf, "Heuristic levelling: %d activities delayed, makespan +%.1f h", rep.delayedActivities,
                  rep.makespanIncreaseHours);
    r.levellingSummary = buf;
    if (report) *report = rep;
    return r;
}

} // namespace openmine::scheduling
