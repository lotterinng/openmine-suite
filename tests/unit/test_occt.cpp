// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Polyline.h"
#include "openmine/occt/SweptSolid.h"

#include <gtest/gtest.h>

#include <vector>

using namespace openmine;

TEST(Occt, WireLengthMatchesCoreGeometry) {
    std::vector<Vec3> pts{{-25000.0, 2900000.0, 1200.0}, {-25090.0, 2900000.0, 1188.75}, {-25090.0, 2900060.0, 1181.25}};
    EXPECT_NEAR(occt::wireLength(pts), geometry::length3d(pts), 1e-6);
}

TEST(Occt, StraightSweepVolumeIsWidthHeightLength) {
    std::vector<Vec3> pts{{0, 0, 0}, {150, 0, 0}};
    auto r = occt::sweepRectangularProfile(pts, 5.5, 5.5);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.valid);
    EXPECT_NEAR(r.volume, 5.5 * 5.5 * 150.0, 1e-6);
    EXPECT_NEAR(r.surfaceArea, 4 * 5.5 * 150.0 + 2 * 5.5 * 5.5, 1e-6);
}

TEST(Occt, SlopingStraightSweepUsesSlopeLength) {
    // 1:8 decline, 120 m plan → slope length sqrt(120² + 15²).
    std::vector<Vec3> pts{{0, 0, 0}, {120, 0, -15}};
    auto r = occt::sweepRectangularProfile(pts, 5.0, 5.0);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_NEAR(r.volume, 25.0 * geometry::length3d(pts), 1e-6);
}

TEST(Occt, RejectsInvalidInput) {
    EXPECT_FALSE(occt::sweepRectangularProfile(std::vector<Vec3>{{0, 0, 0}}, 5, 5).ok);
    EXPECT_FALSE(occt::sweepRectangularProfile(std::vector<Vec3>{{0, 0, 0}, {0, 0, 10}}, 5, 5).ok); // vertical
    EXPECT_FALSE(occt::sweepRectangularProfile(std::vector<Vec3>{{0, 0, 0}, {10, 0, 0}}, 0, 5).ok);
}

// ---- Solid kernel ---------------------------------------------------------------------------------
#include "openmine/cad/Edit.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Profile.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/mine_design/Development.h"
#include "openmine/occt/Solid.h"
#include "openmine/occt/SolidObjects.h"
#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>

namespace {
constexpr double kPi = std::numbers::pi;
std::filesystem::path tempDir() {
    auto d = std::filesystem::temp_directory_path() / ("openmine_occt_" + Uuid::generate().toString());
    std::filesystem::create_directories(d);
    return d;
}
} // namespace

TEST(OcctSolid, PrimitivesHaveExactProperties) {
    const auto box = occt::makeBox({0, 0, 0}, {10, 20, 30});
    const auto bi = occt::analyse(box);
    EXPECT_TRUE(bi.valid);
    EXPECT_TRUE(bi.closed);
    EXPECT_NEAR(bi.volume, 6000, 1e-9);
    EXPECT_NEAR(bi.area, 2 * (200 + 600 + 300), 1e-9);
    EXPECT_EQ(bi.faces, 6);
    EXPECT_EQ(bi.edges, 12);
    EXPECT_EQ(bi.vertices, 8);
    EXPECT_NEAR(bi.centreOfMass.z, 15, 1e-9);
    EXPECT_NEAR(occt::analyse(occt::makeCylinder({0, 0, 0}, {0, 0, 1}, 2, 5)).volume, kPi * 4 * 5, 1e-9);
    EXPECT_NEAR(occt::analyse(occt::makeSphere({1, 2, 3}, 3)).volume, 4.0 / 3.0 * kPi * 27, 1e-6);
    EXPECT_NEAR(occt::analyse(occt::makeCone({0, 0, 0}, {0, 0, 1}, 3, 1, 6)).volume, kPi * 6 / 3 * (9 + 3 + 1), 1e-6);
    EXPECT_THROW(occt::makeBox({0, 0, 0}, {0, 1, 1}), occt::SolidError);
    EXPECT_THROW(occt::makeCylinder({0, 0, 0}, {0, 0, 1}, -1, 5), occt::SolidError);
}

TEST(OcctSolid, FacesFromCurvesExtrudeAndRevolve) {
    // Rectangle 20 × 10 with a circular hole r = 2 → extrude 3 m.
    const auto rect = cad::makeRectangle({0, 0, 0}, {20, 10, 0});
    const auto hole = cad::makeCircle({10, 5, 0}, 2);
    const auto face = occt::planarFace(rect, {&hole});
    EXPECT_NEAR(occt::analyse(face).area, 200 - kPi * 4, 1e-9);
    const auto prism = occt::extrude(face, {0, 0, 3});
    EXPECT_NEAR(occt::analyse(prism).volume, 3 * (200 - kPi * 4), 1e-9);
    // Stadium from a bulged polyline: two semicircles r = 1 and a 4 × 2 rectangle → area 8 + π.
    auto stadium = cad::makePolyline({{0, 0, 0}, {4, 0, 0}, {4, 2, 0}, {0, 2, 0}}, true);
    stadium.bulges = {0.0, 1.0, 0.0, 1.0};
    EXPECT_NEAR(occt::analyse(occt::planarFace(stadium)).area, 8 + kPi, 1e-9);
    // Revolve a 1 × 2 rectangle at radius 3..4 about Z → annulus volume π(16 − 9)·2.
    const auto r = occt::planarFace(std::vector<Vec3>{{3, 0, 0}, {4, 0, 0}, {4, 0, 2}, {3, 0, 2}});
    EXPECT_NEAR(occt::analyse(occt::revolve(r, {0, 0, 0}, {0, 0, 1}, 2 * kPi)).volume, kPi * 7 * 2, 1e-6);
    EXPECT_THROW(occt::planarFace(cad::makeLine({0, 0, 0}, {1, 0, 0})), occt::SolidError);
    EXPECT_THROW(occt::planarFace(std::vector<Vec3>{{0, 0, 0}, {1, 0, 0}, {1, 1, 1}, {0, 1, -1}}), occt::SolidError);
}

TEST(OcctSolid, SweepAlongDeclineArcAndCorners) {
    TunnelProfile tp;
    tp.shape = ProfileShape::ShoulderArch;
    tp.width = 5.5;
    tp.height = 5.5;
    tp.shoulderRadius = 1.0;
    const auto curve = geometry::profileCurve(tp); // exact: shoulders are true arcs
    const auto& prof = curve.points;
    const auto& bul = curve.bulges;
    const double A = geometry::profileArea(tp);
    EXPECT_NEAR(A, 5.5 * 5.5 - 2 * (1 - kPi / 4), 1e-12);
    // Straight 1:7 decline: V = A × slope length.
    const auto straight = cad::makeLine({0, 0, 0}, {140, 0, -20});
    auto s = occt::sweepProfile(prof, bul, straight);
    auto info = occt::analyse(s);
    EXPECT_TRUE(info.valid);
    EXPECT_NEAR(info.volume, A * std::hypot(140.0, 20.0), 1e-9 * A * 140);
    // Horizontal plan arc (bulge): Pappus — the profile centroid is on the path (u = 0), so V = A × arc length.
    auto bend = cad::makePolyline({{0, 0, 0}, {30, 30, 0}});
    bend.bulges = {std::tan(kPi / 8), 0.0}; // 90° arc, radius 30
    info = occt::analyse(occt::sweepProfile(prof, bul, bend));
    EXPECT_TRUE(info.valid);
    EXPECT_NEAR(info.volume, A * 30 * kPi / 2, 1e-4 * A * 47);
    // Polyline with a 90° corner: right-corner sweep equals the core mitred envelope.
    const auto corner = cad::makePolyline({{0, 0, 0}, {50, 0, 0}, {50, 40, 0}});
    const auto polyProf = geometry::profileOutline(tp, 64);
    info = occt::analyse(occt::sweepProfile(polyProf, corner, occt::SweepTransition::RightCorner));
    const auto meshEnv = geometry::sweepProfile(corner.vertices, polyProf);
    EXPECT_TRUE(info.valid);
    EXPECT_NEAR(info.volume, geometry::meshSignedVolume(meshEnv), 1e-6 * info.volume);
    EXPECT_THROW(occt::sweepProfile({{0, 0}, {1, 0}}, straight), occt::SolidError);
}

TEST(OcctSolid, LoftBooleansFilletOffset) {
    const auto frustum = occt::loft({{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}}, {{2.5, 2.5, 10}, {7.5, 2.5, 10}, {7.5, 7.5, 10}, {2.5, 7.5, 10}}}, true, true);
    EXPECT_NEAR(occt::analyse(frustum).volume, 10.0 / 3 * (100 + 25 + 50), 1e-6);

    const auto a = occt::makeBox({0, 0, 0}, {10, 10, 10});
    const auto b = occt::makeBox({5, 5, 5}, {15, 15, 15});
    EXPECT_NEAR(occt::analyse(occt::boolean(a, b, occt::BooleanOp::Union)).volume, 2000 - 125, 1e-6);
    EXPECT_NEAR(occt::analyse(occt::boolean(a, b, occt::BooleanOp::Difference)).volume, 1000 - 125, 1e-6);
    const auto common = occt::boolean(a, b, occt::BooleanOp::Intersection);
    EXPECT_NEAR(occt::analyse(common).volume, 125, 1e-6);
    EXPECT_EQ(occt::analyse(common).faces, 6); // coplanar faces merged
    // Union of two touching boxes merges into one 6-face box.
    const auto u = occt::boolean(a, occt::makeBox({10, 0, 0}, {20, 10, 10}), occt::BooleanOp::Union);
    EXPECT_EQ(occt::analyse(u).faces, 6);
    EXPECT_THROW(occt::boolean(a, occt::makeBox({20, 20, 20}, {30, 30, 30}), occt::BooleanOp::Intersection), occt::SolidError);

    const double L = 10, r = 1;
    const double filleted = L * L * L - 12 * (1 - kPi / 4) * r * r * (L - 2 * r) - 8 * (1 - kPi / 6) * r * r * r;
    EXPECT_NEAR(occt::analyse(occt::filletAllEdges(a, r)).volume, filleted, 1e-4);
    const double d = 1;
    const double grown = L * L * L + 6 * L * L * d + 12 * (kPi / 4) * d * d * L + 4.0 / 3.0 * kPi * d * d * d;
    EXPECT_NEAR(occt::analyse(occt::offsetSolid(a, d)).volume, grown, 1e-4);
}

TEST(OcctSolid, MeshConversionTessellationSections) {
    const auto mesh = geometry::extrudeOutline(std::vector<Vec2>{{0, 0}, {10, 0}, {10, 4}, {0, 4}}, -5, 5);
    const auto s = occt::fromMesh(mesh);
    const auto info = occt::analyse(s);
    EXPECT_TRUE(info.valid);
    EXPECT_TRUE(info.closed);
    EXPECT_EQ(info.solids, 1);
    EXPECT_EQ(info.faces, 6);
    EXPECT_NEAR(info.volume, 400, 1e-9);
    // Open mesh → shell, not a solid.
    MeshData open = mesh;
    open.triangles.pop_back();
    EXPECT_EQ(occt::analyse(occt::fromMesh(open)).solids, 0);

    const auto cyl = occt::makeCylinder({0, 0, 0}, {0, 0, 1}, 2, 4);
    const auto t = occt::tessellate(cyl, 0.001, 0.1);
    EXPECT_TRUE(geometry::topology(t).watertight());
    EXPECT_NEAR(geometry::meshSignedVolume(t), kPi * 16, 0.01 * kPi * 16); // positive: outward
    const auto loops = occt::section(cyl, {0, 0, 2}, {0, 0, 1}, 1e-5);
    ASSERT_EQ(loops.size(), 1u);
    EXPECT_NEAR(geometry::length3d(loops[0]), 2 * kPi * 2, 1e-3);

    const auto box2 = occt::makeBox({13, 0, 0}, {14, 1, 1});
    const auto dist = occt::minimumDistance(occt::makeBox({0, 0, 0}, {10, 10, 10}), box2);
    EXPECT_NEAR(dist.distance, 3, 1e-9);
    EXPECT_EQ(occt::classify(cyl, {0, 0, 1}), occt::PointState::Inside);
    EXPECT_EQ(occt::classify(cyl, {2, 0, 1}), occt::PointState::On);
    EXPECT_EQ(occt::classify(cyl, {3, 0, 1}), occt::PointState::Outside);
}

TEST(OcctSolid, TransformsAndExchangeFormats) {
    const auto box = occt::makeBox({0, 0, 0}, {2, 3, 4});
    EXPECT_NEAR(occt::analyse(occt::transformed(box, Transform3::rotation({1, 1, 0}, 0.7))).volume, 24, 1e-9);
    EXPECT_NEAR(occt::analyse(occt::transformed(box, Transform3::scaling(2.0))).volume, 192, 1e-9);
    EXPECT_NEAR(occt::analyse(occt::transformed(box, Transform3::scaling({2, 1, 1}))).volume, 48, 1e-6);
    EXPECT_NEAR(occt::analyse(occt::transformed(box, Transform3::mirror({0, 0, 0}, {1, 0, 0}))).volume, 24, 1e-6);

    const auto again = occt::fromBrep(occt::toBrep(box));
    EXPECT_NEAR(occt::analyse(again).volume, 24, 1e-12);
    EXPECT_THROW(occt::fromBrep("garbage"), occt::SolidError);

    const auto dir = tempDir();
    const auto cyl = occt::makeCylinder({100, 200, -50}, {0, 0, 1}, 1.5, 3);
    occt::writeStep({box, cyl}, (dir / "two.step").string());
    {
        std::ifstream in(dir / "two.step");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("SI_UNIT($,.METRE.)"), std::string::npos); // declared in metres, not millimetres
    }
    const auto step = occt::readStep((dir / "two.step").string());
    double v = 0;
    for (const auto& s : step) v += occt::analyse(s).volume;
    EXPECT_NEAR(v, 24 + kPi * 2.25 * 3, 1e-6);
    occt::writeIges({box}, (dir / "box.igs").string());
    const auto iges = occt::readIges((dir / "box.igs").string());
    ASSERT_FALSE(iges.empty());
    EXPECT_NEAR(occt::analyse(iges[0]).area, 2 * (6 + 8 + 12), 1e-6);
    {
        std::ifstream in(dir / "box.igs");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find(",6,1HM,"), std::string::npos);  // unit flag 6 = metres
        EXPECT_NE(text.find(",4.,"), std::string::npos);     // maximum coordinate 4 m, not 0.004
    }
    EXPECT_THROW(occt::readStep((dir / "missing.step").string()), occt::SolidError);
    std::filesystem::remove_all(dir);
}

TEST(OcctSolid, ProjectSolidObjectsPersistAndUndo) {
    const auto dir = tempDir();
    auto pp = Project::createNew("Solids", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid layer = ed.addLayer("Solids", 0x808080);
    const Uuid a = occt::addSolidObject(p, occt::makeBox({0, 0, 0}, {10, 10, 10}), "A", layer);
    const Uuid b = occt::addSolidObject(p, occt::makeBox({5, 0, 0}, {15, 10, 10}), "B", layer);
    const std::size_t n = p.model().geometry.size();
    const Uuid u = occt::booleanObjects(p, a, b, occt::BooleanOp::Union, true, "AB");
    EXPECT_EQ(p.model().geometry.size(), n - 1);
    EXPECT_NEAR(occt::analyse(occt::loadSolid(p, *p.model().geometry.find(u))).volume, 1500, 1e-6);
    p.commands().undo(); // inputs back, union gone
    EXPECT_EQ(p.model().geometry.size(), n);
    EXPECT_FALSE(p.model().geometry.find(u));
    p.commands().redo();
    ASSERT_TRUE(p.model().geometry.find(u));

    // Development excavation solid equals the core metrics (area × slope length) for a straight decline.
    GeometryObject cl = cad::makeLine({0, 0, 0}, {100, 0, -12.5});
    cl.layerId = layer;
    const Uuid clId = ed.addGeometry(cl);
    mine_design::NewDevelopment nd;
    nd.headingId = "DEC01";
    nd.profileId = p.model().profiles.all().front().id;
    const Uuid dev = mine_design::createDevelopment(p, clId, nd);
    const Uuid sol = occt::developmentSolid(p, dev, layer);
    const auto metrics = mine_design::computeMetrics(p, *p.model().developments.find(dev));
    EXPECT_NEAR(occt::analyse(occt::loadSolid(p, *p.model().geometry.find(sol))).volume, metrics.volume, 1e-6 * metrics.volume);
    const auto sections = occt::sectionObject(p, sol, {50, 0, 0}, {1, 0, 0}, layer);
    ASSERT_EQ(sections.size(), 1u);
    EXPECT_TRUE(p.model().geometry.find(sections[0])->closed);

    // Save, reload: the B-rep asset travels with the project.
    const auto file = dir / "solids.openmine";
    persistence::saveProject(p, file);
    auto q = persistence::loadProject(file);
    const auto* uq = q->model().geometry.find(u);
    ASSERT_TRUE(uq);
    EXPECT_NEAR(occt::analyse(occt::loadSolid(*q, *uq)).volume, 1500, 1e-6);
    EXPECT_TRUE(persistence::checkProjectFile(file, true).ok);

    // STEP export / import of project solids.
    occt::exportSolids(*q, {u, sol}, dir / "out.step");
    const auto imported = occt::importSolids(*q, dir / "out.step", layer);
    EXPECT_EQ(imported.size(), 2u);
    std::filesystem::remove_all(dir);
}

TEST(OcctSolid, TransformSolidObjectInPlaceAndAsCopy) {
    auto pp = Project::createNew("Transform", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    const Uuid layer = ProjectEditor(p).addLayer("Solids", 0x808080);
    const Vec3 c{51234.5, 7012345.0, 1180.0}; // mine coordinates
    const Uuid id = occt::addSolidObject(p, occt::makeBox(c, c + Vec3{10, 4, 5}), "Block", layer);
    const auto before = *p.model().geometry.find(id);
    const auto info0 = occt::analyse(occt::loadSolid(p, before));

    // In place: rotation about a vertical axis through the corner — same object, new immutable B-rep.
    const Transform3 rot = Transform3::rotation({0, 0, 1}, std::numbers::pi / 2, c);
    EXPECT_EQ(occt::transformSolidObject(p, id, rot, false), id);
    const auto& after = *p.model().geometry.find(id);
    EXPECT_NE(after.brepAsset.id, before.brepAsset.id);
    const auto info = occt::analyse(occt::loadSolid(p, after));
    EXPECT_TRUE(info.valid);
    EXPECT_NEAR(info.volume, info0.volume, 1e-6 * info0.volume);
    EXPECT_NEAR(distance(info.centreOfMass, rot.apply(info0.centreOfMass)), 0.0, 1e-6);
    EXPECT_NEAR(info.bounds.min.x, c.x - 4, 1e-6); // the 4 m side now runs west
    EXPECT_NEAR(info.bounds.max.y, c.y + 10, 1e-6);
    // The display mesh was re-tessellated from the transformed B-rep.
    ASSERT_TRUE(after.mesh);
    for (const auto& v : after.mesh->vertices) EXPECT_LE(v.x, c.x + 1e-6);
    EXPECT_EQ(p.commands().undoText(), "Transform solid");

    // Undo restores the original B-rep and mesh; redo the transform again.
    p.commands().undo();
    EXPECT_EQ(p.model().geometry.find(id)->brepAsset.id, before.brepAsset.id);
    EXPECT_NEAR(occt::analyse(occt::loadSolid(p, *p.model().geometry.find(id))).bounds.max.x, c.x + 10, 1e-6);
    p.commands().redo();
    EXPECT_NEAR(occt::analyse(occt::loadSolid(p, *p.model().geometry.find(id))).bounds.min.x, c.x - 4, 1e-6);

    // As a copy: the original is untouched; scaling multiplies the volume by the cube of the factor.
    const auto n = p.model().geometry.size();
    const Uuid copy = occt::transformSolidObject(p, id, Transform3::scaling(2.0, c), true);
    EXPECT_NE(copy, id);
    EXPECT_EQ(p.model().geometry.size(), n + 1);
    EXPECT_EQ(p.model().geometry.find(copy)->name, "Block (copy)");
    EXPECT_NEAR(occt::analyse(occt::loadSolid(p, *p.model().geometry.find(copy))).volume, 8 * info0.volume, 1e-6 * info0.volume);
    EXPECT_NEAR(occt::analyse(occt::loadSolid(p, *p.model().geometry.find(id))).volume, info0.volume, 1e-6 * info0.volume);

    // A mirror (negative determinant) still yields a valid, closed solid of the same volume.
    const Uuid mirrored = occt::transformSolidObject(p, id, Transform3::mirror(c, {1, 0, 0}), true);
    const auto mi = occt::analyse(occt::loadSolid(p, *p.model().geometry.find(mirrored)));
    EXPECT_TRUE(mi.valid);
    EXPECT_TRUE(mi.closed);
    EXPECT_NEAR(mi.volume, info0.volume, 1e-6 * info0.volume);
    EXPECT_NEAR(mi.bounds.max.x, c.x + 4, 1e-6);

    // Only solid objects can be transformed this way.
    GeometryObject line = cad::makeLine(c, c + Vec3{1, 0, 0});
    line.layerId = layer;
    const Uuid lineId = ProjectEditor(p).addGeometry(line);
    EXPECT_ANY_THROW(occt::transformSolidObject(p, lineId, rot, false));
}

TEST(OcctSolid, MitredSweepOfASymmetricProfileIsAreaTimesCentrelineLength) {
    // A plan corner with a profile symmetric about the centreline: the mitre removes on the
    // inside exactly what it adds on the outside, so V = A × L (also with a grade).
    const auto rect = geometry::rectangleOutline(5.0, 5.0);
    for (const double drop : {0.0, 1.5}) {
        const auto corner = cad::makePolyline({{0, 0, 0}, {30, 0, -drop}, {30, 30, -2 * drop}});
        const double L = geometry::curveLength(corner);
        const auto info = occt::analyse(occt::sweepProfile(rect, corner, occt::SweepTransition::RightCorner));
        EXPECT_TRUE(info.valid);
        EXPECT_NEAR(info.volume, 25.0 * L, 1e-3 * 25.0 * L) << "drop " << drop;
        const auto mesh = geometry::sweepProfile(corner.vertices, rect);
        EXPECT_NEAR(std::abs(geometry::meshSignedVolume(mesh)), 25.0 * L, 1e-3 * 25.0 * L) << "mesh envelope, drop " << drop;
    }
    // An arched profile (exact arcs) around three graded corners of a decline: still valid and A × L.
    TunnelProfile tp;
    tp.shape = ProfileShape::ShoulderArch;
    tp.width = 5.5;
    tp.height = 5.5;
    tp.shoulderRadius = 1.2;
    const auto pc = geometry::profileCurve(tp);
    const auto decline = cad::makePolyline({{0, 0, 0}, {60, 0, -4}, {60, 40, -6.7}, {0, 40, -10.7}, {0, 80, -13.4}});
    const auto info = occt::analyse(occt::sweepProfile(pc.points, pc.bulges, decline, occt::SweepTransition::RightCorner));
    EXPECT_TRUE(info.valid);
    EXPECT_TRUE(info.closed);
    EXPECT_EQ(info.solids, 1);
    EXPECT_NEAR(info.volume, geometry::profileArea(tp) * geometry::curveLength(decline), 2e-3 * info.volume);
}
