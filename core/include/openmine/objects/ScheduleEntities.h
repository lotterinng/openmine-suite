#pragma once
// SPDX-License-Identifier: Apache-2.0
// Scheduling entities: calendars, activities, dependencies, resources, assignments,
// production models, actuals, targets, baselines and scenarios.

#include "openmine/objects/Common.h"

namespace openmine {

/// A recurring work shift. A shift that crosses midnight continues on the next day;
/// `weekdays` refers to the day the shift *starts* (bit 0 = Monday … bit 6 = Sunday).
struct ShiftDefinition {
    std::string name = "Day";
    int startMinute = 0;      ///< minute of day the shift starts
    int durationMinutes = 1440;
    std::uint8_t weekdays = 0b0011111;
    friend bool operator==(const ShiftDefinition&, const ShiftDefinition&) = default;
};

struct TimeRange {
    int startMinute = 0;
    int endMinute = 1440;
    friend bool operator==(const TimeRange&, const TimeRange&) = default;
};

/// A dated exception: a non-working day (holiday/shutdown) when `periods` is empty,
/// or a day with modified working periods.
struct CalendarException {
    Date date;
    std::vector<TimeRange> periods;
    std::string reason;
    friend bool operator==(const CalendarException&, const CalendarException&) = default;
};

enum class CalendarKind { Project, Shift, Resource };

struct WorkCalendar {
    Uuid id;
    std::string name;
    CalendarKind kind = CalendarKind::Project;
    std::vector<ShiftDefinition> shifts;      ///< weekly pattern
    std::vector<CalendarException> exceptions;
    double standardHoursPerDay = 24.0;        ///< working hours that make one "day" for rates/durations shown in days
    std::string description;
    EntityMetadata meta;
};

enum class ActivityKind { Task, Milestone, Summary };

enum class ConstraintType { None, StartNoEarlierThan, FinishNoLaterThan, MustStartOn };

enum class RateUnit { PerHour, PerShift, PerDay };

struct ScheduleActivity {
    Uuid id;
    Uuid parentId;              ///< WBS parent (Summary activity) or nil
    std::string wbsCode;
    std::string name;
    ActivityKind kind = ActivityKind::Task;
    /// Working duration in hours on the activity calendar. Ignored when durationFromRate.
    double durationHours = 24.0;
    bool durationFromRate = false;
    double quantity = 0.0;      ///< e.g. metres of development
    std::string quantityUnit = "m";
    double rate = 0.0;          ///< quantity per rateUnit
    RateUnit rateUnit = RateUnit::PerDay;
    Uuid productionModelId;     ///< DevelopmentCycle; when set and useCycleRate, overrides `rate`
    bool useCycleRate = false;
    /// Legacy single actual value; dated ProductionActual records supersede it when present.
    double actualQuantity = 0.0;
    double manualPercentComplete = 0.0; ///< for activities without quantity
    std::optional<DateTime> actualStart;
    std::optional<DateTime> actualFinish;
    ConstraintType constraint = ConstraintType::None;
    std::optional<DateTime> constraintDate;
    Uuid calendarId;            ///< nil ⇒ project default calendar
    int priority = 0;           ///< higher = scheduled first by levelling/optimisation
    std::string workArea;       ///< restricted work area / zone key for area constraints
    int sortOrder = 0;
    /// Raised when linked design geometry changes; cleared only by an explicit,
    /// audited recalculation (never silently).
    bool designImpactFlag = false;
    std::string designImpactNote;
    AttributeMap attributes;
    EntityMetadata meta;
};

enum class DependencyType { FinishToStart, StartToStart, FinishToFinish, StartToFinish };

struct ActivityDependency {
    Uuid id;
    Uuid predecessorId;
    Uuid successorId;
    DependencyType type = DependencyType::FinishToStart;
    double lagHours = 0.0; ///< working hours on the successor's calendar; negative = lead
    EntityMetadata meta;
};

/// Explicit many-to-many link between schedule activities and design objects
/// (MineDevelopment, Stope or MiningBlock).
struct ActivityDesignLink {
    Uuid activityId;
    Uuid designId;
    std::string designType = "MineDevelopment";
    friend bool operator==(const ActivityDesignLink&, const ActivityDesignLink&) = default;
};

enum class ResourceKind { Equipment, Crew, Material, Other };

struct Resource {
    Uuid id;
    std::string name;
    std::string code;
    ResourceKind kind = ResourceKind::Equipment;
    double capacity = 1.0;      ///< units available simultaneously
    Uuid calendarId;            ///< ResourceCalendar (a WorkCalendar of kind Resource); nil = always available
    double productivityFactor = 1.0; ///< multiplies production rates of activities it drives
    double costPerHour = 0.0;
    std::string description;
    EntityMetadata meta;
};

struct ActivityResourceAssignment {
    Uuid id;
    Uuid activityId;
    Uuid resourceId;
    double units = 1.0;
    bool drivesRate = false; ///< apply the resource's productivity factor to the activity rate
    EntityMetadata meta;
};

/// Development cycle model used to derive an advance rate from drilling, charging,
/// blasting, mucking, ground support and other cycle activities.
struct DevelopmentCycle {
    Uuid id;
    std::string name;
    // Drilling
    double roundLength = 4.0;        ///< drilled depth, m
    double advanceEfficiency = 0.9;  ///< pulled / drilled
    int holesPerRound = 50;
    double drillPenetrationRate = 2.0; ///< m/min per boom
    int drillBooms = 2;
    double drillSetupMinutes = 30;
    double collarMinutesPerHole = 0.75;
    // Charging & blasting
    double chargeMinutesPerHole = 1.5;
    double blastAndReentryMinutes = 60;
    // Mucking
    double swellFactor = 1.4;
    double bucketCapacity = 5.0;     ///< m³ loose
    double bucketFillFactor = 0.85;
    double muckCycleMinutes = 4.0;   ///< per bucket (load–haul–dump)
    // Ground support (bolts per round derived from support class if set, else fixed)
    int boltsPerRound = 20;
    double minutesPerBolt = 4.0;
    double meshMinutes = 30.0;
    double shotcreteMinutes = 0.0;
    // Other
    double scalingMinutes = 30;
    double surveyMarkupMinutes = 20;
    double serviceMinutes = 20;
    double utilisation = 0.85;       ///< fraction of shift time effectively used
    double shiftHours = 10.0;
    int shiftsPerDay = 2;
    std::string notes;
    EntityMetadata meta;
};

enum class ActualSource { Manual, Survey, Import };

/// A dated production actual (e.g. metres advanced on a date).
struct ProductionActual {
    Uuid id;
    Uuid activityId;
    Uuid developmentId;      ///< optional design object
    Date date;
    double quantity = 0.0;   ///< incremental quantity on `date`
    std::string unit = "m";
    ActualSource source = ActualSource::Manual;
    Uuid surveyDatasetId;    ///< when source == Survey
    std::optional<double> faceChainage; ///< surveyed face position, m
    std::string note;
    std::string enteredBy;
    EntityMetadata meta;
};

struct ProductionTarget {
    Uuid id;
    std::string name;
    Date periodStart;
    Date periodEnd;     ///< inclusive
    double quantity = 0.0;
    std::string unit = "m";
    std::string scopeZone;  ///< empty = whole project
    std::string scopeLevel;
    EntityMetadata meta;
};

struct BaselineActivity {
    Uuid activityId;
    std::string name;
    DateTime start;
    DateTime finish;
    double durationHours = 0.0;
    double quantity = 0.0;
    friend bool operator==(const BaselineActivity&, const BaselineActivity&) = default;
};

/// An approved schedule baseline is immutable once captured.
struct ScheduleBaseline {
    Uuid id;
    std::string name;
    std::string capturedAt;
    bool approved = true;
    std::vector<BaselineActivity> activities;
};

enum class LevellingMode { None, Heuristic, Optimised };

/// A scenario overrides selected inputs of the base schedule without copying it.
struct ScenarioOverride {
    Uuid entityId;     ///< activity or resource
    std::string field; ///< "rate", "durationHours", "priority", "capacity", "excluded", "calendarId", "productivityFactor"
    std::string value;
    friend bool operator==(const ScenarioOverride&, const ScenarioOverride&) = default;
};

struct Scenario {
    Uuid id;
    std::string name;
    std::string description;
    std::vector<ScenarioOverride> overrides;
    LevellingMode levelling = LevellingMode::None;
    std::string objective = "Makespan"; ///< Makespan | WeightedCompletion
    double optimiserTimeLimitSeconds = 30.0;
    EntityMetadata meta;
};

} // namespace openmine
