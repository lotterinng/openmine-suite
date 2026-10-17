// SPDX-License-Identifier: Apache-2.0
#include "openmine/mine_design/Layout.h"

#include "openmine/geometry/Polyline.h"
#include "openmine/project/Project.h"

#include <cmath>
#include <numbers>

namespace openmine::mine_design {

namespace {
constexpr double kPi = std::numbers::pi;

void finish(LayoutResult& r) {
    r.bulges.resize(r.vertices.size(), 0.0);
    r.planLength = geometry::horizontalLengthBulged(r.vertices, r.bulges, false);
    r.length3d = geometry::length3dBulged(r.vertices, r.bulges, false);
}
} // namespace

Vec2 bearingVector(double az) {
    const double a = az * kPi / 180.0;
    return {std::sin(a), std::cos(a)};
}

LayoutResult horizontalAlignment(const std::vector<Vec3>& pis, double radius, double gradePercent, bool useVertexElevations) {
    if (pis.size() < 2) throw ValidationError("An alignment needs at least two points");
    if (!(radius > 0)) throw ValidationError("Curve radius must be greater than zero");
    struct Element {
        Vec2 a, b;
        double bulge;
    };
    std::vector<Vec2> tangentPoints{xy(pis.front())};
    std::vector<double> bulges;
    std::vector<double> available(pis.size(), 0);
    double prevT = 0;
    for (std::size_t i = 1; i + 1 < pis.size(); ++i) {
        const Vec2 p0 = xy(pis[i - 1]), p1 = xy(pis[i]), p2 = xy(pis[i + 1]);
        const Vec2 d0 = (p1 - p0).normalized(), d1 = (p2 - p1).normalized();
        const double crs = cross(d0, d1);
        const double delta = std::atan2(std::abs(crs), dot(d0, d1)); // deflection angle
        if (delta < 1e-9) continue;                                   // straight through
        const double T = radius * std::tan(delta / 2);
        if (T + prevT > distance(p0, p1) + 1e-9)
            throw ValidationError("Curve of radius " + std::to_string(radius) + " m does not fit at point " + std::to_string(i + 1));
        if (i + 2 == pis.size() && T > distance(p1, p2) + 1e-9)
            throw ValidationError("Curve does not fit before the end point");
        const Vec2 tc = p1 - d0 * T, ct = p1 + d1 * T;
        tangentPoints.push_back(tc);
        bulges.push_back(0.0);                                     // straight up to the curve
        tangentPoints.push_back(ct);
        bulges.push_back((crs > 0 ? 1.0 : -1.0) * std::tan(delta / 4)); // curve
        prevT = T;
    }
    tangentPoints.push_back(xy(pis.back()));
    bulges.push_back(0.0);
    // Build vertices, elevations by plan chainage.
    LayoutResult r;
    double plan = 0;
    const double z0 = pis.front().z;
    for (std::size_t i = 0; i < tangentPoints.size(); ++i) {
        if (i > 0) {
            const Vec3 a{tangentPoints[i - 1].x, tangentPoints[i - 1].y, 0}, b{tangentPoints[i].x, tangentPoints[i].y, 0};
            const double bl = bulges[i - 1];
            if (auto arc = geometry::bulgeArc(a, b, bl)) plan += arc->planLength;
            else plan += distance(tangentPoints[i - 1], tangentPoints[i]);
        }
        r.vertices.push_back({tangentPoints[i].x, tangentPoints[i].y, z0 + plan * gradePercent / 100.0});
    }
    // Drop zero-length straights produced by back-to-back curves.
    LayoutResult clean;
    for (std::size_t i = 0; i < r.vertices.size(); ++i) {
        if (!clean.vertices.empty() && distance(clean.vertices.back(), r.vertices[i]) < 1e-9) {
            clean.bulges.back() = i < bulges.size() ? bulges[i] : 0.0;
            continue;
        }
        clean.vertices.push_back(r.vertices[i]);
        clean.bulges.push_back(i < bulges.size() ? bulges[i] : 0.0);
    }
    if (useVertexElevations) {
        // Interpolate z between the original PI elevations by plan chainage of their projections.
        std::vector<double> piPlan{0};
        for (std::size_t i = 1; i < pis.size(); ++i) piPlan.push_back(piPlan.back() + horizontalDistance(pis[i - 1], pis[i]));
        double acc = 0;
        const double totalTangent = piPlan.back();
        finish(clean);
        const double totalCurve = clean.planLength;
        for (std::size_t i = 0; i < clean.vertices.size(); ++i) {
            if (i > 0) {
                if (auto arc = geometry::bulgeArc(clean.vertices[i - 1], clean.vertices[i], clean.bulges[i - 1])) acc += arc->planLength;
                else acc += horizontalDistance(clean.vertices[i - 1], clean.vertices[i]);
            }
            const double s = acc / totalCurve * totalTangent;
            std::size_t k = 1;
            while (k + 1 < piPlan.size() && piPlan[k] < s) ++k;
            const double t = (s - piPlan[k - 1]) / std::max(1e-12, piPlan[k] - piPlan[k - 1]);
            clean.vertices[i].z = pis[k - 1].z + (pis[k].z - pis[k - 1].z) * std::clamp(t, 0.0, 1.0);
        }
    }
    finish(clean);
    return clean;
}

LayoutResult declineLayout(const DeclineSpec& s) {
    if (!(s.straightLength > 0) || !(s.curveRadius > 0)) throw ValidationError("Straight length and curve radius must be positive");
    if (s.gradePercent == 0) throw ValidationError("A decline needs a non-zero grade");
    const double drop = s.targetElevation - s.start.z;
    if (drop * s.gradePercent <= 0) throw ValidationError("The target elevation is not in the direction of the grade");
    const double g = s.gradePercent / 100.0;
    const double totalPlan = drop / g; // plan length needed
    LayoutResult r;
    r.vertices.push_back(s.start);
    r.bulges.push_back(0);
    Vec2 pos = xy(s.start), dir = bearingVector(s.startAzimuthDeg);
    double planDone = 0;
    const double side = s.turn == TurnDirection::Left ? 1.0 : -1.0; // left = CCW
    for (int leg = 0; leg < s.maxLegs && planDone < totalPlan - 1e-9; ++leg) {
        // Straight.
        const double sl = std::min(s.straightLength, totalPlan - planDone);
        pos = pos + dir * sl;
        planDone += sl;
        r.vertices.push_back({pos.x, pos.y, s.start.z + planDone * g});
        r.bulges.push_back(0);
        if (planDone >= totalPlan - 1e-9) break;
        // 180° turn (possibly partial at the end).
        const double fullArc = kPi * s.curveRadius;
        const double arcLen = std::min(fullArc, totalPlan - planDone);
        const double sweep = arcLen / s.curveRadius; // radians
        const Vec2 left{-dir.y, dir.x};
        const Vec2 centre = pos + left * (side * s.curveRadius);
        const double a0 = std::atan2(pos.y - centre.y, pos.x - centre.x);
        const double a1 = a0 + side * sweep;
        const Vec2 end{centre.x + s.curveRadius * std::cos(a1), centre.y + s.curveRadius * std::sin(a1)};
        r.bulges.back() = side * std::tan(sweep / 4);
        planDone += arcLen;
        r.vertices.push_back({end.x, end.y, s.start.z + planDone * g});
        r.bulges.push_back(0);
        pos = end;
        // New direction is the arc tangent at its end.
        dir = Vec2{-std::sin(a1), std::cos(a1)} * side;
    }
    if (planDone < totalPlan - 1e-6) r.notes.push_back("Stopped after the maximum number of legs before reaching the target elevation");
    finish(r);
    r.notes.push_back("Grade " + std::to_string(s.gradePercent) + " %, " + std::to_string(r.vertices.size() - 1) + " elements");
    return r;
}

LayoutResult spiralDecline(const Vec3& start, double az, double gradePercent, double radius, TurnDirection turn, double targetElevation) {
    if (!(radius > 0)) throw ValidationError("Spiral radius must be positive");
    const double g = gradePercent / 100.0;
    const double drop = targetElevation - start.z;
    if (g == 0 || drop * g <= 0) throw ValidationError("The target elevation is not in the direction of the grade");
    const double totalPlan = drop / g;
    const double side = turn == TurnDirection::Left ? 1.0 : -1.0;
    const Vec2 dir = bearingVector(az);
    const Vec2 left{-dir.y, dir.x};
    const Vec2 centre = xy(start) + left * (side * radius);
    double a = std::atan2(start.y - centre.y, start.x - centre.x);
    LayoutResult r;
    r.vertices.push_back(start);
    double done = 0;
    while (done < totalPlan - 1e-9) {
        const double arcLen = std::min(totalPlan - done, kPi / 2 * radius); // quarter turns
        const double sweep = arcLen / radius;
        r.bulges.push_back(side * std::tan(sweep / 4));
        a += side * sweep;
        done += arcLen;
        r.vertices.push_back({centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), start.z + done * g});
    }
    r.bulges.push_back(0);
    finish(r);
    return r;
}

} // namespace openmine::mine_design
