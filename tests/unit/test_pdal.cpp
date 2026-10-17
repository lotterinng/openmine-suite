// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/MeshOps.h"
#include "openmine/pdalio/Pdal.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/PointCloudProject.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <numbers>
#include <random>

using namespace openmine;
using namespace openmine::survey;
namespace fs = std::filesystem;

namespace {
fs::path tempDir() {
    auto d = fs::temp_directory_path() / ("openmine_pdal_" + Uuid::generate().toString());
    fs::create_directories(d);
    return d;
}

PointBuffer cloud(std::size_t n) {
    PointSchema s;
    s.intensity = s.rgb = s.classification = s.gpsTime = s.returnInfo = true;
    s.scalarFields = {"Deviation"};
    PointBuffer b(s);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(0, 1);
    for (std::size_t i = 0; i < n; ++i) {
        b.xyz.push_back({-25000.0 + u(rng) * 100, 2900000.0 + u(rng) * 50, -812.0 + u(rng) * 5});
        b.intensity.push_back(static_cast<std::uint16_t>(i * 7 % 65536));
        b.rgb.push_back({static_cast<std::uint16_t>(i % 65536), static_cast<std::uint16_t>((i * 3) % 65536), static_cast<std::uint16_t>((i * 5) % 65536)});
        b.classification.push_back(static_cast<std::uint8_t>(i % 32));
        b.returnNumber.push_back(1);
        b.numberOfReturns.push_back(2);
        b.gpsTime.push_back(2.5e8 + static_cast<double>(i) * 1e-4);
        b.scalars[0].push_back(static_cast<float>(u(rng) - 0.5));
    }
    return b;
}
} // namespace

TEST(Pdal, LasAndLazRoundTripPreserveAttributes) {
    const auto dir = tempDir();
    const auto c = cloud(5000);
    for (const char* name : {"c.las", "c.laz"}) {
        SCOPED_TRACE(name);
        BufferSource src(c);
        pdalio::writePdal(src, dir / name);
        auto in = pdalio::openPdalSource(dir / name);
        EXPECT_EQ(in->countHint(), 5000);
        const auto s = in->schema();
        EXPECT_TRUE(s.intensity && s.rgb && s.classification && s.gpsTime && s.returnInfo);
        ASSERT_NE(std::find(s.scalarFields.begin(), s.scalarFields.end(), "Deviation"), s.scalarFields.end());
        const auto b = readAll(*in);
        ASSERT_EQ(b.size(), c.size());
        const int dev = b.scalarIndex("Deviation");
        for (std::size_t i = 0; i < c.size(); ++i) {
            ASSERT_NEAR(b.xyz[i].x, c.xyz[i].x, 0.5e-4 + 1e-9); // LAS stores 0.1 mm integers
            ASSERT_NEAR(b.xyz[i].z, c.xyz[i].z, 0.5e-4 + 1e-9);
            ASSERT_EQ(b.intensity[i], c.intensity[i]);
            ASSERT_EQ(b.rgb[i], c.rgb[i]);
            ASSERT_EQ(b.classification[i], c.classification[i]);
            ASSERT_EQ(b.numberOfReturns[i], 2);
            ASSERT_EQ(b.gpsTime[i], c.gpsTime[i]);
            ASSERT_EQ(b.scalars[static_cast<std::size_t>(dev)][i], c.scalars[0][i]);
        }
        // Rewind and stream again in small reads.
        in->rewind();
        PointBuffer part;
        std::size_t total = 0, k;
        while ((k = in->read(part, 333)) > 0) total += k;
        EXPECT_EQ(total, 5000u);
    }
    EXPECT_LT(fs::file_size(dir / "c.laz"), fs::file_size(dir / "c.las") / 2);
    fs::remove_all(dir);
}

TEST(Pdal, PlyAndE57RoundTrip) {
    const auto dir = tempDir();
    const auto c = cloud(2000);
    for (const char* name : {"c.ply", "c.e57"}) {
        SCOPED_TRACE(name);
        BufferSource src(c);
        pdalio::writePdal(src, dir / name);
        const auto b = readAll(*pdalio::openPdalSource(dir / name));
        ASSERT_EQ(b.size(), c.size());
        EXPECT_TRUE(b.schema().rgb);
        for (std::size_t i = 0; i < c.size(); i += 37) {
            EXPECT_NEAR(b.xyz[i].x, c.xyz[i].x, 1e-6);
            EXPECT_NEAR(b.xyz[i].y, c.xyz[i].y, 1e-6);
        }
    }
    fs::remove_all(dir);
}

TEST(Pdal, ProjectImportThroughRegistryAndSmrf) {
    pdalio::registerFormats();
    const auto dir = tempDir();
    // Terrain: plane z = 0.05 x with 1 m boxes standing on it (non-ground).
    PointBuffer t;
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> u(0, 1);
    std::size_t objects = 0;
    for (int i = 0; i < 40000; ++i) {
        const double x = u(rng) * 100, y = u(rng) * 100;
        double z = 0.05 * x;
        const bool onBox = (std::fmod(x, 20.0) < 4 && std::fmod(y, 20.0) < 4);
        if (onBox) {
            z += 3.0;
            ++objects;
        }
        t.xyz.push_back({x, y, z});
    }
    BufferSource src(t);
    pdalio::writePdal(src, dir / "terrain.laz");
    auto pp = Project::createNew("Scan", Date::fromYmd(2026, 11, 2));
    const Uuid id = importPointCloud(*pp, dir / "terrain.laz");
    EXPECT_EQ(dataset(*pp, id).sourceFormat, "LAZ");
    const auto pts = datasetPoints(*pp, id);
    ASSERT_EQ(pts.size(), 40000u);
    const auto cls = pdalio::smrfGround(pts);
    std::size_t correct = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const bool high = pts.xyz[i].z > 0.05 * pts.xyz[i].x + 1.5;
        correct += (cls[i] == 2) == !high;
    }
    EXPECT_GT(static_cast<double>(correct) / static_cast<double>(pts.size()), 0.98);
    // Export a dataset to LAZ through the registry.
    exportDataset(*pp, id, dir / "again.laz");
    EXPECT_EQ(readAll(*pdalio::openPdalSource(dir / "again.laz")).size(), 40000u);
    fs::remove_all(dir);
}

TEST(Pdal, PoissonSurfaceOfASphere) {
    PointSchema s;
    s.normals = true;
    PointBuffer b(s);
    std::mt19937 rng(5);
    std::normal_distribution<double> n(0, 1);
    for (int i = 0; i < 20000; ++i) {
        Vec3 d{n(rng), n(rng), n(rng)};
        d = d.normalized();
        b.xyz.push_back(d * 5.0);
        b.normal.push_back({static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)});
    }
    const auto mesh = pdalio::poissonSurface(b, {8, false});
    EXPECT_GT(mesh.triangles.size(), 1000u);
    const double area = geometry::meshArea(mesh);
    EXPECT_NEAR(area, 4 * std::numbers::pi * 25, 0.03 * 4 * std::numbers::pi * 25);
    PointBuffer noNormals;
    noNormals.xyz = b.xyz;
    EXPECT_THROW(pdalio::poissonSurface(noNormals), PointCloudError);
}
