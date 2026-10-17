// SPDX-License-Identifier: Apache-2.0
// Reader for project files of schema version 1 (OpenMine Suite 0.1.0). Kept so old projects
// keep opening: migration is implemented as "read with the version-specific reader, then save
// with the current writer", which cannot drift from the current schema.
#include "SchemaV2.h"
#include "TableMapping.h"
#include "openmine/project/Project.h"

namespace openmine::persistence::detail {

namespace {
constexpr const char* kSchemaV1 = R"SQL(
CREATE TABLE schema_migrations (
    version      INTEGER PRIMARY KEY,
    applied_at   TEXT NOT NULL,
    description  TEXT NOT NULL
);
CREATE TABLE project_info (
    id                   INTEGER PRIMARY KEY CHECK (id = 1),
    uuid                 TEXT NOT NULL,
    name                 TEXT NOT NULL,
    description          TEXT NOT NULL DEFAULT '',
    schedule_start       TEXT NOT NULL,
    status_date          TEXT,
    crs_name             TEXT NOT NULL,
    crs_authority_code   TEXT NOT NULL DEFAULT '',
    vertical_datum       TEXT NOT NULL,
    linear_unit          TEXT NOT NULL CHECK (linear_unit = 'metre'),
    is_local_mine_grid   INTEGER NOT NULL,
    south_orientated     INTEGER NOT NULL,
    default_calendar     TEXT REFERENCES work_calendar(uuid) ON DELETE RESTRICT,
    created_at TEXT NOT NULL, modified_at TEXT NOT NULL, revision INTEGER NOT NULL,
    app_version          TEXT NOT NULL
);
CREATE TABLE cad_layer (
    uuid TEXT PRIMARY KEY, sort_index INTEGER NOT NULL,
    name TEXT NOT NULL UNIQUE, color_rgb INTEGER NOT NULL,
    visible INTEGER NOT NULL, locked INTEGER NOT NULL,
    created_at TEXT NOT NULL, modified_at TEXT NOT NULL, revision INTEGER NOT NULL
);
CREATE TABLE geometry_object (
    uuid TEXT PRIMARY KEY, sort_index INTEGER NOT NULL,
    name TEXT NOT NULL,
    geometry_type TEXT NOT NULL CHECK (geometry_type IN ('Point','Line','Polyline')),
    layer_uuid TEXT NOT NULL REFERENCES cad_layer(uuid) ON DELETE RESTRICT,
    closed INTEGER NOT NULL,
    vertex_count INTEGER NOT NULL CHECK (vertex_count >= 1),
    vertices BLOB NOT NULL CHECK (length(vertices) = vertex_count * 24),
    created_at TEXT NOT NULL, modified_at TEXT NOT NULL, revision INTEGER NOT NULL
);
CREATE INDEX ix_geometry_layer ON geometry_object(layer_uuid);
CREATE TABLE attribute_definition (
    uuid TEXT PRIMARY KEY, sort_index INTEGER NOT NULL,
    entity_type TEXT NOT NULL CHECK (entity_type IN ('GeometryObject','MineDevelopment')),
    key TEXT NOT NULL, display_name TEXT NOT NULL,
    value_type TEXT NOT NULL CHECK (value_type IN ('Text','Real','Integer','Boolean','Date')),
    unit TEXT NOT NULL DEFAULT '', required INTEGER NOT NULL, default_value TEXT NOT NULL DEFAULT '',
    UNIQUE (entity_type, key)
);
CREATE TABLE attribute_value (
    entity_uuid TEXT NOT NULL,
    entity_type TEXT NOT NULL,
    key TEXT NOT NULL,
    value TEXT NOT NULL,
    PRIMARY KEY (entity_uuid, key)
);
CREATE TABLE work_calendar (
    uuid TEXT PRIMARY KEY, sort_index INTEGER NOT NULL,
    name TEXT NOT NULL,
    working_weekdays INTEGER NOT NULL CHECK (working_weekdays BETWEEN 1 AND 127),
    hours_per_day REAL NOT NULL CHECK (hours_per_day > 0 AND hours_per_day <= 24),
    created_at TEXT NOT NULL, modified_at TEXT NOT NULL, revision INTEGER NOT NULL
);
CREATE TABLE calendar_exception (
    calendar_uuid TEXT NOT NULL REFERENCES work_calendar(uuid) ON DELETE CASCADE,
    date TEXT NOT NULL,
    PRIMARY KEY (calendar_uuid, date)
);
CREATE TABLE mine_development (
    uuid TEXT PRIMARY KEY, sort_index INTEGER NOT NULL,
    heading_id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL, zone TEXT NOT NULL, decline_number TEXT NOT NULL, level TEXT NOT NULL,
    activity_type TEXT NOT NULL,
    chainage_from REAL NOT NULL,
    design_width REAL NOT NULL CHECK (design_width > 0),
    design_height REAL NOT NULL CHECK (design_height > 0),
    status TEXT NOT NULL,
    geometry_uuid TEXT NOT NULL UNIQUE REFERENCES geometry_object(uuid) ON DELETE RESTRICT,
    approved_design_length REAL NOT NULL,
    design_revision TEXT NOT NULL,
    created_at TEXT NOT NULL, modified_at TEXT NOT NULL, revision INTEGER NOT NULL
);
CREATE TABLE schedule_activity (
    uuid TEXT PRIMARY KEY, sort_index INTEGER NOT NULL,
    parent_uuid TEXT REFERENCES schedule_activity(uuid) ON DELETE RESTRICT,
    wbs_code TEXT NOT NULL, name TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('Task','Milestone','Summary')),
    duration_days REAL NOT NULL CHECK (duration_days >= 0),
    duration_from_rate INTEGER NOT NULL,
    quantity REAL NOT NULL, quantity_unit TEXT NOT NULL,
    rate_per_day REAL NOT NULL, actual_quantity REAL NOT NULL,
    start_no_earlier_than TEXT,
    calendar_uuid TEXT REFERENCES work_calendar(uuid) ON DELETE RESTRICT,
    sort_order INTEGER NOT NULL,
    design_impact_flag INTEGER NOT NULL, design_impact_note TEXT NOT NULL,
    created_at TEXT NOT NULL, modified_at TEXT NOT NULL, revision INTEGER NOT NULL
);
CREATE INDEX ix_activity_parent ON schedule_activity(parent_uuid);
CREATE TABLE activity_dependency (
    uuid TEXT PRIMARY KEY, sort_index INTEGER NOT NULL,
    predecessor_uuid TEXT NOT NULL REFERENCES schedule_activity(uuid) ON DELETE CASCADE,
    successor_uuid   TEXT NOT NULL REFERENCES schedule_activity(uuid) ON DELETE CASCADE,
    dependency_type TEXT NOT NULL CHECK (dependency_type IN ('FS','SS','FF','SF')),
    lag_days REAL NOT NULL,
    created_at TEXT NOT NULL, modified_at TEXT NOT NULL, revision INTEGER NOT NULL,
    UNIQUE (predecessor_uuid, successor_uuid),
    CHECK (predecessor_uuid <> successor_uuid)
);
CREATE INDEX ix_dependency_successor ON activity_dependency(successor_uuid);
CREATE TABLE activity_design_link (
    activity_uuid    TEXT NOT NULL REFERENCES schedule_activity(uuid) ON DELETE CASCADE,
    development_uuid TEXT NOT NULL REFERENCES mine_development(uuid) ON DELETE CASCADE,
    sort_index INTEGER NOT NULL,
    PRIMARY KEY (activity_uuid, development_uuid)
);
CREATE INDEX ix_link_development ON activity_design_link(development_uuid);
CREATE TABLE schedule_baseline (
    uuid TEXT PRIMARY KEY, sort_index INTEGER NOT NULL,
    name TEXT NOT NULL UNIQUE, captured_at TEXT NOT NULL, approved INTEGER NOT NULL
);
CREATE TABLE baseline_activity (
    baseline_uuid TEXT NOT NULL REFERENCES schedule_baseline(uuid) ON DELETE CASCADE,
    activity_uuid TEXT NOT NULL,           -- intentionally no FK: baselines outlive activities
    sort_index INTEGER NOT NULL,
    name TEXT NOT NULL, start_date TEXT NOT NULL, finish_date TEXT NOT NULL,
    duration_days REAL NOT NULL, quantity REAL NOT NULL,
    PRIMARY KEY (baseline_uuid, activity_uuid)
);
CREATE TABLE asset (
    uuid TEXT PRIMARY KEY,
    kind TEXT NOT NULL,                    -- e.g. 'PointCloud', 'Mesh'
    relative_path TEXT NOT NULL UNIQUE,    -- relative to <file>-assets/
    sha256 TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    created_at TEXT NOT NULL
);
CREATE TABLE audit_log (
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL, user TEXT NOT NULL, action TEXT NOT NULL,
    entity_type TEXT NOT NULL, entity_uuid TEXT, detail TEXT NOT NULL
);
)SQL";

} // namespace

namespace {
Date requireDate(const std::string& s, const char* what) {
    auto d = Date::parse(s);
    if (!d) throw PersistenceError(std::string("Corrupt date in ") + what + ": '" + s + "'");
    return *d;
}
EntityMetadata readMeta(const Stmt& s, int first) { return {s.text(first), s.text(first + 1), s.integer(first + 2)}; }

AttributeMap readAttributes(Db& db, const Uuid& id) {
    AttributeMap out;
    Stmt s(db, "SELECT key, value FROM attribute_value WHERE entity_uuid = ?");
    s.bindUuid(1, id);
    while (s.step()) out[s.text(0)] = s.text(1);
    return out;
}
} // namespace

const char* schemaDdlV1() { return kSchemaV1; }

std::vector<std::string> verifyV1(Db& db) {
    std::vector<std::string> problems;
    Stmt s(db, "PRAGMA integrity_check");
    while (s.step())
        if (s.text(0) != "ok") problems.push_back("integrity_check: " + s.text(0));
    Stmt f(db, "PRAGMA foreign_key_check");
    while (f.step()) problems.push_back("foreign key violation in table " + f.text(0));
    return problems;
}

// Conversion rules v1 → v2 (documented in docs/architecture/project-format.md):
//  * v1 schedules counted whole calendar working days; a v1 calendar becomes one 00:00–24:00 shift on its
//    working weekdays with standardHoursPerDay = 24, so 1 v1 day = 24 working hours and all dates are preserved.
//  * durations and lags: days × 24 → hours; rates "per working day" → RateUnit::PerDay.
//  * start-no-earlier-than → ConstraintType::StartNoEarlierThan at 00:00.
//  * baseline finish dates were inclusive → finish instant = 00:00 of the following day.
//  * the declared CRS becomes a CoordinateReferenceSystem entity referenced by the project.
void readModelV1(Db& db, ProjectModel& m) {
    std::string crsName, crsCode, vertical;
    bool localGrid = false;
    {
        Stmt s(db, "SELECT uuid,name,description,schedule_start,status_date,crs_name,crs_authority_code,vertical_datum,"
                   "linear_unit,is_local_mine_grid,south_orientated,default_calendar,created_at,modified_at,revision "
                   "FROM project_info WHERE id = 1");
        if (!s.step()) throw PersistenceError("Project file has no project_info record");
        auto& i = m.info;
        i.id = s.uuid(0);
        i.name = s.text(1);
        i.description = s.text(2);
        i.scheduleStart = DateTime::fromDate(requireDate(s.text(3), "schedule_start"));
        if (!s.isNull(4)) i.statusDate = DateTime::fromDate(requireDate(s.text(4), "status_date"));
        crsName = s.text(5);
        crsCode = s.text(6);
        vertical = s.text(7);
        localGrid = s.integer(9) != 0;
        i.southOrientatedAxes = s.integer(10) != 0;
        i.defaultCalendarId = s.uuid(11);
        i.meta = readMeta(s, 12);
    }
    {
        CoordinateReferenceSystem c;
        c.id = Uuid::generate();
        c.name = crsName;
        c.authorityCode = crsCode;
        c.verticalDatum = vertical;
        c.kind = localGrid ? CrsKind::Engineering : (crsCode.empty() ? CrsKind::Unknown : CrsKind::Projected);
        c.meta = m.info.meta;
        m.crs.upsert(c);
        m.info.projectCrsId = c.id;
    }
    {
        Stmt s(db, "SELECT uuid,name,color_rgb,visible,locked,created_at,modified_at,revision FROM cad_layer ORDER BY sort_index");
        while (s.step()) {
            CadLayer l;
            l.id = s.uuid(0);
            l.name = s.text(1);
            l.colorRgb = static_cast<std::uint32_t>(s.integer(2));
            l.visible = s.integer(3) != 0;
            l.locked = s.integer(4) != 0;
            l.meta = readMeta(s, 5);
            m.layers.upsert(l);
        }
    }
    {
        Stmt s(db, "SELECT uuid,key,display_name,value_type,unit,required,default_value,entity_type "
                   "FROM attribute_definition ORDER BY sort_index");
        while (s.step()) {
            AttributeDefinition d;
            d.id = s.uuid(0);
            d.key = s.text(1);
            d.displayName = s.text(2);
            auto vt = attributeValueTypeFromString(s.text(3));
            if (!vt) throw PersistenceError("Unknown attribute value type");
            d.valueType = *vt;
            d.unit = s.text(4);
            d.required = s.integer(5) != 0;
            d.defaultValue = s.text(6);
            d.entityType = s.text(7);
            m.attributeDefinitions.upsert(d);
        }
    }
    {
        Stmt s(db, "SELECT uuid,name,geometry_type,layer_uuid,closed,vertex_count,vertices,created_at,modified_at,revision "
                   "FROM geometry_object ORDER BY sort_index");
        while (s.step()) {
            GeometryObject g;
            g.id = s.uuid(0);
            g.name = s.text(1);
            auto t = geometryTypeFromString(s.text(2));
            if (!t) throw PersistenceError("Unknown geometry type '" + s.text(2) + "'");
            g.type = *t;
            g.layerId = s.uuid(3);
            g.closed = s.integer(4) != 0;
            g.vertices = unpackVector<Vec3>(s.blob(6), "v1 vertices");
            if (static_cast<std::int64_t>(g.vertices.size()) != s.integer(5)) throw PersistenceError("Corrupt vertex data");
            g.meta = readMeta(s, 7);
            g.attributes = readAttributes(db, g.id);
            m.geometry.upsert(g);
        }
    }
    {
        Stmt s(db, "SELECT uuid,name,working_weekdays,hours_per_day,created_at,modified_at,revision FROM work_calendar ORDER BY sort_index");
        while (s.step()) {
            WorkCalendar c;
            c.id = s.uuid(0);
            c.name = s.text(1);
            c.shifts = {ShiftDefinition{"Working day", 0, 1440, static_cast<std::uint8_t>(s.integer(2))}};
            c.standardHoursPerDay = 24.0;
            c.description = "Converted from OpenMine 0.1 (" + std::to_string(s.real(3)) + " h/day informational)";
            c.meta = readMeta(s, 4);
            Stmt e(db, "SELECT date FROM calendar_exception WHERE calendar_uuid = ? ORDER BY date");
            e.bindUuid(1, c.id);
            while (e.step()) c.exceptions.push_back({requireDate(e.text(0), "calendar_exception"), {}, "Non-working"});
            m.calendars.upsert(c);
        }
    }
    {
        Stmt s(db, "SELECT uuid,heading_id,name,zone,decline_number,level,activity_type,chainage_from,design_width,"
                   "design_height,status,geometry_uuid,approved_design_length,design_revision,created_at,modified_at,revision "
                   "FROM mine_development ORDER BY sort_index");
        while (s.step()) {
            MineDevelopment d;
            d.id = s.uuid(0);
            d.headingId = s.text(1);
            d.name = s.text(2);
            d.zone = s.text(3);
            d.declineNumber = s.text(4);
            d.level = s.text(5);
            d.activityType = s.text(6);
            d.chainageFrom = s.real(7);
            d.designWidth = s.real(8);
            d.designHeight = s.real(9);
            d.status = s.text(10);
            d.geometryId = s.uuid(11);
            d.approvedDesignLength = s.real(12);
            d.designRevision = s.text(13);
            d.meta = readMeta(s, 14);
            d.attributes = readAttributes(db, d.id);
            m.developments.upsert(d);
        }
    }
    {
        Stmt s(db, "SELECT uuid,parent_uuid,wbs_code,name,kind,duration_days,duration_from_rate,quantity,quantity_unit,"
                   "rate_per_day,actual_quantity,start_no_earlier_than,calendar_uuid,sort_order,design_impact_flag,"
                   "design_impact_note,created_at,modified_at,revision FROM schedule_activity ORDER BY sort_index");
        while (s.step()) {
            ScheduleActivity a;
            a.id = s.uuid(0);
            a.parentId = s.uuid(1);
            a.wbsCode = s.text(2);
            a.name = s.text(3);
            auto k = activityKindFromString(s.text(4));
            if (!k) throw PersistenceError("Unknown activity kind");
            a.kind = *k;
            a.durationHours = s.real(5) * 24.0;
            a.durationFromRate = s.integer(6) != 0;
            a.quantity = s.real(7);
            a.quantityUnit = s.text(8);
            a.rate = s.real(9);
            a.rateUnit = RateUnit::PerDay;
            a.actualQuantity = s.real(10);
            if (!s.isNull(11)) {
                a.constraint = ConstraintType::StartNoEarlierThan;
                a.constraintDate = DateTime::fromDate(requireDate(s.text(11), "start_no_earlier_than"));
            }
            a.calendarId = s.uuid(12);
            a.sortOrder = static_cast<int>(s.integer(13));
            a.designImpactFlag = s.integer(14) != 0;
            a.designImpactNote = s.text(15);
            a.meta = readMeta(s, 16);
            m.activities.upsert(a);
        }
    }
    {
        Stmt s(db, "SELECT uuid,predecessor_uuid,successor_uuid,dependency_type,lag_days,created_at,modified_at,revision "
                   "FROM activity_dependency ORDER BY sort_index");
        while (s.step()) {
            ActivityDependency d;
            d.id = s.uuid(0);
            d.predecessorId = s.uuid(1);
            d.successorId = s.uuid(2);
            auto t = dependencyTypeFromString(s.text(3));
            if (!t) throw PersistenceError("Unknown dependency type");
            d.type = *t;
            d.lagHours = s.real(4) * 24.0;
            d.meta = readMeta(s, 5);
            m.dependencies.upsert(d);
        }
    }
    {
        Stmt s(db, "SELECT activity_uuid, development_uuid FROM activity_design_link ORDER BY sort_index");
        while (s.step()) m.designLinks.push_back({s.uuid(0), s.uuid(1), "MineDevelopment"});
    }
    {
        Stmt s(db, "SELECT uuid,name,captured_at,approved FROM schedule_baseline ORDER BY sort_index");
        while (s.step()) {
            ScheduleBaseline b;
            b.id = s.uuid(0);
            b.name = s.text(1);
            b.capturedAt = s.text(2);
            b.approved = s.integer(3) != 0;
            Stmt a(db, "SELECT activity_uuid,name,start_date,finish_date,duration_days,quantity FROM baseline_activity "
                       "WHERE baseline_uuid = ? ORDER BY sort_index");
            a.bindUuid(1, b.id);
            while (a.step())
                b.activities.push_back({a.uuid(0), a.text(1), DateTime::fromDate(requireDate(a.text(2), "baseline")),
                                        DateTime::fromDate(requireDate(a.text(3), "baseline").addDays(1)), a.real(4) * 24.0, a.real(5)});
            m.baselines.upsert(b);
        }
    }
    {
        Stmt s(db, "SELECT timestamp,user,action,entity_type,entity_uuid,detail FROM audit_log ORDER BY seq");
        while (s.step()) m.audit.push_back({s.text(0), s.text(1), s.text(2), s.text(3), s.uuid(4), s.text(5)});
    }
}

} // namespace openmine::persistence::detail
