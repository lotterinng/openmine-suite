// SPDX-License-Identifier: Apache-2.0
#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <filesystem>
#include <fstream>

using namespace openmine;
namespace fs = std::filesystem;

namespace {
fs::path tempDir() {
    auto d = fs::temp_directory_path() / ("openmine-test-" + Uuid::generate().toString());
    fs::create_directories(d);
    return d;
}

std::unique_ptr<Project> richProject() {
    auto p = Project::createNew("Jupiter Mine — Level 675 ✓", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    ProjectInfo info = p->model().info;
    info.crs.name = "Hartebeesthoek94 / Lo25";
    info.crs.authorityCode = "EPSG:2051";
    info.crs.southOrientatedAxes = true;
    info.statusDate = Date::fromYmd(2026, 11, 20);
    ed.setProjectInfo(info);

    AttributeDefinition def;
    def.entityType = "MineDevelopment";
    def.key = "ground_support";
    def.displayName = "Ground support class";
    ed.addAttributeDefinition(def);

    GeometryObject g;
    g.name = "LEG2 centreline";
    g.layerId = p->model().layers.all()[1].id;
    g.vertices = {{-25000.125, 2900000.5, 1200.0}, {-25080.0, 2900060.25, 1190.0}, {-25120.75, 2900150.0, 1180.5}};
    Uuid gid = ed.addGeometry(g);

    MineDevelopment d;
    d.headingId = "LEG2_675_D3_DEC";
    d.level = "675";
    d.zone = "Legadima 2";
    d.geometryId = gid;
    d.designWidth = 5.5;
    d.designHeight = 5.5;
    d.attributes["ground_support"] = "Class B";
    Uuid did = ed.addDevelopment(d);

    ScheduleActivity s;
    s.name = "Level 675";
    s.kind = ActivityKind::Summary;
    s.wbsCode = "675";
    Uuid sid = ed.addActivity(s);
    ScheduleActivity a;
    a.name = "Develop LEG2_675_D3_DEC";
    a.parentId = sid;
    a.durationFromRate = true;
    a.ratePerDay = 5.0;
    a.quantity = 150.0;
    a.actualQuantity = 12.5;
    a.startNoEarlierThan = Date::fromYmd(2026, 11, 4);
    a.designImpactFlag = true;
    a.designImpactNote = "note";
    Uuid aid = ed.addActivity(a);
    ScheduleActivity b;
    b.name = "Install services";
    b.parentId = sid;
    b.durationDays = 3;
    b.calendarId = p->model().calendars.all()[1].id;
    Uuid bid = ed.addActivity(b);
    ActivityDependency dep;
    dep.predecessorId = aid;
    dep.successorId = bid;
    dep.type = DependencyType::StartToStart;
    dep.lagDays = 2.5;
    ed.addDependency(dep);
    ed.linkActivityToDevelopment(aid, did);

    WorkCalendar cal = *p->model().calendars.find(p->model().info.defaultCalendarId);
    cal.nonWorkingDates = {Date::fromYmd(2026, 12, 16), Date::fromYmd(2026, 12, 25)};
    ed.updateCalendar(cal);

    ScheduleBaseline bl;
    bl.name = "BL-2026-11";
    bl.activities.push_back({aid, a.name, Date::fromYmd(2026, 11, 4), Date::fromYmd(2026, 12, 15), 30.0, 150.0});
    ed.addBaseline(bl);
    return p;
}
} // namespace

TEST(Persistence, RoundTripPreservesEverything) {
    auto dir = tempDir();
    auto file = dir / "jupiter.openmine";
    auto p = richProject();
    ASSERT_TRUE(p->isModified());
    persistence::saveProject(*p, file);
    EXPECT_FALSE(p->isModified());

    auto q = persistence::loadProject(file);
    const auto& a = p->model();
    const auto& b = q->model();
    EXPECT_EQ(b.info.id, a.info.id);
    EXPECT_EQ(b.info.name, a.info.name);
    EXPECT_EQ(b.info.crs.authorityCode, "EPSG:2051");
    EXPECT_TRUE(b.info.crs.southOrientatedAxes);
    ASSERT_TRUE(b.info.statusDate);
    EXPECT_EQ(*b.info.statusDate, *a.info.statusDate);

    ASSERT_EQ(b.geometry.size(), 1u);
    const auto& ga = a.geometry.all()[0];
    const auto& gb = b.geometry.all()[0];
    EXPECT_EQ(gb.id, ga.id);
    ASSERT_EQ(gb.vertices.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) EXPECT_EQ(gb.vertices[i], ga.vertices[i]); // bit-exact doubles
    EXPECT_EQ(gb.meta.createdAt, ga.meta.createdAt);

    const auto& da = a.developments.all()[0];
    const auto& db = b.developments.all()[0];
    EXPECT_EQ(db.id, da.id);
    EXPECT_EQ(db.geometryId, ga.id);
    EXPECT_EQ(db.attributes.at("ground_support"), "Class B");
    EXPECT_DOUBLE_EQ(db.designWidth, 5.5);

    ASSERT_EQ(b.activities.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& x = a.activities.all()[i];
        const auto& y = b.activities.all()[i];
        EXPECT_EQ(y.id, x.id);
        EXPECT_EQ(y.parentId, x.parentId);
        EXPECT_EQ(y.name, x.name);
        EXPECT_EQ(y.kind, x.kind);
        EXPECT_EQ(y.durationFromRate, x.durationFromRate);
        EXPECT_DOUBLE_EQ(y.quantity, x.quantity);
        EXPECT_DOUBLE_EQ(y.actualQuantity, x.actualQuantity);
        EXPECT_EQ(y.startNoEarlierThan, x.startNoEarlierThan);
        EXPECT_EQ(y.calendarId, x.calendarId);
        EXPECT_EQ(y.designImpactFlag, x.designImpactFlag);
    }
    ASSERT_EQ(b.dependencies.size(), 1u);
    EXPECT_EQ(b.dependencies.all()[0].type, DependencyType::StartToStart);
    EXPECT_DOUBLE_EQ(b.dependencies.all()[0].lagDays, 2.5);
    ASSERT_EQ(b.designLinks.size(), 1u);
    EXPECT_EQ(b.designLinks[0], a.designLinks[0]);
    EXPECT_EQ(b.calendars.find(a.info.defaultCalendarId)->nonWorkingDates.size(), 2u);
    ASSERT_EQ(b.baselines.size(), 1u);
    EXPECT_EQ(b.baselines[0].activities[0].finish, Date::fromYmd(2026, 12, 15));
    EXPECT_EQ(b.attributeDefinitions.size(), 1u);
    EXPECT_EQ(b.audit.size(), a.audit.size());
    EXPECT_FALSE(q->isModified());

    // Save → load → save again is stable.
    persistence::saveProject(*q, file);
    EXPECT_TRUE(fs::exists(dir / "jupiter.openmine.bak"));
    auto r = persistence::loadProject(file);
    EXPECT_EQ(r->model().activities.size(), 3u);
    fs::remove_all(dir);
}

TEST(Persistence, FailedSaveLeavesExistingFileUntouched) {
    auto dir = tempDir();
    auto file = dir / "p.openmine";
    auto p = richProject();
    persistence::saveProject(*p, file);
    const auto sizeBefore = fs::file_size(file);

    // Corrupt the in-memory model so the FK verification fails during save.
    p->mutableModel().designLinks.push_back({Uuid::generate(), Uuid::generate()});
    EXPECT_THROW(persistence::saveProject(*p, file), persistence::PersistenceError);
    EXPECT_EQ(fs::file_size(file), sizeBefore);
    EXPECT_FALSE(fs::exists(dir / "p.openmine.saving"));
    EXPECT_NO_THROW(persistence::loadProject(file));
    fs::remove_all(dir);
}

TEST(Persistence, RejectsForeignAndNewerFiles) {
    auto dir = tempDir();
    {
        std::ofstream(dir / "x.openmine") << "this is not sqlite";
        EXPECT_THROW(persistence::loadProject(dir / "x.openmine"), persistence::PersistenceError);
    }
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
    fs::remove_all(dir);
}

TEST(Persistence, IntegrityCheckReportsCleanFile) {
    auto dir = tempDir();
    auto p = richProject();
    persistence::saveProject(*p, dir / "p.openmine");
    auto rep = persistence::checkProjectFile(dir / "p.openmine");
    EXPECT_TRUE(rep.ok);
    EXPECT_EQ(rep.schemaVersion, persistence::kCurrentSchemaVersion);
    fs::remove_all(dir);
}
