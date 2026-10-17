// SPDX-License-Identifier: Apache-2.0
#include "openmine/persistence/ProjectFile.h"

#include "Sqlite.h"

#include <bit>
#include <cstring>
#include <fstream>

namespace openmine::persistence {

using detail::Db;
using detail::Stmt;

static_assert(std::endian::native == std::endian::little,
              "Vertex BLOB encoding assumes a little-endian host (x86-64 / ARM64)");

namespace {

// -----------------------------------------------------------------------------
// Schema (version 1). Migrations are an ordered list; index i upgrades i → i+1.
// -----------------------------------------------------------------------------
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

struct Migration {
    int toVersion;
    const char* description;
    const char* sql;
};
const Migration kMigrations[] = {
    {1, "Initial OpenMine schema", kSchemaV1},
};

void applyMigrations(Db& db, int fromVersion) {
    for (const auto& m : kMigrations) {
        if (m.toVersion <= fromVersion) continue;
        db.exec("BEGIN");
        try {
            db.exec(m.sql);
            Stmt s(db, "INSERT INTO schema_migrations(version, applied_at, description) VALUES (?,?,?)");
            s.bind(1, m.toVersion).bind(2, utcTimestampNow()).bind(3, m.description).run();
            db.exec("PRAGMA user_version = " + std::to_string(m.toVersion));
            db.exec("COMMIT");
        } catch (...) {
            db.exec("ROLLBACK");
            throw;
        }
    }
}

Date requireDate(const std::string& s, const char* what) {
    auto d = Date::parse(s);
    if (!d) throw PersistenceError(std::string("Corrupt date in ") + what + ": '" + s + "'");
    return *d;
}

std::vector<unsigned char> encodeVertices(const std::vector<Vec3>& v) {
    std::vector<unsigned char> out(v.size() * 24);
    for (std::size_t i = 0; i < v.size(); ++i) {
        const double xyz[3] = {v[i].x, v[i].y, v[i].z};
        std::memcpy(out.data() + i * 24, xyz, 24);
    }
    return out;
}

std::vector<Vec3> decodeVertices(const std::vector<unsigned char>& b, std::int64_t count) {
    if (static_cast<std::int64_t>(b.size()) != count * 24) throw PersistenceError("Corrupt vertex data");
    std::vector<Vec3> out(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < out.size(); ++i) {
        double xyz[3];
        std::memcpy(xyz, b.data() + i * 24, 24);
        out[i] = {xyz[0], xyz[1], xyz[2]};
    }
    return out;
}

void bindMeta(Stmt& s, int first, const EntityMetadata& m) {
    s.bind(first, m.createdAt).bind(first + 1, m.modifiedAt).bind(first + 2, m.revision);
}
EntityMetadata readMeta(const Stmt& s, int first) {
    return {s.text(first), s.text(first + 1), s.integer(first + 2)};
}

void writeAttributes(Db& db, const Uuid& id, const char* type, const AttributeMap& attrs) {
    Stmt s(db, "INSERT INTO attribute_value(entity_uuid, entity_type, key, value) VALUES (?,?,?,?)");
    for (const auto& [k, v] : attrs) s.bindUuid(1, id).bind(2, type).bind(3, k).bind(4, v).run();
}

// -----------------------------------------------------------------------------
void writeModel(Db& db, const ProjectModel& m) {
    {
        Stmt s(db, "INSERT INTO project_info VALUES (1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        const auto& i = m.info;
        s.bindUuid(1, i.id).bind(2, i.name).bind(3, i.description).bind(4, i.scheduleStart.toString());
        if (i.statusDate) s.bind(5, i.statusDate->toString()); else s.bindNull(5);
        s.bind(6, i.crs.name).bind(7, i.crs.authorityCode).bind(8, i.crs.verticalDatum).bind(9, i.crs.linearUnit)
            .bind(10, i.crs.isLocalMineGrid).bind(11, i.crs.southOrientatedAxes).bindUuid(12, i.defaultCalendarId);
        bindMeta(s, 13, i.meta);
        s.bind(16, std::string("OpenMine Suite 0.1.0"));
        s.run();
    }
    {
        Stmt s(db, "INSERT INTO cad_layer VALUES (?,?,?,?,?,?,?,?,?)");
        int idx = 0;
        for (const auto& l : m.layers.all()) {
            s.bindUuid(1, l.id).bind(2, idx++).bind(3, l.name).bind(4, static_cast<std::int64_t>(l.colorRgb))
                .bind(5, l.visible).bind(6, l.locked);
            bindMeta(s, 7, l.meta);
            s.run();
        }
    }
    {
        Stmt s(db, "INSERT INTO geometry_object VALUES (?,?,?,?,?,?,?,?,?,?,?)");
        int idx = 0;
        for (const auto& g : m.geometry.all()) {
            const auto blob = encodeVertices(g.vertices);
            s.bindUuid(1, g.id).bind(2, idx++).bind(3, g.name).bind(4, toString(g.type)).bindUuid(5, g.layerId)
                .bind(6, g.closed).bind(7, static_cast<std::int64_t>(g.vertices.size()))
                .bindBlob(8, blob.data(), static_cast<int>(blob.size()));
            bindMeta(s, 9, g.meta);
            s.run();
            writeAttributes(db, g.id, "GeometryObject", g.attributes);
        }
    }
    {
        Stmt s(db, "INSERT INTO attribute_definition VALUES (?,?,?,?,?,?,?,?,?)");
        int idx = 0;
        for (const auto& d : m.attributeDefinitions.all()) {
            s.bindUuid(1, d.id).bind(2, idx++).bind(3, d.entityType).bind(4, d.key).bind(5, d.displayName)
                .bind(6, toString(d.valueType)).bind(7, d.unit).bind(8, d.required).bind(9, d.defaultValue).run();
        }
    }
    {
        Stmt s(db, "INSERT INTO work_calendar VALUES (?,?,?,?,?,?,?,?)");
        Stmt e(db, "INSERT INTO calendar_exception VALUES (?,?)");
        int idx = 0;
        for (const auto& c : m.calendars.all()) {
            s.bindUuid(1, c.id).bind(2, idx++).bind(3, c.name).bind(4, static_cast<int>(c.workingWeekdays))
                .bind(5, c.hoursPerDay);
            bindMeta(s, 6, c.meta);
            s.run();
            for (const auto& d : c.nonWorkingDates) e.bindUuid(1, c.id).bind(2, d.toString()).run();
        }
    }
    {
        Stmt s(db, "INSERT INTO mine_development VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        int idx = 0;
        for (const auto& d : m.developments.all()) {
            s.bindUuid(1, d.id).bind(2, idx++).bind(3, d.headingId).bind(4, d.name).bind(5, d.zone)
                .bind(6, d.declineNumber).bind(7, d.level).bind(8, d.activityType).bind(9, d.chainageFrom)
                .bind(10, d.designWidth).bind(11, d.designHeight).bind(12, d.status).bindUuid(13, d.geometryId)
                .bind(14, d.approvedDesignLength).bind(15, d.designRevision);
            bindMeta(s, 16, d.meta);
            s.run();
            writeAttributes(db, d.id, "MineDevelopment", d.attributes);
        }
    }
    {
        Stmt s(db, "INSERT INTO schedule_activity VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        int idx = 0;
        for (const auto& a : m.activities.all()) {
            s.bindUuid(1, a.id).bind(2, idx++).bindUuid(3, a.parentId).bind(4, a.wbsCode).bind(5, a.name)
                .bind(6, toString(a.kind)).bind(7, a.durationDays).bind(8, a.durationFromRate).bind(9, a.quantity)
                .bind(10, a.quantityUnit).bind(11, a.ratePerDay).bind(12, a.actualQuantity);
            if (a.startNoEarlierThan) s.bind(13, a.startNoEarlierThan->toString()); else s.bindNull(13);
            s.bindUuid(14, a.calendarId).bind(15, a.sortOrder).bind(16, a.designImpactFlag).bind(17, a.designImpactNote);
            bindMeta(s, 18, a.meta);
            s.run();
        }
    }
    {
        Stmt s(db, "INSERT INTO activity_dependency VALUES (?,?,?,?,?,?,?,?,?)");
        int idx = 0;
        for (const auto& d : m.dependencies.all()) {
            s.bindUuid(1, d.id).bind(2, idx++).bindUuid(3, d.predecessorId).bindUuid(4, d.successorId)
                .bind(5, toString(d.type)).bind(6, d.lagDays);
            bindMeta(s, 7, d.meta);
            s.run();
        }
    }
    {
        Stmt s(db, "INSERT INTO activity_design_link VALUES (?,?,?)");
        int idx = 0;
        for (const auto& l : m.designLinks) s.bindUuid(1, l.activityId).bindUuid(2, l.developmentId).bind(3, idx++).run();
    }
    {
        Stmt s(db, "INSERT INTO schedule_baseline VALUES (?,?,?,?,?)");
        Stmt a(db, "INSERT INTO baseline_activity VALUES (?,?,?,?,?,?,?,?)");
        int idx = 0;
        for (const auto& b : m.baselines) {
            s.bindUuid(1, b.id).bind(2, idx++).bind(3, b.name).bind(4, b.capturedAt).bind(5, b.approved).run();
            int j = 0;
            for (const auto& x : b.activities)
                a.bindUuid(1, b.id).bindUuid(2, x.activityId).bind(3, j++).bind(4, x.name).bind(5, x.start.toString())
                    .bind(6, x.finish.toString()).bind(7, x.durationDays).bind(8, x.quantity).run();
        }
    }
    {
        Stmt s(db, "INSERT INTO audit_log(timestamp, user, action, entity_type, entity_uuid, detail) VALUES (?,?,?,?,?,?)");
        for (const auto& e : m.audit)
            s.bind(1, e.timestamp).bind(2, e.user).bind(3, e.action).bind(4, e.entityType).bindUuid(5, e.entityId)
                .bind(6, e.detail).run();
    }
}

AttributeMap readAttributes(Db& db, const Uuid& id) {
    AttributeMap out;
    Stmt s(db, "SELECT key, value FROM attribute_value WHERE entity_uuid = ?");
    s.bindUuid(1, id);
    while (s.step()) out[s.text(0)] = s.text(1);
    return out;
}

void readModel(Db& db, ProjectModel& m) {
    {
        Stmt s(db, "SELECT uuid,name,description,schedule_start,status_date,crs_name,crs_authority_code,vertical_datum,"
                   "linear_unit,is_local_mine_grid,south_orientated,default_calendar,created_at,modified_at,revision "
                   "FROM project_info WHERE id = 1");
        if (!s.step()) throw PersistenceError("Project file has no project_info record");
        auto& i = m.info;
        i.id = s.uuid(0);
        i.name = s.text(1);
        i.description = s.text(2);
        i.scheduleStart = requireDate(s.text(3), "schedule_start");
        if (!s.isNull(4)) i.statusDate = requireDate(s.text(4), "status_date");
        i.crs.name = s.text(5);
        i.crs.authorityCode = s.text(6);
        i.crs.verticalDatum = s.text(7);
        i.crs.linearUnit = s.text(8);
        i.crs.isLocalMineGrid = s.integer(9) != 0;
        i.crs.southOrientatedAxes = s.integer(10) != 0;
        i.defaultCalendarId = s.uuid(11);
        i.meta = readMeta(s, 12);
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
            g.vertices = decodeVertices(s.blob(6), s.integer(5));
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
            c.workingWeekdays = static_cast<std::uint8_t>(s.integer(2));
            c.hoursPerDay = s.real(3);
            c.meta = readMeta(s, 4);
            Stmt e(db, "SELECT date FROM calendar_exception WHERE calendar_uuid = ? ORDER BY date");
            e.bindUuid(1, c.id);
            while (e.step()) c.nonWorkingDates.push_back(requireDate(e.text(0), "calendar_exception"));
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
            a.durationDays = s.real(5);
            a.durationFromRate = s.integer(6) != 0;
            a.quantity = s.real(7);
            a.quantityUnit = s.text(8);
            a.ratePerDay = s.real(9);
            a.actualQuantity = s.real(10);
            if (!s.isNull(11)) a.startNoEarlierThan = requireDate(s.text(11), "start_no_earlier_than");
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
            d.lagDays = s.real(4);
            d.meta = readMeta(s, 5);
            m.dependencies.upsert(d);
        }
    }
    {
        Stmt s(db, "SELECT activity_uuid, development_uuid FROM activity_design_link ORDER BY sort_index");
        while (s.step()) m.designLinks.push_back({s.uuid(0), s.uuid(1)});
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
                b.activities.push_back({a.uuid(0), a.text(1), requireDate(a.text(2), "baseline"),
                                        requireDate(a.text(3), "baseline"), a.real(4), a.real(5)});
            m.baselines.push_back(std::move(b));
        }
    }
    {
        Stmt s(db, "SELECT timestamp,user,action,entity_type,entity_uuid,detail FROM audit_log ORDER BY seq");
        while (s.step()) m.audit.push_back({s.text(0), s.text(1), s.text(2), s.text(3), s.uuid(4), s.text(5)});
    }
}

std::vector<std::string> verify(Db& db) {
    std::vector<std::string> problems;
    {
        Stmt s(db, "PRAGMA integrity_check");
        while (s.step())
            if (s.text(0) != "ok") problems.push_back("integrity_check: " + s.text(0));
    }
    {
        Stmt s(db, "PRAGMA foreign_key_check");
        while (s.step()) problems.push_back("foreign key violation in table " + s.text(0));
    }
    {
        // Polymorphic attribute values must reference an existing entity.
        Stmt s(db, "SELECT entity_uuid FROM attribute_value WHERE entity_uuid NOT IN "
                   "(SELECT uuid FROM geometry_object UNION SELECT uuid FROM mine_development)");
        while (s.step()) problems.push_back("orphaned attribute value for " + s.text(0));
    }
    return problems;
}

std::filesystem::path withSuffix(const std::filesystem::path& p, const char* suffix) {
    auto s = p;
    s += suffix;
    return s;
}

} // namespace

const char* schemaSql() { return kSchemaV1; }

void saveProject(Project& project, const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    if (file.empty()) throw PersistenceError("No file name given");
    const fs::path tmp = withSuffix(file, ".saving");
    std::error_code ec;
    fs::remove(tmp, ec);
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);

    try {
        {
            Db db(tmp, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
            db.exec("PRAGMA journal_mode = DELETE");
            db.exec("PRAGMA foreign_keys = ON");
            db.exec("PRAGMA application_id = " + std::to_string(kApplicationId));
            applyMigrations(db, 0);
            db.exec("BEGIN");
            try {
                db.exec("PRAGMA defer_foreign_keys = ON");
                writeModel(db, project.model());
                db.exec("COMMIT");
            } catch (...) {
                db.exec("ROLLBACK");
                throw;
            }
            auto problems = verify(db);
            if (!problems.empty()) throw PersistenceError("Save verification failed: " + problems.front());
        }
        if (fs::exists(file)) fs::copy_file(file, withSuffix(file, ".bak"), fs::copy_options::overwrite_existing);
        fs::rename(tmp, file); // atomic replace on the same volume
    } catch (const std::exception& e) {
        fs::remove(tmp, ec);
        throw PersistenceError(std::string("Project was NOT saved (existing file unchanged): ") + e.what());
    }
    project.setFilePath(file);
    project.commands().setClean();
}

std::unique_ptr<Project> loadProject(const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    if (!fs::exists(file)) throw PersistenceError("File not found: " + file.string());

    int version = 0;
    {
        Db db(file, SQLITE_OPEN_READONLY);
        Stmt app(db, "PRAGMA application_id");
        app.step();
        if (app.integer(0) != kApplicationId) throw PersistenceError("Not an OpenMine project file: " + file.string());
        Stmt uv(db, "PRAGMA user_version");
        uv.step();
        version = static_cast<int>(uv.integer(0));
    }
    if (version > kCurrentSchemaVersion)
        throw PersistenceError("Project was written by a newer OpenMine version (schema " + std::to_string(version) +
                               "); this build supports up to schema " + std::to_string(kCurrentSchemaVersion));
    if (version < 1) throw PersistenceError("Project file has an invalid schema version");

    fs::path source = file;
    fs::path migrated;
    if (version < kCurrentSchemaVersion) {
        migrated = fs::temp_directory_path() / ("openmine-migrate-" + Uuid::generate().toString() + ".openmine");
        fs::copy_file(file, migrated);
        Db db(migrated, SQLITE_OPEN_READWRITE);
        applyMigrations(db, version);
        source = migrated;
    }

    auto project = std::make_unique<Project>();
    {
        Db db(source, SQLITE_OPEN_READONLY);
        auto problems = verify(db);
        if (!problems.empty()) throw PersistenceError("Project file failed integrity check: " + problems.front());
        EventBus::Mute mute(project->events());
        readModel(db, project->mutableModel());
    }
    if (!migrated.empty()) {
        std::error_code ec;
        fs::remove(migrated, ec);
        project->appendAudit("Migrate", "Project", project->model().info.id,
                             "Schema migrated from v" + std::to_string(version) + " to v" + std::to_string(kCurrentSchemaVersion));
    }
    project->setFilePath(file);
    project->commands().setClean();
    return project;
}

IntegrityReport checkProjectFile(const std::filesystem::path& file) {
    IntegrityReport r;
    try {
        Db db(file, SQLITE_OPEN_READONLY);
        Stmt uv(db, "PRAGMA user_version");
        uv.step();
        r.schemaVersion = static_cast<int>(uv.integer(0));
        r.problems = verify(db);
        r.ok = r.problems.empty();
    } catch (const std::exception& e) {
        r.problems.push_back(e.what());
    }
    return r;
}

} // namespace openmine::persistence
