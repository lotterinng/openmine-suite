// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/Cpm.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>

namespace openmine::scheduling {

namespace {
constexpr double kFloatTolMinutes = 1e-3;

/// Topological order (Kahn) tie-broken by input order; returns false and the names on a cycle.
bool topoOrder(const ScheduleInput& in, std::vector<std::size_t>& order, std::string& cycleNames) {
    const std::size_t n = in.activities.size();
    std::vector<int> indeg(n, 0);
    std::vector<std::vector<std::size_t>> succ(n);
    for (const auto& d : in.dependencies) {
        const auto p = in.index.at(d.predecessorId), s = in.index.at(d.successorId);
        succ[p].push_back(s);
        ++indeg[s];
    }
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t i = 0; i < n; ++i)
        if (indeg[i] == 0) ready.push(i);
    while (!ready.empty()) {
        const auto u = ready.top();
        ready.pop();
        order.push_back(u);
        for (auto v : succ[u])
            if (--indeg[v] == 0) ready.push(v);
    }
    if (order.size() == n) return true;
    for (std::size_t i = 0; i < n; ++i)
        if (indeg[i] > 0) cycleNames += (cycleNames.empty() ? "'" : ", '") + in.activities[i].a.name + "'";
    return false;
}

struct Work {
    double es = 0, ef = 0, ls = 0, lf = 0, rs = 0, lrs = 0; ///< rs = remaining start, lrs = late remaining start
    double d = 0;                                            ///< minutes used (remaining when reforecasting)
};

void rollUpSummaries(const ScheduleInput& in, ScheduleResult& r) {
    const auto& m = *in.model;
    std::vector<const ScheduleActivity*> summaries = in.summaries;
    auto depth = [&](const ScheduleActivity* a) {
        int dpt = 0;
        for (Uuid p = a->parentId; p && dpt < 1000; ++dpt) {
            auto* x = m.activities.find(p);
            p = x ? x->parentId : Uuid{};
        }
        return dpt;
    };
    std::stable_sort(summaries.begin(), summaries.end(), [&](auto* x, auto* y) { return depth(x) > depth(y); });
    for (auto* s : summaries) {
        ActivityTiming t;
        bool any = false;
        double qty = 0, act = 0, durTotal = 0, durDone = 0;
        bool sameUnit = true;
        std::string unit;
        for (const auto& c : m.activities.all()) {
            if (c.parentId != s->id) continue;
            auto it = r.timings.find(c.id);
            if (it == r.timings.end()) continue;
            const auto& ct = it->second;
            if (!any) {
                t = ct;
                any = true;
            } else {
                t.es = std::min(t.es, ct.es);
                t.ef = std::max(t.ef, ct.ef);
                t.ls = std::min(t.ls, ct.ls);
                t.lf = std::max(t.lf, ct.lf);
                t.totalFloatHours = std::min(t.totalFloatHours, ct.totalFloatHours);
                t.freeFloatHours = std::min(t.freeFloatHours, ct.freeFloatHours);
                t.started = t.started || ct.started;
                t.completed = t.completed && ct.completed;
            }
            if (c.kind != ActivityKind::Summary) {
                if (unit.empty()) unit = c.quantityUnit;
                sameUnit = sameUnit && (c.quantityUnit == unit);
            }
            qty += ct.remainingQuantity + ct.actualQuantity;
            act += ct.actualQuantity;
            durTotal += ct.totalHours;
            durDone += ct.totalHours * ct.percentComplete / 100.0;
        }
        if (!any) {
            t.es = t.ef = t.ls = t.lf = r.projectStart;
            r.warnings.push_back("Summary '" + s->name + "' has no scheduled children");
        }
        t.durationHours = (t.ef - t.es) / 60.0;
        t.totalHours = durTotal;
        t.critical = any && t.totalFloatHours * 60.0 <= kFloatTolMinutes;
        t.actualQuantity = sameUnit ? act : 0;
        t.remainingQuantity = sameUnit ? std::max(0.0, qty - act) : 0;
        t.percentComplete = sameUnit && qty > 0 ? act / qty * 100.0 : (durTotal > 0 ? durDone / durTotal * 100.0 : 0.0);
        t.durationBasis = "Summary";
        t.startDate = startDisplayDate(t.es);
        t.finishDate = finishDisplayDate(t.ef, t.es);
        r.timings[s->id] = t;
    }
}

/// Late dates and floats given early dates in `w`.
void backwardPass(const ScheduleInput& in, const std::vector<std::size_t>& topo, std::vector<Work>& w, double projectFinish,
                  ScheduleResult& r) {
    const std::size_t n = in.activities.size();
    std::vector<std::vector<const ActivityDependency*>> succ(n);
    for (const auto& d : in.dependencies) succ[in.index.at(d.predecessorId)].push_back(&d);
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
        const auto i = *it;
        const auto& ia = in.activities[i];
        const auto& cal = *ia.cal;
        auto& x = w[i];
        if (ia.completed && in.statusDate) {
            x.ls = x.es;
            x.lf = x.ef;
            x.lrs = x.ef;
            continue;
        }
        double lf = projectFinish;
        if (ia.a.constraint == ConstraintType::FinishNoLaterThan && ia.a.constraintDate) lf = std::min(lf, toMinutes(*ia.a.constraintDate));
        if (ia.a.constraint == ConstraintType::MustStartOn && ia.a.constraintDate)
            lf = std::min(lf, cal.addWork(toMinutes(*ia.a.constraintDate), x.d));
        for (const auto* dep : succ[i]) {
            const auto s = in.index.at(dep->successorId);
            const auto& scal = *in.activities[s].cal;
            const auto& y = w[s];
            const double lag = dep->lagHours * 60.0;
            switch (dep->type) {
            case DependencyType::FinishToStart: lf = std::min(lf, scal.shift(y.lrs, -lag)); break;
            case DependencyType::StartToStart: lf = std::min(lf, cal.addWork(scal.shift(y.lrs, -lag), x.d)); break;
            case DependencyType::FinishToFinish: lf = std::min(lf, scal.shift(y.lf, -lag)); break;
            case DependencyType::StartToFinish: lf = std::min(lf, cal.addWork(scal.shift(y.lf, -lag), x.d)); break;
            }
        }
        if (x.d > 0) lf = cal.prevWorkEnd(lf);
        x.lf = lf;
        x.lrs = cal.subtractWork(lf, x.d);
        x.ls = ia.started && in.statusDate ? x.es : x.lrs;
    }
    // Floats.
    for (std::size_t i = 0; i < n; ++i) {
        const auto& ia = in.activities[i];
        const auto& cal = *ia.cal;
        const auto& x = w[i];
        auto& t = r.timings[ia.a.id];
        t.es = x.es;
        t.ef = x.ef;
        t.ls = x.ls;
        t.lf = x.lf;
        t.remainingStart = x.rs;
        if (ia.completed && in.statusDate) {
            t.totalFloatHours = t.freeFloatHours = 0;
            t.critical = false;
            continue;
        }
        const double tf = cal.workBetween(x.rs, x.lrs);
        t.totalFloatHours = std::abs(tf) < kFloatTolMinutes ? 0.0 : tf / 60.0;
        t.critical = tf <= kFloatTolMinutes;
        double ff = cal.workBetween(x.ef, projectFinish);
        for (const auto* dep : succ[i]) {
            const auto s = in.index.at(dep->successorId);
            const auto& scal = *in.activities[s].cal;
            const auto& y = w[s];
            const double lag = dep->lagHours * 60.0;
            double slack = 0;
            switch (dep->type) {
            case DependencyType::FinishToStart: slack = cal.workBetween(x.ef, scal.shift(y.rs, -lag)); break;
            case DependencyType::StartToStart: slack = cal.workBetween(x.rs, scal.shift(y.rs, -lag)); break;
            case DependencyType::FinishToFinish: slack = cal.workBetween(x.ef, scal.shift(y.ef, -lag)); break;
            case DependencyType::StartToFinish: slack = cal.workBetween(x.rs, scal.shift(y.ef, -lag)); break;
            }
            ff = std::min(ff, slack);
        }
        t.freeFloatHours = ff < kFloatTolMinutes ? 0.0 : ff / 60.0;
    }
}
} // namespace

ScheduleResult calculateSchedule(const ScheduleInput& in) {
    ScheduleResult r;
    r.errors = in.errors;
    r.warnings = in.warnings;
    r.projectStart = in.projectStart;
    if (!r.errors.empty()) return r;
    if (!in.model->calendars.find(in.model->info.defaultCalendarId)) {
        r.errors.push_back("The project has no default calendar");
        return r;
    }

    std::vector<std::size_t> topo;
    std::string cycle;
    if (!topoOrder(in, topo, cycle)) {
        r.errors.push_back("Circular logic detected involving " + cycle);
        return r;
    }
    const std::size_t n = in.activities.size();
    std::vector<std::vector<const ActivityDependency*>> preds(n);
    for (const auto& d : in.dependencies) preds[in.index.at(d.successorId)].push_back(&d);

    std::vector<Work> w(n);
    double projectFinish = in.projectStart;
    try {
        for (auto i : topo) {
            const auto& ia = in.activities[i];
            const auto& cal = *ia.cal;
            auto& x = w[i];
            const bool reforecast = in.statusDate.has_value();
            x.d = reforecast ? ia.remainingMinutes : ia.totalMinutes;

            if (reforecast && ia.completed) {
                x.ef = ia.actualFinish ? *ia.actualFinish : std::min(*in.statusDate, cal.addWork(ia.actualStart.value_or(in.projectStart), ia.totalMinutes));
                x.es = ia.actualStart ? *ia.actualStart : cal.subtractWork(x.ef, ia.totalMinutes);
                x.rs = x.ef;
                x.d = 0;
                projectFinish = std::max(projectFinish, x.ef);
                continue;
            }

            // Logic-driven earliest start of the (remaining) work.
            double logic = cal.nextWorkStart(in.projectStart);
            for (const auto* dep : preds[i]) {
                const auto& y = w[in.index.at(dep->predecessorId)];
                const double lag = dep->lagHours * 60.0;
                switch (dep->type) {
                case DependencyType::FinishToStart: logic = std::max(logic, cal.shift(y.ef, lag)); break;
                case DependencyType::StartToStart: logic = std::max(logic, cal.shift(y.es, lag)); break;
                case DependencyType::FinishToFinish: logic = std::max(logic, cal.subtractWork(cal.shift(y.ef, lag), x.d)); break;
                case DependencyType::StartToFinish: logic = std::max(logic, cal.subtractWork(cal.shift(y.es, lag), x.d)); break;
                }
            }
            if (ia.a.constraint == ConstraintType::StartNoEarlierThan && ia.a.constraintDate)
                logic = std::max(logic, toMinutes(*ia.a.constraintDate));
            if (ia.a.constraint == ConstraintType::MustStartOn && ia.a.constraintDate) {
                const double mso = toMinutes(*ia.a.constraintDate);
                if (logic > mso + kFloatTolMinutes)
                    r.warnings.push_back("'" + ia.a.name + "' must start on " + ia.a.constraintDate->toString() +
                                         " but its predecessors finish later (negative float)");
                logic = mso;
            }

            if (reforecast && ia.started) {
                x.rs = std::max(*in.statusDate, logic); // retained logic for the remaining work
                if (x.d > 0) x.rs = cal.nextWorkStart(x.rs);
                x.ef = cal.addWork(x.rs, x.d);
                x.es = ia.actualStart ? *ia.actualStart : std::min(logic, *in.statusDate);
            } else {
                double es = logic;
                if (reforecast) es = std::max(es, *in.statusDate);
                if (x.d > 0) es = cal.nextWorkStart(es);
                x.es = x.rs = es;
                x.ef = cal.addWork(es, x.d);
            }
            projectFinish = std::max(projectFinish, x.ef);
        }
        backwardPass(in, topo, w, projectFinish, r);
    } catch (const CalendarError& e) {
        r.errors.push_back(e.what());
        return r;
    }

    for (const auto& ia : in.activities) {
        auto& t = r.timings[ia.a.id];
        t.durationHours = w[in.index.at(ia.a.id)].d / 60.0;
        t.totalHours = ia.totalMinutes / 60.0;
        t.actualQuantity = ia.actualQuantity;
        t.remainingQuantity = std::max(0.0, ia.totalQuantity - ia.actualQuantity);
        t.percentComplete = ia.percentComplete;
        t.started = ia.started;
        t.completed = ia.completed;
        t.durationBasis = ia.durationBasis;
        t.startDate = startDisplayDate(t.es);
        t.finishDate = finishDisplayDate(t.ef, t.es);
        if (ia.a.constraint == ConstraintType::FinishNoLaterThan && ia.a.constraintDate && t.ef > toMinutes(*ia.a.constraintDate) + kFloatTolMinutes)
            r.warnings.push_back("'" + ia.a.name + "' finishes after its deadline " + ia.a.constraintDate->toString());
    }
    r.projectFinish = projectFinish;
    rollUpSummaries(in, r);
    r.projectStartDate = startDisplayDate(in.projectStart);
    r.projectFinishDate = finishDisplayDate(projectFinish, in.projectStart);
    r.ok = true;
    return r;
}

ScheduleResult calculateSchedule(const Project& project) { return calculateSchedule(buildScheduleInput(project)); }

void recomputeFloats(const ScheduleInput& in, ScheduleResult& r) {
    std::vector<std::size_t> topo;
    std::string cycle;
    if (!topoOrder(in, topo, cycle)) return;
    std::vector<Work> w(in.activities.size());
    double finish = in.projectStart;
    for (std::size_t i = 0; i < in.activities.size(); ++i) {
        const auto& t = r.timings.at(in.activities[i].a.id);
        w[i].es = t.es;
        w[i].ef = t.ef;
        w[i].rs = t.remainingStart;
        w[i].d = t.durationHours * 60.0;
        finish = std::max(finish, t.ef);
    }
    r.projectFinish = finish;
    backwardPass(in, topo, w, finish, r);
    for (const auto& ia : in.activities) {
        auto& t = r.timings[ia.a.id];
        t.startDate = startDisplayDate(t.es);
        t.finishDate = finishDisplayDate(t.ef, t.es);
    }
    for (auto* s : in.summaries) r.timings.erase(s->id);
    rollUpSummaries(in, r);
    r.projectFinishDate = finishDisplayDate(finish, in.projectStart);
}

ScheduleBaseline makeBaseline(const Project& project, const ScheduleResult& result, const std::string& name) {
    ScheduleBaseline b;
    b.id = Uuid::generate();
    b.name = name;
    b.capturedAt = utcTimestampNow();
    b.approved = true;
    for (const auto& a : project.model().activities.all()) {
        auto it = result.timings.find(a.id);
        if (it == result.timings.end()) continue;
        b.activities.push_back({a.id, a.name, it->second.start(), it->second.finish(), it->second.totalHours, a.quantity});
    }
    return b;
}

std::vector<Variance> compareToBaseline(const Project& project, const ScheduleResult& f, const ScheduleBaseline& bl) {
    std::vector<Variance> out;
    for (const auto& a : project.model().activities.all()) {
        auto it = f.timings.find(a.id);
        if (it == f.timings.end()) continue;
        Variance v;
        v.activityId = a.id;
        v.name = a.name;
        const BaselineActivity* ba = nullptr;
        for (const auto& x : bl.activities)
            if (x.activityId == a.id) ba = &x;
        if (!ba) {
            v.inBaseline = false;
            out.push_back(v);
            continue;
        }
        v.startVarianceHours = (it->second.es - toMinutes(ba->start)) / 60.0;
        v.finishVarianceHours = (it->second.ef - toMinutes(ba->finish)) / 60.0;
        v.durationVarianceHours = it->second.totalHours - ba->durationHours;
        v.quantityVariance = a.quantity - ba->quantity;
        out.push_back(v);
    }
    return out;
}

} // namespace openmine::scheduling
