// SPDX-License-Identifier: Apache-2.0
#include "openmine/geology/Compositing.h"

#include "openmine/geometry/MeshOps.h"
#include "openmine/util/Csv.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace openmine::geology {

namespace {
std::vector<DrillholeInterval> sorted(const Drillhole& h) {
    auto iv = h.intervals;
    std::sort(iv.begin(), iv.end(), [](const auto& a, const auto& b) { return a.from < b.from; });
    return iv;
}

std::vector<std::pair<double, double>> lengthSpans(double a, double b, const CompositeOptions& o) {
    std::vector<std::pair<double, double>> s;
    const double R = b - a;
    if (R <= 1e-9 || !(o.length > 0)) return s;
    if (o.residual == ResidualMode::Distribute) {
        const int n = std::max(1, static_cast<int>(std::lround(R / o.length)));
        const double L = R / n;
        for (int k = 0; k < n; ++k) s.emplace_back(a + k * L, k + 1 == n ? b : a + (k + 1) * L);
        return s;
    }
    const int n = static_cast<int>(std::floor(R / o.length + 1e-9));
    for (int k = 0; k < n; ++k) s.emplace_back(a + k * o.length, a + (k + 1) * o.length);
    const double start = a + n * o.length;
    const double r = b - start;
    if (r > 1e-9) {
        if (r >= o.minResidualFraction * o.length || s.empty()) s.emplace_back(start, b);
        else if (o.residual == ResidualMode::AddToPrevious) s.back().second = b;
        // Discard: the short end piece is dropped
    }
    return s;
}

/// Runs of [from, to] where the code field is constant and contiguous (no gaps); all data when
/// the field is empty.
std::vector<std::pair<std::pair<double, double>, std::string>> runs(const Drillhole& h, const std::string& field) {
    std::vector<std::pair<std::pair<double, double>, std::string>> out;
    const auto iv = sorted(h);
    if (iv.empty()) return out;
    if (field.empty()) {
        out.push_back({{iv.front().from, iv.back().to}, {}});
        return out;
    }
    for (const auto& i : iv) {
        const auto it = i.codes.find(field);
        if (it == i.codes.end()) continue;
        if (!out.empty() && out.back().second == it->second && std::abs(out.back().first.second - i.from) < 1e-9) out.back().first.second = i.to;
        else out.push_back({{i.from, i.to}, it->second});
    }
    return out;
}

std::string fmt(double v) {
    if (std::isnan(v)) return {};
    std::ostringstream ss;
    ss << std::setprecision(10) << v;
    return ss.str();
}
} // namespace

std::vector<Composite> compositeIntervals(const Drillhole& h, const std::vector<std::pair<double, double>>& spans, const CompositeOptions& o) {
    const auto iv = sorted(h);
    std::set<std::string> fields(o.fields.begin(), o.fields.end()), codeFields;
    for (const auto& i : iv) {
        if (o.fields.empty())
            for (const auto& [k, v] : i.values)
                if (k != o.densityField) fields.insert(k);
        for (const auto& [k, v] : i.codes) codeFields.insert(k);
    }
    const DesurveyedHole d(h, o.method);
    std::vector<Composite> out;
    for (const auto& [a, b] : spans) {
        if (b - a <= 1e-9) continue;
        Composite c;
        c.holeId = h.holeId;
        c.holeEntityId = h.id;
        c.from = a;
        c.to = b;
        c.start = d.at(a);
        c.mid = d.at(0.5 * (a + b));
        c.end = d.at(b);
        const double L = b - a;
        for (const auto& f : fields) {
            double sum = 0, wsum = 0, sampled = 0;
            for (const auto& i : iv) {
                const double ov = std::min(b, i.to) - std::max(a, i.from);
                if (ov <= 1e-12) continue;
                const auto it = i.values.find(f);
                if (it == i.values.end()) continue;
                double w = 1;
                if (!o.densityField.empty()) {
                    const auto di = i.values.find(o.densityField);
                    if (di == i.values.end() || !(di->second > 0)) continue; // no density → not usable for a density-weighted grade
                    w = di->second;
                }
                sum += it->second * w * ov;
                wsum += w * ov;
                sampled += ov;
            }
            double cover = sampled / L;
            if (o.unsampledAsZero && sampled < L) {
                const double meanW = sampled > 0 ? wsum / sampled : 1.0;
                wsum += (L - sampled) * meanW;
                cover = 1.0;
            }
            c.coverage[f] = std::min(1.0, cover);
            if (wsum > 0 && cover + 1e-9 >= o.minCoverage) c.values[f] = sum / wsum;
        }
        if (!o.densityField.empty()) {
            double sum = 0, sampled = 0;
            for (const auto& i : iv) {
                const double ov = std::min(b, i.to) - std::max(a, i.from);
                const auto di = i.values.find(o.densityField);
                if (ov > 1e-12 && di != i.values.end()) {
                    sum += di->second * ov;
                    sampled += ov;
                }
            }
            c.coverage[o.densityField] = sampled / L;
            if (sampled > 0 && sampled / L + 1e-9 >= o.minCoverage) c.values[o.densityField] = sum / sampled;
        }
        for (const auto& f : codeFields) {
            std::map<std::string, double> len;
            for (const auto& i : iv) {
                const double ov = std::min(b, i.to) - std::max(a, i.from);
                const auto it = i.codes.find(f);
                if (ov > 1e-12 && it != i.codes.end()) len[it->second] += ov;
            }
            if (len.empty()) continue;
            const auto best = std::max_element(len.begin(), len.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
            c.codes[f] = best->first;
            c.codeShare[f] = best->second / L;
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Composite> compositeByLength(const Drillhole& h, const CompositeOptions& o) {
    std::vector<std::pair<double, double>> spans;
    for (const auto& [r, code] : runs(h, o.breakField)) {
        if (o.onlyCode && code != *o.onlyCode) continue;
        const auto s = lengthSpans(r.first, r.second, o);
        spans.insert(spans.end(), s.begin(), s.end());
    }
    return compositeIntervals(h, spans, o);
}

std::vector<Composite> compositeByCode(const Drillhole& h, const std::string& field, const CompositeOptions& o) {
    std::vector<std::pair<double, double>> spans;
    for (const auto& [r, code] : runs(h, field))
        if (!o.onlyCode || code == *o.onlyCode) spans.push_back(r);
    return compositeIntervals(h, spans, o);
}

std::vector<Composite> compositeByBench(const Drillhole& h, double base, double height, const CompositeOptions& o) {
    const auto iv = sorted(h);
    if (iv.empty() || !(height > 0)) return {};
    const double a = iv.front().from, b = iv.back().to;
    const DesurveyedHole d(h, o.method);
    double zmin = 1e300, zmax = -1e300;
    for (const auto& p : d.trace(1.0)) {
        zmin = std::min(zmin, p.z);
        zmax = std::max(zmax, p.z);
    }
    std::vector<double> cuts{a, b};
    for (long k = static_cast<long>(std::floor((zmin - base) / height)); k <= static_cast<long>(std::ceil((zmax - base) / height)); ++k)
        for (double dd : d.depthsAtElevation(base + static_cast<double>(k) * height))
            if (dd > a && dd < b) cuts.push_back(dd);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double x, double y) { return std::abs(x - y) < 1e-6; }), cuts.end());
    std::vector<std::pair<double, double>> spans;
    for (std::size_t k = 0; k + 1 < cuts.size(); ++k) spans.emplace_back(cuts[k], cuts[k + 1]);
    return compositeIntervals(h, spans, o);
}

std::vector<DrillholeInterval> codeIntervalsBySolid(const Drillhole& h, const MeshData& mesh, const std::string& field, const std::string& inside,
                                                    const std::string& outside, DesurveyMethod method) {
    const DesurveyedHole d(h, method);
    const geometry::MeshBvh bvh(mesh);
    // Crossing depths along a fine chord approximation of the trace (chord sagitta < 1e-4 m for
    // typical curvature at 0.25 m steps).
    std::vector<double> depths;
    for (double s = 0; s < h.length; s += 0.25) depths.push_back(s);
    depths.push_back(h.length);
    std::vector<double> cuts;
    for (std::size_t k = 0; k + 1 < depths.size(); ++k) {
        const Vec3 p0 = d.at(depths[k]), p1 = d.at(depths[k + 1]);
        const double len = distance(p0, p1);
        if (len <= 0) continue;
        const Vec3 dir = (p1 - p0) / len;
        // Start slightly before the chord and end slightly after it so crossings exactly at a
        // chord end are found (duplicates are removed below).
        constexpr double eps = 1e-7;
        double t0 = -eps;
        for (int guard = 0; guard < 64; ++guard) {
            const auto hit = bvh.raycast(p0 + dir * t0, dir, len + eps - t0);
            if (!hit) break;
            const double t = t0 + hit->distance;
            cuts.push_back(depths[k] + (depths[k + 1] - depths[k]) * std::clamp(t, 0.0, len) / len);
            t0 = t + eps;
            if (t0 >= len) break;
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), cuts.end());
    std::vector<DrillholeInterval> out;
    for (const auto& i : sorted(h)) {
        std::vector<double> c{i.from};
        for (double x : cuts)
            if (x > i.from + 1e-6 && x < i.to - 1e-6) c.push_back(x);
        c.push_back(i.to);
        for (std::size_t k = 0; k + 1 < c.size(); ++k) {
            DrillholeInterval piece = i;
            piece.from = c[k];
            piece.to = c[k + 1];
            const bool in = bvh.contains(d.at(0.5 * (piece.from + piece.to)));
            if (in) piece.codes[field] = inside;
            else if (!outside.empty()) piece.codes[field] = outside;
            else piece.codes.erase(field);
            out.push_back(std::move(piece));
        }
    }
    return out;
}

double weightedQuantile(std::vector<std::pair<double, double>> vw, double q) {
    if (vw.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(vw.begin(), vw.end());
    double W = 0;
    for (const auto& [v, w] : vw) W += w;
    if (!(W > 0)) return std::numeric_limits<double>::quiet_NaN();
    // Cumulative weight at each value's mid-point (Hazen plotting position).
    double acc = 0, prevC = 0, prevV = vw.front().first;
    for (std::size_t i = 0; i < vw.size(); ++i) {
        const double c = (acc + vw[i].second / 2) / W;
        if (q <= c) {
            if (i == 0) return vw[0].first;
            const double t = c > prevC ? (q - prevC) / (c - prevC) : 0;
            return prevV + t * (vw[i].first - prevV);
        }
        acc += vw[i].second;
        prevC = c;
        prevV = vw[i].first;
    }
    return vw.back().first;
}

Statistics statistics(const std::vector<double>& values, const std::vector<double>& weights) {
    Statistics s;
    std::vector<std::pair<double, double>> vw;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double w = weights.empty() ? 1.0 : weights[i];
        if (std::isfinite(values[i]) && w > 0) vw.emplace_back(values[i], w);
    }
    s.count = vw.size();
    if (vw.empty()) return s;
    double W = 0, m = 0;
    s.min = s.max = vw.front().first;
    for (const auto& [v, w] : vw) {
        W += w;
        m += v * w;
        s.min = std::min(s.min, v);
        s.max = std::max(s.max, v);
    }
    m /= W;
    double var = 0;
    for (const auto& [v, w] : vw) var += w * (v - m) * (v - m);
    s.weight = W;
    s.mean = m;
    s.variance = var / W;
    s.stdDev = std::sqrt(s.variance);
    s.cv = m != 0 ? s.stdDev / std::abs(m) : 0;
    s.p10 = weightedQuantile(vw, 0.10);
    s.p25 = weightedQuantile(vw, 0.25);
    s.median = weightedQuantile(vw, 0.50);
    s.p75 = weightedQuantile(vw, 0.75);
    s.p90 = weightedQuantile(vw, 0.90);
    s.p95 = weightedQuantile(vw, 0.95);
    s.p99 = weightedQuantile(vw, 0.99);
    return s;
}

Histogram histogram(const std::vector<double>& values, const std::vector<double>& weights, int bins, bool logScale) {
    Histogram h;
    h.logScale = logScale;
    std::vector<std::pair<double, double>> vw;
    for (std::size_t i = 0; i < values.size(); ++i) {
        double v = values[i];
        if (!std::isfinite(v)) continue;
        if (logScale) {
            if (v <= 0) continue;
            v = std::log10(v);
        }
        vw.emplace_back(v, weights.empty() ? 1.0 : weights[i]);
    }
    if (vw.empty() || bins < 1) return h;
    double lo = vw.front().first, hi = lo;
    for (const auto& [v, w] : vw) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (hi <= lo) hi = lo + 1;
    h.binStart = lo;
    h.binWidth = (hi - lo) / bins;
    h.weights.assign(static_cast<std::size_t>(bins), 0.0);
    for (const auto& [v, w] : vw) {
        auto k = static_cast<std::size_t>(std::floor((v - lo) / h.binWidth));
        if (k >= h.weights.size()) k = h.weights.size() - 1;
        h.weights[k] += w;
    }
    return h;
}

CapResult capComposites(std::vector<Composite>& cs, const std::string& field, double cap) {
    CapResult r;
    r.cap = cap;
    double before = 0, after = 0, W = 0;
    for (auto& c : cs) {
        auto it = c.values.find(field);
        if (it == c.values.end()) continue;
        const double w = c.length();
        before += it->second * w;
        W += w;
        if (it->second > cap) {
            it->second = cap;
            ++r.capped;
        }
        after += it->second * w;
    }
    if (W > 0) {
        r.meanBefore = before / W;
        r.meanAfter = after / W;
    }
    r.metalRemovedFraction = before > 0 ? 1 - after / before : 0;
    return r;
}

double percentileCap(const std::vector<Composite>& cs, const std::string& field, double percentile) {
    std::vector<std::pair<double, double>> vw;
    for (const auto& c : cs)
        if (auto it = c.values.find(field); it != c.values.end()) vw.emplace_back(it->second, c.length());
    return weightedQuantile(std::move(vw), percentile / 100.0);
}

std::string compositesCsv(const std::vector<Composite>& cs) {
    std::set<std::string> values, codes;
    for (const auto& c : cs) {
        for (const auto& [k, v] : c.coverage) values.insert(k);
        for (const auto& [k, v] : c.codes) codes.insert(k);
    }
    std::ostringstream out;
    std::vector<std::string> head{"HOLEID", "FROM", "TO", "LENGTH", "X", "Y", "Z"};
    for (const auto& k : values) {
        head.push_back(k);
        head.push_back(k + "_COVERAGE");
    }
    for (const auto& k : codes) head.push_back(k);
    out << util::csvLine(head) << '\n';
    for (const auto& c : cs) {
        std::vector<std::string> f{c.holeId, fmt(c.from), fmt(c.to), fmt(c.length()), fmt(c.mid.x), fmt(c.mid.y), fmt(c.mid.z)};
        for (const auto& k : values) {
            auto it = c.values.find(k);
            f.push_back(it == c.values.end() ? std::string{} : fmt(it->second));
            auto cv = c.coverage.find(k);
            f.push_back(cv == c.coverage.end() ? std::string{} : fmt(cv->second));
        }
        for (const auto& k : codes) {
            auto it = c.codes.find(k);
            f.push_back(it == c.codes.end() ? std::string{} : it->second);
        }
        out << util::csvLine(f) << '\n';
    }
    return out.str();
}

} // namespace openmine::geology
