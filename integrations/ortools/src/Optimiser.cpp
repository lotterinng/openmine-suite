// SPDX-License-Identifier: Apache-2.0
#include "openmine/optimisation/Optimiser.h"

#include "openmine/commands/CommandStack.h"
#include "openmine/project/ProjectEditor.h"

#include <ortools/base/version.h>
#include <ortools/sat/cp_model.h>
#include <ortools/sat/cp_model.pb.h>
#include <ortools/sat/cp_model_solver.h>
#include <ortools/sat/model.h>
#include <ortools/sat/sat_parameters.pb.h>
#include <ortools/util/time_limit.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace openmine::optimisation {

namespace sat = operations_research::sat;
using scheduling::CalendarTime;
using scheduling::InputActivity;
using scheduling::ScheduleInput;
using scheduling::ScheduleResult;

namespace {
struct Act {
    const InputActivity* ia = nullptr;
    std::size_t index = 0;
    bool fixedAtOrigin = false;
    double workMinutes = 0;
    std::vector<std::int64_t> table; ///< elapsed slots by start slot
    sat::IntVar start, dur, end;
    sat::IntervalVar interval;
};
} // namespace

std::string ortoolsVersion() { return operations_research::OrToolsVersionString(); }

OptimisationResult optimise(const ScheduleInput& in, const ScheduleResult& cpm, const OptimiserOptions& o, jobs::Progress& progress) {
    OptimisationResult res;
    if (!cpm.ok) {
        res.status = OptimisationResult::Status::InvalidModel;
        res.statusText = "The CPM calculation failed: " + (cpm.errors.empty() ? std::string("unknown error") : cpm.errors.front());
        return res;
    }
    const double origin = in.statusDate.value_or(in.projectStart);
    res.cpmMakespanHours = (cpm.projectFinish - origin) / 60.0;
    // Slot size: at most ~6000 slots over the horizon (CP-SAT element tables stay small).
    const double span = std::max(60.0, cpm.projectFinish - origin) * o.horizonFactor + 7 * 24 * 60;
    double slotMin = std::max(1.0, o.slotHours * 60.0);
    if (span / slotMin > 6000) {
        slotMin = std::ceil(span / 6000 / 60.0) * 60.0;
        res.notes.push_back("Time resolution coarsened to " + std::to_string(static_cast<int>(slotMin / 60)) + " h to keep the model tractable");
    }
    const auto H = static_cast<std::int64_t>(std::ceil(span / slotMin));
    auto slotTime = [&](std::int64_t s) { return origin + static_cast<double>(s) * slotMin; };
    auto toSlotFloor = [&](double t) { return static_cast<std::int64_t>(std::floor((t - origin) / slotMin + 1e-9)); };

    // Activities to schedule.
    std::vector<Act> acts;
    std::unordered_map<Uuid, std::size_t> ai;
    for (std::size_t i = 0; i < in.activities.size(); ++i) {
        const auto& ia = in.activities[i];
        if (ia.excluded || ia.completed) continue;
        Act a;
        a.ia = &ia;
        a.index = i;
        a.fixedAtOrigin = ia.started;
        a.workMinutes = ia.started ? ia.remainingMinutes : ia.totalMinutes;
        ai[ia.a.id] = acts.size();
        acts.push_back(std::move(a));
    }
    if (acts.empty()) {
        res.status = OptimisationResult::Status::Optimal;
        res.statusText = "Nothing to schedule (all activities complete or excluded)";
        return res;
    }
    // Capacity pre-check (an impossible demand makes every model infeasible).
    for (const auto& a : acts)
        for (const auto& [rid, units] : a.ia->resources) {
            const auto it = in.resourceIndex.find(rid);
            if (it == in.resourceIndex.end()) continue;
            const auto& r = in.resources[it->second];
            if (units > r.capacity + 1e-9) {
                char buf[300];
                std::snprintf(buf, sizeof buf, "'%s' needs %.2f units of '%s' but its capacity is %.2f", a.ia->a.name.c_str(), units, r.name.c_str(), r.capacity);
                res.conflicts.push_back({"Capacity", buf, {a.ia->a.id}});
            }
        }
    if (!res.conflicts.empty()) {
        res.status = OptimisationResult::Status::Infeasible;
        res.statusText = "Infeasible: resource demand exceeds capacity";
        return res;
    }
    // Elapsed-duration tables (shared between activities with the same calendar and work).
    progress.update(0.05, "Building optimisation model");
    std::map<std::pair<const CalendarTime*, std::int64_t>, std::vector<std::int64_t>> tables;
    for (auto& a : acts) {
        const auto key = std::make_pair(a.ia->cal.get(), static_cast<std::int64_t>(std::llround(a.workMinutes * 1000)));
        auto it = tables.find(key);
        if (it == tables.end()) {
            std::vector<std::int64_t> t(static_cast<std::size_t>(H) + 1, 0);
            for (std::int64_t s = 0; s <= H; ++s) {
                if (a.workMinutes <= 0) break;
                const double ts = slotTime(s);
                t[static_cast<std::size_t>(s)] = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil((a.ia->cal->addWork(ts, a.workMinutes) - ts) / slotMin - 1e-9)));
            }
            it = tables.emplace(key, std::move(t)).first;
        }
        a.table = it->second;
        progress.checkCancelled();
    }

    sat::CpModelBuilder cp;
    std::vector<sat::BoolVar> assumptions;
    std::vector<Conflict> assumptionMeaning;
    std::vector<sat::IntVar> ends;
    for (auto& a : acts) {
        const auto& x = a.ia->a;
        std::int64_t lo = 0, hi = H;
        if (a.fixedAtOrigin) hi = 0;
        // SNET as a domain bound (never conflicts with time 0 itself).
        if (x.constraint == ConstraintType::StartNoEarlierThan && x.constraintDate) lo = std::max<std::int64_t>(lo, toSlotFloor(scheduling::toMinutes(*x.constraintDate)));
        if (a.ia->releaseTime) lo = std::max<std::int64_t>(lo, toSlotFloor(*a.ia->releaseTime));
        lo = std::clamp<std::int64_t>(lo, 0, H);
        if (hi < lo) hi = lo;
        a.start = cp.NewIntVar(operations_research::Domain(lo, hi)).WithName("s_" + x.name);
        const auto [mn, mx] = std::minmax_element(a.table.begin(), a.table.end());
        a.dur = cp.NewIntVar(operations_research::Domain(*mn, *mx));
        a.end = cp.NewIntVar(operations_research::Domain(0, 2 * H + *mx));
        // Duration as a function of the start slot. The table is piecewise constant over long runs
        // (constant on continuous calendars, weekly patterns otherwise), so it is encoded per
        // distinct duration: one literal and the union of start runs having that duration. This is
        // far smaller than an element constraint over every slot (which CP-SAT expands to one
        // literal per slot) and exactly equivalent.
        {
            std::map<std::int64_t, std::vector<std::int64_t>> runs; // duration → flat [a0,b0,a1,b1,...]
            for (std::int64_t sl = lo; sl <= hi; ++sl) {
                const auto d = a.table[static_cast<std::size_t>(sl)];
                auto& v = runs[d];
                if (!v.empty() && v.back() == sl - 1) v.back() = sl;
                else {
                    v.push_back(sl);
                    v.push_back(sl);
                }
            }
            if (runs.size() == 1) cp.AddEquality(a.dur, runs.begin()->first);
            else {
                std::vector<sat::BoolVar> lits;
                for (const auto& [d, v] : runs) {
                    const auto b = cp.NewBoolVar();
                    lits.push_back(b);
                    cp.AddLinearConstraint(a.start, operations_research::Domain::FromFlatIntervals(v))
                        .OnlyEnforceIf(b);
                    cp.AddEquality(a.dur, d).OnlyEnforceIf(b);
                }
                cp.AddExactlyOne(lits);
            }
        }
        cp.AddEquality(a.end, a.start + a.dur);
        a.interval = cp.NewIntervalVar(a.start, a.dur, a.end);
        ends.push_back(a.end);
        if (x.constraint == ConstraintType::MustStartOn && x.constraintDate && !a.fixedAtOrigin) {
            const auto lit = cp.NewBoolVar();
            cp.AddEquality(a.start, std::clamp<std::int64_t>(toSlotFloor(scheduling::toMinutes(*x.constraintDate)), 0, H)).OnlyEnforceIf(lit);
            assumptions.push_back(lit);
            assumptionMeaning.push_back({"Must start on", "'" + x.name + "' must start on " + x.constraintDate->toString(), {x.id}});
        }
        if (x.constraint == ConstraintType::FinishNoLaterThan && x.constraintDate && o.hardDeadlines && o.objective != Objective::Lateness) {
            const auto lit = cp.NewBoolVar();
            cp.AddLessOrEqual(a.end, toSlotFloor(scheduling::toMinutes(*x.constraintDate))).OnlyEnforceIf(lit);
            assumptions.push_back(lit);
            assumptionMeaning.push_back({"Deadline", "'" + x.name + "' must finish by " + x.constraintDate->toString(), {x.id}});
        }
        // Hint: the CPM dates.
        if (const auto t = cpm.timings.find(x.id); t != cpm.timings.end()) cp.AddHint(a.start, std::clamp<std::int64_t>(toSlotFloor(t->second.remainingStart), lo, hi));
    }
    // Dependencies.
    for (const auto& d : in.dependencies) {
        const auto pi = ai.find(d.predecessorId), si = ai.find(d.successorId);
        if (pi == ai.end() || si == ai.end()) continue; // completed/excluded predecessors are satisfied
        const auto& P = acts[pi->second];
        const auto& S = acts[si->second];
        // Lag as elapsed slots on the successor calendar, measured at the CPM date of the predecessor.
        std::int64_t lag = 0;
        if (d.lagHours != 0) {
            const auto t = cpm.timings.find(d.predecessorId);
            const double base = t != cpm.timings.end() ? (d.type == DependencyType::FinishToStart || d.type == DependencyType::FinishToFinish ? t->second.ef : t->second.es) : origin;
            const double shifted = S.ia->cal->shift(base, d.lagHours * 60.0);
            lag = d.lagHours > 0 ? static_cast<std::int64_t>(std::ceil((shifted - base) / slotMin - 1e-9)) : static_cast<std::int64_t>(std::floor((shifted - base) / slotMin + 1e-9));
        }
        switch (d.type) {
        case DependencyType::FinishToStart: cp.AddGreaterOrEqual(S.start, P.end + lag); break;
        case DependencyType::StartToStart: cp.AddGreaterOrEqual(S.start, P.start + lag); break;
        case DependencyType::FinishToFinish: cp.AddGreaterOrEqual(S.end, P.end + lag); break;
        case DependencyType::StartToFinish: cp.AddGreaterOrEqual(S.end, P.start + lag); break;
        }
    }
    // Resources.
    for (std::size_t r = 0; r < in.resources.size(); ++r) {
        const auto& R = in.resources[r];
        std::vector<std::pair<sat::IntervalVar, std::int64_t>> users;
        for (const auto& a : acts)
            for (const auto& [rid, units] : a.ia->resources)
                if (rid == R.id && units > 0) users.push_back({a.interval, std::llround(units * 100)});
        if (users.size() < 2) continue;
        auto cum = cp.AddCumulative(std::llround(R.capacity * 100));
        for (const auto& [iv, dem] : users) cum.AddDemand(iv, dem);
    }
    // Work-area limits.
    for (const auto& [area, limit] : in.areaLimits) {
        std::vector<sat::IntervalVar> ivs;
        for (const auto& a : acts)
            if (a.ia->a.workArea == area && a.workMinutes > 0) ivs.push_back(a.interval);
        if (static_cast<int>(ivs.size()) <= limit) continue;
        auto cum = cp.AddCumulative(limit);
        for (const auto& iv : ivs) cum.AddDemand(iv, 1);
    }
    for (const auto& [x, y] : o.mutuallyExclusive) {
        const auto a = ai.find(x), b = ai.find(y);
        if (a == ai.end() || b == ai.end()) continue;
        cp.AddNoOverlap({acts[a->second].interval, acts[b->second].interval});
    }
    // Objective.
    const auto makespan = cp.NewIntVar(operations_research::Domain(0, 4 * H));
    cp.AddMaxEquality(makespan, ends);
    if (o.objective == Objective::Makespan) cp.Minimize(makespan);
    else if (o.objective == Objective::WeightedCompletion) {
        sat::LinearExpr obj = makespan * static_cast<std::int64_t>(acts.size());
        for (const auto& a : acts) obj += a.end * static_cast<std::int64_t>(1 + std::max(0, a.ia->a.priority));
        cp.Minimize(obj);
    } else {
        sat::LinearExpr obj = makespan;
        for (const auto& a : acts) {
            const auto& x = a.ia->a;
            if (x.constraint != ConstraintType::FinishNoLaterThan || !x.constraintDate) continue;
            const auto late = cp.NewIntVar(operations_research::Domain(0, 4 * H));
            cp.AddGreaterOrEqual(late, a.end - toSlotFloor(scheduling::toMinutes(*x.constraintDate)));
            obj += late * 100;
        }
        cp.Minimize(obj);
    }
    for (const auto& lit : assumptions) cp.AddAssumption(lit);

    const sat::CpModelProto proto = cp.Build();
    res.variables = proto.variables_size();
    res.constraints = proto.constraints_size();
    progress.update(0.2, "Optimising");
    auto solve = [&](int workers) {
        sat::Model model;
        sat::SatParameters params;
        params.set_max_time_in_seconds(o.timeLimitSeconds);
        params.set_num_workers(workers);
        params.set_random_seed(o.seed);
        if (std::getenv("OPENMINE_CPSAT_LOG")) params.set_log_search_progress(true); // diagnostics to stderr
        model.Add(sat::NewSatParameters(params));
        std::atomic<bool> stop{false};
        model.GetOrCreate<operations_research::TimeLimit>()->RegisterExternalBooleanAsLimit(&stop);
        std::atomic<bool> done{false};
        std::thread watcher([&] {
            while (!done) {
                if (progress.isCancelled()) stop = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        const auto r = sat::SolveCpModel(proto, &model);
        done = true;
        watcher.join();
        progress.checkCancelled();
        return r;
    };
    auto r = solve(std::max(1, o.workers));
    res.solveSeconds = r.wall_time();
    switch (r.status()) {
    case sat::CpSolverStatus::OPTIMAL: res.status = OptimisationResult::Status::Optimal; break;
    case sat::CpSolverStatus::FEASIBLE: res.status = OptimisationResult::Status::Feasible; break;
    case sat::CpSolverStatus::INFEASIBLE: res.status = OptimisationResult::Status::Infeasible; break;
    case sat::CpSolverStatus::MODEL_INVALID: res.status = OptimisationResult::Status::InvalidModel; break;
    default: res.status = OptimisationResult::Status::NoSolutionInTime;
    }
    res.statusText = sat::CpSolverStatus_Name(r.status());
    if (res.status == OptimisationResult::Status::Infeasible) {
        // A single worker reports a sufficient set of failed assumptions.
        const auto core = assumptions.empty() ? r : solve(1);
        for (const int v : core.sufficient_assumptions_for_infeasibility())
            for (std::size_t k = 0; k < assumptions.size(); ++k)
                if (assumptions[k].index() == v || assumptions[k].index() == -v - 1) res.conflicts.push_back(assumptionMeaning[k]);
        if (res.conflicts.empty())
            res.conflicts.push_back({"Horizon", "No sequence fits in the planning horizon of " + std::to_string(static_cast<long long>(H * slotMin / 60 / 24)) + " days: increase the horizon factor", {}});
        else
            res.notes.push_back("These constraints cannot all be met together with the dependencies and resource capacities; relax one of them");
        return res;
    }
    if (res.status != OptimisationResult::Status::Optimal && res.status != OptimisationResult::Status::Feasible) return res;
    res.objective = r.objective_value();
    res.bestBound = r.best_objective_bound();
    double last = origin;
    for (const auto& a : acts) {
        OptimisedActivity oa;
        oa.startSlot = static_cast<int>(sat::SolutionIntegerValue(r, a.start));
        oa.endSlot = static_cast<int>(sat::SolutionIntegerValue(r, a.end));
        const double ts = slotTime(oa.startSlot);
        if (a.fixedAtOrigin) {
            oa.start = a.ia->actualStart.value_or(ts);
            oa.finish = a.ia->cal->addWork(ts, a.workMinutes);
        } else {
            oa.start = a.workMinutes > 0 ? a.ia->cal->nextWorkStart(ts) : ts;
            oa.finish = a.ia->cal->addWork(ts, a.workMinutes);
        }
        last = std::max(last, oa.finish);
        res.activities[a.ia->a.id] = oa;
    }
    res.makespanHours = (last - origin) / 60.0;
    progress.update(1.0, "Optimised");
    return res;
}

OptimisationResult optimise(const Project& p, const Scenario* sc, const OptimiserOptions& o, jobs::Progress& progress) {
    const auto in = scheduling::buildScheduleInput(p, sc);
    const auto cpm = scheduling::calculateSchedule(in);
    return optimise(in, cpm, o, progress);
}

Uuid storeAsScenario(Project& p, const OptimisationResult& r, const std::string& name, const Scenario* base) {
    if (r.status != OptimisationResult::Status::Optimal && r.status != OptimisationResult::Status::Feasible)
        throw ValidationError("Only a feasible optimisation result can be stored as a scenario");
    Scenario s;
    if (base) {
        s = *base;
        s.id = Uuid{};
        s.meta = {};
    }
    s.name = name;
    s.description = "Optimised sequence (" + r.statusText + ", makespan " + std::to_string(static_cast<long long>(std::llround(r.makespanHours))) + " h)";
    for (const auto& [id, a] : r.activities) {
        // In-progress work keeps its actual start; only future starts are fixed.
        const auto* act = p.model().activities.find(id);
        if (!act || act->actualStart) continue;
        s.overrides.push_back({id, "startNoEarlierThan", scheduling::toDateTime(std::ceil(a.start - 1e-6)).toString()});
    }
    return ProjectEditor(p).add(s, "Store optimised scenario " + name);
}

} // namespace openmine::optimisation
