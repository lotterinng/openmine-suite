#pragma once
// SPDX-License-Identifier: Apache-2.0
// The schedule of a scenario as every part of OpenMine computes it: the scenario-adjusted input,
// the CPM, and — when the scenario asks for heuristic levelling — the levelled schedule. The
// desktop application, the reports, the 4D views and the Python API share this, so a scenario
// never shows different dates in different places. (Optimised scenarios store their sequence as
// constraints, which the input already contains.)

#include "openmine/scheduling/Cpm.h"
#include "openmine/scheduling/Levelling.h"
#include "openmine/scheduling/ScheduleInput.h"

#include <optional>

namespace openmine::scheduling {

struct ScenarioSchedule {
    ScheduleInput input;      ///< refers to the project model: valid until it changes
    ScheduleResult cpm;       ///< unlevelled
    ScheduleResult result;    ///< levelled when the scenario asks for it, else the CPM
    std::optional<LevellingReport> levelling;
};

/// `scenario` nil = the base schedule.
ScenarioSchedule calculateScenario(const Project& project, const Scenario* scenario = nullptr);

/// Looks a scenario up by id (nil = base); throws ValidationError for an unknown id.
const Scenario* scenarioById(const Project& project, const Uuid& id);

} // namespace openmine::scheduling
