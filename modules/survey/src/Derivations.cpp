// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/Derivations.h"

#include "openmine/geometry/Polygon2D.h"
#include "openmine/survey/PointCloudProject.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace openmine::survey {

namespace {
std::string withEngine(std::string text, const std::string& label) { return label.empty() ? text : text + " (" + label + ")"; }

std::string fixed3(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

std::string kept(std::size_t out, std::size_t in) { return std::to_string(out) + " of " + std::to_string(in) + " points kept"; }

PointBuffer requireSome(PointBuffer out, const char* what) {
    if (out.empty()) throw PointCloudError(std::string("No points remain after the ") + what);
    return out;
}

std::string distanceStats(const std::vector<float>& d, std::string& message) {
    std::vector<double> v;
    for (float x : d)
        if (std::isfinite(x)) v.push_back(x);
    if (v.empty()) {
        message = "no point lies within the maximum distance";
        return {};
    }
    std::sort(v.begin(), v.end());
    double mean = 0, sq = 0;
    for (double x : v) mean += x;
    mean /= static_cast<double>(v.size());
    for (double x : v) sq += (x - mean) * (x - mean);
    const double sd = std::sqrt(sq / static_cast<double>(v.size()));
    const nlohmann::json j{{"count", v.size()},      {"mean", mean},        {"std", sd},
                           {"min", v.front()},       {"max", v.back()},     {"p05", v[v.size() / 20]},
                           {"median", v[v.size() / 2]}, {"p95", v[v.size() * 19 / 20]}};
    char buf[160];
    std::snprintf(buf, sizeof buf, "mean %.4f m, σ %.4f m, 95 %% within %.4f m", mean, sd, v[v.size() * 19 / 20]);
    message = buf;
    return j.dump();
}

PointBuffer labelClusters(PointBuffer& b, const std::vector<int>& labels, bool drop, std::string& message) {
    const int ci = b.scalarIndex("cluster", true);
    int clusters = 0;
    std::size_t unclustered = 0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        b.scalars[static_cast<std::size_t>(ci)][i] = static_cast<float>(labels[i]);
        clusters = std::max(clusters, labels[i] + 1);
        unclustered += labels[i] < 0 ? 1 : 0;
    }
    message = std::to_string(clusters) + " clusters; " + std::to_string(unclustered) + " points in no cluster" + (drop && unclustered ? " (dropped)" : "");
    if (!drop) return b;
    std::vector<char> keep(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) keep[i] = labels[i] >= 0 ? 1 : 0;
    return requireSome(b.filtered(keep), "segmentation");
}
} // namespace

Derivation voxelDerivation(double voxel, VoxelMode mode, const VoxelEngine& engine) {
    if (!(voxel > 0)) throw ValidationError("The voxel size must be positive");
    if (engine.function && mode != VoxelMode::Centroid) throw ValidationError(engine.label + " subsamples to voxel centroids only");
    const std::string text = withEngine((mode == VoxelMode::Centroid ? "voxel centroid " : "voxel nearest-to-centre ") + std::to_string(voxel) + " m", engine.label);
    return {text, [voxel, mode, engine](PointBuffer& b, jobs::Progress&, std::string& msg) {
                auto out = engine.function ? engine.function(b, voxel) : voxelDownsample(b, voxel, mode);
                msg = kept(out.size(), b.size());
                return out;
            }};
}

Derivation minimumSpacingDerivation(double spacing) {
    if (!(spacing > 0)) throw ValidationError("The spacing must be positive");
    return {"minimum spacing " + std::to_string(spacing) + " m", [spacing](PointBuffer& b, jobs::Progress&, std::string& msg) {
                auto out = minimumSpacing(b, spacing);
                msg = kept(out.size(), b.size());
                return out;
            }};
}

Derivation randomSubsampleDerivation(std::size_t count, unsigned seed) {
    if (count < 1) throw ValidationError("The subsample needs at least one point");
    return {"random subsample n=" + std::to_string(count), [count, seed](PointBuffer& b, jobs::Progress&, std::string& msg) {
                auto out = randomSubsample(b, count, seed);
                msg = kept(out.size(), b.size());
                return out;
            }};
}

Derivation statisticalOutlierDerivation(int k, double ratio, const OutlierEngine& engine) {
    if (k < 1 || !(ratio > 0)) throw ValidationError("Statistical outlier removal needs k ≥ 1 and a positive ratio");
    return {withEngine("statistical outlier removal k=" + std::to_string(k) + " n=" + std::to_string(ratio), engine.label),
            [k, ratio, engine](PointBuffer& b, jobs::Progress& pr, std::string& msg) {
                const auto keep = engine.function ? engine.function(b, k, ratio) : statisticalInliers(b, k, ratio, pr);
                auto out = requireSome(b.filtered(keep), "outlier removal");
                msg = std::to_string(b.size() - out.size()) + " outliers removed";
                return out;
            }};
}

Derivation radiusOutlierDerivation(double r, int minN, const RadiusOutlierEngine& engine) {
    if (!(r > 0) || minN < 1) throw ValidationError("Radius outlier removal needs a positive radius and at least one neighbour");
    return {withEngine("radius outlier removal r=" + std::to_string(r) + " min=" + std::to_string(minN), engine.label),
            [r, minN, engine](PointBuffer& b, jobs::Progress& pr, std::string& msg) {
                const auto keep = engine.function ? engine.function(b, r, minN) : radiusInliers(b, r, minN, pr);
                auto out = requireSome(b.filtered(keep), "outlier removal");
                msg = std::to_string(b.size() - out.size()) + " outliers removed";
                return out;
            }};
}

Derivation normalsDerivation(const NormalOptions& options, const NormalsEngine& engine) {
    if (options.k < 3 && !(options.radius > 0)) throw ValidationError("Normals need k ≥ 3 neighbours or a radius");
    return {withEngine("PCA normals k=" + std::to_string(options.k), engine.label), [options, engine](PointBuffer& b, jobs::Progress& pr, std::string&) {
                auto o = options;
                const bool needsViewpoint = o.orientation == NormalOptions::Orientation::TowardsViewpoint || o.orientation == NormalOptions::Orientation::AwayFromViewpoint;
                if (needsViewpoint && o.viewpoint == Vec3{}) o.viewpoint = statistics(b, 0).centroid;
                if (engine.function) engine.function(b, o, pr);
                else estimateNormals(b, o, pr);
                return b;
            }};
}

Derivation euclideanClusterDerivation(double tol, std::size_t minSize, bool drop) {
    if (!(tol > 0)) throw ValidationError("The cluster tolerance must be positive");
    return {"connected components tol=" + std::to_string(tol) + " min=" + std::to_string(minSize),
            [tol, minSize, drop](PointBuffer& b, jobs::Progress& pr, std::string& msg) {
                return labelClusters(b, euclideanClusters(b, tol, minSize, SIZE_MAX, pr), drop, msg);
            }};
}

Derivation regionGrowingDerivation(double angle, std::size_t minSize, bool drop) {
    if (!(angle > 0) || angle >= 90) throw ValidationError("The region-growing angle must be in (0°, 90°)");
    return {"region growing angle=" + std::to_string(angle) + " min=" + std::to_string(minSize), [angle, minSize, drop](PointBuffer& b, jobs::Progress& pr, std::string& msg) {
                if (!b.schema().normals) {
                    NormalOptions no;
                    no.storeCurvature = true;
                    estimateNormals(b, no, pr);
                }
                return labelClusters(b, regionGrowing(b, 16, angle, minSize, pr), drop, msg);
            }};
}

Derivation cropBoxDerivation(const BoundingBox& box, bool inside) {
    if (!(box.max.x > box.min.x && box.max.y > box.min.y && box.max.z > box.min.z)) throw ValidationError("Each maximum of the box must exceed its minimum");
    const std::string text = std::string(inside ? "box crop (inside) " : "box crop (outside) ") + fixed3(box.min.x) + "," + fixed3(box.min.y) + "," +
                             fixed3(box.min.z) + " – " + fixed3(box.max.x) + "," + fixed3(box.max.y) + "," + fixed3(box.max.z);
    return {text, [box, inside](PointBuffer& b, jobs::Progress&, std::string& msg) {
                auto keep = selectBox(b, box);
                if (!inside) keep = invert(std::move(keep));
                auto out = requireSome(b.filtered(keep), "crop");
                msg = kept(out.size(), b.size());
                return out;
            }};
}

Derivation cropPolygonDerivation(const std::vector<Vec3>& polygon, Vec3 viewNormal, bool inside) {
    if (polygon.size() < 3) throw ValidationError("The crop polygon needs at least three points");
    Vec3 n = viewNormal.length() < 1e-9 ? Vec3{0, 0, 1} : viewNormal.normalized();
    const Vec3 u = std::abs(n.z) > 0.9 ? Vec3{1, 0, 0} : Vec3{-n.y, n.x, 0}.normalized();
    const Vec3 v = cross(n, u);
    geometry::Ring ring;
    for (const auto& p : polygon) ring.push_back({dot(p, u), dot(p, v)});
    return {inside ? "crop by polygon" : "remove polygon", [ring, u, v, inside](PointBuffer& b, jobs::Progress&, std::string& msg) {
                std::vector<char> keep(b.size(), 0);
                for (std::size_t i = 0; i < b.size(); ++i) keep[i] = geometry::contains(ring, Vec2{dot(b.xyz[i], u), dot(b.xyz[i], v)}) == inside ? 1 : 0;
                auto out = requireSome(b.filtered(keep), "crop");
                msg = kept(out.size(), b.size());
                return out;
            }};
}

Derivation cloudDistanceDerivation(PointBuffer reference, const std::string& refName, double maxD, bool sign, std::shared_ptr<DistanceStatisticsJson> stats) {
    if (reference.empty()) throw PointCloudError("The reference cloud is empty");
    auto ref = std::make_shared<PointBuffer>(std::move(reference));
    return {"distance to " + refName, [ref, maxD, sign, stats](PointBuffer& b, jobs::Progress& pr, std::string& msg) {
                if (sign && !ref->schema().normals) estimateNormals(*ref);
                const auto d = cloudToCloud(b, *ref, maxD > 0 ? maxD : 1e300, sign, pr);
                const int si = b.scalarIndex("distance", true);
                b.scalars[static_cast<std::size_t>(si)] = d;
                const auto j = distanceStats(d, msg);
                if (stats) stats->json = j;
                return b;
            }};
}

Derivation meshDistanceDerivation(MeshPtr mesh, const std::string& refName, double maxD, bool sign, std::shared_ptr<DistanceStatisticsJson> stats) {
    if (!mesh || mesh->triangles.empty()) throw PointCloudError("The reference mesh has no triangles");
    return {"distance to " + refName, [mesh, maxD, sign, stats](PointBuffer& b, jobs::Progress& pr, std::string& msg) {
                const auto d = cloudToMesh(b, *mesh, sign, maxD > 0 ? maxD : 1e300, pr);
                const int si = b.scalarIndex("distance", true);
                b.scalars[static_cast<std::size_t>(si)] = d;
                const auto j = distanceStats(d, msg);
                if (stats) stats->json = j;
                return b;
            }};
}

DerivedDataset deriveDataset(Project& p, const Uuid& parentId, const Derivation& d, const std::string& name, jobs::Progress& pr) {
    const auto* parent = p.model().pointClouds.find(parentId);
    if (!parent) throw PointCloudError("The point cloud " + parentId.toString() + " does not exist");
    DerivedDataset r;
    r.derivation = d.text;
    pr.subRange(0.0, 0.3);
    auto pts = datasetPoints(p, parentId, true, std::nullopt, pr);
    r.inputCount = pts.size();
    pr.subRange(0.3, 0.8);
    auto out = d.process(pts, pr, r.message);
    if (out.empty()) throw PointCloudError("The operation produced no points");
    r.outputCount = out.size();
    pr.subRange(0.8, 1.0);
    const std::string n = name.empty() ? parent->name + " (" + d.text + ")" : name;
    auto prep = prepareDerived(p, parentId, out, n, d.text, pr);
    r.id = commitDataset(p, std::move(prep), "Derive " + n);
    return r;
}

} // namespace openmine::survey
