// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Polyline.h"
#include "openmine/objects/AttributeSchema.h"
#include "openmine/objects/DateTime.h"
#include "openmine/objects/Uuid.h"

#include <gtest/gtest.h>

#include <set>
#include <vector>

using namespace openmine;

TEST(Uuid, GeneratesUniqueVersion4) {
    std::set<Uuid> ids;
    for (int i = 0; i < 10000; ++i) {
        auto id = Uuid::generate();
        EXPECT_EQ(id.bytes()[6] >> 4, 4);
        EXPECT_EQ(id.bytes()[8] & 0xC0, 0x80);
        ids.insert(id);
    }
    EXPECT_EQ(ids.size(), 10000u);
}

TEST(Uuid, RoundTripsThroughText) {
    auto id = Uuid::generate();
    auto parsed = Uuid::parse(id.toString());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, id);
    EXPECT_EQ(Uuid::parse("6F9619FF-8B86-D011-B42D-00C04FC964FF")->toString(), "6f9619ff-8b86-d011-b42d-00c04fc964ff");
    EXPECT_FALSE(Uuid::parse("not-a-uuid"));
    EXPECT_FALSE(Uuid::parse("6f9619ff-8b86-d011-b42d-00c04fc964fZ"));
    EXPECT_TRUE(Uuid().isNil());
}

TEST(Date, CivilConversionsAndWeekdays) {
    EXPECT_EQ(Date::fromYmd(1970, 1, 1).days(), 0);
    EXPECT_EQ(Date::fromYmd(2026, 10, 17).toString(), "2026-10-17");
    EXPECT_EQ(Date::fromYmd(2026, 10, 17).isoWeekday(), 6); // Saturday
    EXPECT_EQ(Date::fromYmd(2024, 2, 29).addDays(1).toString(), "2024-03-01");
    EXPECT_EQ(Date::fromYmd(1969, 12, 31).isoWeekday(), 3);
    EXPECT_TRUE(Date::parse("2024-02-29"));
    EXPECT_FALSE(Date::parse("2023-02-29"));
    EXPECT_FALSE(Date::parse("2026-13-01"));
    EXPECT_FALSE(Date::parse("26-01-01"));
}

TEST(Polyline, LengthsAndGradient) {
    // 3-4-5 triangle in plan, then a 1:8 decline leg.
    std::vector<Vec3> pts{{0, 0, 0}, {3, 4, 0}, {3, 84, -10}};
    EXPECT_NEAR(geometry::horizontalLength(pts), 85.0, 1e-12);
    EXPECT_NEAR(geometry::length3d(pts), 5.0 + std::sqrt(80.0 * 80.0 + 100.0), 1e-12);
    ASSERT_TRUE(geometry::gradientPercent(pts));
    EXPECT_NEAR(*geometry::gradientPercent(pts), -10.0 / 85.0 * 100.0, 1e-12);
    EXPECT_FALSE(geometry::gradientPercent(std::vector<Vec3>{{0, 0, 0}, {0, 0, 10}}));
}

TEST(Polyline, ClosedAndDistances) {
    std::vector<Vec3> sq{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    EXPECT_NEAR(geometry::length3d(sq, true), 40.0, 1e-12);
    EXPECT_NEAR(*geometry::pointPolylineDistance({5, -3, 4}, sq), 5.0, 1e-12);
    auto mid = geometry::pointAtChainage(sq, 15.0);
    ASSERT_TRUE(mid);
    EXPECT_NEAR(mid->x, 10.0, 1e-12);
    EXPECT_NEAR(mid->y, 5.0, 1e-12);
}

TEST(Polyline, PrecisionAtLargeMineCoordinates) {
    // Lo-system style magnitudes: millimetre differences must survive.
    std::vector<Vec3> pts{{-25000.123, 2900000.456, 1200.0}, {-25000.124, 2900000.456, 1200.0}};
    EXPECT_NEAR(geometry::length3d(pts), 0.001, 1e-9);
}

TEST(AttributeSchema, ValidatesTypedValues) {
    AttributeDefinition d;
    d.displayName = "Rock class";
    d.valueType = AttributeValueType::Integer;
    EXPECT_EQ(validateAttributeValue(d, "3"), "");
    EXPECT_NE(validateAttributeValue(d, "3.5"), "");
    d.valueType = AttributeValueType::Real;
    EXPECT_EQ(validateAttributeValue(d, "-2.75"), "");
    EXPECT_NE(validateAttributeValue(d, "abc"), "");
    d.valueType = AttributeValueType::Date;
    EXPECT_EQ(validateAttributeValue(d, "2026-10-17"), "");
    d.required = true;
    EXPECT_NE(validateAttributeValue(d, ""), "");
    EXPECT_TRUE(isValidAttributeKey("rock_class2"));
    EXPECT_FALSE(isValidAttributeKey("2rock"));
    EXPECT_FALSE(isValidAttributeKey("rock class"));
}
