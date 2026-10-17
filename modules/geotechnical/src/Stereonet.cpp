// SPDX-License-Identifier: Apache-2.0
#include "openmine/geotechnical/Stereonet.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <numbers>
#include <random>
#include <sstream>

namespace openmine::geotechnical {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double wrap360(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0) a += 360;
    if (a >= 360) a -= 360;
    return a;
}
/// Smallest absolute angular difference (0–180°).
double angDiff(double a, double b) {
    double d = std::fmod(std::abs(a - b), 360.0);
    return d > 180 ? 360 - d : d;
}
Vec3 unitv(Vec3 v) {
    const double n = v.length();
    return n > 0 ? v / n : Vec3{0, 0, -1};
}
std::vector<double> unitWeights(const std::vector<double>& w, std::size_t n) { return w.empty() ? std::vector<double>(n, 1.0) : w; }

Vec3 principalAxis(const std::vector<Vec3>& v, const std::vector<double>& w) {
    Eigen::Matrix3d T = Eigen::Matrix3d::Zero();
    for (std::size_t i = 0; i < v.size(); ++i) {
        const Eigen::Vector3d x(v[i].x, v[i].y, v[i].z);
        T += w[i] * x * x.transpose();
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(T);
    const Eigen::Vector3d e = es.eigenvectors().col(2);
    return unitv({e.x(), e.y(), e.z()});
}

std::string num(double v) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << v;
    return ss.str();
}
} // namespace

// ---- orientation algebra -----------------------------------------------------------------------
Vec3 lineVector(Line l) {
    const double t = l.trend * kDeg, p = l.plunge * kDeg;
    return {std::cos(p) * std::sin(t), std::cos(p) * std::cos(t), -std::sin(p)};
}

Line lineOf(Vec3 v) {
    v = unitv(v);
    if (v.z > 0) v = -v;
    const double plunge = std::asin(std::clamp(-v.z, -1.0, 1.0)) / kDeg;
    const double trend = plunge > 90 - 1e-9 ? 0.0 : wrap360(std::atan2(v.x, v.y) / kDeg);
    return {trend, plunge};
}

Line poleOf(Plane p) { return {wrap360(p.dipDirection + 180), 90 - p.dip}; }
Plane planeOfPole(Line l) { return {90 - l.plunge, wrap360(l.trend + 180)}; }
Vec3 poleVector(Plane p) { return lineVector(poleOf(p)); }

std::optional<Line> intersection(Plane a, Plane b) {
    const Vec3 c = cross(poleVector(a), poleVector(b));
    if (c.length() < 1e-9) return std::nullopt;
    return lineOf(c);
}

double apparentDip(Plane p, double dir) {
    return std::atan(std::tan(p.dip * kDeg) * std::cos((dir - p.dipDirection) * kDeg)) / kDeg;
}

double axialAngle(Vec3 a, Vec3 b) { return std::acos(std::clamp(std::abs(dot(unitv(a), unitv(b))), 0.0, 1.0)) / kDeg; }

NetPoint project(Line l, Projection proj) {
    const double theta = (90 - l.plunge) * kDeg;
    const double r = proj == Projection::EqualArea ? std::sqrt(2.0) * std::sin(theta / 2) : std::tan(theta / 2);
    return {r * std::sin(l.trend * kDeg), r * std::cos(l.trend * kDeg)};
}

Line unproject(NetPoint q, Projection proj) {
    const double r = std::min(1.0, std::hypot(q.x, q.y));
    const double theta = proj == Projection::EqualArea ? 2 * std::asin(r / std::sqrt(2.0)) : 2 * std::atan(r);
    const double plunge = 90 - theta / kDeg;
    return {r < 1e-15 ? 0.0 : wrap360(std::atan2(q.x, q.y) / kDeg), plunge};
}

std::vector<NetPoint> greatCircle(Plane p, int n, Projection proj) {
    const double s = (p.dipDirection - 90) * kDeg;
    const Vec3 strike{std::sin(s), std::cos(s), 0};
    const Vec3 d = lineVector({p.dipDirection, p.dip});
    std::vector<NetPoint> pts;
    for (int k = 0; k <= n; ++k) {
        const double a = kPi * k / n;
        pts.push_back(project(lineOf(strike * std::cos(a) + d * std::sin(a)), proj));
    }
    // lineOf folds the strike ends; keep the exact ends on the primitive.
    pts.front() = project({wrap360(p.dipDirection - 90), 0}, proj);
    pts.back() = project({wrap360(p.dipDirection + 90), 0}, proj);
    return pts;
}

std::vector<std::vector<NetPoint>> smallCircle(Line axis, double angleDeg, int n, Projection proj) {
    const Vec3 a = lineVector(axis);
    const Vec3 ref = std::abs(a.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    const Vec3 u = unitv(cross(a, ref)), w = cross(a, u);
    const double t = angleDeg * kDeg;
    std::vector<std::vector<NetPoint>> runs;
    int lastSide = 0;
    for (int k = 0; k <= n; ++k) {
        const double ph = 2 * kPi * k / n;
        const Vec3 v = a * std::cos(t) + (u * std::cos(ph) + w * std::sin(ph)) * std::sin(t);
        const int side = v.z <= 1e-12 ? -1 : 1;
        if (side != lastSide) runs.emplace_back();
        lastSide = side;
        runs.back().push_back(project(lineOf(v), proj));
    }
    return runs;
}

// ---- statistics ---------------------------------------------------------------------------------
FisherStats fisher(const std::vector<Vec3>& axes, const std::vector<double>& wIn) {
    FisherStats f;
    f.n = axes.size();
    if (axes.empty()) return f;
    const auto w = unitWeights(wIn, axes.size());
    const Vec3 e = principalAxis(axes, w);
    Vec3 sum{};
    double N = 0;
    for (std::size_t i = 0; i < axes.size(); ++i) {
        Vec3 v = unitv(axes[i]);
        if (dot(v, e) < 0) v = -v;
        sum += v * w[i];
        N += w[i];
    }
    f.resultant = sum.length();
    f.mean = lineOf(sum);
    f.sphericalVariance = N > 0 ? 1 - f.resultant / N : 0;
    f.k = N - f.resultant > 1e-12 ? (N - 1) / (N - f.resultant) : 1e12;
    if (N >= 2 && f.resultant > 0) {
        const double c = 1 - (N - f.resultant) / f.resultant * (std::pow(20.0, 1.0 / (N - 1)) - 1);
        f.alpha95 = std::acos(std::clamp(c, -1.0, 1.0)) / kDeg;
    } else f.alpha95 = kNaN;
    return f;
}

OrientationTensor orientationTensor(const std::vector<Vec3>& axes, const std::vector<double>& wIn) {
    OrientationTensor t;
    if (axes.empty()) return t;
    const auto w = unitWeights(wIn, axes.size());
    Eigen::Matrix3d T = Eigen::Matrix3d::Zero();
    double W = 0;
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const Vec3 u = unitv(axes[i]);
        const Eigen::Vector3d x(u.x, u.y, u.z);
        T += w[i] * x * x.transpose();
        W += w[i];
    }
    T /= W;
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(T);
    for (int k = 0; k < 3; ++k) {
        t.eigenvalues[static_cast<std::size_t>(k)] = std::max(0.0, es.eigenvalues()(2 - k));
        const Eigen::Vector3d e = es.eigenvectors().col(2 - k);
        t.eigenvectors[static_cast<std::size_t>(k)] = {e.x(), e.y(), e.z()};
    }
    const double l1 = std::max(t.eigenvalues[0], 1e-15), l2 = std::max(t.eigenvalues[1], 1e-15), l3 = std::max(t.eigenvalues[2], 1e-15);
    t.shapeK = std::log(l2 / l3) > 1e-15 ? std::log(l1 / l2) / std::log(l2 / l3) : std::numeric_limits<double>::infinity();
    t.strengthC = std::log(l1 / l3);
    return t;
}

double terzaghiWeight(Vec3 pole, Vec3 line, double maxWeight) {
    const double c = std::abs(dot(unitv(pole), unitv(line)));
    return 1.0 / std::max(c, 1.0 / maxWeight);
}

// ---- density ---------------------------------------------------------------------------------------
std::vector<double> densityAt(const std::vector<Vec3>& poles, const std::vector<double>& wIn, DensityMethod m, const std::vector<Vec3>& at,
                              double* coneAngle) {
    const auto w = unitWeights(wIn, poles.size());
    double W = 0;
    for (double x : w) W += x;
    std::vector<double> out(at.size(), kNaN);
    if (poles.empty() || !(W > 0)) return out;
    std::vector<Vec3> u(poles.size());
    for (std::size_t i = 0; i < poles.size(); ++i) u[i] = unitv(poles[i]);
    double area = 0.01; // fraction of the hemisphere
    double sigma = 1;
    if (m == DensityMethod::Kamb) {
        area = 9.0 / (W + 9.0); // expected count under uniformity = 3σ
        sigma = std::sqrt(W * area * (1 - area));
    }
    const double cosC = 1 - area; // cone of solid angle area·2π
    if (coneAngle) *coneAngle = std::acos(cosC) / kDeg;
    for (std::size_t q = 0; q < at.size(); ++q) {
        const Vec3 c = unitv(at[q]);
        double cnt = 0;
        for (std::size_t k = 0; k < u.size(); ++k)
            if (std::abs(dot(c, u[k])) >= cosC) cnt += w[k];
        out[q] = m == DensityMethod::Kamb ? cnt / sigma : 100.0 * cnt / W;
    }
    return out;
}

DensityGrid poleDensity(const std::vector<Vec3>& poles, const std::vector<double>& wIn, DensityMethod m, int n) {
    DensityGrid g;
    g.n = std::max(3, n);
    g.method = m;
    g.values.assign(static_cast<std::size_t>(g.n) * g.n, kNaN);
    std::vector<Vec3> at;
    std::vector<std::size_t> idx;
    for (int j = 0; j < g.n; ++j)
        for (int i = 0; i < g.n; ++i) {
            const NetPoint q = g.point(i, j);
            if (q.x * q.x + q.y * q.y > 1 + 1e-12) continue;
            at.push_back(lineVector(unproject(q)));
            idx.push_back(static_cast<std::size_t>(j * g.n + i));
        }
    const auto v = densityAt(poles, wIn, m, at, &g.coneAngleDeg);
    g.maxValue = -1;
    for (std::size_t k = 0; k < v.size(); ++k) {
        g.values[idx[k]] = v[k];
        if (v[k] > g.maxValue) {
            g.maxValue = v[k];
            g.maxAt = g.point(static_cast<int>(idx[k] % static_cast<std::size_t>(g.n)), static_cast<int>(idx[k] / static_cast<std::size_t>(g.n)));
        }
    }
    if (poles.empty()) g.maxValue = 0;
    return g;
}

std::vector<std::vector<NetPoint>> contour(const DensityGrid& g, double level) {
    const int n = g.n;
    // Crossing points keyed by grid edge: horizontal edge (i,j)-(i+1,j) = 2·(j·n+i), vertical (i,j)-(i,j+1) = +1.
    std::map<long, NetPoint> pts;
    std::vector<std::pair<long, long>> segs;
    auto edgePoint = [&](int i0, int j0, int i1, int j1) -> long {
        const long id = 2L * (static_cast<long>(j0) * n + i0) + (j1 != j0 ? 1 : 0);
        if (!pts.count(id)) {
            const double a = g.value(i0, j0), b = g.value(i1, j1);
            const double t = std::abs(b - a) > 1e-300 ? (level - a) / (b - a) : 0.5;
            const NetPoint p0 = g.point(i0, j0), p1 = g.point(i1, j1);
            pts[id] = {p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t};
        }
        return id;
    };
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const double v0 = g.value(i, j), v1 = g.value(i + 1, j), v2 = g.value(i + 1, j + 1), v3 = g.value(i, j + 1);
            if (std::isnan(v0) || std::isnan(v1) || std::isnan(v2) || std::isnan(v3)) continue;
            const int c = (v0 >= level ? 1 : 0) | (v1 >= level ? 2 : 0) | (v2 >= level ? 4 : 0) | (v3 >= level ? 8 : 0);
            if (c == 0 || c == 15) continue;
            const long eB = 0, eR = 1, eT = 2, eL = 3;
            auto E = [&](long e) {
                switch (e) {
                case 0: return edgePoint(i, j, i + 1, j);         // bottom
                case 1: return edgePoint(i + 1, j, i + 1, j + 1); // right
                case 2: return edgePoint(i, j + 1, i + 1, j + 1); // top
                default: return edgePoint(i, j, i, j + 1);        // left
                }
            };
            const double centre = 0.25 * (v0 + v1 + v2 + v3);
            switch (c) {
            case 1: case 14: segs.emplace_back(E(eL), E(eB)); break;
            case 2: case 13: segs.emplace_back(E(eB), E(eR)); break;
            case 3: case 12: segs.emplace_back(E(eL), E(eR)); break;
            case 4: case 11: segs.emplace_back(E(eR), E(eT)); break;
            case 6: case 9: segs.emplace_back(E(eB), E(eT)); break;
            case 7: case 8: segs.emplace_back(E(eL), E(eT)); break;
            case 5:
                if (centre >= level) {
                    segs.emplace_back(E(eL), E(eT));
                    segs.emplace_back(E(eB), E(eR));
                } else {
                    segs.emplace_back(E(eL), E(eB));
                    segs.emplace_back(E(eR), E(eT));
                }
                break;
            case 10:
                if (centre >= level) {
                    segs.emplace_back(E(eL), E(eB));
                    segs.emplace_back(E(eR), E(eT));
                } else {
                    segs.emplace_back(E(eL), E(eT));
                    segs.emplace_back(E(eB), E(eR));
                }
                break;
            default: break;
            }
        }
    // Join segments sharing crossing points.
    std::map<long, std::vector<std::size_t>> adj;
    for (std::size_t s = 0; s < segs.size(); ++s) {
        adj[segs[s].first].push_back(s);
        adj[segs[s].second].push_back(s);
    }
    std::vector<char> used(segs.size(), 0);
    std::vector<std::vector<NetPoint>> lines;
    auto walk = [&](long v, std::size_t s) {
        std::vector<NetPoint> l{pts[v]};
        while (true) {
            used[s] = 1;
            const long w = segs[s].first == v ? segs[s].second : segs[s].first;
            l.push_back(pts[w]);
            v = w;
            std::size_t next = segs.size();
            for (auto t : adj[v])
                if (!used[t]) next = t;
            if (next == segs.size()) break;
            s = next;
        }
        lines.push_back(std::move(l));
    };
    for (const auto& [v, list] : adj)
        if (list.size() == 1 && !used[list[0]]) walk(v, list[0]);
    for (std::size_t s = 0; s < segs.size(); ++s)
        if (!used[s]) walk(segs[s].first, s);
    return lines;
}

// ---- joint sets ----------------------------------------------------------------------------------------
namespace {
JointSetResult finishSets(const std::vector<Vec3>& poles, const std::vector<double>& w, std::vector<int> assign, int k, int iterations) {
    JointSetResult r;
    r.iterations = iterations;
    std::vector<JointSet> sets(static_cast<std::size_t>(k));
    for (std::size_t i = 0; i < poles.size(); ++i)
        if (assign[i] >= 0) sets[static_cast<std::size_t>(assign[i])].members.push_back(i);
    std::vector<int> order(static_cast<std::size_t>(k));
    for (int i = 0; i < k; ++i) order[static_cast<std::size_t>(i)] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return sets[static_cast<std::size_t>(a)].members.size() > sets[static_cast<std::size_t>(b)].members.size(); });
    std::vector<int> remap(static_cast<std::size_t>(k), -1);
    for (int rank = 0; rank < k; ++rank) {
        auto s = std::move(sets[static_cast<std::size_t>(order[static_cast<std::size_t>(rank)])]);
        if (s.members.empty()) continue;
        remap[static_cast<std::size_t>(order[static_cast<std::size_t>(rank)])] = static_cast<int>(r.sets.size());
        std::vector<Vec3> v;
        std::vector<double> ww;
        for (auto i : s.members) {
            v.push_back(poles[i]);
            ww.push_back(w[i]);
        }
        s.stats = fisher(v, ww);
        s.meanPole = s.stats.mean;
        s.meanPlane = planeOfPole(s.meanPole);
        s.name = "J" + std::to_string(r.sets.size() + 1);
        r.sets.push_back(std::move(s));
    }
    for (auto& a : assign)
        if (a >= 0) a = remap[static_cast<std::size_t>(a)];
    r.assignment = std::move(assign);
    return r;
}
} // namespace

JointSetResult identifyJointSets(const std::vector<Vec3>& polesIn, int k, double maxAngle, unsigned seed, const std::vector<double>& wIn) {
    if (k < 1) throw std::invalid_argument("At least one joint set is required");
    std::vector<Vec3> poles(polesIn.size());
    for (std::size_t i = 0; i < poles.size(); ++i) poles[i] = unitv(polesIn[i]);
    const auto w = unitWeights(wIn, poles.size());
    if (poles.empty()) return finishSets(poles, w, {}, k, 0);
    k = std::min<int>(k, static_cast<int>(poles.size()));
    std::mt19937 rng(seed);
    std::vector<Vec3> centres;
    centres.push_back(poles[std::uniform_int_distribution<std::size_t>(0, poles.size() - 1)(rng)]);
    while (static_cast<int>(centres.size()) < k) {
        std::vector<double> d2(poles.size());
        for (std::size_t i = 0; i < poles.size(); ++i) {
            double best = 1e300;
            for (const auto& c : centres) best = std::min(best, 1 - std::abs(dot(poles[i], c)));
            d2[i] = best * best * w[i];
        }
        std::discrete_distribution<std::size_t> pick(d2.begin(), d2.end());
        centres.push_back(poles[pick(rng)]);
    }
    std::vector<int> assign(poles.size(), -1);
    int it = 0;
    for (; it < 200; ++it) {
        bool changed = false;
        for (std::size_t i = 0; i < poles.size(); ++i) {
            int best = 0;
            double bd = -1;
            for (int c = 0; c < k; ++c) {
                const double d = std::abs(dot(poles[i], centres[static_cast<std::size_t>(c)]));
                if (d > bd) {
                    bd = d;
                    best = c;
                }
            }
            if (assign[i] != best) {
                assign[i] = best;
                changed = true;
            }
        }
        for (int c = 0; c < k; ++c) {
            std::vector<Vec3> v;
            std::vector<double> ww;
            for (std::size_t i = 0; i < poles.size(); ++i)
                if (assign[i] == c) {
                    v.push_back(poles[i]);
                    ww.push_back(w[i]);
                }
            if (v.empty()) {
                // Reseed an empty cluster with the pole furthest from its centre.
                std::size_t far = 0;
                double fd = 2;
                for (std::size_t i = 0; i < poles.size(); ++i) {
                    const double d = std::abs(dot(poles[i], centres[static_cast<std::size_t>(assign[i])]));
                    if (d < fd) {
                        fd = d;
                        far = i;
                    }
                }
                centres[static_cast<std::size_t>(c)] = poles[far];
                changed = true;
                continue;
            }
            centres[static_cast<std::size_t>(c)] = principalAxis(v, ww);
        }
        if (!changed) break;
    }
    if (maxAngle > 0)
        for (std::size_t i = 0; i < poles.size(); ++i)
            if (axialAngle(poles[i], centres[static_cast<std::size_t>(assign[i])]) > maxAngle) assign[i] = -1;
    return finishSets(poles, w, std::move(assign), k, it + 1);
}

JointSetResult assignToSets(const std::vector<Vec3>& poles, const std::vector<Line>& means, double cone, const std::vector<double>& wIn) {
    const auto w = unitWeights(wIn, poles.size());
    std::vector<int> assign(poles.size(), -1);
    for (std::size_t i = 0; i < poles.size(); ++i) {
        double best = 1e300;
        for (std::size_t s = 0; s < means.size(); ++s) {
            const double a = axialAngle(poles[i], lineVector(means[s]));
            if (a <= cone && a < best) {
                best = a;
                assign[i] = static_cast<int>(s);
            }
        }
    }
    // Keep the given set order (no re-ranking): build directly.
    JointSetResult r;
    r.assignment = assign;
    for (std::size_t s = 0; s < means.size(); ++s) {
        JointSet js;
        js.name = "J" + std::to_string(s + 1);
        std::vector<Vec3> v;
        std::vector<double> ww;
        for (std::size_t i = 0; i < poles.size(); ++i)
            if (assign[i] == static_cast<int>(s)) {
                js.members.push_back(i);
                v.push_back(poles[i]);
                ww.push_back(w[i]);
            }
        js.stats = fisher(v, ww);
        js.meanPole = v.empty() ? means[s] : js.stats.mean;
        js.meanPlane = planeOfPole(js.meanPole);
        r.sets.push_back(std::move(js));
    }
    return r;
}

std::vector<Vec3> measurementPoles(const std::vector<StructuralMeasurement>& m, const std::string& setName) {
    std::vector<Vec3> p;
    for (const auto& s : m)
        if (setName.empty() || s.setName == setName) p.push_back(poleVector({s.dip, s.dipDirection}));
    return p;
}

// ---- kinematics -------------------------------------------------------------------------------------------
namespace {
bool planarCritical(Plane pl, const KinematicOptions& o) {
    return pl.dip > o.frictionDeg && pl.dip < o.face.dip && angDiff(pl.dipDirection, o.face.dipDirection) <= o.planarLateralLimitDeg;
}
bool topplingCritical(Plane pl, const KinematicOptions& o) {
    return angDiff(pl.dipDirection, wrap360(o.face.dipDirection + 180)) <= o.topplingLateralLimitDeg && pl.dip >= (90 - o.face.dip) + o.frictionDeg;
}
bool wedgeCritical(Line i, const KinematicOptions& o) {
    if (angDiff(i.trend, o.face.dipDirection) >= 90) return false; // plunges into the face
    return i.plunge > o.frictionDeg && i.plunge < apparentDip(o.face, i.trend);
}
} // namespace

KinematicResult kinematicAnalysis(const std::vector<Vec3>& poles, const std::vector<Plane>& setPlanes, const KinematicOptions& o) {
    KinematicResult r;
    for (std::size_t i = 0; i < poles.size(); ++i) {
        const Plane pl = planeOfPole(lineOf(poles[i]));
        if (planarCritical(pl, o)) r.planar.critical.push_back(i);
        if (topplingCritical(pl, o)) r.toppling.critical.push_back(i);
    }
    if (!poles.empty()) {
        r.planar.percent = 100.0 * r.planar.critical.size() / poles.size();
        r.toppling.percent = 100.0 * r.toppling.critical.size() / poles.size();
    }
    for (std::size_t a = 0; a < setPlanes.size(); ++a)
        for (std::size_t b = a + 1; b < setPlanes.size(); ++b) {
            const auto i = intersection(setPlanes[a], setPlanes[b]);
            if (!i) continue;
            WedgeResult w;
            w.setA = a;
            w.setB = b;
            w.intersection = *i;
            w.critical = wedgeCritical(*i, o);
            if (w.critical) {
                // Hocking (1976): if a plane's dip direction lies between the intersection trend and
                // the face dip direction, sliding takes place on that plane alone.
                for (std::size_t k : {a, b}) {
                    const double dd = setPlanes[k].dipDirection;
                    const double span = angDiff(i->trend, o.face.dipDirection);
                    if (angDiff(dd, i->trend) <= span && angDiff(dd, o.face.dipDirection) <= span && planarCritical({setPlanes[k].dip, dd}, {o.face, o.frictionDeg, 90, 0})) {
                        w.singlePlaneSliding = true;
                        w.slidingPlane = k;
                    }
                }
            }
            r.wedges.push_back(w);
        }
    return r;
}

double wedgeIntersectionPercent(const std::vector<Vec3>& poles, const KinematicOptions& o) {
    std::size_t total = 0, crit = 0;
    for (std::size_t a = 0; a < poles.size(); ++a)
        for (std::size_t b = a + 1; b < poles.size(); ++b) {
            const Vec3 c = cross(unitv(poles[a]), unitv(poles[b]));
            if (c.length() < 1e-6) continue;
            ++total;
            if (wedgeCritical(lineOf(c), o)) ++crit;
        }
    return total ? 100.0 * crit / total : 0;
}

std::array<Plane, 2> driveWalls(double trend) { return {Plane{90, wrap360(trend + 90)}, Plane{90, wrap360(trend - 90)}}; }

// ---- SVG -------------------------------------------------------------------------------------------------
std::string stereonetSvg(const std::vector<Vec3>& poles, const std::vector<JointSet>& sets, const DensityGrid* density, const StereonetSvgOptions& o) {
    const double S = o.size, c = S / 2, R = S / 2 - 24;
    auto X = [&](NetPoint p) { return num(c + p.x * R); };
    auto Y = [&](NetPoint p) { return num(c - p.y * R); };
    auto path = [&](const std::vector<NetPoint>& pts) {
        std::string d;
        for (std::size_t i = 0; i < pts.size(); ++i) d += (i ? " L" : "M") + X(pts[i]) + " " + Y(pts[i]);
        return d;
    };
    std::ostringstream s;
    s << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << num(S) << "\" height=\"" << num(S) << "\" viewBox=\"0 0 " << num(S) << ' ' << num(S)
      << "\" font-family=\"Helvetica, Arial, sans-serif\">\n";
    s << "<title>" << (o.title.empty() ? "Stereonet" : o.title) << " (lower hemisphere, " << (o.projection == Projection::EqualArea ? "equal area" : "equal angle")
      << ")</title>\n";
    s << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";
    if (o.grid) {
        s << "<g stroke=\"#d0d0d0\" stroke-width=\"0.6\" fill=\"none\">\n";
        for (int dip = 10; dip < 90; dip += 10)
            for (double dd : {90.0, 270.0}) s << "<path d=\"" << path(greatCircle({double(dip), dd}, 90, o.projection)) << "\"/>\n";
        for (int a = 10; a < 90; a += 10)
            for (const auto& run : smallCircle({0, 0}, a, 180, o.projection))
                if (run.size() > 1) s << "<path d=\"" << path(run) << "\"/>\n";
        s << "<path d=\"M" << num(c) << ' ' << num(c - R) << " L" << num(c) << ' ' << num(c + R) << "\"/>\n</g>\n";
    }
    if (density && !o.contourLevels.empty()) {
        s << "<g stroke=\"#c0392b\" stroke-width=\"1\" fill=\"none\">\n";
        for (double lv : o.contourLevels)
            for (const auto& l : contour(*density, lv)) s << "<path data-level=\"" << num(lv) << "\" d=\"" << path(l) << "\"/>\n";
        s << "</g>\n";
    }
    if (o.frictionDeg) {
        s << "<g stroke=\"#2e86c1\" stroke-width=\"1.2\" stroke-dasharray=\"4 3\" fill=\"none\">\n";
        for (const auto& run : smallCircle({0, 90}, *o.frictionDeg, 180, o.projection)) s << "<path d=\"" << path(run) << "\"/>\n";
        s << "</g>\n";
    }
    s << "<g fill=\"#222\" stroke=\"none\">\n";
    for (const auto& p : poles) {
        const auto q = project(lineOf(p), o.projection);
        s << "<circle cx=\"" << X(q) << "\" cy=\"" << Y(q) << "\" r=\"2\"/>\n";
    }
    s << "</g>\n";
    static const char* colours[] = {"#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b"};
    for (std::size_t k = 0; k < sets.size(); ++k) {
        const auto* col = colours[k % 6];
        s << "<path fill=\"none\" stroke=\"" << col << "\" stroke-width=\"1.8\" d=\"" << path(greatCircle(sets[k].meanPlane, 90, o.projection)) << "\"/>\n";
        const auto q = project(sets[k].meanPole, o.projection);
        s << "<rect x=\"" << num(c + q.x * R - 4) << "\" y=\"" << num(c - q.y * R - 4) << "\" width=\"8\" height=\"8\" fill=\"" << col << "\"/>\n";
        s << "<text x=\"" << num(c + q.x * R + 6) << "\" y=\"" << num(c - q.y * R - 6) << "\" font-size=\"11\" fill=\"" << col << "\">" << sets[k].name << ' '
          << std::setfill('0') << std::setw(2) << static_cast<int>(std::lround(sets[k].meanPlane.dip)) << '/' << std::setw(3)
          << static_cast<int>(std::lround(sets[k].meanPlane.dipDirection)) % 360 << std::setfill(' ') << "</text>\n";
    }
    s << "<circle cx=\"" << num(c) << "\" cy=\"" << num(c) << "\" r=\"" << num(R) << "\" fill=\"none\" stroke=\"#000\" stroke-width=\"1.5\"/>\n";
    s << "<text x=\"" << num(c) << "\" y=\"" << num(c - R - 8) << "\" font-size=\"13\" text-anchor=\"middle\">N</text>\n";
    s << "<text x=\"8\" y=\"" << num(S - 8) << "\" font-size=\"10\">n = " << poles.size() << "</text>\n";
    s << "</svg>\n";
    return s.str();
}

} // namespace openmine::geotechnical
