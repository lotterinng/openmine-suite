// SPDX-License-Identifier: Apache-2.0
// Integration tests of the PostgreSQL/PostGIS repository. They need a database with PostGIS
// available: set OPENMINE_PG_DSN (e.g. "host=127.0.0.1 port=5432 dbname=openmine_test user=postgres").
// Without it the tests are skipped (reported as skipped, not passed).
#include "openmine/persistence/EntityCodec.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/repository/PgRepository.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

using namespace openmine;
using namespace openmine::repository;

namespace {
std::string dsn() {
    const char* d = std::getenv("OPENMINE_PG_DSN");
    return d ? d : "";
}

struct Fixture : ::testing::Test {
    std::unique_ptr<PgRepository> repo;
    std::unique_ptr<Project> origin;
    WorkingCopy wcOrigin;
    Uuid layer, line, point, cube, asset, folder;

    void SetUp() override {
        if (dsn().empty()) GTEST_SKIP() << "OPENMINE_PG_DSN not set";
        repo = std::make_unique<PgRepository>(dsn());
        repo->initialise();
        origin = Project::createNew("Repo test", Date::fromYmd(2026, 11, 2));
        Project& p = *origin;
        ProjectEditor ed(p);
        layer = ed.addLayer("Survey", 0x00FF00);
        AttributeDefinition def;
        def.entityType = "GeometryObject";
        def.key = "status";
        def.displayName = "Status";
        ed.add(def);
        GeometryObject g;
        g.name = "Drive CL";
        g.layerId = layer;
        g.vertices = {{1000, 2000, 100}, {1100, 2000, 99}, {1100, 2100, 98}};
        g.attributes["status"] = "design";
        line = ed.addGeometry(g);
        GeometryObject pt;
        pt.type = GeometryType::Point;
        pt.name = "Far station";
        pt.layerId = layer;
        pt.vertices = {{5000, 5000, 0}};
        point = ed.addGeometry(pt);
        // A file asset and a folder asset (as point clouds use).
        Asset a = p.assets().writeBytes(std::string("block model bytes \x01\x02\x03", 21), AssetKind::BlockModel, "f64");
        asset = ed.add(a);
        auto [fid, dir] = p.assets().beginFolder(AssetKind::PointCloud);
        std::filesystem::create_directories(dir / "nodes");
        std::ofstream(dir / "nodes" / "r.bin", std::ios::binary) << std::string(5000, 'x');
        std::ofstream(dir / "header.json") << "{\"points\": 1}";
        folder = ed.add(p.assets().finalizeFolder(fid, AssetKind::PointCloud));
        wcOrigin = repo->createProject(p, "alice", "Initial import");
    }
    void TearDown() override {
        if (repo && origin) repo->deleteProject(origin->model().info.id);
    }
    std::unique_ptr<Project> checkout(WorkingCopy& wc, std::int64_t rev = 0) { return repo->checkout(origin->model().info.id, wc, rev); }
    static void rename(Project& p, const Uuid& id, const std::string& name) {
        auto g = *p.model().geometry.find(id);
        g.name = name;
        ProjectEditor(p).updateGeometry(g);
    }
};
} // namespace

TEST_F(Fixture, CheckoutReproducesTheProjectAndItsAssets) {
    EXPECT_EQ(repo->schemaVersion(), 1);
    EXPECT_FALSE(repo->postgisVersion().empty());
    EXPECT_EQ(repo->headRevision(origin->model().info.id), 1);
    WorkingCopy wc;
    auto p = checkout(wc);
    const auto a = persistence::encodeModel(origin->model()), b = persistence::encodeModel(p->model());
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i].json, b[i].json) << a[i].type;
    EXPECT_TRUE(localChanges(*p, wc).empty());
    // Assets downloaded and verified.
    const auto& as = *p->model().assets.find(asset);
    EXPECT_EQ(p->assets().readBytes(as), std::string("block model bytes \x01\x02\x03", 21));
    const auto& fo = *p->model().assets.find(folder);
    EXPECT_EQ(p->assets().verify(fo, true).ok, true);
    // The working-copy file round-trips and never stores a password.
    const auto json = wc.toJson();
    EXPECT_EQ(json.find("password"), std::string::npos);
    EXPECT_EQ(WorkingCopy::fromJson(json).baseHashes, wc.baseHashes);
    EXPECT_THROW(repo->createProject(*origin, "alice"), RepositoryError); // already there
}

TEST_F(Fixture, TwoUsersPublishAndPullWithoutConflicts) {
    WorkingCopy wa, wb;
    auto a = checkout(wa), b = checkout(wb);
    rename(*a, line, "Drive CL (surveyed)");
    const auto pa = repo->publish(*a, wa, "alice", "Survey pickup");
    ASSERT_TRUE(pa.published) << pa.refusal;
    EXPECT_EQ(pa.revision, 2);
    // Bob edits another entity; publishing is refused until he pulls.
    rename(*b, point, "Station 7");
    const auto pb0 = repo->publish(*b, wb, "bob", "Rename station");
    EXPECT_FALSE(pb0.published);
    EXPECT_NE(pb0.refusal.find("pull first"), std::string::npos);
    const auto pull = repo->pull(*b, wb);
    EXPECT_FALSE(pull.aborted);
    EXPECT_TRUE(pull.conflicts.empty());
    EXPECT_EQ(b->model().geometry.find(line)->name, "Drive CL (surveyed)");
    EXPECT_EQ(b->model().geometry.find(point)->name, "Station 7"); // local edit kept
    // The pull is one undoable step.
    EXPECT_NE(b->commands().undoText().find("Pull revision 2"), std::string::npos);
    const auto pb = repo->publish(*b, wb, "bob", "Rename station");
    ASSERT_TRUE(pb.published) << pb.refusal;
    EXPECT_EQ(pb.changes, 1u);
    EXPECT_EQ(repo->pull(*a, wa).applied, 1u);
    EXPECT_EQ(a->model().geometry.find(point)->name, "Station 7");
    // Both working copies are now identical and clean.
    const auto da = persistence::encodeModel(a->model()), db = persistence::encodeModel(b->model());
    ASSERT_EQ(da.size(), db.size());
    for (std::size_t i = 0; i < da.size(); ++i) EXPECT_EQ(da[i].json, db[i].json);
    EXPECT_TRUE(localChanges(*a, wa).empty());
    EXPECT_TRUE(localChanges(*b, wb).empty());
    EXPECT_EQ(repo->publish(*a, wa, "alice", "nothing").refusal, "Nothing to publish");
    // Revisions and per-entity history.
    const auto revs = repo->revisions(origin->model().info.id);
    ASSERT_EQ(revs.size(), 3u);
    EXPECT_EQ(revs[1].author, "alice");
    EXPECT_EQ(revs[2].message, "Rename station");
    const auto h = repo->history(origin->model().info.id, "GeometryObject", point.toString());
    ASSERT_EQ(h.size(), 2u);
    EXPECT_EQ(h[1].author, "bob");
    // Historical checkout.
    WorkingCopy w1;
    auto old = checkout(w1, 1);
    EXPECT_EQ(old->model().geometry.find(line)->name, "Drive CL");
    EXPECT_EQ(w1.baseRevision, 1);
}

TEST_F(Fixture, ConflictsAreReportedAndResolvedExplicitly) {
    WorkingCopy wa, wb;
    auto a = checkout(wa), b = checkout(wb);
    rename(*a, line, "Alice's name");
    ASSERT_TRUE(repo->publish(*a, wa, "alice", "A").published);
    rename(*b, line, "Bob's name");
    // Default: abort, nothing applied.
    const auto abortRes = repo->pull(*b, wb);
    EXPECT_TRUE(abortRes.aborted);
    ASSERT_EQ(abortRes.conflicts.size(), 1u);
    EXPECT_EQ(abortRes.conflicts[0].key, line.toString());
    EXPECT_NE(abortRes.conflicts[0].reason.find("revision 2 by alice"), std::string::npos);
    EXPECT_EQ(b->model().geometry.find(line)->name, "Bob's name");
    EXPECT_EQ(wb.baseRevision, 1);
    // Keep local: Bob's version stays and is published over Alice's.
    WorkingCopy wbKeep = wb;
    const auto keep = repo->pull(*b, wbKeep, ConflictPolicy::KeepLocal);
    EXPECT_FALSE(keep.aborted);
    EXPECT_EQ(b->model().geometry.find(line)->name, "Bob's name");
    const auto pub = repo->publish(*b, wbKeep, "bob", "B wins");
    ASSERT_TRUE(pub.published) << pub.refusal;
    // Take remote on Alice's side after a new local edit conflicts again.
    rename(*a, line, "Alice again");
    const auto take = repo->pull(*a, wa, ConflictPolicy::TakeRemote);
    EXPECT_EQ(take.conflicts.size(), 1u);
    EXPECT_EQ(a->model().geometry.find(line)->name, "Bob's name");
    EXPECT_TRUE(localChanges(*a, wa).empty());
}

TEST_F(Fixture, DeletionsLocksAndSpatialQueries) {
    WorkingCopy wa, wb;
    auto a = checkout(wa), b = checkout(wb);
    const Uuid id = origin->model().info.id;
    // Spatial: the drive centreline is near (1050, 2000); the far station is not.
    const auto hits = repo->intersecting(id, 1040, 1990, 1060, 2010);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].key, line.toString());
    EXPECT_EQ(hits[0].name, "Drive CL");
    EXPECT_EQ(repo->within(id, 4990, 5000, 15, {"GeometryObject"}).size(), 1u);
    EXPECT_TRUE(repo->within(id, 4990, 5000, 5).empty());
    // Locks: Alice locks the station; Bob cannot publish a change to it.
    EXPECT_TRUE(repo->lock(id, "GeometryObject", point.toString(), "alice", std::chrono::seconds(600)));
    EXPECT_FALSE(repo->lock(id, "GeometryObject", point.toString(), "bob", std::chrono::seconds(600)));
    EXPECT_EQ(repo->locks(id).size(), 1u);
    rename(*b, point, "bob was here");
    const auto refused = repo->publish(*b, wb, "bob", "x");
    EXPECT_FALSE(refused.published);
    ASSERT_EQ(refused.conflicts.size(), 1u);
    EXPECT_EQ(refused.conflicts[0].reason, "locked by alice");
    repo->unlock(id, "GeometryObject", point.toString(), "alice");
    EXPECT_TRUE(repo->publish(*b, wb, "bob", "x").published);
    // Deletion propagates and leaves the spatial index.
    ProjectEditor(*a).removeGeometry(point);
    ASSERT_TRUE(repo->pull(*a, wa, ConflictPolicy::TakeRemote).conflicts.size() == 1u); // deleted locally, changed remotely
    // After taking remote the point is back; delete again and publish.
    ASSERT_TRUE(a->model().geometry.find(point));
    ProjectEditor(*a).removeGeometry(point);
    const auto del = repo->publish(*a, wa, "alice", "Remove station");
    ASSERT_TRUE(del.published) << del.refusal;
    EXPECT_TRUE(repo->within(id, 4990, 5000, 15).empty());
    EXPECT_EQ(repo->pull(*b, wb).applied, 1u);
    EXPECT_FALSE(b->model().geometry.find(point));
    const auto h = repo->history(id, "GeometryObject", point.toString());
    EXPECT_TRUE(h.back().deleted);
}

TEST_F(Fixture, InconsistentMergesAreRolledBack) {
    WorkingCopy wa, wb;
    auto a = checkout(wa), b = checkout(wb);
    // Alice deletes the layer's only remaining use and the layer itself; Bob adds an object on it.
    ProjectEditor ea(*a);
    ea.removeGeometry(line);
    ea.removeGeometry(point);
    ea.remove<CadLayer>(layer);
    ASSERT_TRUE(repo->publish(*a, wa, "alice", "Remove survey layer").published);
    GeometryObject g;
    g.name = "New pickup";
    g.layerId = layer;
    g.vertices = {{0, 0, 0}, {1, 1, 1}};
    ProjectEditor(*b).addGeometry(g);
    const auto before = persistence::encodeModel(b->model()).size();
    EXPECT_THROW(repo->pull(*b, wb), RepositoryError); // would leave "New pickup" on a deleted layer
    EXPECT_EQ(persistence::encodeModel(b->model()).size(), before); // rolled back
    EXPECT_EQ(wb.baseRevision, 1);
}

TEST_F(Fixture, EntityOrderIsPreservedAndMerged) {
    WorkingCopy wa, wb;
    auto a = checkout(wa), b = checkout(wb);
    auto names = [](const Project& p) {
        std::vector<std::string> v;
        for (const auto& l : p.model().layers.all()) v.push_back(l.name);
        return v;
    };
    EXPECT_EQ(names(*a), names(*origin)); // checkout keeps the original order (not uuid order)
    ProjectEditor(*a).addLayer("Alice layer", 1);
    ProjectEditor(*b).addLayer("Bob layer", 2);
    ASSERT_TRUE(repo->publish(*a, wa, "alice", "layer").published);
    const auto pr = repo->pull(*b, wb);
    EXPECT_TRUE(pr.conflicts.empty());
    auto nb = names(*b);
    ASSERT_EQ(nb.size(), names(*origin).size() + 2);
    EXPECT_EQ(nb[nb.size() - 2], "Alice layer");
    EXPECT_EQ(nb.back(), "Bob layer");
    ASSERT_TRUE(repo->publish(*b, wb, "bob", "layer").published);
    repo->pull(*a, wa);
    EXPECT_EQ(names(*a), names(*b));
    EXPECT_TRUE(localChanges(*a, wa).empty());
}
