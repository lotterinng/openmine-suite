// SPDX-License-Identifier: Apache-2.0
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/Octree.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/TextFormats.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

using namespace openmine;
using namespace openmine::survey;
namespace fs = std::filesystem;

namespace {
fs::path tempDir() {
    auto d = fs::temp_directory_path() / ("openmine_survey_" + Uuid::generate().toString());
    fs::create_directories(d);
    return d;
}

PointBuffer randomCloud(std::size_t n, unsigned seed, bool attributes = true) {
    PointSchema s;
    s.intensity = attributes;
    s.rgb = attributes;
    s.classification = attributes;
    s.gpsTime = attributes;
    if (attributes) s.scalarFields = {"deviation"};
    PointBuffer b(s);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    for (std::size_t i = 0; i < n; ++i) {
        // A tunnel-like surface far from the origin (large coordinates as in a mine grid).
        const double t = u(rng) * 200, a = u(rng) * 3.14159;
        b.xyz.push_back({-25000.0 + t, 2900000.0 + 3 * std::cos(a), -812.25 + 3 * std::sin(a) + u(rng) * 0.01});
        if (attributes) {
            b.intensity.push_back(static_cast<std::uint16_t>(rng() & 0xFFFF));
            b.rgb.push_back({static_cast<std::uint16_t>(rng() & 0xFF00), static_cast<std::uint16_t>(rng() & 0xFF00), static_cast<std::uint16_t>(rng() & 0xFF00)});
            b.classification.push_back(static_cast<std::uint8_t>(rng() % 20));
            b.gpsTime.push_back(1e8 + static_cast<double>(i) * 1e-3);
            b.scalars[0].push_back(static_cast<float>(u(rng)));
        }
    }
    return b;
}

std::vector<std::array<double, 4>> sortedKeys(const PointBuffer& b) {
    std::vector<std::array<double, 4>> k;
    for (std::size_t i = 0; i < b.size(); ++i) k.push_back({b.xyz[i].x, b.xyz[i].y, b.xyz[i].z, b.schema().gpsTime ? b.gpsTime[i] : 0.0});
    std::sort(k.begin(), k.end());
    return k;
}
} // namespace

TEST(SurveyText, CsvRoundTripWithAllAttributes) {
    const auto dir = tempDir();
    auto cloud = randomCloud(1000, 1);
    {
        TextWriteOptions o;
        o.decimals = 6;
        auto sink = textSink(dir / "c.csv", o);
        sink->begin(cloud.schema());
        sink->write(cloud);
        sink->finish();
    }
    auto src = openTextSource(dir / "c.csv");
    EXPECT_EQ(src->formatName(), "CSV");
    const auto s = src->schema();
    EXPECT_TRUE(s.intensity && s.rgb && s.classification && s.gpsTime);
    ASSERT_EQ(s.scalarFields, std::vector<std::string>{"deviation"});
    const auto back = readAll(*src);
    ASSERT_EQ(back.size(), cloud.size());
    for (std::size_t i = 0; i < cloud.size(); ++i) {
        EXPECT_NEAR(back.xyz[i].x, cloud.xyz[i].x, 5e-7);
        EXPECT_NEAR(back.xyz[i].z, cloud.xyz[i].z, 5e-7);
        EXPECT_EQ(back.intensity[i], cloud.intensity[i]);
        EXPECT_EQ(back.rgb[i], cloud.rgb[i]);
        EXPECT_EQ(back.classification[i], cloud.classification[i]);
        EXPECT_NEAR(back.scalars[0][i], cloud.scalars[0][i], 1e-6);
    }
    // Rewind gives the same points again.
    src->rewind();
    EXPECT_EQ(readAll(*src).size(), cloud.size());
    fs::remove_all(dir);
}

TEST(SurveyText, HeaderlessXyzPtsAndErrors) {
    const auto dir = tempDir();
    {
        std::ofstream o(dir / "a.xyz");
        o << "# comment\n1 2 3\n4 5 6\n\n7\t8\t9\n";
        std::ofstream p(dir / "b.pts");
        p << "2\n1.5 2.5 3.5 -2048 255 0 0\n4 5 6 2047 0 255 0\n1\n7 8 9 0 0 0 255\n";
        std::ofstream bad(dir / "bad.csv");
        bad << "x,y,z\n1,2,3\n4,abc,6\n";
        std::ofstream two(dir / "two.txt");
        two << "1 2\n";
    }
    auto xyz = readAll(*openTextSource(dir / "a.xyz"));
    ASSERT_EQ(xyz.size(), 3u);
    EXPECT_EQ(xyz.xyz[2].z, 9.0);
    EXPECT_FALSE(xyz.schema().intensity);
    auto pts = openPtsSource(dir / "b.pts");
    EXPECT_EQ(pts->countHint(), 2);
    const auto pb = readAll(*pts);
    ASSERT_EQ(pb.size(), 3u); // second scan included
    EXPECT_EQ(pb.intensity[0], 0);
    EXPECT_EQ(pb.intensity[1], 65535);
    EXPECT_EQ(pb.rgb[0].r, 65535);
    EXPECT_EQ(pb.rgb[2].b, 65535);
    EXPECT_THROW(readAll(*openTextSource(dir / "bad.csv")), PointCloudError);
    EXPECT_THROW(openTextSource(dir / "two.txt"), PointCloudError);
    EXPECT_THROW(openPointFile(dir / "missing.las"), PointCloudError);
    // PTS export: intensity mapped to −2048…2047, colours to 8 bit.
    {
        auto sink = ptsSink(dir / "out.pts");
        sink->begin(pb.schema());
        sink->write(pb);
        sink->finish();
    }
    const auto again = readAll(*openPtsSource(dir / "out.pts"));
    ASSERT_EQ(again.size(), 3u);
    EXPECT_EQ(again.intensity[0], 0);
    EXPECT_EQ(again.intensity[1], 65535);
    EXPECT_EQ(again.rgb[0], pb.rgb[0]);
    fs::remove_all(dir);
}

TEST(SurveyText, PtxScansAreTransformedAndNoReturnsSkipped) {
    const auto dir = tempDir();
    {
        std::ofstream o(dir / "s.ptx");
        // Scan 1: 2 × 2 grid, translated by (100, 200, 300) and rotated 90° about Z.
        o << "2\n2\n100 200 300\n1 0 0\n0 1 0\n0 0 1\n";
        o << "0 1 0 0\n-1 0 0 0\n0 0 1 0\n100 200 300 1\n";
        o << "1 0 0 0.5\n0 0 0 0.5\n2 0 0 1.0\n0 1 0 0\n";
        // Scan 2: identity, one row of 1.
        o << "1\n1\n0 0 0\n1 0 0\n0 1 0\n0 0 1\n1 0 0 0\n0 1 0 0\n0 0 1 0\n0 0 0 1\n5 6 7 0.25\n";
    }
    auto src = openPtxSource(dir / "s.ptx");
    EXPECT_EQ(src->countHint(), 5);
    const auto b = readAll(*src);
    ASSERT_EQ(b.size(), 4u); // one (0,0,0) no-return skipped
    // Rotation 90° about Z: (1,0,0) → (0,1,0); then + (100,200,300).
    EXPECT_NEAR(b.xyz[0].x, 100, 1e-12);
    EXPECT_NEAR(b.xyz[0].y, 201, 1e-12);
    EXPECT_NEAR(b.xyz[0].z, 300, 1e-12);
    EXPECT_NEAR(b.xyz[2].x, 99, 1e-12); // (0,1,0) → (−1,0,0)
    EXPECT_EQ(b.intensity[0], 32768);
    EXPECT_EQ(b.xyz[3], (Vec3{5, 6, 7}));
    const auto sc = ptxScannerPositions(dir / "s.ptx");
    ASSERT_EQ(sc.size(), 2u);
    EXPECT_EQ(sc[0], (Vec3{100, 200, 300}));
    fs::remove_all(dir);
}

TEST(SurveyOctree, ChunkedBuildStoresEveryPointOnceBitExact) {
    const auto dir = tempDir();
    const auto cloud = randomCloud(120000, 7);
    BufferSource src(cloud);
    OctreeBuildOptions o;
    o.maxNodePoints = 3000;
    o.maxChunkPoints = 20000; // forces the out-of-core path
    o.grid = 32;
    const auto info = buildOctree(src, dir / "oct", o);
    EXPECT_EQ(info.pointCount, 120000);
    EXPECT_GT(info.nodes.size(), 20u);
    OctreeReader r(dir / "oct");
    EXPECT_EQ(r.info().pointCount, 120000);
    std::int64_t total = 0;
    for (const auto& n : r.info().nodes) {
        total += n.count;
        if (n.name != "r") EXPECT_TRUE(r.node(n.name.substr(0, n.name.size() - 1))) << "orphan node " << n.name;
        EXPECT_TRUE(n.cube.contains(n.bounds.min) && n.cube.contains(n.bounds.max)) << n.name;
    }
    EXPECT_EQ(total, 120000);
    const auto all = r.readAll();
    EXPECT_EQ(sortedKeys(all), sortedKeys(cloud)); // bit-exact
    // Attributes travel with their points.
    std::map<double, std::size_t> byTime;
    for (std::size_t i = 0; i < cloud.size(); ++i) byTime[cloud.gpsTime[i]] = i;
    for (std::size_t i = 0; i < all.size(); i += 997) {
        const auto j = byTime.at(all.gpsTime[i]);
        EXPECT_EQ(all.intensity[i], cloud.intensity[j]);
        EXPECT_EQ(all.rgb[i], cloud.rgb[j]);
        EXPECT_EQ(all.scalars[0][i], cloud.scalars[0][j]);
    }
    // Level of detail: an inner node owns at most one point per sampling cell.
    for (const auto& n : r.info().nodes) {
        if (r.children(n.name).empty()) continue;
        const auto pts = r.readNode(n.name);
        std::set<std::array<long, 3>> cells;
        for (const auto& p : pts.xyz)
            cells.insert({static_cast<long>((p.x - n.cube.min.x) / n.spacing), static_cast<long>((p.y - n.cube.min.y) / n.spacing),
                          static_cast<long>((p.z - n.cube.min.z) / n.spacing)});
        EXPECT_EQ(cells.size(), pts.size()) << n.name;
    }
    // Coarse selection reads far fewer points.
    const auto coarse = r.select([](const OctreeNodeInfo& n) { return n.level < 1; });
    std::int64_t coarseCount = 0;
    for (const auto* n : coarse) coarseCount += n->count;
    EXPECT_LT(coarseCount, 120000 / 4);
    // Box query.
    const BoundingBox box{{-24950, 2899990, -900}, {-24940, 2900010, -800}};
    const auto inBox = r.readAll(box);
    std::size_t expected = 0;
    for (const auto& p : cloud.xyz) expected += box.contains(p);
    EXPECT_EQ(inBox.size(), expected);
    // Streaming source over the octree.
    EXPECT_EQ(readAll(*r.source()).size(), 120000u);
    // A non-empty target folder is refused.
    BufferSource again(cloud);
    EXPECT_THROW(buildOctree(again, dir / "oct", o), PointCloudError);
    fs::remove_all(dir);
}

TEST(SurveyProject, ImportTransformDeriveExport) {
    const auto dir = tempDir();
    auto pp = Project::createNew("Scan", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    {
        std::ofstream o(dir / "scan.csv");
        o << "x,y,z,intensity\n";
        for (int i = 0; i < 2000; ++i) o << i * 0.01 << "," << (i % 50) * 0.1 << ",-100," << (i % 256) << "\n";
    }
    ImportOptions io;
    io.transform = Transform3::translation({1000, 2000, 0});
    io.scanner = "Test scanner";
    const Uuid id = importPointCloud(p, dir / "scan.csv", io);
    const auto& d = dataset(p, id);
    EXPECT_EQ(d.pointCount, 2000);
    EXPECT_EQ(d.sourceFormat, "CSV");
    EXPECT_EQ(d.colorMode, "Intensity");
    EXPECT_NEAR(d.storedBounds.max.x, 19.99, 1e-12);
    const auto pts = datasetPoints(p, id);
    EXPECT_NEAR(pts.bounds().min.x, 1000, 1e-9);
    const auto stored = datasetPoints(p, id, false);
    EXPECT_NEAR(stored.bounds().min.x, 0, 1e-12);
    const auto boxed = datasetPoints(p, id, true, BoundingBox{{1000, 2000, -200}, {1001, 2001, 0}});
    EXPECT_EQ(boxed.size(), 23u); // x ∈ [0,1] → i ≤ 100 and y ≤ 1 → i % 50 ≤ 10: i = 0…10, 50…60, 100
    // One undo step removes the dataset and its asset record.
    const auto before = p.model().pointClouds.size();
    p.commands().undo();
    EXPECT_EQ(p.model().pointClouds.size(), before - 1);
    p.commands().redo();
    // Derived dataset in project coordinates.
    PointBuffer half = pts.subset([&] {
        std::vector<std::size_t> v;
        for (std::size_t i = 0; i < pts.size(); i += 2) v.push_back(i);
        return v;
    }());
    const Uuid did = commitDataset(p, prepareDerived(p, id, half, "Every second point", "decimate 1:2"), "Decimate");
    const auto& dd = dataset(p, did);
    EXPECT_EQ(dd.parentDatasetId, id);
    EXPECT_TRUE(dd.transform.isIdentity());
    EXPECT_EQ(dd.pointCount, 1000);
    // Export in project coordinates.
    exportDataset(p, id, dir / "out.csv");
    const auto exported = readAll(*openTextSource(dir / "out.csv"));
    EXPECT_EQ(exported.size(), 2000u);
    EXPECT_NEAR(exported.bounds().min.x, 1000, 1e-9);
    EXPECT_THROW(exportDataset(p, id, dir / "out.unknown"), PointCloudError);
    fs::remove_all(dir);
}

TEST(SurveyProject, DeclaredCrsIsComparedNeverApplied) {
    EXPECT_EQ(authorityOf("epsg:2051"), "EPSG:2051");
    EXPECT_EQ(authorityOf(R"(PROJCS["Hartebeesthoek94 / Lo25",GEOGCS["Hartebeesthoek94",DATUM["Hartebeesthoek94",AUTHORITY["EPSG","6148"]],AUTHORITY["EPSG","4148"]],UNIT["metre",1],AUTHORITY["EPSG","2051"]])"),
              "EPSG:2051");
    EXPECT_EQ(authorityOf(R"(PROJCRS["WGS 84 / UTM zone 35S",BASEGEOGCRS["WGS 84",ID["EPSG",4326]],CS[Cartesian,2],ID["EPSG",32735]])"), "EPSG:32735");
    EXPECT_EQ(authorityOf(R"(LOCAL_CS["mine grid"])"), "");
    EXPECT_EQ(authorityOf("+proj=tmerc +lon_0=25"), "");

    auto p = Project::createNew("CRS", Date::fromYmd(2026, 11, 2));
    PointCloudDataset d;
    d.name = "scan";
    EXPECT_FALSE(crsMismatch(*p, d)); // declares nothing
    d.sourceCrs = "EPSG:2051";
    ASSERT_TRUE(crsMismatch(*p, d));  // the project CRS is not declared
    EXPECT_NE(crsMismatch(*p, d)->find("not declared"), std::string::npos);
    CoordinateReferenceSystem lo25;
    lo25.name = "Hartebeesthoek94 / Lo25";
    lo25.authorityCode = "EPSG:2051";
    lo25.kind = CrsKind::Projected;
    ProjectEditor ed(*p);
    ProjectInfo info = p->model().info;
    info.projectCrsId = ed.add(lo25);
    ed.setProjectInfo(info);
    EXPECT_FALSE(crsMismatch(*p, d));
    d.sourceCrs = "EPSG:32735";
    ASSERT_TRUE(crsMismatch(*p, d));
    EXPECT_NE(crsMismatch(*p, d)->find("not transformed"), std::string::npos);
}
