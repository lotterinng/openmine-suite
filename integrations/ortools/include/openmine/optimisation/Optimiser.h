#pragma once
// SPDX-License-Identifier: Apache-2.0
// Resource-constrained schedule optimisation with Google OR-Tools CP-SAT (Apache-2.0).
//
// The optimiser is a separate layer on top of the CPM engine (modules/scheduling): it reads the
// same ScheduleInput (calendars, remaining work, dependencies, resources, work-area limits,
// constraints) and returns a sequence; it never modifies the base schedule. To use a sequence,
// it is stored as a scenario whose "startNoEarlierThan" overrides reproduce it, and CPM then
// recalculates the exact calendar dates and floats.
//
// Model (time in slots of `slotHours`, from the status date or project start):
//  * each remaining activity: start variable; elapsed duration = table over start slots computed
//    on its effective calendar (so night shifts, weekends and holidays are respected exactly at the
//    slot resolution);
//  * dependencies FS/SS/FF/SF with lags (lags converted to elapsed slots on the successor calendar
//    at the CPM date);
//  * resources: cumulative capacity (units × 100 as integers); work-area limits: cumulative of 1s;
//    mutually exclusive activity pairs: no-overlap;
//  * constraints: SNET/MSO hard; FNLT hard (with infeasibility explanation) or soft (lateness);
//  * in-progress activities are fixed at the status date; completed activities are omitted.
// Infeasibility: hard constraints carry assumption literals, and an infeasible model is re-solved
// to extract a sufficient set of conflicting constraints, reported in plain language.

#include "openmine/jobs/Job.h"
#include "openmine/project/Project.h"
#include "openmine/scheduling/Cpm.h"

#include <map>
#include <string>
#include <vector>

namespace openmine::optimisation {

enum class Objective {
    Makespan,               ///< finish everything as early as possible
    WeightedCompletion,     ///< Σ (1 + priority) · finish, plus makespan
    Lateness,               ///< Σ lateness against FNLT deadlines (soft), plus makespan
};

struct OptimiserOptions {
    double slotHours = 1.0;
    double horizonFactor = 3.0;          ///< horizon = factor × CPM duration (+ slack)
    Objective objective = Objective::Makespan;
    double timeLimitSeconds = 30;
    int workers = 8;
    int seed = 1;
    bool hardDeadlines = true;           ///< FNLT as hard constraints (Lateness objective makes them soft)
    std::vector<std::pair<Uuid, Uuid>> mutuallyExclusive; ///< activity pairs that may not overlap
};

struct OptimisedActivity {
    double start = 0, finish = 0; ///< minutes since epoch (work start / finish on the calendar)
    int startSlot = 0, endSlot = 0;
};

struct Conflict {
    std::string kind;        ///< "Deadline", "Must start on", "Start no earlier than", "Capacity"
    std::string description;
    std::vector<Uuid> activities;
};

struct OptimisationResult {
    enum class Status { Optimal, Feasible, Infeasible, NoSolutionInTime, InvalidModel } status = Status::NoSolutionInTime;
    std::string statusText;
    double makespanHours = 0;        ///< from the time origin to the last finish
    double cpmMakespanHours = 0;     ///< resource-unconstrained CPM for comparison
    double objective = 0, bestBound = 0;
    double solveSeconds = 0;
    std::map<Uuid, OptimisedActivity> activities;
    std::vector<Conflict> conflicts;
    std::vector<std::string> notes;
    int variables = 0, constraints = 0;
};

OptimisationResult optimise(const scheduling::ScheduleInput& input, const scheduling::ScheduleResult& cpm, const OptimiserOptions& options = {},
                            jobs::Progress& progress = jobs::nullProgress());
/// Builds the input (optionally of a scenario), runs CPM and optimises.
OptimisationResult optimise(const Project& project, const Scenario* scenario = nullptr, const OptimiserOptions& options = {},
                            jobs::Progress& progress = jobs::nullProgress());

/// Stores a feasible sequence as a new scenario (startNoEarlierThan overrides on top of the given
/// base scenario's overrides). Returns the scenario id. One undo step.
Uuid storeAsScenario(Project& project, const OptimisationResult& result, const std::string& name, const Scenario* base = nullptr);

std::string ortoolsVersion();

} // namespace openmine::optimisation
