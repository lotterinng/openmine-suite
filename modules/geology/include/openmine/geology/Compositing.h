#pragma once
// SPDX-License-Identifier: Apache-2.0
// Downhole compositing and sample statistics.
//
// A composite's value for a field is the length-weighted mean of the sampled parts of the
// intervals it overlaps (optionally also weighted by a density field: Σ g·ρ·l / Σ ρ·l). When the
// sampled length of a field is below `minCoverage` × composite length the value is left missing
// (and the composite's coverage is reported) — unsampled core is never treated as zero unless
// `unsampledAsZero` is set. The code of a composite for each code field is the code covering the
// greatest length (its share is reported).

#include "openmine/geology/Desurvey.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openmine::geology {

enum class ResidualMode {
    Discard,        ///< drop an end piece shorter than minResidualFraction × length
    AddToPrevious,  ///< append a short end piece to the previous composite
    Distribute,     ///< adjust the composite length per run so all composites are equal
};

struct CompositeOptions {
    double length = 2.0;
    std::vector<std::string> fields;          ///< numeric fields (empty = all)
    double minCoverage = 0.5;                 ///< sampled fraction required for a value
    ResidualMode residual = ResidualMode::Distribute;
    double minResidualFraction = 0.5;
    std::string breakField;                   ///< restart compositing at changes of this code
    std::optional<std::string> onlyCode;      ///< with breakField: composite only runs with this code
    std::string densityField;                 ///< optional weighting by density
    bool unsampledAsZero = false;
    DesurveyMethod method = DesurveyMethod::MinimumCurvature;
};

struct Composite {
    std::string holeId;
    Uuid holeEntityId;
    double from = 0, to = 0;
    Vec3 start, mid, end;
    std::map<std::string, double> values;
    std::map<std::string, double> coverage;   ///< sampled fraction per field (0…1)
    std::map<std::string, std::string> codes; ///< dominant code per code field
    std::map<std::string, double> codeShare;  ///< share of the dominant code (0…1)
    double length() const { return to - from; }
};

/// Composites over explicit downhole boundaries (the core routine; others build boundaries).
std::vector<Composite> compositeIntervals(const Drillhole& hole, const std::vector<std::pair<double, double>>& spans, const CompositeOptions& o);
/// Fixed-length composites from the start of each run (whole hole, or each run of breakField).
std::vector<Composite> compositeByLength(const Drillhole& hole, const CompositeOptions& o);
/// One composite per contiguous run of the same code (e.g. lithology or domain intercepts).
std::vector<Composite> compositeByCode(const Drillhole& hole, const std::string& codeField, const CompositeOptions& o);
/// Composites between bench elevations base + k·height (desurveyed crossings).
std::vector<Composite> compositeByBench(const Drillhole& hole, double benchBase, double benchHeight, const CompositeOptions& o);

/// Codes intervals by whether their desurveyed midpoint is inside the closed mesh (split at the
/// mesh boundary first, so each interval is wholly inside or outside). Returns the new interval list.
std::vector<DrillholeInterval> codeIntervalsBySolid(const Drillhole& hole, const MeshData& closedMesh, const std::string& codeField,
                                                    const std::string& insideCode, const std::string& outsideCode = {},
                                                    DesurveyMethod method = DesurveyMethod::MinimumCurvature);

// ---- statistics ---------------------------------------------------------------------------------
struct Statistics {
    std::size_t count = 0;
    double weight = 0;
    double mean = 0, variance = 0, stdDev = 0, cv = 0;
    double min = 0, max = 0;
    double p10 = 0, p25 = 0, median = 0, p75 = 0, p90 = 0, p95 = 0, p99 = 0;
};
/// Weighted statistics (weights default to 1). Quantiles are weighted (interpolated on the
/// cumulative weight at mid-points). Variance uses the weighted population formula.
Statistics statistics(const std::vector<double>& values, const std::vector<double>& weights = {});
double weightedQuantile(std::vector<std::pair<double, double>> valueWeight, double q);

struct Histogram {
    double binStart = 0, binWidth = 1;
    std::vector<double> weights; ///< per bin
    bool logScale = false;       ///< bins in log10(value)
};
Histogram histogram(const std::vector<double>& values, const std::vector<double>& weights, int bins, bool logScale = false);

struct CapResult {
    double cap = 0;
    std::size_t capped = 0;
    double meanBefore = 0, meanAfter = 0;
    double metalRemovedFraction = 0; ///< 1 − Σ capped·w / Σ v·w
};
/// Caps (top-cuts) the field of the composites at a value; values above become the cap.
CapResult capComposites(std::vector<Composite>& composites, const std::string& field, double cap);
/// Value at the weighted percentile (0–100) of the field over the composites (weights = length).
double percentileCap(const std::vector<Composite>& composites, const std::string& field, double percentile);

std::string compositesCsv(const std::vector<Composite>& composites);

} // namespace openmine::geology
