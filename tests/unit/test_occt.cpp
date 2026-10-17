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
