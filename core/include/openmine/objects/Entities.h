#pragma once
// SPDX-License-Identifier: Apache-2.0
// Persistent domain entities of the unified OpenMine object model.
//
// Entities are plain value types. They are owned by openmine::Project and are only
// changed through openmine::commands (which gives undo/redo and audit for free).
// Every persistent entity carries a Uuid that never changes for its lifetime.

#include "openmine/geometry/Vec3.h"
#include "openmine/objects/DateTime.h"
#include "openmine/objects/Uuid.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openmine {

/// Free-form, schema-validated custom attributes (key → canonical text value).
using AttributeMap = std::map<std::string, std::string>;

struct EntityMetadata {
    std::string createdAt;  ///< ISO-8601 UTC
    std::string modifiedAt; ///< ISO-8601 UTC
    std::int64_t revision = 1; ///< Incremented on every committed change.
};

// ---------------------------------------------------------------------------
// Project-level metadata
// ---------------------------------------------------------------------------

/// The project CRS is *declared* by the user. OpenMine never infers a CRS from
/// coordinate magnitudes or file extensions.
struct CoordinateReferenceSystem {
    std::string name = "Unspecified";       ///< e.g. "Hartebeesthoek94 / Lo25" (user-declared)
    std::string authorityCode;              ///< e.g. "EPSG:2051" or empty when unknown/local grid
    std::string verticalDatum = "Unspecified";
    std::string linearUnit = "metre";       ///< Only metres are supported in v0.1.
    bool isLocalMineGrid = false;
    /// South-African Lo-system convention flag: true when +Y is West / +X is South.
    /// Recorded as metadata only; geometry is always stored in the declared axes.
    bool southOrientatedAxes = false;
};

struct ProjectInfo {
    Uuid id;
    std::string name;
    std::string description;
    Date scheduleStart;                     ///< Project/schedule start date.
    std::optional<Date> statusDate;         ///< Data date for progress reforecasts.
    CoordinateReferenceSystem crs;
    Uuid defaultCalendarId;
    EntityMetadata meta;
};

// ---------------------------------------------------------------------------
// CAD
// ---------------------------------------------------------------------------

struct CadLayer {
    Uuid id;
    std::string name;
    std::uint32_t colorRgb = 0xD0D0D0; ///< 0xRRGGBB
    bool visible = true;
    bool locked = false;
    EntityMetadata meta;
};

enum class GeometryType { Point, Line, Polyline };

struct GeometryObject {
    Uuid id;
    std::string name;
    GeometryType type = GeometryType::Polyline;
    Uuid layerId;
    std::vector<Vec3> vertices;
    bool closed = false;
    AttributeMap attributes;
    EntityMetadata meta;
};

// ---------------------------------------------------------------------------
// Mine design
// ---------------------------------------------------------------------------

/// An underground development heading — a domain object that *references*
/// centreline geometry rather than being a bare polyline.
/// Length, chainage-to and gradient are derived from geometry (never typed in).
struct MineDevelopment {
    Uuid id;
    std::string headingId;      ///< Mine-assigned code, e.g. "LEG2_675_D3_DEC"
    std::string name;
    std::string zone;
    std::string declineNumber;
    std::string level;
    std::string activityType = "Development"; ///< Decline, Crosscut, Ore drive, ...
    double chainageFrom = 0.0;  ///< m
    double designWidth = 5.0;   ///< m
    double designHeight = 5.0;  ///< m
    std::string status = "Planned"; ///< Planned | In progress | Complete | Suspended
    Uuid geometryId;            ///< Centreline (required)
    /// Design length (m) that the linked schedule quantities were last approved against.
    /// Used to detect geometry changes that affect the schedule.
    double approvedDesignLength = 0.0;
    std::string designRevision = "A";
    AttributeMap attributes;
    EntityMetadata meta;
};

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

struct WorkCalendar {
    Uuid id;
    std::string name;
    /// Bit (n-1) set ⇒ ISO weekday n is a working day (bit0 = Monday).
    std::uint8_t workingWeekdays = 0b0011111; // Mon–Fri
    double hoursPerDay = 24.0;                 ///< informational in v0.1
    std::vector<Date> nonWorkingDates;        ///< holidays / planned shutdowns
    EntityMetadata meta;
};

enum class ActivityKind { Task, Milestone, Summary };

struct ScheduleActivity {
    Uuid id;
    Uuid parentId;              ///< WBS parent (Summary activity) or nil
    std::string wbsCode;
    std::string name;
    ActivityKind kind = ActivityKind::Task;
    /// Duration in working days. Ignored when durationFromRate is true.
    double durationDays = 1.0;
    bool durationFromRate = false;
    double quantity = 0.0;      ///< e.g. metres of development
    std::string quantityUnit = "m";
    double ratePerDay = 0.0;    ///< quantity per working day
    double actualQuantity = 0.0;
    std::optional<Date> startNoEarlierThan;
    Uuid calendarId;            ///< nil ⇒ project default calendar
    int sortOrder = 0;
    /// Raised when linked design geometry changes; cleared only by an explicit,
    /// audited recalculation (never silently).
    bool designImpactFlag = false;
    std::string designImpactNote;
    EntityMetadata meta;
};

enum class DependencyType { FinishToStart, StartToStart, FinishToFinish, StartToFinish };

struct ActivityDependency {
    Uuid id;
    Uuid predecessorId;
    Uuid successorId;
    DependencyType type = DependencyType::FinishToStart;
    double lagDays = 0.0; ///< working days; may be negative (lead)
    EntityMetadata meta;
};

/// Explicit many-to-many link between schedule activities and mine design objects.
struct ActivityDesignLink {
    Uuid activityId;
    Uuid developmentId;
    friend bool operator==(const ActivityDesignLink&, const ActivityDesignLink&) = default;
};

struct BaselineActivity {
    Uuid activityId;
    std::string name;
    Date start;
    Date finish;
    double durationDays = 0.0;
    double quantity = 0.0;
};

/// An approved schedule baseline is immutable once captured.
struct ScheduleBaseline {
    Uuid id;
    std::string name;
    std::string capturedAt;
    bool approved = true;
    std::vector<BaselineActivity> activities;
};

// ---------------------------------------------------------------------------
// Audit
// ---------------------------------------------------------------------------

struct AuditEntry {
    std::string timestamp;
    std::string user;
    std::string action;     ///< e.g. "Create", "Modify", "Delete", "Recalculate"
    std::string entityType; ///< e.g. "GeometryObject"
    Uuid entityId;
    std::string detail;
};

// String conversions used by persistence, UI and scripting.
const char* toString(GeometryType t);
std::optional<GeometryType> geometryTypeFromString(const std::string& s);
const char* toString(ActivityKind k);
std::optional<ActivityKind> activityKindFromString(const std::string& s);
const char* toString(DependencyType t);        ///< "FS", "SS", "FF", "SF"
std::optional<DependencyType> dependencyTypeFromString(const std::string& s);

} // namespace openmine
