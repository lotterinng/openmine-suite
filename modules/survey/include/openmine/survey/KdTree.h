#pragma once
// SPDX-License-Identifier: Apache-2.0
// 3D k-d tree over a point array (nanoflann, BSD). The array must outlive the tree.

#include "openmine/geometry/Vec3.h"

#include <memory>
#include <optional>
#include <vector>
#include <utility>

namespace openmine::survey {

class KdTree {
public:
    explicit KdTree(const std::vector<Vec3>& points);
    ~KdTree();
    KdTree(const KdTree&) = delete;
    KdTree& operator=(const KdTree&) = delete;

    /// The k nearest points (sorted by distance); squared distances optional.
    std::vector<std::size_t> knn(Vec3 q, std::size_t k, std::vector<double>* sqDist = nullptr) const;
    /// All points within `radius` (sorted by distance).
    std::vector<std::size_t> radius(Vec3 q, double radius, std::vector<double>* sqDist = nullptr) const;
    /// Nearest point and its distance.
    std::optional<std::pair<std::size_t, double>> nearest(Vec3 q) const;
    std::size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openmine::survey
