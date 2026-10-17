// SPDX-License-Identifier: Apache-2.0
#include "openmine/geology/BlockModel.h"
#include "openmine/geology/Compositing.h"
#include "openmine/geology/Desurvey.h"
#include "openmine/geology/Drillholes.h"
#include "openmine/geology/Estimation.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/util/Csv.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace openmine;
using namespace openmine::geology;

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

MeshData box(Vec3 lo, Vec3 hi) {
    MeshData m;
    for (int i = 0; i < 8; ++i) m.vertices.push_back({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
    const std::uint32_t f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                    {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (auto& t : f) m.triangles.push_back({t[0], t[1], t[2]});
    return m;
}

MeshData sphere(Vec3 c, double r, int n = 64) {
    MeshData m;
    for (int i = 0; i <= n; ++i) {
        const double th = kPi * i / n;
        for (int j = 0; j < 2 * n; ++j) {
            const double ph = kPi * j / n;
            m.vertices.push_back(c + Vec3{r * std::sin(th) * std::cos(ph), r * std::sin(th) * std::sin(ph), r * std::cos(th)});
        }
    }
    auto id = [&](int i, int j) { return static_cast<std::uint32_t>(i * 2 * n + (j % (2 * n))); };
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < 2 * n; ++j) {
            if (i > 0) m.triangles.push_back({id(i, j), id(i + 1, j), id(i, j + 1)});
            if (i < n - 1) m.triangles.push_back({id(i, j + 1), id(i + 1, j), id(i + 1, j + 1)});
        }
    return m;
}

Drillhole simpleHole() {
    Drillhole h;
    h.holeId = "DH1";
    h.collar = {1000, 2000, 100};
    h.length = 7;
    h.surveys = {{0, 0, -90}};
    DrillholeInterval a{0, 1, {{"au", 1.0}}, {{"lith", "BAS"}}};
    DrillholeInterval b{1, 3, {{"au", 3.0}}, {{"lith", "BAS"}}};
    DrillholeInterval c{3, 4, {}, {{"lith", "QV"}}};
    DrillholeInterval d{4, 7, {{"au", 2.0}}, {{"lith", "QV"}}};
    h.intervals = {a, b, c, d};
    return h;
}
} // namespace

// ---- desurvey -------------------------------------------------------------------------------------
TEST(Desurvey, MinimumCurvatureIsExactOnConstantBuildArc) {
    // Dip changes linearly from −60° to −30° over 100 m in azimuth 090 (constant build rate): the
    // true trace is a circular arc, which minimum curvature reproduces exactly at any depth.
    Drillhole h;
    h.holeId = "ARC";
    h.collar = {0, 0, 0};
    h.length = 100;
    for (int s = 0; s <= 100; s += 25) h.surveys.push_back({double(s), 90, -60 + 0.3 * s});
    const DesurveyedHole d(h);
    const double k = 0.3 * kDeg, d0 = -60 * kDeg;
    for (double s : {12.3, 35.7, 50.0, 99.0, 100.0}) {
        const double dd = d0 + k * s;
        const Vec3 expected{(std::sin(dd) - std::sin(d0)) / k, 0, -(std::cos(dd) - std::cos(d0)) / k};
        const Vec3 p = d.at(s);
        EXPECT_NEAR(p.x, expected.x, 1e-9) << s;
        EXPECT_NEAR(p.y, 0, 1e-9);
        EXPECT_NEAR(p.z, expected.z, 1e-9) << s;
        const auto [az, dip] = azimuthDip(d.directionAt(s));
        EXPECT_NEAR(az, 90, 1e-9);
        EXPECT_NEAR(dip, -60 + 0.3 * s, 1e-9);
    }
    // Tangential and balanced tangential are approximations with larger errors.
    const double dd = d0 + k * 100;
    const Vec3 exact{(std::sin(dd) - std::sin(d0)) / k, 0, -(std::cos(dd) - std::cos(d0)) / k};
    const double eT = distance(DesurveyedHole(h, DesurveyMethod::Tangent).at(100), exact);
    const double eB = distance(DesurveyedHole(h, DesurveyMethod::BalancedTangent).at(100), exact);
    EXPECT_GT(eT, 1.0);
    EXPECT_GT(eB, 0.01);
    EXPECT_LT(eB, eT);
    // Dogleg severity: 7.5° per 25 m = 9°/30 m.
    for (double dls : doglegSeverity(d)) EXPECT_NEAR(dls, 9.0, 1e-9);
}

TEST(Desurvey, VerticalDefaultsExtrapolationAndCrossings) {
    Drillhole h;
    h.collar = {10, 20, 300};
    h.length = 80;
    const DesurveyedHole v(h);
    EXPECT_NEAR(v.at(50).z, 250, 1e-12);
    EXPECT_NEAR(v.at(500).z, 220, 1e-12); // clamped to the hole length
    // Straight extrapolation below the last survey and above the first.
    h.surveys = {{20, 45, -45}, {40, 45, -45}};
    const DesurveyedHole d(h);
    const Vec3 dir = directionVector(45, -45);
    EXPECT_NEAR(distance(d.at(80), h.collar + dir * 80), 0, 1e-9);
    const auto z = d.depthsAtElevation(300 - 30 * std::sqrt(0.5));
    ASSERT_EQ(z.size(), 1u);
    EXPECT_NEAR(z[0], 30, 1e-8);
    EXPECT_EQ(d.trace(10).size(), 9u); // 0..80 every 10 m (stations coincide)
    const auto [az, dip] = azimuthDip(directionVector(271.5, -12));
    EXPECT_NEAR(az, 271.5, 1e-9);
    EXPECT_NEAR(dip, -12, 1e-9);
}

// ---- import --------------------------------------------------------------------------------------
TEST(Drillholes, CsvImportReportsEveryRejectedRecord) {
    DrillholeTables t;
    t.collars = util::parseCsv("HOLEID,EAST,NORTH,RL,DEPTH,DATE\nA1,100,200,50,30,2026-03-04\nA2,110,200,50,,\nA3,x,1,1,10,\nA1,1,1,1,1,\n");
    t.surveys = util::parseCsv("BHID;DEPTH;AZI;DIP\nA1;0;90;-60\nA1;25;95;-58\nA1;40;95;-58\nZZ;0;0;-90\nA2;0;400;-90\n");
    t.intervals.push_back({"assay", util::parseCsv("HOLEID,FROM,TO,Au_ppm,SAMPLE\nA1,0,2,0.5,S1\nA1,2,4,<0.02,S2\nA1,4,6,-,S3\nA1,5,7,1.0,S4\n"
                                                   "A1,28,31,0.3,S5\nA2,0,3,\">10\",S6\n")});
    t.intervals.push_back({"lith", util::parseCsv("HOLEID\tFROM\tTO\tLITH\nA1\t0\t3\tBAS\nA1\t3\t6\tQV\nA2\t0\t5\tBAS\n")});
    DrillholeImportOptions o;
    o.codeColumns = {"SAMPLE"};
    const auto r = readDrillholes(t, o);
    ASSERT_EQ(r.holes.size(), 2u); // A3 rejected (bad collar), duplicate A1 rejected
    EXPECT_EQ(r.rejectedHoles, 1u);
    EXPECT_EQ(r.belowDetectionCount, 1u);
    EXPECT_EQ(r.aboveDetectionCount, 1u);
    EXPECT_EQ(std::count(r.numericFields.begin(), r.numericFields.end(), "Au_ppm"), 1);
    EXPECT_EQ(std::count(r.codeFields.begin(), r.codeFields.end(), "LITH"), 1);
    auto has = [&](const std::string& text) {
        return std::any_of(r.issues.begin(), r.issues.end(), [&](const auto& i) { return i.message.find(text) != std::string::npos; });
    };
    EXPECT_TRUE(has("Duplicate collar"));
    EXPECT_TRUE(has("not numeric"));
    EXPECT_TRUE(has("overlaps"));
    EXPECT_TRUE(has("beyond the hole length"));  // A1 28–31 m (length 30) and survey at 40 m
    EXPECT_TRUE(has("No collar for this hole")); // ZZ
    EXPECT_TRUE(has("normalised to 40"));        // azimuth 400
    const Drillhole* a1 = nullptr;
    for (const auto& h : r.holes)
        if (h.holeId == "A1") a1 = &h;
    ASSERT_TRUE(a1);
    EXPECT_EQ(a1->drilledDate->toString(), "2026-03-04");
    EXPECT_EQ(a1->surveys.size(), 2u);
    // Merged at the union of boundaries 0,2,3,4,6: values and codes on every piece.
    ASSERT_EQ(a1->intervals.size(), 4u);
    EXPECT_DOUBLE_EQ(a1->intervals[0].to, 2);
    EXPECT_DOUBLE_EQ(a1->intervals[1].values.at("Au_ppm"), 0.01); // half the detection limit
    EXPECT_EQ(a1->intervals[1].codes.at("LITH"), "BAS");
    EXPECT_EQ(a1->intervals[2].codes.at("LITH"), "QV");
    EXPECT_FALSE(a1->intervals[3].values.count("Au_ppm")); // "-" is missing, not zero
    EXPECT_EQ(a1->intervals[3].codes.at("SAMPLE"), "S3");
    // Length of A2 derived from its deepest record.
    for (const auto& h : r.holes)
        if (h.holeId == "A2") EXPECT_DOUBLE_EQ(h.length, 5);

    auto p = Project::createNew("DH", Date::fromYmd(2026, 11, 2));
    const auto s = importDrillholes(*p, r);
    EXPECT_EQ(s.created.size(), 2u);
    EXPECT_EQ(p->model().drillholes.size(), 2u);
    const auto again = importDrillholes(*p, r);
    EXPECT_EQ(again.skipped.size(), 2u);
    p->commands().undo();
    p->commands().undo();
    EXPECT_EQ(p->model().drillholes.size(), 0u);

    const auto csv = exportDrillholesCsv(r.holes);
    const auto back = util::parseCsv(csv.intervals);
    EXPECT_TRUE(back.column("ZM").has_value());
    EXPECT_EQ(back.rows.size(), 4u + r.holes[1].intervals.size());
}

TEST(Drillholes, QualityChecks) {
    auto h = simpleHole();
    h.intervals.erase(h.intervals.begin() + 2);
    h.surveys = {{0, 0, -90}, {10, 0, -60}};
    h.length = 20;
    const auto issues = checkDrillhole(h, 6.0);
    auto has = [&](const std::string& t) { return std::any_of(issues.begin(), issues.end(), [&](const auto& i) { return i.message.find(t) != std::string::npos; }); };
    EXPECT_TRUE(has("Gap 3–4 m"));
    EXPECT_TRUE(has("below 7 m"));
    EXPECT_TRUE(has("Dogleg severity 90°/30 m"));
}

// ---- compositing ----------------------------------------------------------------------------------
TEST(Compositing, LengthWeightedWithCoverageAndResiduals) {
    const auto h = simpleHole();
    CompositeOptions o;
    o.length = 2;
    o.fields = {"au"};
    o.residual = ResidualMode::Discard;
    auto c = compositeByLength(h, o); // [0,2] [2,4] [4,6] + residual [6,7] (1 m ≥ 0.5 × 2 m → kept)
    ASSERT_EQ(c.size(), 4u);
    EXPECT_DOUBLE_EQ(c[0].values.at("au"), 2.0);
    EXPECT_DOUBLE_EQ(c[1].values.at("au"), 3.0); // half sampled: coverage 0.5 ≥ minimum
    EXPECT_DOUBLE_EQ(c[1].coverage.at("au"), 0.5);
    EXPECT_EQ(c[1].codes.at("lith"), "BAS");     // 1 m BAS, 1 m QV: first maximum
    EXPECT_DOUBLE_EQ(c[3].length(), 1.0);
    EXPECT_NEAR(c[0].mid.z, 99, 1e-12);
    o.minCoverage = 0.6;
    EXPECT_FALSE(compositeByLength(h, o)[1].values.count("au"));
    o.unsampledAsZero = true;
    EXPECT_DOUBLE_EQ(compositeByLength(h, o)[1].values.at("au"), 1.5);
    o = {};
    o.length = 2;
    o.residual = ResidualMode::Discard;
    o.minResidualFraction = 0.6;
    EXPECT_EQ(compositeByLength(h, o).size(), 3u);
    o.residual = ResidualMode::AddToPrevious;
    c = compositeByLength(h, o);
    ASSERT_EQ(c.size(), 3u);
    EXPECT_DOUBLE_EQ(c[2].to, 7);
    EXPECT_DOUBLE_EQ(c[2].values.at("au"), 2.0);
    o.residual = ResidualMode::Distribute; // 7 m → 3 or 4 composites: round(3.5) = 4 of 1.75 m
    c = compositeByLength(h, o);
    ASSERT_EQ(c.size(), 4u);
    EXPECT_DOUBLE_EQ(c[0].length(), 1.75);
    EXPECT_NEAR(c[0].values.at("au"), (1 * 1 + 3 * 0.75) / 1.75, 1e-12);
    // Mass balance: Σ composite length × grade = Σ interval length × grade (full coverage).
    double m1 = 0, m2 = 0;
    o.minCoverage = 0;
    for (const auto& x : compositeByLength(h, o))
        if (x.values.count("au")) m1 += x.values.at("au") * x.length() * x.coverage.at("au");
    for (const auto& i : h.intervals)
        if (i.values.count("au")) m2 += i.values.at("au") * (i.to - i.from);
    EXPECT_NEAR(m1, m2, 1e-12);
}

TEST(Compositing, DomainsBenchesDensityAndSolids) {
    auto h = simpleHole();
    CompositeOptions o;
    const auto byCode = compositeByCode(h, "lith", o);
    ASSERT_EQ(byCode.size(), 2u);
    EXPECT_NEAR(byCode[0].values.at("au"), (1 + 6) / 3.0, 1e-12);
    EXPECT_NEAR(byCode[1].values.at("au"), 2.0, 1e-12);
    EXPECT_NEAR(byCode[1].coverage.at("au"), 0.75, 1e-12);
    o.breakField = "lith";
    o.onlyCode = "QV";
    o.length = 1;
    EXPECT_EQ(compositeByLength(h, o).size(), 4u);
    // Density weighting: Σ g ρ l / Σ ρ l.
    h.intervals[0].values["sg"] = 3.0;
    h.intervals[1].values["sg"] = 2.0;
    CompositeOptions dw;
    dw.densityField = "sg";
    dw.fields = {"au"};
    const auto c = compositeIntervals(h, {{0, 3}}, dw);
    EXPECT_NEAR(c[0].values.at("au"), (1 * 3 * 1 + 3 * 2 * 2) / (3.0 + 4.0), 1e-12);
    EXPECT_NEAR(c[0].values.at("sg"), (3 + 4) / 3.0, 1e-12);
    // Bench composites: collar RL 100, benches every 2.5 m from RL 0 → breaks at depths 2.5 and 5.
    const auto b = compositeByBench(simpleHole(), 0, 2.5, {});
    ASSERT_EQ(b.size(), 3u);
    EXPECT_NEAR(b[0].to, 2.5, 1e-9);
    EXPECT_NEAR(b[1].to, 5.0, 1e-9);
    EXPECT_NEAR(b[2].to, 7.0, 1e-9);
    // Solid coding: a box from RL 94.5 to 97.5 (depths 2.5–5.5) splits intervals at its faces.
    const auto iv = codeIntervalsBySolid(simpleHole(), box({990, 1990, 94.5}, {1010, 2010, 97.5}), "dom", "ORE", "WASTE");
    std::vector<std::pair<double, std::string>> seq;
    for (const auto& x : iv) seq.emplace_back(x.to, x.codes.at("dom"));
    ASSERT_EQ(seq.size(), 6u);
    EXPECT_NEAR(seq[1].first, 2.5, 1e-6);
    EXPECT_EQ(seq[1].second, "WASTE");
    EXPECT_EQ(seq[2].second, "ORE");
    EXPECT_NEAR(seq[4].first, 5.5, 1e-6);
    EXPECT_EQ(seq[4].second, "ORE");
    EXPECT_EQ(seq[5].second, "WASTE");
}

TEST(Compositing, StatisticsQuantilesAndCapping) {
    const auto s = statistics({1, 2, 3, 4}, {1, 1, 1, 1});
    EXPECT_DOUBLE_EQ(s.mean, 2.5);
    EXPECT_DOUBLE_EQ(s.variance, 1.25);
    EXPECT_DOUBLE_EQ(s.median, 2.5);
    EXPECT_DOUBLE_EQ(s.min, 1);
    EXPECT_DOUBLE_EQ(s.max, 4);
    const auto w = statistics({1, 10}, {3, 1});
    EXPECT_DOUBLE_EQ(w.mean, 13.0 / 4);
    const auto hst = histogram({1, 10, 100, 1000}, {}, 3, true);
    ASSERT_EQ(hst.weights.size(), 3u);
    EXPECT_DOUBLE_EQ(hst.weights[0] + hst.weights[1] + hst.weights[2], 4);
    auto c = compositeByLength(simpleHole(), {});
    const double before = percentileCap(c, "au", 100);
    EXPECT_DOUBLE_EQ(before, 3.0);
    const auto r = capComposites(c, "au", 2.5);
    EXPECT_EQ(r.capped, 1u);
    EXPECT_GT(r.metalRemovedFraction, 0);
    EXPECT_LT(r.meanAfter, r.meanBefore);
    const auto csv = util::parseCsv(compositesCsv(c));
    EXPECT_TRUE(csv.column("au_COVERAGE").has_value());
}

// ---- block model ---------------------------------------------------------------------------------
TEST(BlockModels, StorageGeometryUndoAndCsv) {
    auto pp = Project::createNew("BM", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    BlockModel h;
    h.name = "Res";
    h.origin = {1000, 2000, 0};
    h.blockSize = {10, 5, 2};
    h.nx = 4;
    h.ny = 3;
    h.nz = 2;
    h.rotationDeg = 30;
    const Uuid id = createBlockModel(p, h);
    auto d = loadBlockModel(p, id);
    EXPECT_TRUE(d.values.empty());
    const BlockGrid g(d.header);
    // Rotated geometry: centroid ↔ cell round trip, local frame orthonormal.
    for (std::size_t i = 0; i < g.count(); ++i) {
        const auto c = g.cellOf(g.centroid(i));
        ASSERT_TRUE(c);
        EXPECT_EQ(g.index((*c)[0], (*c)[1], (*c)[2]), i);
    }
    const Vec3 c0 = g.centroid(0, 0, 0);
    EXPECT_NEAR(c0.x, 1000 + 5 * std::cos(30 * kDeg) - 2.5 * std::sin(30 * kDeg), 1e-9);
    EXPECT_NEAR(c0.y, 2000 + 5 * std::sin(30 * kDeg) + 2.5 * std::cos(30 * kDeg), 1e-9);
    EXPECT_FALSE(g.cellOf({999, 1999, 1}));
    auto& cu = d.attribute("cu");
    for (std::size_t i = 0; i < cu.size(); ++i) cu[i] = 0.1 * i;
    d.attribute("sg").assign(d.count(), 2.5);
    saveBlockModel(p, d);
    const auto back = loadBlockModel(p, id);
    EXPECT_EQ(back.header.attributes, (std::vector<std::string>{"cu", "sg"}));
    EXPECT_EQ(back.values.at("cu"), cu);
    auto d2 = back;
    d2.attribute("cu")[0] = 99;
    saveBlockModel(p, d2);
    EXPECT_DOUBLE_EQ(loadBlockModel(p, id).values.at("cu")[0], 99);
    p.commands().undo();
    EXPECT_DOUBLE_EQ(loadBlockModel(p, id).values.at("cu")[0], 0); // previous asset restored
    // CSV round trip through the same grid.
    const auto csv = exportBlockModelCsv(back);
    const auto imp = importBlockModelCsv(csv, back.header);
    EXPECT_EQ(imp.placed, back.count());
    EXPECT_TRUE(imp.issues.empty());
    for (std::size_t i = 0; i < back.count(); ++i) EXPECT_NEAR(imp.data.values.at("cu")[i], back.values.at("cu")[i], 1e-9);
    const auto bad = importBlockModelCsv("X,Y,Z,cu\n1000.5,2000.5,1,1\n", back.header);
    EXPECT_EQ(bad.placed, 0u);
    EXPECT_FALSE(bad.issues.empty());
    // Grid inference (unrotated).
    const auto inf = inferGridFromCsv("XC,YC,ZC,v\n5,2.5,1,1\n25,2.5,1,1\n5,12.5,5,1\n");
    EXPECT_DOUBLE_EQ(inf.blockSize.x, 20);
    EXPECT_EQ(inf.nx, 2);
    EXPECT_EQ(inf.ny, 2);
    EXPECT_DOUBLE_EQ(inf.blockSize.z, 4);
    EXPECT_DOUBLE_EQ(inf.origin.x, -5);
    // Covering grid snaps to whole blocks.
    const auto cov = gridCovering({{3, 4, 5}, {27, 9, 11}}, {5, 5, 5});
    EXPECT_DOUBLE_EQ(cov.origin.x, 0);
    EXPECT_EQ(cov.nx, 6);
    EXPECT_EQ(cov.nz, 2); // z 5–15
}

TEST(BlockModels, DomainProportionsAndGradeTonnage) {
    BlockModelData d;
    d.header.origin = {0, 0, 0};
    d.header.blockSize = {2, 2, 2};
    d.header.nx = d.header.ny = d.header.nz = 10;
    const BlockGrid g(d.header);
    // Axis-aligned box covering blocks exactly and half a block layer.
    const auto f = fractionInside(g, box({4, 4, 4}, {10, 10, 9.5}), 4);
    EXPECT_NEAR(f[g.index(2, 2, 2)], 1.0, 1e-12);
    EXPECT_NEAR(f[g.index(4, 4, 4)], 0.75, 1e-12); // z 8–10, box to 9.5
    EXPECT_NEAR(f[g.index(0, 0, 0)], 0.0, 1e-12);
    // Sphere volume from proportions (r = 7 inside a 20 m cube).
    const auto fs = fractionInside(g, sphere({10, 10, 10}, 7), 8);
    double vol = 0;
    for (double x : fs) vol += x * g.blockVolume();
    EXPECT_NEAR(vol, 4.0 / 3 * kPi * 343, 0.01 * 4.0 / 3 * kPi * 343);
    // Below a plane z = 5 (mesh larger than the model).
    MeshData plane;
    plane.vertices = {{-5, -5, 5}, {25, -5, 5}, {25, 25, 5}, {-5, 25, 5}};
    plane.triangles = {{0, 1, 2}, {0, 2, 3}};
    const auto fb = fractionBelow(g, plane, 4);
    EXPECT_NEAR(fb[g.index(3, 3, 1)], 1.0, 1e-12);
    EXPECT_NEAR(fb[g.index(3, 3, 2)], 0.5, 1e-12);
    EXPECT_NEAR(fb[g.index(3, 3, 3)], 0.0, 1e-12);
    // Grade–tonnage.
    auto& au = d.attribute("au");
    auto& prop = d.attribute("prop");
    for (std::size_t i = 0; i < d.count(); ++i) {
        au[i] = (i % 4) * 1.0; // 0,1,2,3
        prop[i] = 1.0;
    }
    prop[1] = 0.5; // an au=1 block half in the domain
    GradeTonnageOptions o;
    o.gradeField = "au";
    o.proportionField = "prop";
    o.defaultDensity = 2.5;
    o.cutoffs = {0, 1.5, 3};
    const auto rows = gradeTonnage(d, o);
    ASSERT_EQ(rows.size(), 3u);
    const double t = 8 * 2.5;
    EXPECT_NEAR(rows[0].tonnes, (1000 - 0.5) * t, 1e-6);
    EXPECT_NEAR(rows[1].tonnes, 500 * t, 1e-6);
    EXPECT_NEAR(rows[1].grades.at("au"), 2.5, 1e-12);
    EXPECT_NEAR(rows[2].metal.at("au"), 250 * t * 3, 1e-6);
    EXPECT_FALSE(gradeTonnageCsv(rows).empty());
    EXPECT_EQ(codeBlocksInside(d, box({4, 4, 4}, {10, 10, 9.5}), "dom", 1), 3u * 3u * 3u);
}

// ---- variography & estimation --------------------------------------------------------------------
TEST(Estimation, VariogramModelsAndAnisotropy) {
    VariogramModel m;
    m.nugget = 0.2;
    m.structures.push_back({VariogramType::Spherical, 0.8, Anisotropy{100, 50, 10, 90, 0, 0}});
    EXPECT_DOUBLE_EQ(m.gamma({0, 0, 0}), 0);
    EXPECT_NEAR(m.gamma({50, 0, 0}), 0.2 + 0.8 * 0.6875, 1e-12); // half range along the major (east)
    EXPECT_NEAR(m.gamma({0, 25, 0}), 0.2 + 0.8 * 0.6875, 1e-12); // half range along the semi (south/north)
    EXPECT_NEAR(m.gamma({0, 0, 20}), 1.0, 1e-12);
    EXPECT_NEAR(m.covariance({1e6, 0, 0}), 0, 1e-12);
    EXPECT_NEAR(VariogramModel::unitGamma(VariogramType::Exponential, 1), 1 - std::exp(-3), 1e-15);
    // Plunging, tilted ellipsoid: orthonormal axes, major along azimuth 045 plunging 30°.
    const Anisotropy a{100, 60, 20, 45, 30, 15};
    const auto ax = a.axes();
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(ax[static_cast<std::size_t>(i)].length(), 1, 1e-12);
        for (int j = i + 1; j < 3; ++j) EXPECT_NEAR(dot(ax[static_cast<std::size_t>(i)], ax[static_cast<std::size_t>(j)]), 0, 1e-12);
    }
    const auto [az, dip] = azimuthDip(ax[0]);
    EXPECT_NEAR(az, 45, 1e-9);
    EXPECT_NEAR(dip, -30, 1e-9);
    EXPECT_NEAR(a.normalised(ax[0] * 100), 1, 1e-12);
    EXPECT_NEAR(a.normalised(ax[2] * 10), 0.5, 1e-12);
}

TEST(Estimation, ExperimentalVariogramAndFit) {
    // A linear trend z = x along a line: γ(h) = h²/2 exactly.
    SampleSet s;
    for (int i = 0; i < 50; ++i) {
        s.points.push_back({double(i), 0, 0});
        s.values.push_back(i);
        s.group.push_back(0);
    }
    ExperimentalOptions o;
    o.lag = 1;
    o.lags = 5;
    const auto v = experimentalVariogram(s, o);
    for (int k = 1; k <= 5; ++k) {
        EXPECT_NEAR(v[static_cast<std::size_t>(k - 1)].gamma, k * k / 2.0, 1e-12);
        EXPECT_EQ(v[static_cast<std::size_t>(k - 1)].pairs, static_cast<std::size_t>(50 - k));
    }
    o.azimuth = 0; // north: no pairs along a line running east
    o.angleTolerance = 22.5;
    EXPECT_EQ(experimentalVariogram(s, o)[0].pairs, 0u);
    o.azimuth = 270; // directions are symmetric
    EXPECT_EQ(experimentalVariogram(s, o)[0].pairs, 49u);
    // Fit recovers a known model from exact lag values.
    VariogramModel truth;
    truth.nugget = 0.15;
    truth.structures.push_back({VariogramType::Spherical, 0.5, Anisotropy::isotropic(40)});
    truth.structures.push_back({VariogramType::Exponential, 0.35, Anisotropy::isotropic(150)});
    std::vector<LagPoint> lags;
    for (int k = 1; k <= 30; ++k) lags.push_back({k * 8.0, truth.gamma({k * 8.0, 0, 0}), 100});
    VariogramModel init = truth;
    init.nugget = 0.3;
    init.structures[0].sill = 0.3;
    init.structures[0].ranges = Anisotropy::isotropic(25);
    init.structures[1].sill = 0.6;
    init.structures[1].ranges = Anisotropy::isotropic(250);
    const auto f = fitVariogram(lags, init, {1, 0, 0});
    EXPECT_NEAR(f.model.nugget, 0.15, 2e-3);
    EXPECT_NEAR(f.model.structures[0].ranges.major, 40, 0.5);
    EXPECT_NEAR(f.model.structures[1].ranges.major, 150, 3);
    EXPECT_NEAR(f.model.totalSill(), 1.0, 5e-3);
    EXPECT_LT(f.weightedSse, 1e-6);
}

TEST(Estimation, KrigingAndIdwProperties) {
    VariogramModel vg;
    vg.nugget = 0;
    vg.structures.push_back({VariogramType::Spherical, 1, Anisotropy::isotropic(100)});
    SampleSet s;
    const Vec3 corners[4] = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {10, 10, 0}};
    const double vals[4] = {1, 2, 3, 6};
    for (int i = 0; i < 4; ++i) {
        s.points.push_back(corners[i]);
        s.values.push_back(vals[i]);
        s.group.push_back(i);
    }
    EstimationOptions o;
    o.variogram = vg;
    o.passes[0].ellipse = Anisotropy::isotropic(200);
    o.passes[0].maxSamples = 4;
    std::vector<double> kv;
    // Symmetric configuration: equal weights → the mean.
    auto e = estimateAt({{5, 5, 0}}, s, o, &kv);
    EXPECT_NEAR(e[0], 3.0, 1e-10);
    EXPECT_GT(kv[0], 0);
    // Exact interpolator at a sample with zero nugget.
    e = estimateAt({{10, 10, 0}}, s, o, &kv);
    EXPECT_NEAR(e[0], 6.0, 1e-9);
    EXPECT_NEAR(kv[0], 0.0, 1e-9);
    // Unbiased: a constant field is reproduced anywhere.
    SampleSet c = s;
    c.values = {4, 4, 4, 4};
    EXPECT_NEAR(estimateAt({{2, 7, 3}}, c, o)[0], 4.0, 1e-10);
    // One sample: OK = the sample; KV = 2γ(h).
    o.passes[0].maxSamples = 1;
    e = estimateAt({{3, 4, 0}}, s, o, &kv);
    EXPECT_NEAR(e[0], 1.0, 1e-12);
    EXPECT_NEAR(kv[0], 2 * vg.gamma({3, 4, 0}), 1e-12);
    // Simple kriging far from data returns the mean.
    o.method = EstimationMethod::SimpleKriging;
    o.simpleMean = 2.75;
    o.passes[0].ellipse = Anisotropy::isotropic(1000);
    o.passes[0].maxSamples = 4;
    EXPECT_NEAR(estimateAt({{500, 500, 0}}, s, o)[0], 2.75, 1e-10);
    // IDW, power 2: samples at distances 1 and 2.
    SampleSet t;
    t.points = {{1, 0, 0}, {-2, 0, 0}};
    t.values = {10, 20};
    t.group = {0, 1};
    EstimationOptions idw;
    idw.method = EstimationMethod::InverseDistance;
    idw.passes[0].ellipse = Anisotropy::isotropic(10);
    EXPECT_NEAR(estimateAt({{0, 0, 0}}, t, idw)[0], (10 + 0.25 * 20) / 1.25, 1e-12);
    idw.method = EstimationMethod::NearestNeighbour;
    EXPECT_DOUBLE_EQ(estimateAt({{0, 0, 0}}, t, idw)[0], 10);
    // Minimum samples not met in the only pass → not estimated.
    idw.passes[0].minSamples = 3;
    EXPECT_TRUE(std::isnan(estimateAt({{0, 0, 0}}, t, idw)[0]));
    // Validation of options.
    EstimationOptions bad;
    bad.variogram = VariogramModel{};
    EXPECT_THROW(estimateAt({{0, 0, 0}}, t, bad), ValidationError);
}

TEST(Estimation, BlockEstimationSearchLimitsAndCrossValidation) {
    // Two holes (vertical lines of samples) with a gradient in x.
    SampleSet s;
    for (int hId = 0; hId < 4; ++hId)
        for (int k = 0; k < 10; ++k) {
            const Vec3 p{5.0 + 10 * hId, 5, 0.5 + k};
            s.points.push_back(p);
            s.values.push_back(p.x / 10 + 0.01 * k);
            s.group.push_back(hId);
        }
    s.groupNames = {"H1", "H2", "H3", "H4"};
    BlockModelData d;
    d.header.blockSize = {10, 10, 5};
    d.header.nx = 4;
    d.header.ny = 1;
    d.header.nz = 2;
    auto& mask = d.attribute("mask");
    mask.assign(d.count(), 1);
    mask[7] = 0;
    EstimationOptions o;
    o.variogram.nugget = 0.05;
    o.variogram.structures.push_back({VariogramType::Spherical, 0.95, Anisotropy::isotropic(40)});
    o.passes[0].ellipse = Anisotropy::isotropic(12);
    o.passes[0].minSamples = 6;
    o.passes[0].maxSamples = 12;
    o.passes[0].maxPerHole = 3;
    SearchPass wide;
    wide.ellipse = Anisotropy::isotropic(60);
    wide.maxSamples = 20;
    o.passes.push_back(wide);
    o.discretisation = {3, 3, 2};
    o.maskField = "mask";
    o.threads = 3;
    const auto sum = estimate(d, s, o);
    EXPECT_EQ(sum.skippedMask, 1u);
    EXPECT_EQ(sum.estimated, 7u);
    EXPECT_EQ(sum.singularSystems, 0u);
    const auto& est = *d.find("est");
    const auto& ns = *d.find("est_NS");
    const auto& pass = *d.find("est_PASS");
    EXPECT_TRUE(std::isnan(est[7]));
    for (std::size_t i = 0; i < 7; ++i) {
        EXPECT_TRUE(std::isfinite(est[i]));
        EXPECT_TRUE(std::isfinite((*d.find("est_KV"))[i]));
        if (pass[i] == 1) EXPECT_LE(ns[i], 9); // ≤ 3 per hole; the 12 m ellipse reaches at most three holes
    }
    EXPECT_GT(sum.perPass[0] + sum.perPass[1], 0u);
    // Estimates follow the x-gradient.
    const BlockGrid g(d.header);
    EXPECT_LT(est[g.index(0, 0, 0)], est[g.index(3, 0, 0)]);
    // Cancellation leaves the model untouched.
    BlockModelData d2 = d;
    jobs::Progress pr;
    pr.cancel();
    o.outputField = "est2";
    EXPECT_THROW(estimate(d2, s, o, pr), jobs::CancelledError);
    EXPECT_FALSE(d2.find("est2"));
    // Cross-validation (leave one hole out) produces finite, sensible statistics.
    EstimationOptions cvo = o;
    cvo.maskField.clear();
    cvo.passes = {wide};
    const auto cv = crossValidate(s, cvo, true);
    EXPECT_EQ(cv.estimated, s.points.size());
    EXPECT_GT(cv.correlation, 0.8);
    // Declustering weights.
    const auto w = cellDeclusterWeights({{0.5, 0.5, 0.5}, {0.6, 0.5, 0.5}, {0.7, 0.5, 0.5}, {0.8, 0.5, 0.5}, {50, 50, 50}}, 10, 1);
    EXPECT_NEAR(w[0], 5.0 / 8, 1e-12);
    EXPECT_NEAR(w[4], 2.5, 1e-12);
    // Samples from composites, filtered by code, duplicates merged.
    std::vector<Composite> comps(3);
    comps[0].holeId = comps[1].holeId = "A";
    comps[2].holeId = "B";
    comps[0].mid = comps[1].mid = {1, 1, 1};
    comps[2].mid = {5, 5, 5};
    comps[0].values["au"] = 1;
    comps[1].values["au"] = 3;
    comps[2].values["au"] = 9;
    for (auto& c : comps) c.codes["dom"] = "ORE";
    comps[2].codes["dom"] = "WASTE";
    const auto ss = samplesFromComposites(comps, "au", "dom", "ORE");
    ASSERT_EQ(ss.points.size(), 1u);
    EXPECT_DOUBLE_EQ(ss.values[0], 2);
    EXPECT_EQ(ss.mergedDuplicates, 1u);
}
