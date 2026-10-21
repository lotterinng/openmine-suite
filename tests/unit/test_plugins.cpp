// SPDX-License-Identifier: Apache-2.0
#include "openmine/plugins/PluginManager.h"
#include "openmine/project/ProjectEditor.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <fstream>

using namespace openmine;
using namespace openmine::plugins;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
const fs::path kDir = OPENMINE_TEST_PLUGIN_DIR;

fs::path tempFile(const std::string& name, const std::string& content) {
    const auto d = fs::temp_directory_path() / ("openmine-plugin-" + Uuid::generate().toString());
    fs::create_directories(d);
    std::ofstream(d / name) << content;
    return d / name;
}
} // namespace

TEST(Plugins, DiscoveryVersionChecksAndRegistrations) {
    PluginManager pm;
    std::vector<std::string> problems;
    const auto found = pm.discover({kDir}, &problems);
    EXPECT_GE(found.size(), 3u);
    EXPECT_EQ(pm.loadAll({kDir}, {}), 2);
    const PluginStatus* future = nullptr;
    for (const auto& s : pm.plugins())
        if (s.manifest.id == "org.openmine.tests.future") future = &s;
    ASSERT_TRUE(future);
    EXPECT_FALSE(future->loaded);
    EXPECT_NE(future->error.find("API version 99"), std::string::npos);
    std::set<std::string> cmds;
    for (const auto& c : pm.commands()) cmds.insert(c.name);
    EXPECT_TRUE(cmds.count("stations.summary") && cmds.count("stations.traverse") && cmds.count("bad.open-edit"));
    ASSERT_TRUE(pm.importerFor("pickup.STN"));
    EXPECT_EQ(pm.importerFor("pickup.stn")->pluginId, "org.openmine.samples.stations");
    EXPECT_TRUE(pm.exporterFor("x.geojson"));
    EXPECT_FALSE(pm.importerFor("x.las"));
    // Loading again is a no-op; disabling skips.
    PluginManager pm2;
    EXPECT_EQ(pm2.loadAll({kDir}, {"org.openmine.samples.stations"}), 1);
    EXPECT_TRUE(pm2.commands().size() >= 1);
    EXPECT_THROW(readManifest(kDir / "does-not-exist"), std::runtime_error);
}

TEST(Plugins, SamplePluginImportsExportsAndRunsCommandsUndoably) {
    PluginManager pm;
    pm.loadAll({kDir});
    std::vector<std::string> sunk;
    pm.setLogSink([&](int, const std::string& id, const std::string& m) { sunk.push_back(id + ": " + m); });
    auto p = Project::createNew("Plugins", Date::fromYmd(2026, 11, 2));
    const auto file = tempFile("pickup.stn", "# name,E,N,Z,code\nSTN1, 1000.0, 2000.0, 100.0, CP\nSTN2,1010,2000,101\nbad line\nSTN3,1010,2010,102,BM\n");
    const auto r = pm.runImport(*p, file, R"({"horizontal_sigma": 0.002})");
    ASSERT_TRUE(r.ok) << r.error;
    const json rep = json::parse(r.text);
    EXPECT_EQ(rep.at("added"), 3);
    ASSERT_EQ(rep.at("problems").size(), 1u); // the bad line is reported, not dropped silently
    EXPECT_NE(rep.at("problems")[0].get<std::string>().find("line 4"), std::string::npos);
    EXPECT_EQ(p->model().controlPoints.size(), 3u);
    EXPECT_DOUBLE_EQ(p->model().controlPoints.all()[0].horizontalSigma, 0.002);
    EXPECT_EQ(p->model().controlPoints.all()[0].code, "CP");
    EXPECT_FALSE(sunk.empty());
    // One undo step for the whole import.
    EXPECT_NE(p->commands().undoText().find("Import survey stations"), std::string::npos);
    p->commands().undo();
    EXPECT_EQ(p->model().controlPoints.size(), 0u);
    p->commands().redo();
    // Re-import without replace: every existing station is reported.
    const auto again = json::parse(pm.runImport(*p, file).text);
    EXPECT_EQ(again.at("added"), 0);
    EXPECT_EQ(again.at("problems").size(), 4u);
    // Commands.
    const auto s = pm.runCommand(*p, "stations.summary");
    ASSERT_TRUE(s.ok) << s.error;
    EXPECT_EQ(json::parse(s.text).at("count"), 3);
    EXPECT_EQ(json::parse(s.text).at("extent")[1][2], 102.0);
    const auto t = pm.runCommand(*p, "stations.traverse", R"({"stations":["STN1","STN3","STN2"],"layer":"0","name":"Loop","closed":true})");
    ASSERT_TRUE(t.ok) << t.error;
    const auto gid = *Uuid::parse(json::parse(t.text).at("geometry").get<std::string>());
    const auto* g = p->model().geometry.find(gid);
    ASSERT_TRUE(g);
    EXPECT_EQ(g->vertices.size(), 3u);
    EXPECT_TRUE(g->closed);
    EXPECT_EQ(g->vertices[1], (Vec3{1010, 2010, 102}));
    EXPECT_FALSE(pm.runCommand(*p, "stations.traverse", R"({"stations":["STN1","NOPE"]})").ok);
    // Export.
    const auto out = file.parent_path() / "stations.geojson";
    const auto e = pm.runExport(*p, out);
    ASSERT_TRUE(e.ok) << e.error;
    std::ifstream in(out);
    const json fc = json::parse(in);
    EXPECT_EQ(fc.at("features").size(), 3u);
    EXPECT_EQ(fc.at("features")[0].at("geometry").at("coordinates")[0], 1000.0);
    EXPECT_FALSE(pm.runCommand(*p, "no.such").ok);
    fs::remove_all(file.parent_path());
}

TEST(Plugins, HostProtectsTheModelFromMisbehavingPlugins) {
    PluginManager pm;
    pm.loadAll({kDir});
    auto p = Project::createNew("Plugins", Date::fromYmd(2026, 11, 2));
    const auto before = p->model().geometry.size();
    const auto open = pm.runCommand(*p, "bad.open-edit");
    EXPECT_FALSE(open.ok);
    EXPECT_TRUE(open.rolledBack);
    EXPECT_EQ(p->model().geometry.size(), before);
    const auto invalid = pm.runCommand(*p, "bad.invalid");
    EXPECT_FALSE(invalid.ok);
    EXPECT_NE(invalid.error.find("name"), std::string::npos);
    EXPECT_EQ(p->model().controlPoints.size(), 0u);
    const auto late = pm.runCommand(*p, "bad.late");
    EXPECT_FALSE(late.ok);
    EXPECT_NE(late.error.find("initialisation"), std::string::npos);
    const auto thr = pm.runCommand(*p, "bad.throws");
    EXPECT_FALSE(thr.ok);
    EXPECT_NE(thr.error.find("exception"), std::string::npos);
    pm.unload("org.openmine.tests.misbehaving");
    EXPECT_FALSE(pm.runCommand(*p, "bad.open-edit").ok);
    EXPECT_EQ(PluginManager::apiVersion(), 1);
}

TEST(Plugins, RescanReportsCurrentStateAndAppliesDisabling) {
    auto count = [](const PluginManager& pm, const std::string& id, bool loadedOnly) {
        return std::count_if(pm.plugins().begin(), pm.plugins().end(),
                             [&](const PluginStatus& s) { return s.manifest.id == id && (!loadedOnly || s.loaded); });
    };
    const std::string stations = "org.openmine.samples.stations", future = "org.openmine.tests.future";
    PluginManager pm;
    EXPECT_EQ(pm.loadAll({kDir}, {}), 2);
    const auto entries = pm.plugins().size();
    // Rescanning neither duplicates entries nor turns a stale failure into "already present".
    EXPECT_EQ(pm.loadAll({kDir}, {}), 2);
    EXPECT_EQ(pm.plugins().size(), entries);
    EXPECT_EQ(count(pm, future, false), 1);
    for (const auto& s : pm.plugins())
        if (s.manifest.id == future) EXPECT_NE(s.error.find("API version 99"), std::string::npos) << s.error;
    // Disabling a loaded plugin unloads it at the next scan; its registrations disappear.
    EXPECT_EQ(pm.loadAll({kDir}, {stations}), 1);
    EXPECT_EQ(count(pm, stations, true), 0);
    EXPECT_EQ(count(pm, stations, false), 1);
    EXPECT_FALSE(pm.importerFor("a.stn"));
    // Enabling it again loads it.
    EXPECT_EQ(pm.loadAll({kDir}, {}), 2);
    EXPECT_EQ(count(pm, stations, true), 1);
    EXPECT_TRUE(pm.importerFor("a.stn"));
    // Unloading removes every entry of the id.
    pm.unload(stations);
    EXPECT_EQ(count(pm, stations, false), 0);
    // The same id found in a second folder is reported, not loaded twice.
    const auto copy = fs::temp_directory_path() / ("openmine-plugin-dup-" + Uuid::generate().toString());
    fs::create_directories(copy);
    fs::copy(kDir / "station_import", copy / "station_import", fs::copy_options::recursive);
    EXPECT_EQ(pm.loadAll({kDir, copy}, {}), 2);
    EXPECT_EQ(count(pm, stations, true), 1);
    bool reported = false;
    for (const auto& s : pm.plugins())
        if (s.manifest.id == stations && !s.loaded) reported = s.error.find("already loaded from") != std::string::npos;
    EXPECT_TRUE(reported);
    fs::remove_all(copy);
}
