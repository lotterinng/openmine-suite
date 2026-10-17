// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Edit.h"
#include "openmine/crs/Crs.h"
#include "openmine/geometry/LocalGrid.h"
#include "openmine/project/ProjectEditor.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <random>

using namespace openmine;
using namespace openmine::crs;

namespace {
constexpr double kPi = std::numbers::pi;

/// Independent transverse Mercator (Snyder, USGS PP 1395, eqs. 8-9 … 8-12), k0 = 1, on GRS80.
Vec2 snyderTm(double latDeg, double lonDeg, double lon0Deg) {
    const double a = 6378137.0, f = 1 / 298.257222101, e2 = f * (2 - f), ep2 = e2 / (1 - e2);
    const double phi = latDeg * kPi / 180, lam = (lonDeg - lon0Deg) * kPi / 180;
    const double N = a / std::sqrt(1 - e2 * std::sin(phi) * std::sin(phi));
    const double T = std::tan(phi) * std::tan(phi), C = ep2 * std::cos(phi) * std::cos(phi), A = lam * std::cos(phi);
    const double e4 = e2 * e2, e6 = e4 * e2;
    const double M = a * ((1 - e2 / 4 - 3 * e4 / 64 - 5 * e6 / 256) * phi - (3 * e2 / 8 + 3 * e4 / 32 + 45 * e6 / 1024) * std::sin(2 * phi) +
                          (15 * e4 / 256 + 45 * e6 / 1024) * std::sin(4 * phi) - (35 * e6 / 3072) * std::sin(6 * phi));
    const double x = N * (A + (1 - T + C) * std::pow(A, 3) / 6 + (5 - 18 * T + T * T + 72 * C - 58 * ep2) * std::pow(A, 5) / 120);
    const double y = M + N * std::tan(phi) * (A * A / 2 + (5 - T + 9 * C + 4 * C * C) * std::pow(A, 4) / 24 + (61 - 58 * T + T * T + 600 * C - 330 * ep2) * std::pow(A, 6) / 720);
    return {x, y}; // easting, northing (false origin 0)
}
} // namespace

TEST(Crs, DescribesSouthAfricanLoSystemsWithTheirAxes) {
    const auto cape = describe("EPSG:22285");
    EXPECT_EQ(cape.name, "Cape / Lo25");
    EXPECT_EQ(cape.authorityCode, "EPSG:22285");
    EXPECT_EQ(cape.kind, CrsKind::Projected);
    EXPECT_EQ(cape.horizontalDatum, "Cape");
    ASSERT_EQ(cape.axes.size(), 2u);
    EXPECT_EQ(cape.axes[0].direction, "west");
    EXPECT_EQ(cape.axes[1].direction, "south");
    EXPECT_EQ(cape.linearUnit, "metre");
    ASSERT_TRUE(cape.areaOfUse);
    const auto h = describe("2051");
    EXPECT_EQ(h.name, "Hartebeesthoek94 / Lo25");
    EXPECT_NE(h.horizontalDatum.find("Hartebeesthoek94"), std::string::npos);
    EXPECT_THROW(describe("EPSG:999999"), CrsError);
    EXPECT_THROW(describe("EPSG:1165"), CrsError); // a transformation, not a CRS
    const auto found = search("Lo25");
    EXPECT_GE(found.size(), 2u);
    EXPECT_FALSE(projVersion().empty());
}

TEST(Crs, TransformsAgreeWithIndependentFormulaAndRoundTrip) {
    // WGS 84 → Hartebeesthoek94 / Lo25 (null datum shift): independent TM gives westing = −E, southing = −N.
    Transformer t("EPSG:4326", "EPSG:2051");
    EXPECT_FALSE(t.operation().ballpark);
    for (const auto& [lat, lon] : std::vector<std::pair<double, double>>{{-28.74, 24.77}, {-26.2, 25.9}, {-33.9, 24.1}}) {
        const Vec3 lo = t.forward({lat, lon, 0}); // EPSG:4326 order: latitude, longitude
        const Vec2 en = snyderTm(lat, lon, 25.0);
        EXPECT_NEAR(lo.x, -en.x, 0.002) << lat << "," << lon;
        EXPECT_NEAR(lo.y, -en.y, 0.002);
        const Vec3 back = t.inverse(lo);
        EXPECT_NEAR(back.x, lat, 1e-10);
        EXPECT_NEAR(back.y, lon, 1e-10);
    }
    // Cape → Hartebeesthoek94: a real datum shift with a stated accuracy.
    const auto ops = candidateOperations("EPSG:22285", "EPSG:2051");
    ASSERT_FALSE(ops.empty());
    EXPECT_GT(ops.front().accuracy, 0);
    Transformer c("EPSG:22285", "EPSG:2051");
    const Vec3 shifted = c.forward({22426.512216, 3180179.334637, 0});
    // Reference: `cs2cs EPSG:22285 EPSG:2051` (PROJ 9.4, same best operation).
    EXPECT_LT(distance(shifted, {22470.015656, 3180474.882942, 0}), 1e-5);
    EXPECT_GT(distance(shifted, {22426.512216, 3180179.334637, 0}), 250.0); // the Cape datum shift is ~300 m here
    EXPECT_THROW(Transformer("EPSG:22285", "EPSG:2051", {99, false}), CrsError);
}

TEST(Crs, AreaOfUseCatchesSwappedAxes) {
    EXPECT_TRUE(checkAreaOfUse("EPSG:2051", {{22466.8, 3180474.3, 0}}).empty());
    const auto w = checkAreaOfUse("EPSG:2051", {{3180474.3, 22466.8, 0}});
    ASSERT_EQ(w.size(), 1u);
    EXPECT_NE(w[0].find("axis order"), std::string::npos);
}

TEST(LocalGrid, HelmertAffineAnd3DFitsRecoverParameters) {
    const double th = 32.5 * kPi / 180, s = 0.99985;
    auto truth = [&](Vec3 l) {
        return Vec3{s * (std::cos(th) * l.x - std::sin(th) * l.y) + 22000.0, s * (std::sin(th) * l.x + std::cos(th) * l.y) + 3180000.0, l.z - 1450.0};
    };
    std::vector<geometry::GridControlPair> pairs;
    const std::vector<Vec3> local = {{1000, 2000, 1500}, {3500, 2100, 1480}, {3400, 5200, 1510}, {900, 5000, 1520}, {2000, 3000, 1495}};
    for (std::size_t i = 0; i < local.size(); ++i) pairs.push_back({"P" + std::to_string(i), local[i], truth(local[i]), i < 4, {}});
    const auto h = geometry::fitLocalGrid(pairs, LocalGridMethod::Helmert2D);
    EXPECT_NEAR(h.parameters[3], s, 1e-12);
    EXPECT_NEAR(h.parameters[4], th, 1e-12);
    EXPECT_NEAR(h.parameters[2], -1450.0, 1e-9);
    EXPECT_LT(h.rms, 1e-8);
    EXPECT_EQ(h.fitCount, 4);
    EXPECT_EQ(h.checkCount, 1);
    EXPECT_LT(h.checkRms, 1e-8);
    // A mirrored grid (x north, y east) needs the affine method; Helmert leaves large residuals.
    std::vector<geometry::GridControlPair> mirrored;
    for (const auto& pr : pairs) mirrored.push_back({pr.name, {pr.local.y, pr.local.x, pr.local.z}, pr.base, pr.use, {}});
    EXPECT_GT(geometry::fitLocalGrid(mirrored, LocalGridMethod::Helmert2D).rms, 100);
    const auto aff = geometry::fitLocalGrid(mirrored, LocalGridMethod::Affine2D);
    EXPECT_LT(aff.rms, 1e-8);
    EXPECT_LT(aff.localToBase.determinant3(), 0);
    // 3D Helmert with a small tilt.
    const Transform3 T3 = Transform3::translation({22000, 3180000, -1450}) * Transform3::rotation(Vec3{0.01, 0.02, 1}.normalized(), 0.3) * Transform3::scaling(1.0001);
    std::vector<geometry::GridControlPair> p3;
    for (const auto& l : local) p3.push_back({"", l, T3.apply(l), true, {}});
    const auto h3 = geometry::fitLocalGrid(p3, LocalGridMethod::Helmert3D);
    EXPECT_LT(h3.rms, 1e-7);
    EXPECT_NEAR(h3.parameters[3], 1.0001, 1e-12);
    // JSON round trip of pairs with residuals.
    const auto back = geometry::pairsFromJson(geometry::pairsJson(h.pairs));
    ASSERT_EQ(back.size(), 5u);
    EXPECT_FALSE(back[4].use);
    EXPECT_THROW(geometry::fitLocalGrid({pairs[0]}, LocalGridMethod::Helmert2D), std::invalid_argument);
    EXPECT_THROW(geometry::fitLocalGrid({pairs[0], pairs[1], {"c", {6000, 2200, 0}, {0, 0, 0}, true, {}}}, LocalGridMethod::Affine2D), std::invalid_argument);
}

TEST(Crs, ProjectLocalGridAndExplicitObjectTransformation) {
    auto pp = Project::createNew("CRS", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid hart = declareCrs(p, "EPSG:2051");
    EXPECT_EQ(declareCrs(p, "EPSG:2051"), hart); // not duplicated
    const Uuid cape = declareCrs(p, "EPSG:22285");
    EXPECT_THROW(declareCrs(p, "EPSG:4326"), CrsError);
    EXPECT_THROW(declareCrs(p, "EPSG:2227"), CrsError); // US survey feet
    // Local mine grid on Hartebeesthoek94 / Lo25.
    std::vector<geometry::GridControlPair> pairs;
    const Transform3 l2b = Transform3::translation({22000, 3180000, -1450}) * Transform3::rotation({0, 0, 1}, 0.5);
    for (const auto& l : std::vector<Vec3>{{0, 0, 1500}, {500, 0, 1500}, {500, 500, 1500}, {0, 500, 1500}}) pairs.push_back({"", l, l2b.apply(l), true, {}});
    const auto fit = geometry::fitLocalGrid(pairs, LocalGridMethod::Helmert2D);
    const Uuid mine = defineLocalGrid(p, "Mine grid", hart, fit);
    setProjectCrs(p, mine);
    EXPECT_EQ(p.model().info.projectCrsId, mine);
    // A centreline in mine grid, transformed explicitly into Cape / Lo25 copies.
    const Uuid layer = ed.addLayer("Cape", 0xFF0000);
    GeometryObject g = cad::makePolyline({{100, 100, 1480}, {300, 120, 1470}});
    g.layerId = layer;
    g.name = "decline";
    const Uuid gid = ed.addGeometry(g);
    const auto out = transformObjects(p, {gid}, mine, cape, layer);
    ASSERT_EQ(out.size(), 1u);
    const auto* copy = p.model().geometry.find(out[0]);
    Transformer t("EPSG:2051", "EPSG:22285");
    const Vec3 expected = t.forward(l2b.apply({100, 100, 1480}));
    EXPECT_LT(distance(copy->vertices[0], expected), 1e-6);
    EXPECT_EQ(p.model().geometry.find(gid)->vertices[0], (Vec3{100, 100, 1480})); // original untouched
    ASSERT_EQ(p.model().transformations.size(), 1u);
    const auto& rec = p.model().transformations.all().front();
    EXPECT_EQ(rec.targetCrs, "EPSG:22285");
    EXPECT_GT(rec.accuracyMetres, 1.0); // Cape ↔ Hartebeesthoek94 is a metre-level transformation
    EXPECT_NE(rec.pipeline.find("Mine grid"), std::string::npos);
    p.commands().undo();
    EXPECT_EQ(p.model().transformations.size(), 0u);
}

TEST(Units, ParsingAndConversion) {
    EXPECT_DOUBLE_EQ(parseLength("12.5"), 12.5);
    EXPECT_DOUBLE_EQ(parseLength("1200 mm"), 1.2);
    EXPECT_DOUBLE_EQ(parseLength("10ft"), 3.048);
    EXPECT_DOUBLE_EQ(parseLength("10'"), 3.048);
    EXPECT_NEAR(parseLength("1 usft"), 1200.0 / 3937.0, 1e-15);
    EXPECT_DOUBLE_EQ(fromMetres(1.0, LengthUnit::Centimetre), 100.0);
    EXPECT_THROW(parseLength("12 furlongs"), CrsError);
    EXPECT_THROW(parseLength("abc"), CrsError);
}
