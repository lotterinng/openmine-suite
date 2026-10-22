// SPDX-License-Identifier: Apache-2.0
#include "openmine/formats/MeshFormats.h"
#include "TempDir.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/objects/Uuid.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace openmine;
using namespace openmine::formats;
namespace fs = std::filesystem;

namespace {
test::TempDir tempDir() { return test::TempDir("openmine_mesh_"); }
MeshData sample() {
    // A closed prism at mine-grid coordinates.
    auto m = geometry::extrudeOutline(std::vector<Vec2>{{-25000.125, 2900000.5}, {-24990.0, 2900000.5}, {-24990.0, 2900010.25}, {-25000.125, 2900007.0}}, -812.3, -805.1);
    return m;
}
} // namespace

TEST(MeshFormats, ExactRoundTripsObjPlyAsciiStl) {
    const auto dir = tempDir();
    const auto m = sample();
    for (const char* name : {"m.obj", "m.ply", "a.stl"}) {
        SCOPED_TRACE(name);
        if (std::string(name) == "a.stl") writeStl(dir / name, m, StlEncoding::Ascii, "prism");
        else writeMesh(dir / name, m, "prism");
        const auto f = readMesh(dir / name);
        EXPECT_EQ(f.mesh.triangles.size(), m.triangles.size());
        EXPECT_EQ(f.mesh.vertices.size(), m.vertices.size());
        EXPECT_DOUBLE_EQ(geometry::meshSignedVolume(f.mesh), geometry::meshSignedVolume(m));
        EXPECT_TRUE(geometry::topology(f.mesh).watertight());
    }
    writePly(dir / "ascii.ply", m, false);
    EXPECT_DOUBLE_EQ(geometry::meshSignedVolume(readMesh(dir / "ascii.ply").mesh), geometry::meshSignedVolume(m));
    // Binary STL is single precision: vertices re-merge, volume within float accuracy.
    const auto w = writeMesh(dir / "b.stl", m, "prism");
    ASSERT_EQ(w.size(), 1u); // mine-grid coordinates do not fit float32
    EXPECT_NE(w[0].find("float32"), std::string::npos);
    const auto b = readMesh(dir / "b.stl");
    EXPECT_EQ(b.mesh.vertices.size(), m.vertices.size());
    EXPECT_FALSE(b.warnings.empty());
    EXPECT_EQ(b.name, "OpenMine prism");
}

TEST(MeshFormats, ObjPolygonsNegativeIndicesAndErrors) {
    const auto dir = tempDir();
    {
        std::ofstream o(dir / "p.obj");
        // An L-shaped (concave) hexagon, a quad with relative indices and texture/normal references.
        o << "o shapes\nv 0 0 0\nv 2 0 0\nv 2 1 0\nv 1 1 0\nv 1 2 0\nv 0 2 0\nf 1 2 3 4 5 6\n";
        o << "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\nf -4/1/1 -3/2/1 -2/3/1 -1/4/1\n";
        std::ofstream bad(dir / "bad.obj");
        bad << "v 0 0 0\nf 1 2 3\n";
    }
    const auto f = readMesh(dir / "p.obj");
    EXPECT_EQ(f.name, "shapes");
    EXPECT_EQ(f.polygonsTriangulated, 2u);
    EXPECT_EQ(f.mesh.triangles.size(), 4u + 2u);
    double area = 0;
    for (const auto& t : f.mesh.triangles) area += cross(f.mesh.vertices[t[1]] - f.mesh.vertices[t[0]], f.mesh.vertices[t[2]] - f.mesh.vertices[t[0]]).z / 2;
    EXPECT_NEAR(area, 3.0 + 1.0, 1e-12); // all counter-clockwise
    EXPECT_THROW(readMesh(dir / "bad.obj"), MeshFormatError);
    EXPECT_THROW(readMesh(dir / "missing.stl"), MeshFormatError);
    EXPECT_THROW(readMesh(dir / "p.unknown"), MeshFormatError);
}
