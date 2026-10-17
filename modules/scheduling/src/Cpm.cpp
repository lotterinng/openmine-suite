// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/Cpm.h"

#include "openmine/scheduling/WorkingTime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <unordered_map>

namespace openmine::scheduling {

namespace {
constexpr double kFloatTolerance = 1e-6; // working days
}

double activityDuration(const ScheduleActivity& a, bool remainingOnly) {
    if (a.kind != ActivityKind::Task) return 0.0;
    const double remainingQty = std::max(0.0, a.quantity - a.actualQuantity);
    if (a.durationFromRate) {
        if (a.ratePerDay <= 0) return 0.0;
        return (remainingOnly ? remainingQty : a.quantity) / a.ratePerDay;
    }
    if (remainingOnly && a.quantity > 0) return a.durationDays * (remainingQty / a.quantity);
    return a.durationDays;
}

ScheduleResult calculateSchedule(const Project& project) {
    const auto& m = project.model();
    ScheduleResult r;

    const WorkCalendar* cal = m.calendars.find(m.info.defaultCalendarId);
    if (!cal) {
        r.errors.push_back("The project has no default calendar");
        return r;
    }
    WorkingTime wt(*cal, m.info.scheduleStart);
    r.projectStartDate = wt.startDate(0);

    const bool reforecast = m.info.statusDate.has_value();
    const double statusOffset = reforecast ? static_cast<double>(wt.workingDaysBefore(*m.info.statusDate)) : 0.0;

    // ---- Collect schedulable (non-summary) activities in a deterministic order.
    std::vector<const ScheduleActivity*> tasks;
    for (const auto& a : m.activities.all())
        if (a.kind != ActivityKind::Summary) tasks.push_back(&a);
    std::stable_sort(tasks.begin(), tasks.end(), [](auto* x, auto* y) { return x->sortOrder < y->sortOrder; });

    std::unordered_map<Uuid, std::vector<const ActivityDependency*>> preds, succs;
    std::unordered_map<Uuid, int> indegree;
    for (auto* a : tasks) indegree[a->id] = 0;
    for (const auto& d : m.dependencies.all()) {
        if (!indegree.count(d.predecessorId) || !indegree.count(d.successorId)) {
            r.errors.push_back("Dependency " + d.id.toString() + " references a missing or summary activity");
            continue;
        }
        preds[d.successorId].push_back(&d);
        succs[d.predecessorId].push_back(&d);
        ++indegree[d.successorId];
    }
    for (auto* a : tasks)
        if (a->calendarId && a->calendarId != m.info.defaultCalendarId) {
            r.warnings.push_back("'" + a->name + "' has its own calendar; v0.1 schedules all activities on the project calendar");
        }
    if (!r.errors.empty()) return r;

    // ---- Topological order (Kahn), tie-broken by sortOrder for stable output.
    std::unordered_map<Uuid, int> order;
    for (std::size_t i = 0; i < tasks.size(); ++i) order[tasks[i]->id] = static_cast<int>(i);
    auto cmp = [&](const Uuid& x, const Uuid& y) { return order[x] > order[y]; };
    std::priority_queue<Uuid, std::vector<Uuid>, decltype(cmp)> ready(cmp);
    for (auto* a : tasks)
        if (indegree[a->id] == 0) ready.push(a->id);
    std::vector<Uuid> topo;
    while (!ready.empty()) {
        Uuid u = ready.top();
        ready.pop();
        topo.push_back(u);
        for (auto* d : succs[u])
            if (--indegree[d->successorId] == 0) ready.push(d->successorId);
    }
    if (topo.size() != tasks.size()) {
        std::string names;
        for (auto* a : tasks)
            if (indegree[a->id] > 0) names += (names.empty() ? "'" : ", '") + a->name + "'";
        r.errors.push_back("Circular logic detected involving " + names);
        return r;
    }

    // ---- Forward pass.
    auto& T = r.timings;
    for (const Uuid& id : topo) {
        const auto& a = *m.activities.find(id);
        auto& t = T[id];
        const double remainingQty = std::max(0.0, a.quantity - a.actualQuantity);
        t.remainingQuantity = remainingQty;
        t.percentComplete = a.quantity > 0 ? std::min(100.0, a.actualQuantity / a.quantity * 100.0) : 0.0;
        t.duration = activityDuration(a, reforecast);

        double es = 0.0;
        if (a.startNoEarlierThan) es = std::max(es, static_cast<double>(wt.workingDaysBefore(*a.startNoEarlierThan)));
        if (reforecast && t.duration > 0) es = std::max(es, statusOffset);
        for (auto* d : preds[id]) {
            const auto& p = T[d->predecessorId];
            switch (d->type) {
            case DependencyType::FinishToStart: es = std::max(es, p.ef + d->lagDays); break;
            case DependencyType::StartToStart: es = std::max(es, p.es + d->lagDays); break;
            case DependencyType::FinishToFinish: es = std::max(es, p.ef + d->lagDays - t.duration); break;
            case DependencyType::StartToFinish: es = std::max(es, p.es + d->lagDays - t.duration); break;
            }
        }
        t.es = std::max(0.0, es);
        t.ef = t.es + t.duration;
        r.projectFinish = std::max(r.projectFinish, t.ef);
    }

    // ---- Backward pass.
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
        auto& t = T[*it];
        double lf = r.projectFinish;
        for (auto* d : succs[*it]) {
            const auto& s = T[d->successorId];
            switch (d->type) {
            case DependencyType::FinishToStart: lf = std::min(lf, s.ls - d->lagDays); break;
            case DependencyType::StartToStart: lf = std::min(lf, s.ls - d->lagDays + t.duration); break;
            case DependencyType::FinishToFinish: lf = std::min(lf, s.lf - d->lagDays); break;
            case DependencyType::StartToFinish: lf = std::min(lf, s.lf - d->lagDays + t.duration); break;
            }
        }
        t.lf = lf;
        t.ls = lf - t.duration;
        t.totalFloat = t.ls - t.es;
        t.critical = t.totalFloat <= kFloatTolerance;

        double ff = r.projectFinish - t.ef;
        for (auto* d : succs[*it]) {
            const auto& s = T[d->successorId];
            double slack = 0;
            switch (d->type) {
            case DependencyType::FinishToStart: slack = s.es - d->lagDays - t.ef; break;
            case DependencyType::StartToStart: slack = s.es - d->lagDays - t.es; break;
            case DependencyType::FinishToFinish: slack = s.ef - d->lagDays - t.ef; break;
            case DependencyType::StartToFinish: slack = s.ef - d->lagDays - t.es; break;
            }
            ff = std::min(ff, slack);
        }
        t.freeFloat = std::max(0.0, ff);
        if (std::abs(t.totalFloat) < kFloatTolerance) t.totalFloat = 0.0;
        if (t.freeFloat < kFloatTolerance) t.freeFloat = 0.0;
    }

    // ---- Summary (WBS) roll-up, children before parents.
    std::vector<const ScheduleActivity*> summaries;
    for (const auto& a : m.activities.all())
        if (a.kind == ActivityKind::Summary) summaries.push_back(&a);
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
        double qty = 0, act = 0;
        for (const auto& c : m.activities.all()) {
            if (c.parentId != s->id) continue;
            auto it = T.find(c.id);
            if (it == T.end()) continue;
            const auto& ct = it->second;
            if (!any) {
                t = ct;
                any = true;
            } else {
                t.es = std::min(t.es, ct.es);
                t.ef = std::max(t.ef, ct.ef);
                t.ls = std::min(t.ls, ct.ls);
                t.lf = std::max(t.lf, ct.lf);
                t.totalFloat = std::min(t.totalFloat, ct.totalFloat);
                t.freeFloat = std::min(t.freeFloat, ct.freeFloat);
            }
            qty += c.quantity;
            act += c.actualQuantity;
        }
        if (!any) {
            t.es = t.ef = t.ls = t.lf = 0;
            r.warnings.push_back("Summary '" + s->name + "' has no scheduled children");
        }
        t.duration = t.ef - t.es;
        t.critical = any && t.totalFloat <= kFloatTolerance;
        t.remainingQuantity = std::max(0.0, qty - act);
        t.percentComplete = qty > 0 ? act / qty * 100.0 : 0.0;
        T[s->id] = t;
    }

    for (auto& [id, t] : T) {
        t.startDate = wt.startDate(t.es);
        t.finishDate = wt.finishDate(t.es, t.ef);
    }
    r.projectFinishDate = wt.finishDate(0, r.projectFinish);
    r.ok = true;
    return r;
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
        b.activities.push_back({a.id, a.name, it->second.startDate, it->second.finishDate, it->second.duration, a.quantity});
    }
    return b;
}

} // namespace openmine::scheduling
