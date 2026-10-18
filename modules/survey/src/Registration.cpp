// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/Registration.h"

#include "openmine/commands/CommandStack.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/KdTree.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/Processing.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>

namespace openmine::survey {

namespace {
Transform3 fromEigen(const Eigen::Matrix3d& R, double s, const Eigen::Vector3d& t) {
    std::array<double, 16> m{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m[static_cast<std::size_t>(r * 4 + c)] = s * R(r, c);
        m[static_cast<std::size_t>(r * 4 + 3)] = t(r);
    }
    m[15] = 1;
    return Transform3(m);
}
Eigen::Vector3d E(Vec3 v) { return {v.x, v.y, v.z}; }

double rmsOf(const std::vector<Vec3>& r) {
    if (r.empty()) return 0;
    double s = 0;
    for (const auto& v : r) s += v.lengthSquared();
    return std::sqrt(s / static_cast<double>(r.size()));
}
} // namespace

RigidFit fitTransform(const std::vector<Vec3>& src, const std::vector<Vec3>& dst, bool allowScale, const std::vector<double>* w) {
    if (src.size() != dst.size()) throw PointCloudError("Source and target need the same number of points");
    if (src.size() < 3) throw PointCloudError("At least three point pairs are needed for a registration");
    if (w && w->size() != src.size()) throw PointCloudError("One weight per pair is required");
    const std::size_t n = src.size();
    // Local origins keep the arithmetic exact at mine-grid coordinates.
    const Vec3 os = src[0], od = dst[0];
    double W = 0;
    Eigen::Vector3d ms = Eigen::Vector3d::Zero(), md = Eigen::Vector3d::Zero();
    for (std::size_t i = 0; i < n; ++i) {
        const double wi = w ? (*w)[i] : 1.0;
        if (wi < 0) throw PointCloudError("Weights must not be negative");
        W += wi;
        ms += wi * E(src[i] - os);
        md += wi * E(dst[i] - od);
    }
    if (!(W > 0)) throw PointCloudError("The weights sum to zero");
    ms /= W;
    md /= W;
    Eigen::Matrix3d S = Eigen::Matrix3d::Zero();
    double varS = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double wi = w ? (*w)[i] : 1.0;
        const Eigen::Vector3d a = E(src[i] - os) - ms, b = E(dst[i] - od) - md;
        S += wi * b * a.transpose();
        varS += wi * a.squaredNorm();
    }
    S /= W;
    varS /= W;
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(S, Eigen::ComputeFullU | Eigen::ComputeFullV);
    const auto sv = svd.singularValues();
    if (sv(1) < 1e-12 * std::max(1.0, sv(0))) throw PointCloudError("The pair points are collinear: the rotation is undetermined");
    Eigen::Matrix3d D = Eigen::Matrix3d::Identity();
    if ((svd.matrixU() * svd.matrixV().transpose()).determinant() < 0) D(2, 2) = -1;
    const Eigen::Matrix3d R = svd.matrixU() * D * svd.matrixV().transpose();
    const double s = allowScale ? (sv.asDiagonal() * D).trace() / varS : 1.0;
    // x_dst = s R (x_src − os − ms) + od + md
    const Eigen::Vector3d t = E(od) + md - s * R * (E(os) + ms);
    RigidFit f;
    f.transform = fromEigen(R, s, t);
    f.scale = s;
    for (std::size_t i = 0; i < n; ++i) f.residuals.push_back(dst[i] - f.transform.apply(src[i]));
    f.rms = rmsOf(f.residuals);
    for (const auto& r : f.residuals) f.max = std::max(f.max, r.length());
    return f;
}

IcpResult icp(const PointBuffer& source, const PointBuffer& target, const Transform3& initial, const IcpOptions& o, jobs::Progress& progress) {
    if (source.size() < 3 || target.size() < 3) throw PointCloudError("ICP needs at least three points in each cloud");
    if (!(o.maxCorrespondence > 0)) throw PointCloudError("The maximum correspondence distance must be positive");
    // Local frame at the target centroid.
    const Vec3 origin = statistics(target, 1).centroid;
    std::vector<Vec3> tgt(target.size());
    for (std::size_t i = 0; i < target.size(); ++i) tgt[i] = target.xyz[i] - origin;
    std::vector<Vec3> tn;
    if (o.mode == IcpMode::PointToPlane) {
        if (target.schema().normals) {
            tn.resize(target.size());
            for (std::size_t i = 0; i < target.size(); ++i) tn[i] = Vec3{target.normal[i][0], target.normal[i][1], target.normal[i][2]}.normalized();
        } else {
            PointBuffer t2;
            t2.xyz = tgt;
            NormalOptions no;
            no.k = o.normalNeighbours;
            no.storeCurvature = false;
            estimateNormals(t2, no);
            tn.resize(t2.size());
            for (std::size_t i = 0; i < t2.size(); ++i) tn[i] = {t2.normal[i][0], t2.normal[i][1], t2.normal[i][2]};
        }
    }
    const KdTree tree(tgt);
    // Moving sample (in local frame, after the initial transform).
    std::vector<std::size_t> idx(source.size());
    std::iota(idx.begin(), idx.end(), 0);
    if (o.sampleSize > 0 && idx.size() > o.sampleSize) {
        std::mt19937_64 rng(o.seed);
        std::shuffle(idx.begin(), idx.end(), rng);
        idx.resize(o.sampleSize);
    }
    const Transform3 toLocal = Transform3::translation(-origin), toWorld = Transform3::translation(origin);
    Transform3 T = toLocal * initial; // stored(source) → local target frame
    std::vector<Vec3> src(idx.size());
    for (std::size_t k = 0; k < idx.size(); ++k) src[k] = source.xyz[idx[k]];
    IcpResult res;
    const double maxSq = o.maxCorrespondence * o.maxCorrespondence;
    for (int it = 0; it < o.maxIterations; ++it) {
        progress.update(static_cast<double>(it) / o.maxIterations, "ICP");
        // Correspondences.
        struct Pair {
            Vec3 p, q, n;
            double d2;
        };
        std::vector<Pair> pairs;
        pairs.reserve(src.size());
        std::vector<Pair> tmp(src.size());
        std::vector<char> ok(src.size(), 0);
        jobs::parallelFor(src.size(), [&](std::size_t lo, std::size_t hi) {
            std::vector<double> d;
            for (std::size_t k = lo; k < hi; ++k) {
                const Vec3 p = T.apply(src[k]);
                const auto j = tree.knn(p, 1, &d);
                if (j.empty() || d[0] > maxSq) continue;
                tmp[k] = {p, tgt[j[0]], tn.empty() ? Vec3{} : tn[j[0]], d[0]};
                ok[k] = 1;
            }
        });
        for (std::size_t k = 0; k < src.size(); ++k)
            if (ok[k]) pairs.push_back(tmp[k]);
        if (pairs.size() < 6) throw PointCloudError("ICP found too few correspondences: improve the initial alignment or increase the correspondence distance");
        if (o.trimFraction > 0) {
            std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.d2 < b.d2; });
            pairs.resize(std::max<std::size_t>(6, static_cast<std::size_t>(static_cast<double>(pairs.size()) * (1 - o.trimFraction))));
        }
        double ss = 0;
        for (const auto& pr : pairs) ss += o.mode == IcpMode::PointToPlane ? std::pow(dot(pr.p - pr.q, pr.n), 2) : pr.d2;
        res.rmsHistory.push_back(std::sqrt(ss / static_cast<double>(pairs.size())));
        res.fitness = static_cast<double>(std::count(ok.begin(), ok.end(), 1)) / static_cast<double>(src.size());
        // Increment.
        Transform3 inc;
        if (o.mode == IcpMode::PointToPoint) {
            std::vector<Vec3> a, b;
            for (const auto& pr : pairs) {
                a.push_back(pr.p);
                b.push_back(pr.q);
            }
            inc = fitTransform(a, b, false).transform;
        } else {
            // Linearised point-to-plane: minimise Σ ((p + ω×p + t − q)·n)².
            Eigen::Matrix<double, 6, 6> A = Eigen::Matrix<double, 6, 6>::Zero();
            Eigen::Matrix<double, 6, 1> rhs = Eigen::Matrix<double, 6, 1>::Zero();
            for (const auto& pr : pairs) {
                const Eigen::Vector3d p = E(pr.p), n = E(pr.n);
                Eigen::Matrix<double, 6, 1> J;
                J.head<3>() = p.cross(n);
                J.tail<3>() = n;
                const double r = (p - E(pr.q)).dot(n);
                A += J * J.transpose();
                rhs -= J * r;
            }
            const Eigen::Matrix<double, 6, 1> x = A.ldlt().solve(rhs);
            if (!x.allFinite()) throw PointCloudError("ICP is degenerate: the overlap does not constrain all six degrees of freedom");
            const Eigen::Vector3d w = x.head<3>();
            const double ang = w.norm();
            Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
            if (ang > 0) R = Eigen::AngleAxisd(ang, w / ang).toRotationMatrix();
            inc = fromEigen(R, 1.0, x.tail<3>());
        }
        T = inc * T;
        ++res.iterations;
        const Vec3 dt = inc.translationPart();
        const double rot = std::acos(std::clamp((inc(0, 0) + inc(1, 1) + inc(2, 2) - 1) / 2, -1.0, 1.0));
        if (dt.length() < o.translationTolerance && rot < o.rotationTolerance) {
            res.converged = true;
            break;
        }
    }
    // Final statistics with the final transform.
    std::size_t matched = 0;
    double ss = 0;
    std::vector<double> d;
    for (const auto& s : src) {
        const Vec3 p = T.apply(s);
        const auto j = tree.knn(p, 1, &d);
        if (j.empty() || d[0] > maxSq) continue;
        ++matched;
        ss += o.mode == IcpMode::PointToPlane ? std::pow(dot(p - tgt[j[0]], tn[j[0]]), 2) : d[0];
    }
    res.fitness = static_cast<double>(matched) / static_cast<double>(src.size());
    res.inlierRms = matched ? std::sqrt(ss / static_cast<double>(matched)) : 0;
    res.transform = toWorld * T;
    return res;
}

// ---- Engines ---------------------------------------------------------------------------------------------
namespace {
struct EngineEntry {
    std::string name, description;
    IcpEngine run;
};
std::mutex& engineMutex() {
    static std::mutex m;
    return m;
}
std::vector<EngineEntry>& engines() {
    static std::vector<EngineEntry> e = {{"OpenMine", "OpenMine ICP",
                                          [](const PointBuffer& s, const PointBuffer& t, const Transform3& i, const IcpOptions& o, jobs::Progress& p) {
                                              return icp(s, t, i, o, p);
                                          }}};
    return e;
}
} // namespace

void registerIcpEngine(const std::string& name, IcpEngine engine, const std::string& description) {
    if (name.empty() || !engine) throw PointCloudError("An ICP engine needs a name and an implementation");
    std::lock_guard lk(engineMutex());
    for (auto& e : engines())
        if (e.name == name) {
            if (name == "OpenMine") throw PointCloudError("The built-in ICP engine cannot be replaced");
            e = {name, description.empty() ? name : description, std::move(engine)};
            return;
        }
    engines().push_back({name, description.empty() ? name : description, std::move(engine)});
}

std::vector<std::string> icpEngines() {
    std::lock_guard lk(engineMutex());
    std::vector<std::string> v;
    for (const auto& e : engines()) v.push_back(e.name);
    return v;
}

std::string icpEngineDescription(const std::string& name) {
    std::lock_guard lk(engineMutex());
    for (const auto& e : engines())
        if (e.name == name) return e.description;
    return name;
}

IcpResult runIcp(const PointBuffer& source, const PointBuffer& target, const Transform3& initial, const IcpOptions& options, jobs::Progress& progress) {
    IcpEngine run;
    {
        std::lock_guard lk(engineMutex());
        for (const auto& e : engines())
            if (e.name == (options.engine.empty() ? std::string("OpenMine") : options.engine)) run = e.run;
    }
    if (!run) throw PointCloudError("ICP engine '" + options.engine + "' is not available in this build");
    return run(source, target, initial, options, progress);
}

// ---- Project integration -------------------------------------------------------------------------------
namespace {
void evaluate(Registration& r, const Transform3& full) {
    std::vector<Vec3> fit, check;
    for (auto& p : r.pairs) {
        p.residual = p.target - full.apply(p.source);
        (p.role == PairRole::Fit ? fit : check).push_back(p.residual);
    }
    r.fitCount = static_cast<int>(fit.size());
    r.fitRms = rmsOf(fit);
    r.fitMax = 0;
    for (const auto& v : fit) r.fitMax = std::max(r.fitMax, v.length());
    r.checkCount = static_cast<int>(check.size());
    if (check.empty()) {
        r.checkRms = r.checkMax = -1;
    } else {
        r.checkRms = rmsOf(check);
        r.checkMax = 0;
        for (const auto& v : check) r.checkMax = std::max(r.checkMax, v.length());
    }
}
} // namespace

Registration computePairRegistration(const Project& p, const Uuid& id, std::vector<RegistrationPair> pairs, bool allowScale,
                                     RegistrationMethod method) {
    const auto& d = dataset(p, id);
    std::vector<Vec3> s, t;
    for (const auto& pr : pairs)
        if (pr.role == PairRole::Fit) {
            s.push_back(pr.source);
            t.push_back(pr.target);
        }
    const auto fit = fitTransform(s, t, allowScale);
    Registration r;
    r.datasetId = id;
    r.method = method;
    r.allowScale = allowScale;
    r.previousTransform = d.transform;
    const auto inv = d.transform.inverse();
    if (!inv) throw PointCloudError("Dataset transform is not invertible");
    r.transform = fit.transform * *inv; // increment applied on top of the current transform
    r.pairs = std::move(pairs);
    evaluate(r, fit.transform);
    return r;
}

Registration computeControlRegistration(const Project& p, const Uuid& id, std::vector<RegistrationPair> pairs, bool allowScale) {
    for (auto& pr : pairs) {
        const auto* c = p.model().controlPoints.find(pr.targetControlId);
        if (!c) throw PointCloudError("Control point of pair '" + pr.name + "' does not exist");
        pr.target = c->position;
        if (c->role == ControlPointRole::Check) pr.role = PairRole::Check; // check points never enter the fit
    }
    return computePairRegistration(p, id, std::move(pairs), allowScale, RegistrationMethod::ControlPoints);
}

Registration computeIcpRegistration(const Project& p, const Uuid& movingId, const Uuid& targetId, const IcpOptions& o,
                                    std::vector<RegistrationPair> checks, jobs::Progress& progress) {
    if (movingId == targetId) throw PointCloudError("A dataset cannot be registered to itself");
    const auto& d = dataset(p, movingId);
    const auto moving = datasetPoints(p, movingId, false, std::nullopt, progress); // stored coordinates
    const auto target = datasetPoints(p, targetId, true, std::nullopt, progress);
    const auto res = runIcp(moving, target, d.transform, o, progress);
    Registration r;
    r.datasetId = movingId;
    r.targetDatasetId = targetId;
    r.method = o.mode == IcpMode::PointToPlane ? RegistrationMethod::IcpPointToPlane : RegistrationMethod::IcpPointToPoint;
    r.previousTransform = d.transform;
    r.transform = res.transform * *d.transform.inverse();
    r.icpFitness = res.fitness;
    r.icpInlierRms = res.inlierRms;
    r.icpIterations = res.iterations;
    r.icpConverged = res.converged;
    r.icpMaxCorrespondence = o.maxCorrespondence;
    r.notes = "ICP engine: " + icpEngineDescription(o.engine.empty() ? "OpenMine" : o.engine);
    for (auto& c : checks) c.role = PairRole::Check;
    r.pairs = std::move(checks);
    evaluate(r, res.transform);
    return r;
}

Uuid commitRegistration(Project& p, Registration r, bool apply) {
    ProjectEditor ed(p);
    const auto& d = dataset(p, r.datasetId);
    if (!(d.transform == r.previousTransform))
        throw PointCloudError("The dataset was re-registered after this registration was computed: compute it again");
    CommandStack::MacroScope macro(p.commands(), apply ? "Apply registration" : "Store registration");
    r.applied = apply;
    const Uuid id = ed.add(r);
    if (apply) {
        PointCloudDataset nd = d;
        nd.transform = r.transform * r.previousTransform;
        ed.update(nd, "Register " + nd.name);
    }
    macro.commit();
    return id;
}

void revertRegistration(Project& p, const Uuid& rid) {
    const auto* r = p.model().registrations.find(rid);
    if (!r) throw PointCloudError("Registration does not exist");
    if (!r->applied) throw PointCloudError("The registration is not applied");
    const auto& d = dataset(p, r->datasetId);
    if (!(d.transform == r->transform * r->previousTransform))
        throw PointCloudError("A later registration is applied to this dataset: revert that one first");
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Revert registration");
    PointCloudDataset nd = d;
    nd.transform = r->previousTransform;
    ed.update(nd, "Revert registration of " + nd.name);
    // The registration record stays (audit trail) but is marked as not applied: a new record.
    Registration copy = *r;
    copy.id = Uuid{};
    copy.applied = false;
    copy.notes = "Reverted registration " + rid.toString();
    ed.add(copy);
    macro.commit();
}

} // namespace openmine::survey
