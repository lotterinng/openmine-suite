// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/ScenarioSchedule.h"

namespace openmine::scheduling {

ScenarioSchedule calculateScenario(const Project& p, const Scenario* sc) {
    ScenarioSchedule s;
    s.input = buildScheduleInput(p, sc);
    s.cpm = calculateSchedule(s.input);
    if (sc && sc->levelling == LevellingMode::Heuristic && s.cpm.ok) {
        LevellingReport rep;
        s.result = levelResources(s.input, s.cpm, &rep);
        s.levelling = rep;
    } else {
        s.result = s.cpm;
    }
    return s;
}

const Scenario* scenarioById(const Project& p, const Uuid& id) {
    if (id.isNil()) return nullptr;
    const auto* sc = p.model().scenarios.find(id);
    if (!sc) throw ValidationError("Scenario " + id.toString() + " does not exist");
    return sc;
}

} // namespace openmine::scheduling
