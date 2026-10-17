// SPDX-License-Identifier: Apache-2.0
#include "openmine/mine_design/Headings.h"

#include "openmine/cad/Edit.h"
#include "openmine/commands/CommandStack.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/project/ProjectEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace openmine::mine_design {

namespace {
constexpr double kPi = std::numbers::pi;

double bulgeAt(const std::vector<double>& b, std::size_t i) { return i < b.size() ? b[i] : 0.0; }

/// Plan length of segment i (exact for arcs).
double segPlanLength(const std::vector<Vec3>& v, const std::vector<double>& b, std::size_t i) {
    if (const auto arc = geometry::bulgeArc(v[i], v[i + 1], bulgeAt(b, i))) return arc->planLength;
    return horizontalDistance(v[i], v[i + 1]);
}

/// Point at fraction t (0…1 of plan length) of segment i, and the bulge of the sub-arc [t0, t1].
Vec3 segPoint(const std::vector<Vec3>& v, const std::vector<double>& b, std::size_t i, double t) {
    if (const auto arc = geometry::bulgeArc(v[i], v[i + 1], bulgeAt(b, i))) {
        const double a = arc->startAngle + arc->sweep * t;
        return {arc->center.x + arc->radius * std::cos(a), arc->center.y + arc->radius * std::sin(a), v[i].z + (v[i + 1].z - v[i].z) * t};
    }
    return lerp(v[i], v[i + 1], t);
}
double subBulge(const std::vector<Vec3>& v, const std::vector<double>& b, std::size_t i, double t0, double t1) {
    if (const auto arc = geometry::bulgeArc(v[i], v[i + 1], bulgeAt(b, i))) return std::tan(arc->sweep * (t1 - t0) / 4);
    return 0;
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

Vec2 rotate(Vec2 v, double a) { return {v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)}; }

LayoutResult finish(std::vector<Vec3> v, std::vector<double> b) {
    LayoutResult r;
    b.resize(v.size(), 0.0);
    r.vertices = std::move(v);
    r.bulges = std::move(b);
    r.planLength = geometry::horizontalLengthBulged(r.vertices, r.bulges, false);
    r.length3d = geometry::length3dBulged(r.vertices, r.bulges, false);
    bool any = false;
    for (double x : r.bulges) any = any || std::abs(x) > 1e-15;
    if (!any) r.bulges.clear();
    return r;
}
} // namespace

std::string GradeSegment::oneIn() const {
    if (std::abs(gradePercent) < 1e-9) return {};
    return "1 in " + fmt("%.1f", 100.0 / std::abs(gradePercent));
}

std::vector<GradeSegment> gradientProfile(const std::vector<Vec3>& v, const std::vector<double>& b) {
    std::vector<GradeSegment> out;
    double ch = 0;
    for (std::size_t i = 0; i + 1 < v.size(); ++i) {
        GradeSegment s;
        s.index = i;
        s.planLength = segPlanLength(v, b, i);
        s.rise = v[i + 1].z - v[i].z;
        s.gradePercent = s.planLength > 1e-12 ? 100.0 * s.rise / s.planLength : (s.rise == 0 ? 0 : std::copysign(1e9, s.rise));
        if (const auto arc = geometry::bulgeArc(v[i], v[i + 1], bulgeAt(b, i))) s.radius = arc->radius;
        s.chainageFrom = ch;
        ch += std::hypot(s.planLength, s.rise);
        s.chainageTo = ch;
        out.push_back(s);
    }
    return out;
}

std::vector<RuleViolation> checkDesignRules(const std::vector<Vec3>& v, const std::vector<double>& b, const DesignRules& r) {
    std::vector<RuleViolation> out;
    const auto prof = gradientProfile(v, b);
    for (std::size_t k = 0; k < prof.size(); ++k) {
        const auto& s = prof[k];
        const double g = std::abs(s.gradePercent);
        if (s.radius > 0) {
            if (s.radius < r.minRadius - 1e-9) out.push_back({s.chainageFrom, "Minimum curve radius", s.radius, r.minRadius});
            if (g > r.maxGradeOnCurvesPercent + 1e-9) out.push_back({s.chainageFrom, "Maximum grade on curves", g, r.maxGradeOnCurvesPercent});
        } else if (g > r.maxGradePercent + 1e-9) out.push_back({s.chainageFrom, "Maximum grade", g, r.maxGradePercent});
        if (k > 0 && std::abs(s.gradePercent - prof[k - 1].gradePercent) > r.maxGradeChangePercent + 1e-9)
            out.push_back({s.chainageFrom, "Grade change without vertical curve", std::abs(s.gradePercent - prof[k - 1].gradePercent), r.maxGradeChangePercent});
    }
    // Interior vertices of straight-to-straight polylines are sharp plan corners.
    for (std::size_t i = 1; i + 1 < v.size(); ++i) {
        if (std::abs(bulgeAt(b, i - 1)) > 1e-15 || std::abs(bulgeAt(b, i)) > 1e-15) continue;
        const Vec2 d0 = xy(v[i] - v[i - 1]).normalized(), d1 = xy(v[i + 1] - v[i]).normalized();
        const double turn = std::acos(std::clamp(dot(d0, d1), -1.0, 1.0)) * 180 / kPi;
        if (turn > 1.0) out.push_back({prof[i].chainageFrom, "Sharp plan corner (no curve)", turn, 1.0});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& c) { return a.chainage < c.chainage; });
    return out;
}

std::vector<Vec3> applyGrade(std::vector<Vec3> v, const std::vector<double>& b, double z0, double grade) {
    double s = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += segPlanLength(v, b, i - 1);
        v[i].z = z0 + grade / 100.0 * s;
    }
    return v;
}

std::vector<Vec3> gradeBetweenEnds(std::vector<Vec3> v, const std::vector<double>& b) {
    if (v.size() < 2) return v;
    const double L = geometry::horizontalLengthBulged(v, b, false);
    if (L <= 0) throw ValidationError("The centreline has no plan length");
    return applyGrade(v, b, v.front().z, 100.0 * (v.back().z - v.front().z) / L);
}

double verticalElevation(const std::vector<Vpi>& vpis, double s) {
    if (vpis.size() < 2) throw ValidationError("A vertical alignment needs at least two VPIs");
    // Locate the tangent segment.
    auto grade = [&](std::size_t k) { return (vpis[k + 1].elevation - vpis[k].elevation) / (vpis[k + 1].chainage - vpis[k].chainage); };
    for (std::size_t k = 1; k + 1 < vpis.size(); ++k) {
        const double L = vpis[k].curveLength;
        if (L <= 0) continue;
        const double a = vpis[k].chainage - L / 2;
        if (s >= a && s <= vpis[k].chainage + L / 2) {
            const double g1 = grade(k - 1), g2 = grade(k), x = s - a;
            return vpis[k].elevation - g1 * L / 2 + g1 * x + (g2 - g1) / (2 * L) * x * x;
        }
    }
    std::size_t k = 0;
    while (k + 2 < vpis.size() && s > vpis[k + 1].chainage) ++k;
    return vpis[k].elevation + grade(k) * (s - vpis[k].chainage);
}

LayoutResult verticalAlignment(const std::vector<Vec3>& h, const std::vector<double>& b, const std::vector<Vpi>& vpis0, double step) {
    if (h.size() < 2) throw ValidationError("The horizontal alignment needs at least two points");
    if (!(step > 0)) throw ValidationError("Sampling step must be positive");
    auto vpis = vpis0;
    std::sort(vpis.begin(), vpis.end(), [](const Vpi& a, const Vpi& c) { return a.chainage < c.chainage; });
    if (vpis.size() < 2) throw ValidationError("A vertical alignment needs at least two VPIs");
    const double L = geometry::horizontalLengthBulged(h, b, false);
    if (vpis.front().chainage > 1e-9 || vpis.back().chainage < L - 1e-6) throw ValidationError("The VPIs must cover the whole alignment (0 to its plan length)");
    for (std::size_t k = 0; k + 1 < vpis.size(); ++k) {
        if (vpis[k + 1].chainage - vpis[k].chainage < 1e-9) throw ValidationError("VPIs must have distinct chainages");
        const double endK = vpis[k].chainage + (k == 0 ? 0 : vpis[k].curveLength / 2), startN = vpis[k + 1].chainage - (k + 2 == vpis.size() ? 0 : vpis[k + 1].curveLength / 2);
        if (endK > startN + 1e-9) throw ValidationError("Vertical curves overlap between chainages " + fmt("%.3f", vpis[k].chainage) + " and " + fmt("%.3f", vpis[k + 1].chainage));
    }
    if (vpis.front().curveLength > 0 || vpis.back().curveLength > 0) throw ValidationError("The first and last VPIs cannot carry vertical curves");
    std::vector<Vec3> out;
    std::vector<double> outB;
    double s0 = 0;
    for (std::size_t i = 0; i + 1 < h.size(); ++i) {
        const double len = segPlanLength(h, b, i);
        std::vector<double> cuts = {0.0, 1.0};
        for (std::size_t k = 1; k + 1 < vpis.size(); ++k) {
            const double c = vpis[k].chainage, Lc = vpis[k].curveLength;
            auto addCut = [&](double s) {
                if (s > s0 + 1e-9 && s < s0 + len - 1e-9) cuts.push_back((s - s0) / len);
            };
            if (Lc <= 0) addCut(c);
            else {
                addCut(c - Lc / 2);
                addCut(c + Lc / 2);
                for (double s = c - Lc / 2 + step; s < c + Lc / 2 - 1e-9; s += step) addCut(s);
            }
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double c) { return std::abs(a - c) < 1e-12; }), cuts.end());
        for (std::size_t c = 0; c + 1 < cuts.size(); ++c) {
            Vec3 p = segPoint(h, b, i, cuts[c]);
            p.z = verticalElevation(vpis, s0 + cuts[c] * len);
            out.push_back(p);
            outB.push_back(subBulge(h, b, i, cuts[c], cuts[c + 1]));
        }
        s0 += len;
    }
    Vec3 last = h.back();
    last.z = verticalElevation(vpis, L);
    out.push_back(last);
    outB.push_back(0);
    auto r = finish(std::move(out), std::move(outB));
    r.notes.push_back(std::to_string(vpis.size()) + " VPIs");
    return r;
}

// ---- Generators -------------------------------------------------------------------------------------------
std::vector<LayoutResult> crosscutLayouts(const std::vector<Vec3>& parent, const std::vector<double>& pb, const CrosscutSpec& s) {
    if (parent.size() < 2) throw ValidationError("The parent centreline needs at least two points");
    if (!(s.length > 0)) throw ValidationError("Crosscut length must be positive");
    if (!(s.angleDeg > 0 && s.angleDeg < 180)) throw ValidationError("Crosscut angle must be between 0° and 180°");
    std::vector<Vec3> pts = pb.empty() ? parent : geometry::expandBulges(parent, pb, false, 0.001);
    const double L = geometry::length3d(pts);
    std::vector<double> ch = s.chainages;
    if (ch.empty()) {
        if (!(s.interval > 0)) throw ValidationError("Crosscut interval must be positive");
        for (double c = s.firstChainage; c <= L + 1e-9; c += s.interval) ch.push_back(c);
    }
    std::vector<LayoutResult> out;
    for (double c : ch) {
        if (c < -1e-9 || c > L + 1e-9) throw ValidationError("Crosscut chainage " + fmt("%.3f", c) + " is outside the parent (0–" + fmt("%.3f", L) + ")");
        const Vec3 p = *geometry::pointAtChainage(pts, std::clamp(c, 0.0, L));
        const Vec2 t = xy(*geometry::tangentAtChainage(pts, std::clamp(c, 0.0, L))).normalized();
        if (t.length() == 0) throw ValidationError("The parent is vertical at chainage " + fmt("%.3f", c));
        const double a = s.angleDeg * kPi / 180;
        auto make = [&](bool right) {
            const Vec2 d = rotate(t, right ? -a : a);
            const Vec3 st{p.x + d.x * s.startOffset, p.y + d.y * s.startOffset, p.z};
            const Vec3 en{st.x + d.x * s.length, st.y + d.y * s.length, st.z + s.gradePercent / 100 * s.length};
            auto r = finish({st, en}, {});
            r.notes.push_back("from parent chainage " + formatChainage(c));
            out.push_back(std::move(r));
        };
        if (s.side != Side::Left) make(true);
        if (s.side != Side::Right) make(false);
    }
    return out;
}

LayoutResult oreDriveLayout(const std::vector<Vec3>& contact, const OreDriveSpec& s) {
    if (contact.size() < 2) throw ValidationError("The contact string needs at least two points");
    if (!(s.offset > 0)) throw ValidationError("Offset must be positive");
    // Side point: left/right of the first segment.
    const Vec2 d = xy(contact[1] - contact[0]).normalized();
    const Vec2 n = s.side == Side::Right ? Vec2{d.y, -d.x} : Vec2{-d.y, d.x};
    const Vec3 mid = lerp(contact[0], contact[1], 0.5);
    const Vec3 side{mid.x + n.x * s.offset * 0.5, mid.y + n.y * s.offset * 0.5, mid.z};
    GeometryObject g = cad::makePolyline(contact);
    GeometryObject off;
    try {
        off = cad::offset(g, s.offset, side, cad::JoinStyle::Miter);
    } catch (const cad::EditError& e) {
        throw ValidationError(std::string("Ore drive offset failed: ") + e.what());
    }
    std::vector<Vec3> v = off.vertices;
    if (s.followContactElevation) {
        for (auto& p : v) {
            // Elevation of the nearest contact point in plan.
            double best = 1e300, z = p.z;
            for (std::size_t i = 0; i + 1 < contact.size(); ++i) {
                const Vec2 a = xy(contact[i]), c = xy(contact[i + 1]), q = xy(p);
                const Vec2 e = c - a;
                const double t = std::clamp(dot(q - a, e) / std::max(dot(e, e), 1e-300), 0.0, 1.0);
                const double dd = distance(q, a + e * t);
                if (dd < best) {
                    best = dd;
                    z = contact[i].z + (contact[i + 1].z - contact[i].z) * t;
                }
            }
            p.z = z;
        }
        return finish(std::move(v), off.bulges);
    }
    return finish(applyGrade(std::move(v), off.bulges, s.startElevation.value_or(contact.front().z), s.gradePercent), off.bulges);
}

LayoutResult accessDriveLayout(Vec3 P, double az, Vec3 T, double R, double maxGrade) {
    const Vec2 h = bearingVector(az);
    const Vec2 toT = xy(T - P);
    if (toT.length() < 1e-6) throw ValidationError("The target coincides with the start");
    const double cr = cross(h, toT);
    std::vector<Vec3> v;
    std::vector<double> b;
    double arcLen = 0;
    Vec3 Q = P;
    double sweep = 0;
    if (std::abs(cr) < 1e-9 * toT.length() && dot(h, toT) > 0) {
        // Target straight ahead: no curve.
    } else {
        if (!(R > 0)) throw ValidationError("Curve radius must be positive");
        const bool left = cr > 0;
        const Vec2 nrm = left ? Vec2{-h.y, h.x} : Vec2{h.y, -h.x};
        const Vec2 C = xy(P) + nrm * R;
        const Vec2 ct = xy(T) - C;
        const double d = ct.length();
        if (d <= R + 1e-9) throw ValidationError("The target lies inside the turning circle: use a smaller radius or move the target");
        const double beta = std::atan2(ct.y, ct.x), alpha = std::acos(R / d);
        const double phiQ = left ? beta - alpha : beta + alpha;
        const double phiP = std::atan2(P.y - C.y, P.x - C.x);
        double sw = phiQ - phiP;
        if (left)
            while (sw < 0) sw += 2 * kPi;
        else
            while (sw > 0) sw -= 2 * kPi;
        if (std::abs(sw) >= 2 * kPi - 1e-12) sw = 0;
        sweep = sw;
        arcLen = R * std::abs(sw);
        Q = {C.x + R * std::cos(phiQ), C.y + R * std::sin(phiQ), 0};
    }
    const double straight = horizontalDistance(Q, T);
    const double planL = arcLen + straight;
    const double grade = 100.0 * (T.z - P.z) / planL;
    if (std::abs(grade) > maxGrade + 1e-9) throw ValidationError("The connection needs a grade of " + fmt("%.2f", grade) + " %, above the " + fmt("%.2f", maxGrade) + " % limit");
    v.push_back(P);
    if (arcLen > 1e-9) {
        b.push_back(std::tan(sweep / 4));
        Q.z = P.z + grade / 100 * arcLen;
        v.push_back(Q);
        b.push_back(0);
    } else b.push_back(0);
    v.push_back(T);
    b.push_back(0);
    auto r = finish(std::move(v), std::move(b));
    r.notes.push_back("grade " + fmt("%.2f", grade) + " %");
    return r;
}

LayoutResult rampLayout(const RampSpec& s) {
    if (s.straightLength > 0) {
        DeclineSpec d;
        d.start = s.start;
        d.startAzimuthDeg = s.startAzimuthDeg;
        d.gradePercent = s.gradePercent;
        d.straightLength = s.straightLength;
        d.curveRadius = s.radius;
        d.turn = s.turn;
        d.targetElevation = s.targetElevation;
        return declineLayout(d);
    }
    return spiralDecline(s.start, s.startAzimuthDeg, s.gradePercent, s.radius, s.turn, s.targetElevation);
}

// ---- Project integration -----------------------------------------------------------------------------
std::vector<Uuid> createHeadings(Project& p, const std::vector<LayoutResult>& layouts, const NewDevelopment& t, const Uuid& layer, const std::string& prefix,
                                 const std::string& text) {
    if (layouts.empty()) return {};
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), text);
    std::vector<Uuid> ids;
    for (std::size_t k = 0; k < layouts.size(); ++k) {
        char seq[24];
        std::snprintf(seq, sizeof seq, "%02zu", k + 1);
        const std::string hid = layouts.size() == 1 ? prefix : prefix + seq;
        GeometryObject g;
        g.type = GeometryType::Polyline;
        g.layerId = layer;
        g.name = hid + " centreline";
        g.vertices = layouts[k].vertices;
        g.bulges = layouts[k].bulges;
        const Uuid gid = ed.addGeometry(g);
        NewDevelopment nd = t;
        nd.headingId = hid;
        if (nd.name.empty() || layouts.size() > 1) nd.name = hid;
        ids.push_back(createDevelopment(p, gid, nd));
    }
    macro.commit();
    return ids;
}

std::vector<Uuid> createCrosscuts(Project& p, const Uuid& parentId, const CrosscutSpec& spec, const Uuid& layer) {
    const auto* d = p.model().developments.find(parentId);
    if (!d) throw ValidationError("Parent development does not exist");
    const auto* g = p.model().geometry.find(d->geometryId);
    if (!g) throw ValidationError("Parent centreline does not exist");
    const auto layouts = crosscutLayouts(g->vertices, g->bulges, spec);
    NewDevelopment t;
    t.zone = d->zone;
    t.level = d->level;
    t.levelId = d->levelId;
    t.declineId = d->declineId;
    t.declineNumber = d->declineNumber;
    t.profileId = d->profileId;
    t.materialId = d->materialId;
    t.supportClassId = d->supportClassId;
    t.width = d->designWidth;
    t.height = d->designHeight;
    t.activityType = "Crosscut";
    return createHeadings(p, layouts, t, layer, d->headingId + "_XC", "Create crosscuts off " + d->headingId);
}

int assignDevelopmentsToLevels(Project& p, double tol) {
    const auto& levels = p.model().levels.all();
    if (levels.empty()) return 0;
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Assign developments to levels");
    int changed = 0;
    const auto devs = p.model().developments.all(); // copy: updates modify the store
    for (const auto& d : devs) {
        const auto pts = centrelinePoints(p, d);
        if (pts.empty()) continue;
        double z = 0;
        for (const auto& v : pts) z += v.z;
        z /= static_cast<double>(pts.size());
        const MineLevel* best = nullptr;
        for (const auto& l : levels)
            if (std::abs(l.elevation - z) <= tol && (!best || std::abs(l.elevation - z) < std::abs(best->elevation - z))) best = &l;
        if (!best || d.levelId == best->id) continue;
        MineDevelopment nd = d;
        nd.levelId = best->id;
        nd.level = best->code.empty() ? best->name : best->code;
        ed.updateDevelopment(nd, "Assign " + d.headingId + " to " + best->name);
        ++changed;
    }
    macro.commit();
    return changed;
}

} // namespace openmine::mine_design
