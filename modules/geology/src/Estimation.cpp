// SPDX-License-Identifier: Apache-2.0
#include "openmine/geology/Estimation.h"

#include <Eigen/Dense>
#include <nanoflann.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <mutex>
#include <thread>
#include <tuple>

namespace openmine::geology {

namespace {
constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Adaptor {
    const std::vector<Vec3>& pts;
    std::size_t kdtree_get_point_count() const { return pts.size(); }
    double kdtree_get_pt(std::size_t i, std::size_t dim) const { return pts[i][static_cast<int>(dim)]; }
    template <class B> bool kdtree_get_bbox(B&) const { return false; }
};
using Tree = nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<double, Adaptor>, Adaptor, 3, std::size_t>;

Vec3 centroidOf(const std::vector<Vec3>& p) {
    Vec3 c{};
    for (const auto& x : p) c += x;
    return p.empty() ? c : c / static_cast<double>(p.size());
}
} // namespace

// ---- anisotropy & variogram -----------------------------------------------------------------------
std::array<Vec3, 3> Anisotropy::axes() const {
    const double a = azimuth * kDeg, d = dip * kDeg, t = tilt * kDeg;
    const Vec3 m{std::cos(d) * std::sin(a), std::cos(d) * std::cos(a), -std::sin(d)};
    const Vec3 s0{std::cos(a), -std::sin(a), 0};
    const Vec3 n0 = cross(s0, m);
    const Vec3 s = s0 * std::cos(t) + n0 * std::sin(t);
    const Vec3 n = n0 * std::cos(t) - s0 * std::sin(t);
    return {m, s, n};
}

double Anisotropy::normalised(Vec3 h) const {
    const auto ax = axes();
    const double x = dot(h, ax[0]) / major, y = dot(h, ax[1]) / semi, z = dot(h, ax[2]) / minor;
    return std::sqrt(x * x + y * y + z * z);
}

const char* toString(VariogramType t) {
    switch (t) {
    case VariogramType::Spherical: return "Spherical";
    case VariogramType::Exponential: return "Exponential";
    case VariogramType::Gaussian: return "Gaussian";
    }
    return "?";
}

double VariogramModel::unitGamma(VariogramType t, double r) {
    switch (t) {
    case VariogramType::Spherical: return r >= 1 ? 1.0 : 1.5 * r - 0.5 * r * r * r;
    case VariogramType::Exponential: return 1 - std::exp(-3 * r);
    case VariogramType::Gaussian: return 1 - std::exp(-3 * r * r);
    }
    return 1;
}

double VariogramModel::totalSill() const {
    double s = nugget;
    for (const auto& st : structures) s += st.sill;
    return s;
}

double VariogramModel::gamma(Vec3 h) const {
    if (h.lengthSquared() < 1e-24) return 0;
    double g = nugget;
    for (const auto& st : structures) g += st.sill * unitGamma(st.type, st.ranges.normalised(h));
    return g;
}

// ---- samples --------------------------------------------------------------------------------------
SampleSet samplesFromComposites(const std::vector<Composite>& cs, const std::string& field, const std::string& codeField, const std::string& code,
                                double tol) {
    SampleSet s;
    std::map<std::string, int> holes;
    std::map<std::tuple<long long, long long, long long>, std::size_t> seen;
    std::vector<int> counts;
    for (const auto& c : cs) {
        const auto v = c.values.find(field);
        if (v == c.values.end() || !std::isfinite(v->second)) continue;
        if (!codeField.empty()) {
            const auto k = c.codes.find(codeField);
            if (k == c.codes.end() || k->second != code) continue;
        }
        auto [it, inserted] = holes.emplace(c.holeId, static_cast<int>(s.groupNames.size()));
        if (inserted) s.groupNames.push_back(c.holeId);
        const auto key = std::make_tuple(std::llround(c.mid.x / tol), std::llround(c.mid.y / tol), std::llround(c.mid.z / tol));
        if (auto d = seen.find(key); d != seen.end()) {
            auto& n = counts[d->second];
            s.values[d->second] = (s.values[d->second] * n + v->second) / (n + 1);
            ++n;
            ++s.mergedDuplicates;
            continue;
        }
        seen.emplace(key, s.points.size());
        s.points.push_back(c.mid);
        s.values.push_back(v->second);
        s.group.push_back(it->second);
        counts.push_back(1);
    }
    return s;
}

std::vector<double> cellDeclusterWeights(const std::vector<Vec3>& p, double cell, int offsets) {
    std::vector<double> w(p.size(), 0.0);
    if (p.empty() || !(cell > 0)) return std::vector<double>(p.size(), 1.0);
    Vec3 lo = p.front();
    for (const auto& x : p) lo = componentMin(lo, x);
    offsets = std::max(1, offsets);
    for (int o = 0; o < offsets; ++o) {
        const double sh = cell * o / offsets;
        std::map<std::tuple<long, long, long>, int> n;
        std::vector<std::tuple<long, long, long>> key(p.size());
        for (std::size_t i = 0; i < p.size(); ++i) {
            const Vec3 q = p[i] - lo + Vec3{sh, sh, sh};
            key[i] = {static_cast<long>(std::floor(q.x / cell)), static_cast<long>(std::floor(q.y / cell)), static_cast<long>(std::floor(q.z / cell))};
            ++n[key[i]];
        }
        const double occ = static_cast<double>(n.size());
        for (std::size_t i = 0; i < p.size(); ++i) w[i] += static_cast<double>(p.size()) / (occ * n[key[i]]) / offsets;
    }
    return w;
}

// ---- experimental variogram -----------------------------------------------------------------------
std::vector<LagPoint> experimentalVariogram(const SampleSet& s, const ExperimentalOptions& o) {
    std::vector<LagPoint> out(static_cast<std::size_t>(std::max(0, o.lags)));
    if (s.points.size() < 2 || o.lags < 1 || !(o.lag > 0)) return out;
    const double tol = o.lagTolerance > 0 ? o.lagTolerance : o.lag / 2;
    const double maxD = o.lags * o.lag + tol;
    const Vec3 c0 = centroidOf(s.points);
    std::vector<Vec3> local(s.points.size());
    for (std::size_t i = 0; i < local.size(); ++i) local[i] = s.points[i] - c0;
    Adaptor ad{local};
    Tree tree(3, ad, nanoflann::KDTreeSingleIndexAdaptorParams(16));
    const bool directional = o.angleTolerance < 90;
    const Vec3 u{std::cos(o.dip * kDeg) * std::sin(o.azimuth * kDeg), std::cos(o.dip * kDeg) * std::cos(o.azimuth * kDeg), -std::sin(o.dip * kDeg)};
    const double cosTol = std::cos(o.angleTolerance * kDeg);
    std::vector<double> sumD(out.size(), 0), sumG(out.size(), 0);
    std::vector<nanoflann::ResultItem<std::size_t, double>> m;
    nanoflann::SearchParameters params;
    params.sorted = false;
    for (std::size_t i = 0; i < local.size(); ++i) {
        const double q[3] = {local[i].x, local[i].y, local[i].z};
        m.clear();
        tree.radiusSearch(q, maxD * maxD, m, params);
        for (const auto& r : m) {
            const std::size_t j = r.first;
            if (j <= i) continue;
            const Vec3 h = local[j] - local[i];
            const double d = std::sqrt(r.second);
            if (d <= 0) continue;
            if (directional) {
                const double along = dot(h, u);
                if (std::abs(along) / d < cosTol) continue;
                const double perp = (h - u * along).length();
                if (perp > o.bandwidth) continue;
            }
            const auto k = static_cast<long>(std::lround(d / o.lag));
            if (k < 1 || k > o.lags || std::abs(d - k * o.lag) > tol) continue;
            const auto b = static_cast<std::size_t>(k - 1);
            double g;
            const double zi = s.values[i], zj = s.values[j];
            if (o.kind == VariogramKind::PairwiseRelative) {
                const double mean = 0.5 * (zi + zj);
                if (!(mean > 0)) continue;
                g = (zi - zj) / mean;
                g *= g;
            } else g = (zi - zj) * (zi - zj);
            sumD[b] += d;
            sumG[b] += g;
            ++out[b].pairs;
        }
    }
    for (std::size_t b = 0; b < out.size(); ++b) {
        if (out[b].pairs) {
            out[b].distance = sumD[b] / out[b].pairs;
            out[b].gamma = sumG[b] / (2.0 * out[b].pairs);
        } else out[b].distance = (b + 1) * o.lag;
    }
    return out;
}

VariogramFit fitVariogram(const std::vector<LagPoint>& lags, const VariogramModel& init, Vec3 direction, bool fixNugget) {
    VariogramFit f;
    f.model = init;
    std::vector<const LagPoint*> pts;
    for (const auto& l : lags)
        if (l.pairs > 0) pts.push_back(&l);
    if (pts.empty() || init.structures.empty()) return f;
    direction = direction.normalized();
    double gmax = 0;
    for (auto* p : pts) gmax = std::max(gmax, p->gamma);
    const double floorG = std::max(1e-12, 1e-6 * gmax);
    // Parameters (log space): [nugget], per structure sill and range scale.
    const int ns = static_cast<int>(init.structures.size());
    const int np = (fixNugget ? 0 : 1) + 2 * ns;
    Eigen::VectorXd th(np);
    int k = 0;
    if (!fixNugget) th(k++) = std::log(std::max(init.nugget, floorG));
    for (const auto& s : init.structures) {
        th(k++) = std::log(std::max(s.sill, floorG));
        th(k++) = 0.0; // scale factor 1
    }
    auto build = [&](const Eigen::VectorXd& t) {
        VariogramModel m = init;
        int q = 0;
        if (!fixNugget) m.nugget = std::exp(t(q++));
        for (auto& s : m.structures) {
            s.sill = std::exp(t(q++));
            const double sc = std::exp(t(q++));
            s.ranges.major *= sc;
            s.ranges.semi *= sc;
            s.ranges.minor *= sc;
        }
        return m;
    };
    auto residuals = [&](const Eigen::VectorXd& t, const VariogramModel& weightsModel) {
        const VariogramModel m = build(t);
        Eigen::VectorXd r(static_cast<Eigen::Index>(pts.size()));
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const Vec3 h = direction * pts[i]->distance;
            const double gm = std::max(weightsModel.gamma(h), floorG);
            const double w = static_cast<double>(pts[i]->pairs) / (gm * gm);
            r(static_cast<Eigen::Index>(i)) = std::sqrt(w) * (pts[i]->gamma - m.gamma(h));
        }
        return r;
    };
    double lambda = 1e-3;
    VariogramModel wm = build(th);
    Eigen::VectorXd r = residuals(th, wm);
    double sse = r.squaredNorm();
    for (f.iterations = 0; f.iterations < 200; ++f.iterations) {
        wm = build(th); // Cressie weights from the current model
        r = residuals(th, wm);
        sse = r.squaredNorm();
        Eigen::MatrixXd J(r.size(), np);
        for (int j = 0; j < np; ++j) {
            Eigen::VectorXd t2 = th;
            const double eps = 1e-6 * std::max(1.0, std::abs(th(j)));
            t2(j) += eps;
            J.col(j) = (residuals(t2, wm) - r) / eps;
        }
        const Eigen::MatrixXd A = J.transpose() * J;
        const Eigen::VectorXd g = J.transpose() * r;
        bool improved = false;
        for (int tries = 0; tries < 20; ++tries) {
            Eigen::MatrixXd Ad = A;
            for (int j = 0; j < np; ++j) Ad(j, j) += lambda * std::max(A(j, j), 1e-12);
            const Eigen::VectorXd step = Ad.ldlt().solve(-g);
            Eigen::VectorXd t2 = th + step;
            for (int j = 0; j < np; ++j) t2(j) = std::clamp(t2(j), -50.0, 50.0);
            const double s2 = residuals(t2, wm).squaredNorm();
            if (s2 < sse) {
                th = t2;
                lambda = std::max(lambda / 3, 1e-12);
                improved = true;
                if ((sse - s2) < 1e-12 * std::max(1.0, sse) || step.norm() < 1e-10) f.converged = true;
                sse = s2;
                break;
            }
            lambda *= 4;
        }
        if (!improved) {
            f.converged = true;
            break;
        }
        if (f.converged) break;
    }
    f.model = build(th);
    f.weightedSse = sse;
    return f;
}

// ---- estimation engine ----------------------------------------------------------------------------
const char* toString(EstimationMethod m) {
    switch (m) {
    case EstimationMethod::NearestNeighbour: return "Nearest neighbour";
    case EstimationMethod::InverseDistance: return "Inverse distance";
    case EstimationMethod::OrdinaryKriging: return "Ordinary kriging";
    case EstimationMethod::SimpleKriging: return "Simple kriging";
    }
    return "?";
}

namespace {
struct Result {
    double value = kNaN, kv = kNaN, slope = kNaN, negW = kNaN, dist = kNaN;
    int n = 0, pass = 0;
    bool singular = false;
};

class Engine {
public:
    Engine(const SampleSet& s, const EstimationOptions& o) : s_(s), o_(o) {
        if (o.passes.empty()) throw ValidationError("At least one search pass is required");
        for (const auto& p : o.passes) {
            if (!(p.ellipse.major > 0 && p.ellipse.semi > 0 && p.ellipse.minor > 0)) throw ValidationError("Search radii must be positive");
            if (p.minSamples < 1 || p.maxSamples < p.minSamples) throw ValidationError("Search needs 1 ≤ min samples ≤ max samples");
        }
        if ((o.method == EstimationMethod::OrdinaryKriging || o.method == EstimationMethod::SimpleKriging) && !(o.variogram.totalSill() > 0))
            throw ValidationError("Kriging needs a variogram with a positive sill");
        if (o.method == EstimationMethod::InverseDistance && !(o.power >= 0)) throw ValidationError("IDW power must be ≥ 0");
        c0_ = centroidOf(s.points);
        for (const auto& p : o.passes) {
            auto& ps = passes_.emplace_back();
            ps.axes = p.ellipse.axes();
            ps.scale = {p.ellipse.major, p.ellipse.semi, p.ellipse.minor};
            ps.pts.resize(s.points.size());
            for (std::size_t i = 0; i < s.points.size(); ++i) ps.pts[i] = toEllipse(ps, s.points[i]);
        }
        for (auto& ps : passes_) {
            ps.adaptor = std::make_unique<Adaptor>(Adaptor{ps.pts});
            ps.tree = std::make_unique<Tree>(3, *ps.adaptor, nanoflann::KDTreeSingleIndexAdaptorParams(16));
        }
    }

    /// Estimate for a support given by its discretisation points (centre = first argument).
    template <class Exclude> Result run(Vec3 centre, const std::vector<Vec3>& disc, Exclude&& excluded) const {
        Result res;
        std::vector<nanoflann::ResultItem<std::size_t, double>> m;
        nanoflann::SearchParameters params;
        params.sorted = true;
        for (std::size_t pi = 0; pi < passes_.size(); ++pi) {
            const auto& ps = passes_[pi];
            const auto& sp = o_.passes[pi];
            const Vec3 q = toEllipse(ps, centre);
            const double qq[3] = {q.x, q.y, q.z};
            m.clear();
            ps.tree->radiusSearch(qq, 1.0 + 1e-12, m, params);
            std::vector<std::size_t> sel;
            std::map<int, int> perHole;
            int octant[8] = {};
            for (const auto& r : m) {
                const auto j = r.first;
                if (excluded(j)) continue;
                if (sp.maxPerHole > 0 && perHole[s_.group[j]] >= sp.maxPerHole) continue;
                if (sp.maxPerOctant > 0) {
                    const Vec3 d = ps.pts[j] - q;
                    const int oc = (d.x >= 0 ? 1 : 0) | (d.y >= 0 ? 2 : 0) | (d.z >= 0 ? 4 : 0);
                    if (octant[oc] >= sp.maxPerOctant) continue;
                    ++octant[oc];
                }
                ++perHole[s_.group[j]];
                sel.push_back(j);
                if (static_cast<int>(sel.size()) >= sp.maxSamples) break;
            }
            if (static_cast<int>(sel.size()) < sp.minSamples) continue;
            res.pass = static_cast<int>(pi) + 1;
            res.n = static_cast<int>(sel.size());
            double sd = 0;
            for (auto j : sel) sd += distance(s_.points[j], centre);
            res.dist = sd / sel.size();
            compute(res, sel, disc, ps, o_.passes[pi]);
            return res;
        }
        return res;
    }

private:
    struct Pass {
        std::array<Vec3, 3> axes;
        Vec3 scale;
        std::vector<Vec3> pts;
        std::unique_ptr<Adaptor> adaptor;
        std::unique_ptr<Tree> tree;
    };
    Vec3 toEllipse(const Pass& ps, Vec3 p) const {
        const Vec3 d = p - c0_;
        return {dot(d, ps.axes[0]) / ps.scale.x, dot(d, ps.axes[1]) / ps.scale.y, dot(d, ps.axes[2]) / ps.scale.z};
    }

    void compute(Result& res, const std::vector<std::size_t>& sel, const std::vector<Vec3>& disc, const Pass& ps, const SearchPass& sp) const {
        (void)sp;
        const auto n = sel.size();
        switch (o_.method) {
        case EstimationMethod::NearestNeighbour: {
            // Nearest by anisotropic distance to the centre of the support (the first selected).
            res.value = s_.values[sel.front()];
            return;
        }
        case EstimationMethod::InverseDistance: {
            double total = 0;
            for (const auto& p : disc) {
                const Vec3 q = toEllipse(ps, p);
                double sw = 0, swz = 0;
                bool exact = false;
                for (auto j : sel) {
                    const double d = (ps.pts[j] - q).length() * ps.scale.x; // anisotropic distance in major-axis metres
                    if (d < 1e-9) {
                        swz = s_.values[j];
                        sw = 1;
                        exact = true;
                        break;
                    }
                    const double w = 1.0 / std::pow(d, o_.power);
                    sw += w;
                    swz += w * s_.values[j];
                }
                total += exact ? swz : swz / sw;
            }
            res.value = total / disc.size();
            return;
        }
        case EstimationMethod::OrdinaryKriging:
        case EstimationMethod::SimpleKriging: break;
        }
        const auto& vg = o_.variogram;
        const bool ok = o_.method == EstimationMethod::OrdinaryKriging;
        const Eigen::Index N = static_cast<Eigen::Index>(n) + (ok ? 1 : 0);
        Eigen::MatrixXd A(N, N);
        Eigen::VectorXd b(N);
        const double c00 = vg.totalSill();
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i; j < n; ++j) {
                const double c = i == j ? c00 : vg.covariance(s_.points[sel[j]] - s_.points[sel[i]]);
                A(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = A(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i)) = c;
            }
            double cb = 0;
            for (const auto& p : disc) cb += vg.covariance(p - s_.points[sel[i]]);
            b(static_cast<Eigen::Index>(i)) = cb / disc.size();
        }
        if (ok) {
            for (Eigen::Index i = 0; i < N - 1; ++i) A(i, N - 1) = A(N - 1, i) = 1.0;
            A(N - 1, N - 1) = 0;
            b(N - 1) = 1;
        }
        // Average covariance within the support (GSLib: the nugget enters only for coincident points).
        double cbb = 0;
        for (std::size_t i = 0; i < disc.size(); ++i)
            for (std::size_t j = 0; j < disc.size(); ++j) cbb += i == j ? c00 : vg.covariance(disc[j] - disc[i]);
        cbb /= static_cast<double>(disc.size() * disc.size());
        const Eigen::FullPivLU<Eigen::MatrixXd> lu(A);
        if (lu.rank() < N) {
            res.singular = true;
            res.value = kNaN;
            return;
        }
        const Eigen::VectorXd x = lu.solve(b);
        double est = 0, lc = 0, neg = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double l = x(static_cast<Eigen::Index>(i));
            const double z = ok ? s_.values[sel[i]] : s_.values[sel[i]] - o_.simpleMean;
            est += l * z;
            lc += l * b(static_cast<Eigen::Index>(i));
            if (l < 0) neg += l;
        }
        res.negW = neg;
        if (ok) {
            const double mu = x(N - 1);
            res.value = est;
            res.kv = cbb - lc - mu;
            const double d = cbb - res.kv;
            res.slope = (d + 2 * std::abs(mu)) > 0 ? (d + std::abs(mu)) / (d + 2 * std::abs(mu)) : kNaN;
        } else {
            res.value = o_.simpleMean + est;
            res.kv = cbb - lc;
        }
        if (res.kv < 0 && res.kv > -1e-9 * c00) res.kv = 0;
    }

    const SampleSet& s_;
    const EstimationOptions& o_;
    Vec3 c0_;
    std::vector<Pass> passes_;
};

std::vector<Vec3> discretise(const BlockGrid& g, std::size_t idx, std::array<int, 3> d) {
    const auto& m = g.model();
    const auto c = g.ijk(idx);
    std::vector<Vec3> pts;
    for (int a = 0; a < std::max(1, d[0]); ++a)
        for (int b = 0; b < std::max(1, d[1]); ++b)
            for (int e = 0; e < std::max(1, d[2]); ++e)
                pts.push_back(g.toWorld({(c[0] + (a + 0.5) / std::max(1, d[0])) * m.blockSize.x, (c[1] + (b + 0.5) / std::max(1, d[1])) * m.blockSize.y,
                                         (c[2] + (e + 0.5) / std::max(1, d[2])) * m.blockSize.z}));
    return pts;
}

template <class F> void parallelFor(std::size_t n, int threads, jobs::Progress& progress, const std::string& stage, F&& f) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned T = threads > 0 ? static_cast<unsigned>(threads) : hw;
    std::atomic<std::size_t> next{0}, done{0};
    std::atomic<bool> stop{false};
    std::exception_ptr error;
    std::mutex em;
    auto worker = [&](unsigned id) {
        try {
            while (!stop) {
                const std::size_t start = next.fetch_add(256);
                if (start >= n) break;
                const std::size_t end = std::min(n, start + 256);
                for (std::size_t i = start; i < end; ++i) f(i);
                done += end - start;
                if (progress.isCancelled()) stop = true;
                if (id == 0) progress.update(static_cast<double>(done) / n, stage);
            }
        } catch (...) {
            std::lock_guard lk(em);
            if (!error) error = std::current_exception();
            stop = true;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < T; ++t) pool.emplace_back(worker, t);
    worker(0);
    for (auto& t : pool) t.join();
    if (error) std::rethrow_exception(error);
    progress.checkCancelled();
    progress.update(1.0, stage);
}
} // namespace

EstimationSummary estimate(BlockModelData& d, const SampleSet& s, const EstimationOptions& o, jobs::Progress& progress) {
    if (!isValidAttributeKey(o.outputField)) throw ValidationError("Invalid output attribute name '" + o.outputField + "'");
    const Engine eng(s, o);
    const BlockGrid g(d.header);
    const auto* mask = o.maskField.empty() ? nullptr : d.find(o.maskField);
    if (!o.maskField.empty() && !mask) throw ValidationError("Block model has no attribute '" + o.maskField + "'");
    EstimationSummary sum;
    sum.perPass.assign(o.passes.size(), 0);
    std::vector<std::size_t> targets;
    for (std::size_t i = 0; i < d.count(); ++i) {
        if (mask && !((*mask)[i] > 0)) {
            ++sum.skippedMask;
            continue;
        }
        targets.push_back(i);
    }
    std::vector<Result> res(targets.size());
    auto none = [](std::size_t) { return false; };
    parallelFor(targets.size(), o.threads, progress, std::string("Estimating (") + toString(o.method) + ")", [&](std::size_t t) {
        const auto idx = targets[t];
        res[t] = eng.run(g.centroid(idx), discretise(g, idx, o.discretisation), none);
    });
    // Write only after a complete, uncancelled run.
    auto& out = d.attribute(o.outputField);
    std::vector<double>*ns = nullptr, *dist = nullptr, *pass = nullptr, *kv = nullptr, *slope = nullptr, *negw = nullptr;
    const bool kriging = o.method == EstimationMethod::OrdinaryKriging || o.method == EstimationMethod::SimpleKriging;
    if (o.diagnostics) {
        ns = &d.attribute(o.outputField + "_NS");
        dist = &d.attribute(o.outputField + "_DIST");
        pass = &d.attribute(o.outputField + "_PASS");
        if (kriging) {
            kv = &d.attribute(o.outputField + "_KV");
            negw = &d.attribute(o.outputField + "_NEGW");
            if (o.method == EstimationMethod::OrdinaryKriging) slope = &d.attribute(o.outputField + "_SLOPE");
        }
    }
    for (std::size_t t = 0; t < targets.size(); ++t) {
        const auto idx = targets[t];
        const auto& r = res[t];
        out[idx] = r.value;
        if (r.singular) ++sum.singularSystems;
        if (std::isnan(r.value)) {
            if (!r.singular) ++sum.notEstimated;
        } else {
            ++sum.estimated;
            ++sum.perPass[static_cast<std::size_t>(r.pass - 1)];
            if (r.negW < 0) ++sum.negativeWeightBlocks;
        }
        if (ns) {
            (*ns)[idx] = r.n ? r.n : kNaN;
            (*dist)[idx] = r.dist;
            (*pass)[idx] = r.pass ? r.pass : kNaN;
        }
        if (kv) (*kv)[idx] = r.kv;
        if (negw) (*negw)[idx] = r.negW;
        if (slope) (*slope)[idx] = r.slope;
    }
    return sum;
}

std::vector<double> estimateAt(const std::vector<Vec3>& pts, const SampleSet& s, const EstimationOptions& o, std::vector<double>* kvOut) {
    const Engine eng(s, o);
    std::vector<double> v(pts.size(), kNaN);
    if (kvOut) kvOut->assign(pts.size(), kNaN);
    auto none = [](std::size_t) { return false; };
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const auto r = eng.run(pts[i], {pts[i]}, none);
        v[i] = r.value;
        if (kvOut) (*kvOut)[i] = r.kv;
    }
    return v;
}

CrossValidation crossValidate(const SampleSet& s, const EstimationOptions& o, bool excludeHole) {
    const Engine eng(s, o);
    CrossValidation cv;
    cv.estimates.assign(s.points.size(), kNaN);
    cv.errors.assign(s.points.size(), kNaN);
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        auto ex = [&](std::size_t j) { return excludeHole ? s.group[j] == s.group[i] : j == i; };
        const auto r = eng.run(s.points[i], {s.points[i]}, ex);
        cv.estimates[i] = r.value;
        if (!std::isnan(r.value)) cv.errors[i] = r.value - s.values[i];
    }
    double se = 0, sse = 0, mx = 0, my = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        if (std::isnan(cv.errors[i])) continue;
        se += cv.errors[i];
        sse += cv.errors[i] * cv.errors[i];
        mx += cv.estimates[i];
        my += s.values[i];
        ++n;
    }
    cv.estimated = n;
    if (n == 0) return cv;
    cv.meanError = se / n;
    cv.meanSquaredError = sse / n;
    mx /= n;
    my /= n;
    double sxy = 0, sxx = 0, syy = 0;
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        if (std::isnan(cv.errors[i])) continue;
        const double dx = cv.estimates[i] - mx, dy = s.values[i] - my;
        sxy += dx * dy;
        sxx += dx * dx;
        syy += dy * dy;
    }
    cv.correlation = sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : 0;
    cv.slope = sxx > 0 ? sxy / sxx : 0;
    return cv;
}

} // namespace openmine::geology
