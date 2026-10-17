// SPDX-License-Identifier: Apache-2.0
#include "openmine/geotechnical/RockMass.h"
#include "openmine/geotechnical/Stereonet.h"
#include "openmine/project/ProjectEditor.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <random>

using namespace openmine;
using namespace openmine::geotechnical;

namespace {
constexpr double kDeg = std::numbers::pi / 180.0;

/// Fisher-distributed axes about `mean` (Wood 1994 sampling of the colatitude).
std::vector<Vec3> fisherSample(Line mean, double kappa, int n, std::mt19937& rng) {
    std::uniform_real_distribution<double> U(0, 1);
    const Vec3 m = lineVector(mean);
    const Vec3 ref = std::abs(m.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    const Vec3 u = cross(m, ref).normalized(), w = cross(m, u);
    std::vector<Vec3> out;
    for (int i = 0; i < n; ++i) {
        const double x = U(rng);
        const double c = 1 + std::log(x + (1 - x) * std::exp(-2 * kappa)) / kappa;
        const double s = std::sqrt(std::max(0.0, 1 - c * c));
        const double ph = 2 * std::numbers::pi * U(rng);
        Vec3 v = m * c + (u * std::cos(ph) + w * std::sin(ph)) * s;
        if (U(rng) < 0.5) v = -v; // axial data: either end
        out.push_back(v);
    }
    return out;
}
} // namespace

// ---- Q ------------------------------------------------------------------------------------------
TEST(QSystem, ValueClassSupportAndValidation) {
    const QInputs in{90, 9, 1.5, 1, 1, 1};
    const auto r = computeQ(in);
    EXPECT_DOUBLE_EQ(r.q, 15);
    EXPECT_EQ(r.rockClass, "Good");
    EXPECT_DOUBLE_EQ(r.blockSize, 10);
    const auto low = computeQ({5, 9, 1, 4, 0.66, 2.5});
    ASSERT_EQ(low.warnings.size(), 1u); // RQD raised to 10
    EXPECT_NEAR(low.q, 10.0 / 9 * 0.25 * 0.66 / 2.5, 1e-12);
    EXPECT_EQ(qClass(0.005), "Exceptionally poor");
    EXPECT_EQ(qClass(400), "Exceptionally good");
    EXPECT_THROW(computeQ({90, 9, 5, 1, 1, 1}), ClassificationError); // Jr > 4
    EXPECT_THROW(computeQ({90, 9, 1, 0.5, 1, 1}), ClassificationError); // Ja < 0.75
    EXPECT_THROW(computeQ({120, 9, 1, 1, 1, 1}), ClassificationError);
    const auto s = qSupport(in, 6, 5, 1.6);
    EXPECT_DOUBLE_EQ(s.equivalentDimension, 3.75);
    EXPECT_NEAR(s.maxUnsupportedSpan, 2 * 1.6 * std::pow(15.0, 0.4), 1e-12);
    EXPECT_TRUE(s.unsupportedSpanExceeded == (6 > s.maxUnsupportedSpan));
    EXPECT_DOUBLE_EQ(s.roofBoltLength, 2 + 0.15 * 6 / 1.6);
    EXPECT_DOUBLE_EQ(s.wallBoltLength, 2 + 0.15 * 5 / 1.6);
    EXPECT_DOUBLE_EQ(s.qWall, 75);
    EXPECT_NEAR(s.roofPressureMPa, 2 * 3 * std::pow(15.0, -1.0 / 3) / 4.5 * 0.0980665, 1e-12);
    EXPECT_LT(s.wallPressureMPa, s.roofPressureMPa);
    EXPECT_DOUBLE_EQ(qc(15, 50), 7.5);
    EXPECT_EQ(srfFromStress(150, 10, 30).code, "J");
    const auto burst = srfFromStress(150, 40, 120); // σc/σ1 3.75 (L) but σθ/σc 0.8 (M): the more severe governs
    EXPECT_EQ(burst.code, "M");
    EXPECT_DOUBLE_EQ(burst.srfMin, 50);
    EXPECT_EQ(jnTable().size(), 9u);
    EXPECT_EQ(esrTable().front().value, 3);
}

// ---- RMR, GSI, Hoek–Brown -------------------------------------------------------------------------
TEST(Rmr89, RatingsClassOrientationAndGsi) {
    RmrInputs in;
    in.ucsMPa = 120;
    in.rqd = 80;
    in.spacingM = 0.5;
    in.conditionDetail = RmrConditionDetail{2, 0.5, Roughness::Rough, Infilling::None, Weathering::Slightly};
    in.inflowLitresPerMinPer10m = 5;
    in.orientation = OrientationClass::Fair;
    const auto r = computeRmr89(in);
    EXPECT_EQ(r.ucsRating, 12);
    EXPECT_EQ(r.rqdRating, 17);
    EXPECT_EQ(r.spacingRating, 10);
    EXPECT_EQ(r.conditionRating, 4 + 4 + 5 + 6 + 5);
    EXPECT_EQ(r.groundwaterRating, 10);
    EXPECT_EQ(r.basic, 73);
    EXPECT_EQ(r.rmr, 68);
    EXPECT_EQ(r.classNumber, 2);
    EXPECT_EQ(r.rockClass, "II — Good rock");
    EXPECT_DOUBLE_EQ(gsiFromRmr89(r), 73 - 10 + 15 - 5);
    // Overall condition and general groundwater instead of the detailed inputs.
    RmrInputs b = in;
    b.conditionDetail.reset();
    b.condition = JointCondition::SoftGouge;
    b.inflowLitresPerMinPer10m.reset();
    b.groundwater = GroundwaterCondition::Flowing;
    b.ucsMPa = 0.5;
    b.rqd = 10;
    b.spacingM = 0.05;
    b.orientation = OrientationClass::VeryUnfavourable;
    const auto poor = computeRmr89(b);
    EXPECT_EQ(poor.basic, 0 + 3 + 5 + 0 + 0); // RMR89′ = 23: GSI not defined
    EXPECT_EQ(poor.classNumber, 5);
    EXPECT_THROW(gsiFromRmr89(poor), ClassificationError);
    RmrInputs pl = in;
    pl.ucsMPa.reset();
    pl.pointLoadMPa = 0.5;
    EXPECT_THROW(computeRmr89(pl), ClassificationError);
    pl.pointLoadMPa = 5;
    EXPECT_EQ(computeRmr89(pl).ucsRating, 12);
    // Tunnel orientation classes (joints striking E–W).
    EXPECT_EQ(tunnelOrientationClass(60, 0, 0), OrientationClass::VeryFavourable);   // drive with dip
    EXPECT_EQ(tunnelOrientationClass(30, 0, 10), OrientationClass::Favourable);
    EXPECT_EQ(tunnelOrientationClass(60, 0, 180), OrientationClass::Fair);           // against dip
    EXPECT_EQ(tunnelOrientationClass(30, 0, 180), OrientationClass::Unfavourable);
    EXPECT_EQ(tunnelOrientationClass(60, 0, 90), OrientationClass::VeryUnfavourable); // strike parallel to the axis
    EXPECT_EQ(tunnelOrientationClass(30, 0, 270), OrientationClass::Fair);
    EXPECT_EQ(tunnelOrientationClass(10, 0, 0), OrientationClass::Fair);
}

TEST(HoekBrown, GeneralisedCriterionAndModulus) {
    const auto h = hoekBrown(50, 10, 100, 0);
    EXPECT_NEAR(h.mb, 10 * std::exp(-50.0 / 28), 1e-12);
    EXPECT_NEAR(h.mb, 1.677, 1e-3); // Hoek et al. 2002 worked values
    EXPECT_NEAR(h.s, 0.0039, 1e-4);
    EXPECT_NEAR(h.a, 0.506, 1e-3);
    EXPECT_NEAR(hoekBrownSigma1(h, 100, 0), h.sigmaC, 1e-12);
    EXPECT_GT(hoekBrownSigma1(h, 100, 5), hoekBrownSigma1(h, 100, 1));
    EXPECT_NEAR(h.sigmaT, -h.s * 100 / h.mb, 1e-12);
    EXPECT_NEAR(h.modulusMPa, 100000 / (1 + std::exp(25.0 / 11)), 1e-6);
    EXPECT_NEAR(hoekBrown(50, 10, 100, 0, 50000).modulusMPa, 50000 * (0.02 + 1 / (1 + std::exp(10.0 / 11))), 1e-6);
    EXPECT_LT(hoekBrown(50, 10, 100, 0.7).mb, h.mb); // blast damage weakens the mass
    EXPECT_THROW(hoekBrown(50, 10, 100, 1.5), ClassificationError);
    EXPECT_DOUBLE_EQ(rmrFromQBarton1995(10), 65);
    EXPECT_NEAR(qFromRmrBarton1995(65), 10, 1e-12);
    EXPECT_NEAR(rmrFromQBieniawski1976(1), 44, 1e-12);
}

TEST(Geotechnical, ClassificationsAreRecordedWithInputs) {
    auto p = Project::createNew("Geotech", Date::fromYmd(2026, 11, 2));
    const Uuid q = recordQ(*p, {}, 125.5, {1, 2, 3}, {5, 9, 1, 4, 0.66, 2.5}, "A. Mapper", Date::fromYmd(2026, 11, 3));
    const auto* g = p->model().geotechClassifications.find(q);
    ASSERT_TRUE(g);
    EXPECT_EQ(g->method, GeotechMethod::QSystem);
    EXPECT_NE(g->inputsJson.find("\"Jn\":9"), std::string::npos);
    EXPECT_NE(g->notes.find("RQD"), std::string::npos);
    EXPECT_EQ(g->rockClass, qClass(g->value));
    RmrInputs in;
    in.ucsMPa = 80;
    in.condition = JointCondition::SlightlyRough;
    in.groundwater = GroundwaterCondition::Damp;
    const Uuid r = recordRmr(*p, {}, 130, {}, in);
    EXPECT_EQ(p->model().geotechClassifications.find(r)->rockClass, computeRmr89(in).rockClass);
    p->commands().undo();
    EXPECT_FALSE(p->model().geotechClassifications.find(r));
}

// ---- stereonet ------------------------------------------------------------------------------------
TEST(Stereonet, OrientationAlgebraAndProjections) {
    const Line pole = poleOf({60, 120});
    EXPECT_DOUBLE_EQ(pole.trend, 300);
    EXPECT_DOUBLE_EQ(pole.plunge, 30);
    const Plane back = planeOfPole(pole);
    EXPECT_DOUBLE_EQ(back.dip, 60);
    EXPECT_DOUBLE_EQ(back.dipDirection, 120);
    const Line l = lineOf(lineVector({237.5, 41.25}));
    EXPECT_NEAR(l.trend, 237.5, 1e-9);
    EXPECT_NEAR(l.plunge, 41.25, 1e-9);
    const Line up = lineOf(Vec3{0, 0.5, 0.5}); // upward vector folds down
    EXPECT_NEAR(up.trend, 180, 1e-9);
    EXPECT_NEAR(up.plunge, 45, 1e-9);
    for (auto pr : {Projection::EqualArea, Projection::EqualAngle}) {
        const auto c = project({0, 90}, pr);
        EXPECT_NEAR(std::hypot(c.x, c.y), 0, 1e-12);
        const auto e = project({90, 0}, pr);
        EXPECT_NEAR(e.x, 1, 1e-12);
        EXPECT_NEAR(e.y, 0, 1e-12);
        const auto q = project({123, 37}, pr);
        const auto u = unproject(q, pr);
        EXPECT_NEAR(u.trend, 123, 1e-9);
        EXPECT_NEAR(u.plunge, 37, 1e-9);
    }
    EXPECT_NEAR(std::hypot(project({0, 45}).x, project({0, 45}).y), std::sqrt(2.0) * std::sin(22.5 * kDeg), 1e-12);
    const auto i = intersection({45, 90}, {45, 270});
    ASSERT_TRUE(i);
    EXPECT_NEAR(i->plunge, 0, 1e-9);
    EXPECT_NEAR(std::sin(i->trend * kDeg), 0, 1e-9);
    EXPECT_FALSE(intersection({30, 10}, {30, 10}));
    EXPECT_NEAR(apparentDip({60, 0}, 0), 60, 1e-12);
    EXPECT_NEAR(apparentDip({60, 0}, 90), 0, 1e-12);
    const auto gc = greatCircle({40, 200}, 90);
    EXPECT_NEAR(std::hypot(gc.front().x, gc.front().y), 1, 1e-12);
    EXPECT_NEAR(std::hypot(gc.back().x, gc.back().y), 1, 1e-12);
    const auto mid = project({200, 40});
    EXPECT_NEAR(gc[45].x, mid.x, 1e-9);
    EXPECT_NEAR(gc[45].y, mid.y, 1e-9);
    // A small circle about a shallow axis crosses the primitive: two runs.
    EXPECT_GE(smallCircle({0, 10}, 30).size(), 2u);
    EXPECT_EQ(smallCircle({0, 90}, 30).size(), 1u);
}

TEST(Stereonet, FisherTensorTerzaghiAndDensity) {
    std::mt19937 rng(7);
    const Line mean{140, 35};
    const auto s = fisherSample(mean, 50, 600, rng);
    const auto f = fisher(s);
    EXPECT_LT(axialAngle(lineVector(f.mean), lineVector(mean)), 1.5);
    EXPECT_NEAR(f.k, 50, 7.5);
    EXPECT_NEAR(f.alpha95, 140 / std::sqrt(f.k * 600), 0.15);
    const auto same = fisher({{0, 0, -1}, {0, 0, 1}, {0, 0, -1}}); // axial: opposite ends are the same axis
    EXPECT_GT(same.k, 1e11);
    EXPECT_NEAR(same.alpha95, 0, 1e-6);
    // Girdle about the north axis: smallest eigenvector = the girdle axis.
    std::vector<Vec3> girdle;
    for (int a = 0; a < 180; a += 5) girdle.push_back({std::cos(a * kDeg), 0, -std::sin(a * kDeg)});
    const auto t = orientationTensor(girdle);
    EXPECT_NEAR(t.eigenvalues[2], 0, 1e-12);
    EXPECT_NEAR(std::abs(t.eigenvectors[2].y), 1, 1e-9);
    EXPECT_LT(t.shapeK, 1); // girdle-like
    EXPECT_DOUBLE_EQ(terzaghiWeight({0, 0, -1}, {0, 0, 1}), 1);
    EXPECT_DOUBLE_EQ(terzaghiWeight({1, 0, 0}, {0, 0, 1}), 10);
    // Density: a single tight cluster peaks at 100 %; uniform data average ≈ 1 % (Schmidt) and 3σ (Kamb).
    const auto peak = poleDensity(std::vector<Vec3>(20, {0, 0, -1}));
    EXPECT_NEAR(peak.maxValue, 100, 1e-9);
    // The 100 % plateau spans the counting cone (8.1°, radius ≈ 0.1 on the net) around the centre.
    EXPECT_LE(std::hypot(peak.maxAt.x, peak.maxAt.y), std::sqrt(2.0) * std::sin(peak.coneAngleDeg / 2 * kDeg) + 1e-9);
    std::vector<Vec3> uni;
    std::normal_distribution<double> N(0, 1);
    for (int i = 0; i < 4000; ++i) uni.push_back(Vec3{N(rng), N(rng), N(rng)}.normalized());
    auto avg = [](const DensityGrid& g) {
        double s2 = 0;
        int n = 0;
        for (double v : g.values)
            if (!std::isnan(v)) {
                s2 += v;
                ++n;
            }
        return s2 / n;
    };
    EXPECT_NEAR(avg(poleDensity(uni, {}, DensityMethod::Schmidt1Percent, 41)), 1.0, 0.1);
    EXPECT_NEAR(avg(poleDensity(uni, {}, DensityMethod::Kamb, 41)), 3.0, 0.3);
    // A contour around a cluster is a closed loop enclosing the maximum.
    const auto cl = poleDensity(fisherSample({30, 60}, 40, 300, rng), {}, DensityMethod::Schmidt1Percent, 61);
    const auto lines = contour(cl, cl.maxValue / 2);
    ASSERT_FALSE(lines.empty());
    const auto& loop = *std::max_element(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.size() < b.size(); });
    EXPECT_NEAR(loop.front().x, loop.back().x, 1e-12);
    EXPECT_NEAR(loop.front().y, loop.back().y, 1e-12);
    double wind = 0;
    for (std::size_t k = 0; k + 1 < loop.size(); ++k) {
        const double a0 = std::atan2(loop[k].y - cl.maxAt.y, loop[k].x - cl.maxAt.x), a1 = std::atan2(loop[k + 1].y - cl.maxAt.y, loop[k + 1].x - cl.maxAt.x);
        double d = a1 - a0;
        while (d > std::numbers::pi) d -= 2 * std::numbers::pi;
        while (d < -std::numbers::pi) d += 2 * std::numbers::pi;
        wind += d;
    }
    EXPECT_NEAR(std::abs(wind), 2 * std::numbers::pi, 1e-6);
}

TEST(Stereonet, JointSetsAreRecovered) {
    std::mt19937 rng(11);
    const std::vector<Plane> truth = {{60, 45}, {80, 150}, {15, 270}};
    std::vector<Vec3> poles;
    std::vector<int> label;
    for (int s = 0; s < 3; ++s)
        for (const auto& v : fisherSample(poleOf(truth[static_cast<std::size_t>(s)]), 80, 100, rng)) {
            poles.push_back(v);
            label.push_back(s);
        }
    const auto r = identifyJointSets(poles, 3, 0, 3);
    ASSERT_EQ(r.sets.size(), 3u);
    for (const auto& t : truth) {
        double best = 180;
        for (const auto& s : r.sets) best = std::min(best, axialAngle(poleVector(s.meanPlane), poleVector(t)));
        EXPECT_LT(best, 3.0);
    }
    // Purity: each true set maps to one found set.
    int agree = 0;
    std::map<std::pair<int, int>, int> m;
    for (std::size_t i = 0; i < poles.size(); ++i) ++m[{label[i], r.assignment[i]}];
    for (int s = 0; s < 3; ++s) {
        int best = 0;
        for (const auto& [k, v] : m)
            if (k.first == s) best = std::max(best, v);
        agree += best;
    }
    EXPECT_GT(agree, 290);
    // Deterministic.
    EXPECT_EQ(identifyJointSets(poles, 3, 0, 3).assignment, r.assignment);
    // Outlier cut-off and explicit assignment.
    poles.push_back(poleVector({45, 330}));
    const auto cut = identifyJointSets(poles, 3, 20, 3);
    EXPECT_EQ(cut.assignment.back(), -1);
    std::vector<Line> means;
    for (const auto& t : truth) means.push_back(poleOf(t));
    const auto a = assignToSets(poles, means, 20);
    EXPECT_EQ(a.assignment.back(), -1);
    EXPECT_EQ(a.sets.size(), 3u);
    EXPECT_GT(a.sets[0].members.size(), 95u);
    std::vector<StructuralMeasurement> ms(2);
    ms[0].dip = 60;
    ms[0].dipDirection = 45;
    ms[0].setName = "J1";
    ms[1].setName = "J2";
    EXPECT_EQ(measurementPoles(ms, "J1").size(), 1u);
}

TEST(Stereonet, KinematicAnalysisAndSvg) {
    KinematicOptions o;
    o.face = {70, 180};
    o.frictionDeg = 30;
    const std::vector<Vec3> poles = {poleVector({50, 180}), poleVector({50, 250}), poleVector({20, 180}), poleVector({80, 180}), poleVector({75, 0}), poleVector({40, 0})};
    const auto r = kinematicAnalysis(poles, {{60, 140}, {60, 220}}, o);
    EXPECT_EQ(r.planar.critical, (std::vector<std::size_t>{0}));
    EXPECT_EQ(r.toppling.critical, (std::vector<std::size_t>{4}));
    EXPECT_NEAR(r.planar.percent, 100.0 / 6, 1e-12);
    ASSERT_EQ(r.wedges.size(), 1u);
    EXPECT_TRUE(r.wedges[0].critical);
    EXPECT_NEAR(r.wedges[0].intersection.trend, 180, 1e-9);
    EXPECT_NEAR(r.wedges[0].intersection.plunge, std::atan(std::tan(60 * kDeg) * std::cos(40 * kDeg)) / kDeg, 1e-9);
    EXPECT_FALSE(r.wedges[0].singlePlaneSliding);
    // Hocking: plane A (50/165) lies between the intersection trend (≈178°) and the face (150°).
    KinematicOptions h = o;
    h.face = {70, 150};
    const auto hr = kinematicAnalysis({}, {{50, 165}, {80, 100}}, h);
    ASSERT_EQ(hr.wedges.size(), 1u);
    EXPECT_TRUE(hr.wedges[0].critical);
    EXPECT_TRUE(hr.wedges[0].singlePlaneSliding);
    EXPECT_EQ(hr.wedges[0].slidingPlane, 0u);
    EXPECT_GT(wedgeIntersectionPercent({poleVector({60, 140}), poleVector({60, 220}), poleVector({10, 0})}, o), 0);
    const auto walls = driveWalls(0);
    EXPECT_DOUBLE_EQ(walls[0].dipDirection, 90);
    EXPECT_DOUBLE_EQ(walls[1].dipDirection, 270);
    // SVG renders poles, sets, contours and the friction circle.
    std::mt19937 rng(5);
    const auto pts = fisherSample({200, 50}, 30, 50, rng);
    const auto sets = identifyJointSets(pts, 1).sets;
    const auto dens = poleDensity(pts);
    StereonetSvgOptions so;
    so.frictionDeg = 30;
    so.contourLevels = {2, 4, 8};
    so.title = "Test";
    const auto svg = stereonetSvg(pts, sets, &dens, so);
    EXPECT_NE(svg.find("<svg"), std::string::npos);
    EXPECT_EQ(std::count(svg.begin(), svg.end(), 'c') > 50, true);
    EXPECT_NE(svg.find("J1 "), std::string::npos);
    EXPECT_NE(svg.find("data-level=\"4.00\""), std::string::npos);
    EXPECT_NE(svg.find("stroke-dasharray"), std::string::npos);
}
