// SPDX-License-Identifier: Apache-2.0
#include "openmine/persistence/ProjectFile.h"
#include "TempDir.h"
#include "openmine/assets/AssetStore.h"
#include "openmine/cad/Edit.h"
#include "openmine/project/Attachments.h"
#include "openmine/project/ProjectEditor.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>
#include <fstream>
#include <iterator>

using namespace openmine;
namespace fs = std::filesystem;

namespace {
test::TempDir tempDir() { return test::TempDir("openmine-test-"); }

struct Rich {
    std::unique_ptr<Project> p = Project::createNew("Jupiter Mine — Level 675 ✓", Date::fromYmd(2026, 11, 2));
    Uuid geom, arcGeom, meshGeom, dev, stope, block, sid, aid, bid, res, cyc, cloud, reg, ctrl, hole, crs, rev;
};

Rich richProject() {
    Rich r;
    auto& p = *r.p;
    ProjectEditor ed(p);
    CoordinateReferenceSystem crs;
    crs.name = "Cape / Lo25";
    crs.authorityCode = "EPSG:22285";
    crs.kind = CrsKind::Projected;
    crs.horizontalDatum = "Cape";
    crs.axisDescription = "Y westing, X southing";
    r.crs = ed.add(crs);
    ProjectInfo info = p.model().info;
    info.projectCrsId = r.crs;
    info.southOrientatedAxes = true;
    info.statusDate = DateTime::fromDate(Date::fromYmd(2026, 11, 20), 6 * 60);
    ed.setProjectInfo(info);

    AttributeDefinition def;
    def.entityType = "MineDevelopment";
    def.key = "ground_support";
    def.displayName = "Ground support class";
    ed.add(def);

    GeometryObject g;
    g.name = "LEG2 centreline";
    g.layerId = p.model().layers.all()[1].id;
    g.vertices = {{-25000.125, 2900000.5, 1200.0}, {-25080.0, 2900060.25, 1190.0}, {-25120.75, 2900150.0, 1180.5}};
    g.bulges = {0.25, 0.0, 0.0};
    r.geom = ed.add(g);

    GeometryObject arc;
    arc.type = GeometryType::Arc;
    arc.layerId = p.model().layers.all()[0].id;
    arc.arc = {{1, 2, 3}, 4.5, {0, 0, 1}, {1, 0, 0}, 0.25, 2.5};
    arc.colorOverride = 0xFF8800;
    arc.transparency = 0.3;
    r.arcGeom = ed.add(arc);

    GeometryObject mesh;
    mesh.type = GeometryType::Mesh;
    mesh.name = "Stope shape";
    mesh.layerId = p.model().layers.all()[0].id;
    auto md = std::make_shared<MeshData>();
    md->vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    md->triangles = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
    md->vertexScalars = {0.1f, 0.2f, 0.3f, 0.4f};
    md->scalarName = "distance";
    mesh.mesh = md;
    r.meshGeom = ed.add(mesh);

    GeometryObject text;
    text.type = GeometryType::Text;
    text.layerId = p.model().layers.all()[0].id;
    text.vertices = {{5, 5, 0}};
    text.text = {"LEG2 675", 2.5, 0.5, TextAlign::Center};
    ed.add(text);

    MineLevel lv;
    lv.name = "675 Level";
    lv.code = "675";
    lv.elevation = 1190;
    const Uuid lvId = ed.add(lv);

    MineDevelopment d;
    d.headingId = "LEG2_675_D3_DEC";
    d.level = "675";
    d.levelId = lvId;
    d.zone = "Legadima 2";
    d.geometryId = r.geom;
    d.profileId = p.model().profiles.all()[0].id;
    d.materialId = p.model().materials.all()[0].id;
    d.designWidth = 5.5;
    d.designHeight = 5.5;
    d.attributes["ground_support"] = "Class B";
    r.dev = ed.add(d);

    Stope st;
    st.name = "S675-01";
    st.geometryId = r.meshGeom;
    st.levelId = lvId;
    st.grade = 4.2;
    r.stope = ed.add(st);
    MiningBlock blk;
    blk.name = "S675-01 B1";
    blk.stopeId = r.stope;
    blk.geometryId = r.meshGeom;
    r.block = ed.add(blk);

    ScheduleActivity s;
    s.name = "Level 675";
    s.kind = ActivityKind::Summary;
    s.wbsCode = "675";
    r.sid = ed.add(s);
    ScheduleActivity a;
    a.name = "Develop LEG2_675_D3_DEC";
    a.parentId = r.sid;
    a.durationFromRate = true;
    a.rate = 2.5;
    a.rateUnit = RateUnit::PerShift;
    a.quantity = 150.0;
    a.actualStart = DateTime::fromDate(Date::fromYmd(2026, 11, 4), 6 * 60);
    a.constraint = ConstraintType::StartNoEarlierThan;
    a.constraintDate = DateTime::fromDate(Date::fromYmd(2026, 11, 4));
    a.designImpactFlag = true;
    a.designImpactNote = "note";
    a.priority = 5;
    a.workArea = "LEG2";
    r.aid = ed.add(a);
    ScheduleActivity b;
    b.name = "Install services";
    b.parentId = r.sid;
    b.durationHours = 30;
    b.calendarId = p.model().calendars.all()[1].id;
    r.bid = ed.add(b);
    ActivityDependency dep;
    dep.predecessorId = r.aid;
    dep.successorId = r.bid;
    dep.type = DependencyType::StartToStart;
    dep.lagHours = 2.5;
    ed.add(dep);
    ed.linkActivityToDesign(r.aid, r.dev);
    ed.linkActivityToDesign(r.aid, r.block);

    Resource jumbo;
    jumbo.name = "Jumbo 1";
    jumbo.capacity = 1;
    jumbo.productivityFactor = 1.1;
    r.res = ed.add(jumbo);
    ActivityResourceAssignment asg;
    asg.activityId = r.aid;
    asg.resourceId = r.res;
    asg.drivesRate = true;
    ed.add(asg);
    DevelopmentCycle cyc;
    cyc.name = "Twin boom";
    r.cyc = ed.add(cyc);
    ProductionActual act;
    act.activityId = r.aid;
    act.developmentId = r.dev;
    act.date = Date::fromYmd(2026, 11, 5);
    act.quantity = 7.5;
    act.faceChainage = 7.5;
    ed.add(act);
    ProductionTarget tg;
    tg.name = "Nov";
    tg.periodStart = Date::fromYmd(2026, 11, 1);
    tg.periodEnd = Date::fromYmd(2026, 11, 30);
    tg.quantity = 120;
    ed.add(tg);
    Scenario sc;
    sc.name = "Two jumbos";
    sc.overrides = {{r.res, "capacity", "2"}, {Uuid{}, "areaLimit", "LEG2=1"}};
    sc.levelling = LevellingMode::Heuristic;
    ed.add(sc);

    WorkCalendar cal = *p.model().calendars.find(p.model().info.defaultCalendarId);
    cal.exceptions = {{Date::fromYmd(2026, 12, 16), {}, "Reconciliation Day"}, {Date::fromYmd(2026, 12, 24), {{0, 720}}, "Half day"}};
    ed.updateCalendar(cal);

    ScheduleBaseline bl;
    bl.name = "BL-2026-11";
    bl.activities.push_back({r.aid, a.name, DateTime::fromDate(Date::fromYmd(2026, 11, 4)), DateTime::fromDate(Date::fromYmd(2026, 12, 15)), 720, 150});
    ed.add(bl);

    // Survey
    SurveyControlPoint cp;
    cp.name = "SS675-01";
    cp.position = {-25010.0, 2900005.0, 1201.5};
    cp.horizontalSigma = 0.003;
    cp.surveyDate = Date::fromYmd(2026, 10, 1);
    r.ctrl = ed.add(cp);
    const Asset asset = p.assets().writeBytes("octree placeholder", AssetKind::PointCloud, "bin");
    ed.add(asset);
    PointCloudDataset pc;
    pc.name = "Scan 675";
    pc.asset.id = asset.id;
    pc.pointCount = 12345;
    pc.storedBounds = {{0, 0, 0}, {10, 20, 30}};
    pc.attributes.intensity = true;
    pc.attributes.scalarFields = {"distance"};
    pc.transform = Transform3::translation({1, 2, 3});
    pc.sourceFormat = "LAZ";
    pc.developmentId = r.dev;
    r.cloud = ed.add(pc);
    Registration reg;
    reg.datasetId = r.cloud;
    reg.method = RegistrationMethod::ControlPoints;
    reg.transform = Transform3::rotation({0, 0, 1}, 0.01);
    reg.pairs = {{"T1", {1, 2, 3}, {4, 5, 6}, r.ctrl, PairRole::Fit, {0.001, -0.002, 0.0}},
                 {"T2", {7, 8, 9}, {10, 11, 12}, Uuid{}, PairRole::Check, {0.004, 0, 0.001}}};
    reg.fitRms = 0.002;
    reg.checkRms = 0.004;
    reg.checkCount = 1;
    r.reg = ed.add(reg);

    // Geology
    Drillhole h;
    h.holeId = "DD001";
    h.collar = {-25000, 2900000, 1250};
    h.length = 200;
    h.surveys = {{0, 90, -60}, {100, 92, -58}};
    h.intervals = {{0, 1.5, {{"Au", 0.1}}, {{"lith", "QZ"}}}, {1.5, 3, {{"Au", 5.2}}, {{"lith", "BIF"}}}};
    r.hole = ed.add(h);
    StructuralMeasurement sm;
    sm.setName = "J1";
    sm.dip = 65;
    sm.dipDirection = 120;
    ed.add(sm);
    GeotechClassification gc;
    gc.method = GeotechMethod::QSystem;
    gc.value = 4.2;
    gc.rockClass = "Fair";
    gc.developmentId = r.dev;
    ed.add(gc);
    r.rev = ed.add(DesignRevision{Uuid{}, r.dev, "MineDevelopment", "A", "{}", 150, 4000, false, "", "", "first", "", ""});
    return r;
}
} // namespace

TEST(Persistence, RoundTripPreservesEveryEntityType) {
    auto dir = tempDir();
    auto file = dir / "jupiter.openmine";
    auto r = richProject();
    auto& p = *r.p;
    persistence::saveProject(p, file);
    EXPECT_FALSE(p.isModified());
    EXPECT_TRUE(fs::exists(dir / "jupiter.openmine-assets"));

    persistence::LoadReport rep;
    auto q = persistence::loadProject(file, &rep);
    EXPECT_FALSE(rep.migrated);
    EXPECT_TRUE(rep.warnings.empty()) << rep.warnings.front();
    const auto& a = p.model();
    const auto& b = q->model();
#define OPENMINE_CHECK_COUNT(T, member, Kind) EXPECT_EQ(b.member.size(), a.member.size()) << #member;
    OPENMINE_ENTITY_TYPES(OPENMINE_CHECK_COUNT)
#undef OPENMINE_CHECK_COUNT
    EXPECT_EQ(b.info.name, a.info.name);
    EXPECT_EQ(b.info.statusDate->toString(), "2026-11-20 06:00");
    EXPECT_EQ(b.projectCrs()->authorityCode, "EPSG:22285");
    EXPECT_TRUE(b.info.southOrientatedAxes);

    const auto* g = b.geometry.find(r.geom);
    ASSERT_TRUE(g);
    EXPECT_EQ(g->vertices, a.geometry.find(r.geom)->vertices); // bit-exact doubles
    EXPECT_EQ(g->bulges, a.geometry.find(r.geom)->bulges);
    const auto* arc = b.geometry.find(r.arcGeom);
    EXPECT_EQ(arc->arc, a.geometry.find(r.arcGeom)->arc);
    EXPECT_EQ(arc->colorOverride.value_or(0), 0xFF8800u);
    EXPECT_DOUBLE_EQ(arc->transparency.value_or(0), 0.3);
    const auto* mesh = b.geometry.find(r.meshGeom);
    ASSERT_TRUE(mesh->mesh);
    EXPECT_EQ(mesh->mesh->triangles, a.geometry.find(r.meshGeom)->mesh->triangles);
    EXPECT_EQ(mesh->mesh->vertexScalars, a.geometry.find(r.meshGeom)->mesh->vertexScalars);
    EXPECT_EQ(mesh->mesh->scalarName, "distance");
    bool foundText = false;
    for (const auto& x : b.geometry.all())
        if (x.type == GeometryType::Text) {
            foundText = true;
            EXPECT_EQ(x.text.text, "LEG2 675");
            EXPECT_EQ(x.text.align, TextAlign::Center);
        }
    EXPECT_TRUE(foundText);

    const auto* d = b.developments.find(r.dev);
    EXPECT_EQ(d->attributes.at("ground_support"), "Class B");
    EXPECT_EQ(d->profileId, a.developments.find(r.dev)->profileId);
    const auto* act = b.activities.find(r.aid);
    EXPECT_EQ(act->rateUnit, RateUnit::PerShift);
    EXPECT_EQ(act->actualStart->toString(), "2026-11-04 06:00");
    EXPECT_EQ(act->constraint, ConstraintType::StartNoEarlierThan);
    EXPECT_EQ(act->workArea, "LEG2");
    EXPECT_EQ(b.designLinks.size(), 2u);
    EXPECT_EQ(b.designLinks[1].designType, "MiningBlock");
    EXPECT_DOUBLE_EQ(b.dependencies.all()[0].lagHours, 2.5);
    EXPECT_EQ(b.calendars.find(a.info.defaultCalendarId)->exceptions, a.calendars.find(a.info.defaultCalendarId)->exceptions);
    EXPECT_EQ(b.scenarios.all()[0].overrides, a.scenarios.all()[0].overrides);
    EXPECT_EQ(b.baselines.all()[0].activities, a.baselines.all()[0].activities);
    EXPECT_EQ(b.actuals.all()[0].faceChainage.value_or(0), 7.5);
    const auto* reg = b.registrations.find(r.reg);
    ASSERT_EQ(reg->pairs.size(), 2u);
    EXPECT_EQ(reg->pairs, a.registrations.find(r.reg)->pairs);
    EXPECT_EQ(reg->transform, a.registrations.find(r.reg)->transform);
    EXPECT_EQ(b.pointClouds.find(r.cloud)->attributes, a.pointClouds.find(r.cloud)->attributes);
    EXPECT_EQ(b.pointClouds.find(r.cloud)->storedBounds, a.pointClouds.find(r.cloud)->storedBounds);
    EXPECT_EQ(b.drillholes.find(r.hole)->surveys, a.drillholes.find(r.hole)->surveys);
    EXPECT_EQ(b.drillholes.find(r.hole)->intervals, a.drillholes.find(r.hole)->intervals);
    EXPECT_EQ(b.audit.size(), a.audit.size());
    EXPECT_EQ(q->assets().readBytes(*b.assets.all().begin()), "octree placeholder");

    // Save → load → save again is stable.
    persistence::saveProject(*q, file);
    EXPECT_TRUE(fs::exists(dir / "jupiter.openmine.bak"));
    EXPECT_TRUE(persistence::checkProjectFile(file, true).ok);
}

TEST(Persistence, SaveAsCopiesSidecarAssets) {
    auto dir = tempDir();
    auto r = richProject();
    persistence::saveProject(*r.p, dir / "a.openmine");
    persistence::saveProject(*r.p, dir / "sub" / "b.openmine");
    fs::remove_all(dir / "a.openmine-assets");
    auto q = persistence::loadProject(dir / "sub" / "b.openmine");
    EXPECT_EQ(q->assets().readBytes(q->model().assets.all()[0]), "octree placeholder");
    // Missing sidecar is reported, not fatal.
    persistence::LoadReport rep;
    auto a = persistence::loadProject(dir / "a.openmine", &rep);
    EXPECT_FALSE(rep.warnings.empty());
}

TEST(Persistence, FailedSaveLeavesExistingFileUntouched) {
    auto dir = tempDir();
    auto file = dir / "p.openmine";
    auto r = richProject();
    persistence::saveProject(*r.p, file);
    const auto sizeBefore = fs::file_size(file);
    r.p->mutableModel().designLinks.push_back({Uuid::generate(), Uuid::generate(), "MineDevelopment"});
    EXPECT_THROW(persistence::saveProject(*r.p, file), persistence::PersistenceError);
    EXPECT_EQ(fs::file_size(file), sizeBefore);
    EXPECT_FALSE(fs::exists(dir / "p.openmine.saving"));
    EXPECT_NO_THROW(persistence::loadProject(file));
}

TEST(Persistence, RejectsForeignAndNewerFiles) {
    auto dir = tempDir();
    std::ofstream(dir / "x.openmine") << "this is not sqlite";
    EXPECT_THROW(persistence::loadProject(dir / "x.openmine"), persistence::PersistenceError);
    auto file = dir / "p.openmine";
    auto p = Project::createNew("P", Date::fromYmd(2026, 1, 1));
    persistence::saveProject(*p, file);
    sqlite3* db = nullptr;
    sqlite3_open(file.string().c_str(), &db);
    sqlite3_exec(db, "PRAGMA user_version = 99", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    try {
        persistence::loadProject(file);
        FAIL() << "expected PersistenceError";
    } catch (const persistence::PersistenceError& e) {
        EXPECT_NE(std::string(e.what()).find("newer"), std::string::npos);
    }
    EXPECT_THROW(persistence::loadProject(dir / "missing.openmine"), persistence::PersistenceError);
}

TEST(Persistence, MigratesSchemaVersion1Projects) {
    auto dir = tempDir();
    const auto file = dir / "v1.openmine";
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(file.string().c_str(), &db), SQLITE_OK);
    std::string sql = "PRAGMA application_id = " + std::to_string(persistence::kApplicationId) + ";" + persistence::legacySchemaV1Sql() + R"SQL(
INSERT INTO schema_migrations VALUES (1, '2026-10-17T09:00:00Z', 'Initial OpenMine schema');
INSERT INTO work_calendar VALUES ('11111111-1111-4111-8111-111111111111', 0, 'Standard 5-day', 31, 24.0, 't', 't', 1);
INSERT INTO calendar_exception VALUES ('11111111-1111-4111-8111-111111111111', '2026-12-16');
INSERT INTO project_info VALUES (1, '22222222-2222-4222-8222-222222222222', 'Old project', '', '2026-11-02', '2026-11-16',
  'Hartebeesthoek94 / Lo25', 'EPSG:2051', 'MSL', 'metre', 0, 1, '11111111-1111-4111-8111-111111111111', 't', 't', 3, 'OpenMine Suite 0.1.0');
INSERT INTO cad_layer VALUES ('33333333-3333-4333-8333-333333333333', 0, '0', 13684944, 1, 0, 't', 't', 1);
INSERT INTO geometry_object VALUES ('44444444-4444-4444-8444-444444444444', 0, 'CL', 'Polyline', '33333333-3333-4333-8333-333333333333', 0, 2,
  X'000000000000000000000000000000000000000000000000000000000000594000000000000000000000000000000000', 't', 't', 1);
INSERT INTO mine_development VALUES ('55555555-5555-4555-8555-555555555555', 0, 'H1', 'H1', '', '', '675', 'Development', 0, 5, 5,
  'Planned', '44444444-4444-4444-8444-444444444444', 100.0, 'A', 't', 't', 1);
INSERT INTO schedule_activity VALUES ('66666666-6666-4666-8666-666666666666', 0, NULL, '', 'Develop H1', 'Task', 1, 1, 100, 'm', 5, 20,
  '2026-11-04', NULL, 10, 0, '', 't', 't', 1);
INSERT INTO schedule_activity VALUES ('77777777-7777-4777-8777-777777777777', 1, NULL, '', 'Next', 'Task', 3, 0, 0, 'm', 0, 0,
  NULL, NULL, 20, 0, '', 't', 't', 1);
INSERT INTO activity_dependency VALUES ('88888888-8888-4888-8888-888888888888', 0, '66666666-6666-4666-8666-666666666666',
  '77777777-7777-4777-8777-777777777777', 'FS', 2, 't', 't', 1);
INSERT INTO activity_design_link VALUES ('66666666-6666-4666-8666-666666666666', '55555555-5555-4555-8555-555555555555', 0);
INSERT INTO schedule_baseline VALUES ('99999999-9999-4999-8999-999999999999', 0, 'BL1', 't', 1);
INSERT INTO baseline_activity VALUES ('99999999-9999-4999-8999-999999999999', '66666666-6666-4666-8666-666666666666', 0, 'Develop H1',
  '2026-11-04', '2026-12-01', 20, 100);
INSERT INTO audit_log(timestamp, user, action, entity_type, entity_uuid, detail) VALUES ('t', 'u', 'Create', 'Project', NULL, 'x');
PRAGMA user_version = 1;
)SQL";
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    sqlite3_close(db);

    persistence::LoadReport rep;
    auto p = persistence::loadProject(file, &rep);
    EXPECT_EQ(rep.fileSchemaVersion, 1);
    EXPECT_TRUE(rep.migrated);
    const auto& m = p->model();
    EXPECT_EQ(m.projectCrs()->authorityCode, "EPSG:2051");
    EXPECT_TRUE(m.info.southOrientatedAxes);
    EXPECT_EQ(m.info.statusDate->toString(), "2026-11-16 00:00");
    const auto* cal = m.calendars.all().data();
    ASSERT_EQ(cal->shifts.size(), 1u);
    EXPECT_EQ(cal->shifts[0].weekdays, 31);
    ASSERT_EQ(cal->exceptions.size(), 1u);
    const auto* a = m.activities.find(*Uuid::parse("66666666-6666-4666-8666-666666666666"));
    EXPECT_DOUBLE_EQ(a->rate, 5);
    EXPECT_EQ(a->rateUnit, RateUnit::PerDay);
    EXPECT_EQ(a->constraint, ConstraintType::StartNoEarlierThan);
    EXPECT_DOUBLE_EQ(m.dependencies.all()[0].lagHours, 48);
    EXPECT_DOUBLE_EQ(m.activities.find(*Uuid::parse("77777777-7777-4777-8777-777777777777"))->durationHours, 72);
    EXPECT_EQ(m.baselines.all()[0].activities[0].finish.toString(), "2026-12-02 00:00");
    EXPECT_DOUBLE_EQ(m.geometry.all()[0].vertices[1].x, 100.0);
    ASSERT_EQ(m.designLinks.size(), 1u);
    EXPECT_EQ(m.audit.back().action, "Migrate");
    // Saving writes schema 2; the result reloads as current.
    persistence::saveProject(*p, dir / "v2.openmine");
    persistence::LoadReport rep2;
    auto q = persistence::loadProject(dir / "v2.openmine", &rep2);
    EXPECT_EQ(rep2.fileSchemaVersion, persistence::kCurrentSchemaVersion);
    EXPECT_FALSE(rep2.migrated);
}

TEST(Persistence, SchemaHasExplicitRelationships) {
    const auto& ddl = persistence::schemaSql();
    for (const char* s : {"REFERENCES mine_development(uuid)", "REFERENCES geometry_object(uuid) ON DELETE RESTRICT",
                          "REFERENCES schedule_activity(uuid) ON DELETE CASCADE", "REFERENCES resource(uuid) ON DELETE CASCADE",
                          "REFERENCES point_cloud_dataset(uuid) ON DELETE CASCADE", "REFERENCES survey_control_point(uuid) ON DELETE SET NULL",
                          "CREATE TABLE production_actual", "CREATE TABLE design_revision", "CREATE TABLE activity_resource_assignment"})
        EXPECT_NE(ddl.find(s), std::string::npos) << s;
    // Every *_uuid column is a foreign key, except the documented polymorphic references,
    // snapshots and the audit log (docs/architecture/database-schema.md).
    const std::set<std::string> documented = {
        "attachment.entity_uuid",          "design_revision.entity_uuid",      "attribute_value.entity_uuid",
        "activity_design_link.design_uuid", "scenario_override.entity_uuid",    "baseline_activity.activity_uuid",
        "audit_log.entity_uuid",            "mine_development.current_revision_uuid", "stope.current_revision_uuid"};
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, ddl.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    auto rows = [&](const std::string& sql, int column) {
        std::vector<std::string> out;
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
        while (sqlite3_step(st) == SQLITE_ROW) out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, column)));
        sqlite3_finalize(st);
        return out;
    };
    int fkCount = 0;
    std::set<std::string> undocumented;
    for (const auto& t : rows("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'", 0)) {
        const auto fks = rows("PRAGMA foreign_key_list('" + t + "')", 3);
        fkCount += static_cast<int>(fks.size());
        for (const auto& c : rows("PRAGMA table_info('" + t + "')", 1))
            if (c.size() > 5 && c.ends_with("_uuid") && std::find(fks.begin(), fks.end(), c) == fks.end() && !documented.count(t + "." + c))
                undocumented.insert(t + "." + c);
    }
    sqlite3_close(db);
    EXPECT_TRUE(undocumented.empty()) << *undocumented.begin();
    EXPECT_EQ(fkCount, 57);
}

// ---- entity documents (repositories) --------------------------------------------------------------
#include "openmine/persistence/EntityCodec.h"

TEST(EntityCodec, DocumentsRoundTripLosslesslyAndDeterministically) {
    auto r = richProject();
    auto& p = *r.p;
    {
        ProjectEditor ed(p);
        auto h = *p.model().drillholes.find(r.hole);
        h.intervals.push_back({90, 91, {{"au", std::numeric_limits<double>::quiet_NaN()}, {"cu", 1e-300}}, {{"lith", "Ünïcødé ✓"}}});
        h.length = std::max(h.length, 91.0);
        ed.update(h);
    }
    const auto docs = persistence::encodeModel(p.model());
    std::set<std::string> types;
    for (const auto& d : docs) types.insert(d.type);
    EXPECT_GE(types.size(), 30u);
    EXPECT_TRUE(types.count("ProjectInfo") && types.count("ActivityDesignLink") && types.count("Drillhole") && types.count("Registration"));
    // Same model → identical documents.
    const auto again = persistence::encodeModel(p.model());
    ASSERT_EQ(docs.size(), again.size());
    for (std::size_t i = 0; i < docs.size(); ++i) EXPECT_EQ(docs[i].json, again[i].json);
    // Decode into an empty model and re-encode: byte-identical.
    Project q;
    for (const auto& d : docs) persistence::insertDocument(q.mutableModel(), d.type, d.key, d.json);
    const auto back = persistence::encodeModel(q.model());
    ASSERT_EQ(back.size(), docs.size());
    for (std::size_t i = 0; i < docs.size(); ++i) {
        EXPECT_EQ(back[i].type, docs[i].type);
        EXPECT_EQ(back[i].json, docs[i].json) << docs[i].type;
    }
    EXPECT_TRUE(persistence::checkModel(q).empty());
    const auto& hole = *q.model().drillholes.find(r.hole);
    EXPECT_TRUE(std::isnan(hole.intervals.back().values.at("au")));
    EXPECT_EQ(hole.intervals.back().values.at("cu"), 1e-300);
    EXPECT_EQ(persistence::documentHash(docs[0].json).size(), 64u);
}

TEST(EntityCodec, SetCommandsAreUndoableAndIntegrityIsChecked) {
    auto r = richProject();
    auto& p = *r.p;
    auto g = *p.model().geometry.find(r.geom);
    g.name = "renamed remotely";
    const auto doc = persistence::encodeEntity(g);
    p.commands().execute(persistence::setDocumentCommand(p.model(), "GeometryObject", g.id.toString(), doc, "Pull"));
    EXPECT_EQ(p.model().geometry.find(r.geom)->name, "renamed remotely");
    p.commands().undo();
    EXPECT_NE(p.model().geometry.find(r.geom)->name, "renamed remotely");
    // Deleting a layer still referenced by geometry is detected by checkModel.
    const Uuid layer = g.layerId;
    p.commands().execute(persistence::setDocumentCommand(p.model(), "CadLayer", layer.toString(), std::nullopt, "Pull"));
    EXPECT_FALSE(persistence::checkModel(p).empty());
    p.commands().undo();
    EXPECT_TRUE(persistence::checkModel(p).empty());
    EXPECT_THROW(persistence::encodeDocument(p.model(), "NoSuchType", "x"), persistence::PersistenceError);
    EXPECT_FALSE(persistence::encodeDocument(p.model(), "GeometryObject", Uuid::generate().toString()));
}

namespace {
std::string fileBytes(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

TEST(Persistence, AttachmentsAreStoredVerifiedAndFollowTheirEntity) {
    const auto dir = tempDir();
    auto p = Project::createNew("Attach", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    GeometryObject g = cad::makePolyline({{0, 0, 0}, {10, 0, 0}});
    g.name = "Face line";
    g.layerId = p->model().layers.all().front().id;
    const Uuid gid = ed.addGeometry(g);
    std::string photo(5000, '\0');
    for (std::size_t i = 0; i < photo.size(); ++i) photo[i] = static_cast<char>(i * 31 % 256);
    std::ofstream(dir / "Face 0+120.JPG", std::ios::binary) << photo;
    std::ofstream(dir / "permit.pdf", std::ios::binary) << "%PDF-1.4 permit";

    const Uuid aid = attachFile(*p, "GeometryObject", gid, dir / "Face 0+120.JPG", "Face photo");
    const auto* a = p->model().attachments.find(aid);
    ASSERT_TRUE(a);
    EXPECT_EQ(a->fileName, "Face 0+120.JPG");
    EXPECT_EQ(a->mimeType, "image/jpeg");
    EXPECT_EQ(a->entityType, "GeometryObject");
    EXPECT_EQ(a->description, "Face photo");
    EXPECT_EQ(p->commands().undoText(), "Attach Face 0+120.JPG");
    const Uuid projectLevel = attachFile(*p, "", Uuid{}, dir / "permit.pdf");
    EXPECT_EQ(p->model().attachments.find(projectLevel)->entityType, "Project");
    EXPECT_EQ(attachmentsOf(*p, gid).size(), 1u);
    EXPECT_EQ(attachmentsOf(*p, Uuid{}).size(), 1u);
    EXPECT_THROW(attachFile(*p, "GeometryObject", Uuid::generate(), dir / "permit.pdf"), ValidationError);
    EXPECT_THROW(attachFile(*p, "NoSuchType", gid, dir / "permit.pdf"), ValidationError);
    EXPECT_THROW(attachFile(*p, "GeometryObject", gid, dir / "missing.pdf"), AssetError);

    // Saved, reopened and extracted byte for byte.
    persistence::saveProject(*p, dir / "a.openmine");
    persistence::LoadReport rep;
    auto q = persistence::loadProject(dir / "a.openmine", &rep);
    EXPECT_TRUE(rep.warnings.empty());
    saveAttachment(*q, aid, dir / "out" / "photo.jpg");
    EXPECT_EQ(fileBytes(dir / "out" / "photo.jpg"), photo);
    // A stored file changed behind OpenMine's back is detected.
    const auto* asset = q->model().assets.find(q->model().attachments.find(aid)->asset.id);
    std::ofstream(q->assets().pathOf(*asset), std::ios::binary | std::ios::app) << "x";
    EXPECT_THROW(saveAttachment(*q, aid, dir / "out" / "tampered.jpg"), AssetError);

    // Deleting the entity deletes its attachments; undo brings both back.
    ProjectEditor(*p).removeGeometry(gid);
    EXPECT_FALSE(p->model().attachments.contains(aid));
    EXPECT_TRUE(p->model().attachments.contains(projectLevel));
    p->commands().undo();
    EXPECT_TRUE(p->model().geometry.contains(gid));
    EXPECT_TRUE(p->model().attachments.contains(aid));
}

TEST(Persistence, CompactRemovesUnreferencedAndUnregisteredAssetsOnly) {
    const auto dir = tempDir();
    auto p = Project::createNew("Compact", Date::fromYmd(2026, 11, 2));
    std::ofstream(dir / "keep.txt") << "kept";
    std::ofstream(dir / "drop.txt") << "dropped file content";
    EXPECT_THROW(persistence::compactProject(*p), persistence::PersistenceError); // never saved
    const Uuid keep = attachFile(*p, "", Uuid{}, dir / "keep.txt");
    const Uuid drop = attachFile(*p, "", Uuid{}, dir / "drop.txt");
    const auto file = dir / "c.openmine";
    persistence::saveProject(*p, file);
    const auto sidecar = AssetStore::sidecarFor(file);
    const Asset dropped = *p->model().assets.find(p->model().attachments.find(drop)->asset.id);
    ProjectEditor(*p).remove<Attachment>(drop); // the asset stays registered, now unused
    std::ofstream(sidecar / "attachment" / (Uuid::generate().toString() + ".bin")) << "left by a cancelled import";
    fs::create_directories(sidecar / "pointcloud" / Uuid::generate().toString() / "nodes");
    std::ofstream(sidecar / "README.txt") << "a user's own file next to the kind folders";

    const auto dry = persistence::compactProject(*p, true);
    EXPECT_TRUE(dry.dryRun);
    ASSERT_EQ(dry.unreferencedAssets.size(), 1u);
    EXPECT_EQ(dry.unreferencedAssets[0], dropped.relativePath);
    EXPECT_EQ(dry.orphanFiles.size(), 2u);
    EXPECT_GT(dry.bytes, 0);
    EXPECT_TRUE(fs::exists(p->assets().pathOf(dropped)));
    EXPECT_TRUE(p->commands().canUndo());

    const auto done = persistence::compactProject(*p);
    EXPECT_EQ(done.unreferencedAssets, dry.unreferencedAssets);
    EXPECT_EQ(done.bytes, dry.bytes);
    EXPECT_FALSE(fs::exists(sidecar / dropped.relativePath));
    EXPECT_FALSE(p->model().assets.contains(dropped.id));
    EXPECT_FALSE(p->commands().canUndo());
    EXPECT_TRUE(p->commands().isClean());
    EXPECT_TRUE(fs::exists(sidecar / "README.txt"));
    EXPECT_EQ(fs::directory_iterator(sidecar / "pointcloud"), fs::directory_iterator());
    EXPECT_EQ(p->model().audit.back().action, "Compact");
    // The saved file matches: no missing-asset warnings, the kept attachment intact.
    persistence::LoadReport rep;
    auto q = persistence::loadProject(file, &rep);
    EXPECT_TRUE(rep.warnings.empty()) << rep.warnings.front();
    saveAttachment(*q, keep, dir / "keep-out.txt");
    EXPECT_EQ(fileBytes(dir / "keep-out.txt"), "kept");
    const auto again = persistence::compactProject(*q, true);
    EXPECT_TRUE(again.unreferencedAssets.empty() && again.orphanFiles.empty());
}
