// SPDX-License-Identifier: Apache-2.0
// Performance benchmarks of the engine (no UI): schedule calculation, levelling, project save/load,
// octree build/read, k-d tree queries and point-cloud filtering at realistic sizes.
//
//   openmine_bench [--quick] [--json <file>] [--check <budgets.json>]
//
// --quick runs small sizes (a functional check for ctest). --check compares the seconds of each
// benchmark with the budget of the same name and exits with status 1 when one is exceeded.
#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Cpm.h"
#include "openmine/scheduling/Levelling.h"
#include "openmine/scheduling/ScheduleInput.h"
#include "openmine/survey/KdTree.h"
#include "openmine/survey/Octree.h"
#include "openmine/survey/Processing.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace openmine;
namespace fs = std::filesystem;

namespace {

struct Result {
    std::string name;
    std::size_t size = 0;
    std::string unit;
    double seconds = 0;
};

double timeIt(const std::function<void()>& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

/// A schedule of `n` tasks in `chains` parallel chains with cross links, all on one resource pool.
std::unique_ptr<Project> scheduleProject(std::size_t n, std::size_t chains, int resources) {
    auto p = Project::createNew("Bench", Date::fromYmd(2026, 1, 5));
    ProjectEditor ed(*p);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> dur(8, 120);
    std::vector<Uuid> ids;
    ids.reserve(n);
    std::vector<Uuid> pool;
    for (int r = 0; r < resources; ++r) {
        Resource res;
        res.name = "Jumbo " + std::to_string(r + 1);
        res.capacity = 1;
        pool.push_back(ed.add(res));
    }
    for (std::size_t i = 0; i < n; ++i) {
        ScheduleActivity a;
        a.name = "Task " + std::to_string(i);
        a.durationHours = dur(rng);
        ids.push_back(ed.addActivity(a));
        if (i >= chains) {
            ActivityDependency d;
            d.predecessorId = ids[i - chains];
            d.successorId = ids[i];
            d.type = DependencyType::FinishToStart;
            ed.add(d);
        }
        if (i >= 3 * chains && i % 7 == 0) { // a cross link to another chain
            ActivityDependency d;
            d.predecessorId = ids[i - 3 * chains + 1];
            d.successorId = ids[i];
            d.type = DependencyType::StartToStart;
            d.lagHours = 24;
            ed.add(d);
        }
        if (!pool.empty()) {
            ActivityResourceAssignment asg;
            asg.activityId = ids[i];
            asg.resourceId = pool[i % pool.size()];
            ed.add(asg);
        }
    }
    return p;
}

survey::PointBuffer cloud(std::size_t n) {
    survey::PointBuffer b;
    b.xyz.reserve(n);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(0, 200);
    std::normal_distribution<double> noise(0, 0.02);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = u(rng), y = u(rng);
        b.xyz.push_back({50000 + x, 7000000 + y, 900 + 0.05 * x + 2 * std::sin(y / 15) + noise(rng)});
    }
    return b;
}

} // namespace

int main(int argc, char** argv) {
    bool quick = false;
    std::string jsonOut, budgets;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quick") quick = true;
        else if (a == "--json" && i + 1 < argc) jsonOut = argv[++i];
        else if (a == "--check" && i + 1 < argc) budgets = argv[++i];
        else {
            std::cerr << "usage: openmine_bench [--quick] [--json file] [--check budgets.json]\n";
            return 2;
        }
    }
    const std::size_t nAct = quick ? 500 : 10000, nLevel = quick ? 200 : 2000, nGeom = quick ? 500 : 20000, nPts = quick ? 50000 : 2000000;
    std::vector<Result> results;
    const fs::path dir = fs::temp_directory_path() / ("openmine-bench-" + Uuid::generate().toString());
    fs::create_directories(dir);

    // Schedule: build the input and run CPM.
    {
        auto p = scheduleProject(nAct, 40, 0);
        scheduling::ScheduleResult r;
        results.push_back({"cpm", nAct, "activities", timeIt([&] { r = scheduling::calculateSchedule(*p); })});
        if (!r.ok) {
            std::cerr << "CPM failed: " << (r.errors.empty() ? "" : r.errors.front()) << "\n";
            return 1;
        }
    }
    // Levelling on a shared resource pool.
    {
        auto p = scheduleProject(nLevel, 20, 6);
        const auto in = scheduling::buildScheduleInput(*p);
        const auto cpm = scheduling::calculateSchedule(in);
        scheduling::ScheduleResult lv;
        results.push_back({"levelling", nLevel, "activities", timeIt([&] { lv = scheduling::levelResources(in, cpm); })});
        if (!lv.ok) {
            std::cerr << "levelling failed\n";
            return 1;
        }
    }
    // Project save and load with many polylines.
    {
        auto p = Project::createNew("Geometry", Date::fromYmd(2026, 1, 5));
        ProjectEditor ed(*p);
        const Uuid layer = p->model().layers.all().front().id;
        std::mt19937 rng(5);
        std::uniform_real_distribution<double> u(0, 1000);
        for (std::size_t i = 0; i < nGeom; ++i) {
            GeometryObject g;
            g.name = "Line " + std::to_string(i);
            g.layerId = layer;
            for (int k = 0; k < 20; ++k) g.vertices.push_back({-25000 + u(rng), 2900000 + u(rng), 1200 + u(rng) / 10});
            ed.addGeometry(std::move(g));
        }
        const fs::path f = dir / "bench.openmine";
        results.push_back({"project-save", nGeom, "polylines (20 vertices)", timeIt([&] { persistence::saveProject(*p, f); })});
        std::unique_ptr<Project> q;
        results.push_back({"project-load", nGeom, "polylines (20 vertices)", timeIt([&] { q = persistence::loadProject(f); })});
        if (q->model().geometry.size() != nGeom) {
            std::cerr << "load lost objects\n";
            return 1;
        }
    }
    // Point clouds.
    {
        const auto pts = cloud(nPts);
        survey::OctreeInfo info;
        results.push_back({"octree-build", nPts, "points", timeIt([&] {
                               survey::BufferSource src(pts);
                               info = survey::buildOctree(src, dir / "octree");
                           })});
        survey::PointBuffer all;
        results.push_back({"octree-read", nPts, "points", timeIt([&] { all = survey::OctreeReader(dir / "octree").readAll(); })});
        if (all.size() != nPts || info.pointCount != static_cast<std::int64_t>(nPts)) {
            std::cerr << "octree lost points\n";
            return 1;
        }
        std::unique_ptr<survey::KdTree> tree;
        results.push_back({"kdtree-build", nPts, "points", timeIt([&] { tree = std::make_unique<survey::KdTree>(pts.xyz); })});
        const std::size_t nq = quick ? 10000 : 200000;
        std::size_t found = 0;
        results.push_back({"kdtree-knn16", nq, "queries", timeIt([&] {
                               for (std::size_t i = 0; i < nq; ++i) found += tree->knn(pts.xyz[(i * 7919) % pts.size()], 16).size();
                           })});
        if (found != nq * 16) {
            std::cerr << "k-d tree queries incomplete\n";
            return 1;
        }
        results.push_back({"statistical-outliers", nPts, "points", timeIt([&] { (void)survey::statisticalInliers(pts, 16, 2.0); })});
        results.push_back({"voxel-0.25m", nPts, "points", timeIt([&] { (void)survey::voxelDownsample(pts, 0.25); })});
    }
    fs::remove_all(dir);

    std::printf("%-22s %10s  %-26s %10s %14s\n", "benchmark", "size", "unit", "seconds", "per second");
    nlohmann::json j = nlohmann::json::array();
    for (const auto& r : results) {
        std::printf("%-22s %10zu  %-26s %10.3f %14.0f\n", r.name.c_str(), r.size, r.unit.c_str(), r.seconds, r.seconds > 0 ? r.size / r.seconds : 0.0);
        j.push_back({{"name", r.name}, {"size", r.size}, {"unit", r.unit}, {"seconds", r.seconds}});
    }
    if (!jsonOut.empty()) std::ofstream(jsonOut) << j.dump(2) << "\n";
    if (!budgets.empty()) {
        std::ifstream in(budgets);
        if (!in) {
            std::cerr << "cannot read " << budgets << "\n";
            return 2;
        }
        const auto b = nlohmann::json::parse(in);
        int over = 0;
        for (const auto& r : results)
            if (b.contains(r.name) && r.seconds > b[r.name].get<double>()) {
                std::fprintf(stderr, "over budget: %s %.3f s > %.3f s\n", r.name.c_str(), r.seconds, b[r.name].get<double>());
                ++over;
            }
        if (over) return 1;
        std::puts("all benchmarks within budget");
    }
    return 0;
}
