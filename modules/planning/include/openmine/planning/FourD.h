#pragma once
// SPDX-License-Identifier: Apache-2.0
// 4D mine planning: the state of every scheduled design object at an instant of the schedule —
// planned progress (work-time interpolation on the activity calendar), actual progress from
// dated actuals, the excavated part of each development, the resources working on it, and a
// colour for display (by state, by planned start period, by resource or by activity). Used by
// the viewport's schedule playback and for scenario comparison.

#include "openmine/scheduling/Cpm.h"

#include <map>
#include <optional>
#include <vector>

namespace openmine::planning {

enum class ObjectState { NotScheduled, Future, InProgress, Complete };
const char* toString(ObjectState s);

enum class ColorMode { State, StartPeriod, Resource, Activity };

struct ObjectSnapshot {
    Uuid designId;
    std::string designType;            ///< "MineDevelopment", "Stope", "MiningBlock"
    std::string name;
    ObjectState state = ObjectState::NotScheduled;
    double plannedFraction = 0;        ///< planned progress at the instant (0…1)
    double actualFraction = 0;         ///< reported progress to the status date (0…1)
    double baselineFraction = 0;       ///< progress the baseline planned by the instant (0…1)
    bool behindPlan = false;           ///< reported progress < baseline progress at the status date
    std::vector<Uuid> activities;      ///< linked activities
    std::vector<Uuid> activeResources; ///< resources of activities working at the instant
    std::optional<double> plannedStart, plannedFinish; ///< minutes
    std::vector<Vec3> excavatedCentreline; ///< developments: centreline from the start to the planned face
    double plannedFace = 0;            ///< developments: planned face chainage (m)
    std::uint32_t color = 0x808080;
};

struct PlanSnapshot {
    double time = 0;                   ///< minutes
    std::vector<ObjectSnapshot> objects;
    std::map<Uuid, std::vector<Uuid>> resourceAssignments; ///< resource → design objects worked at the instant
    double plannedMetres = 0;          ///< developed metres planned up to the instant
    double actualMetres = 0;           ///< reported metres up to the status date
};

PlanSnapshot snapshot(const Project& project, const scheduling::ScheduleInput& input, const scheduling::ScheduleResult& result, double timeMinutes,
                      ColorMode mode = ColorMode::State, const ScheduleBaseline* baseline = nullptr);

/// Playback instants from the schedule start to its finish every `stepMinutes` (inclusive).
std::vector<double> playbackFrames(const scheduling::ScheduleResult& result, double stepMinutes);

/// Excavation envelope of the first `fraction` of a development (for partial display).
MeshData excavatedEnvelope(const Project& project, const Uuid& developmentId, double fraction);

struct ObjectDifference {
    Uuid designId;
    std::string name;
    double fractionA = 0, fractionB = 0;
    std::optional<double> finishA, finishB; ///< minutes
    double finishShiftHours = 0;            ///< B − A
};
/// Per-object differences of two schedules (e.g. base vs scenario) at the same instant.
std::vector<ObjectDifference> compareSnapshots(const PlanSnapshot& a, const PlanSnapshot& b);

/// Perceptual colour ramp (viridis) for t in [t0, t1].
std::uint32_t timeColor(double t, double t0, double t1);
/// Stable categorical colour for an id.
std::uint32_t categoricalColor(const Uuid& id);

} // namespace openmine::planning
