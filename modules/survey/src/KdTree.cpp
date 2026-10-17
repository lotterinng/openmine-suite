// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/KdTree.h"

#include <nanoflann.hpp>

#include <cmath>

namespace openmine::survey {

namespace {
struct Adaptor {
    const std::vector<Vec3>& pts;
    std::size_t kdtree_get_point_count() const { return pts.size(); }
    double kdtree_get_pt(std::size_t i, std::size_t dim) const { return pts[i][static_cast<int>(dim)]; }
    template <class B> bool kdtree_get_bbox(B&) const { return false; }
};
using Tree = nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<double, Adaptor>, Adaptor, 3, std::size_t>;
} // namespace

struct KdTree::Impl {
    Adaptor adaptor;
    Tree tree;
    explicit Impl(const std::vector<Vec3>& p) : adaptor{p}, tree(3, adaptor, nanoflann::KDTreeSingleIndexAdaptorParams(16)) {}
};

KdTree::KdTree(const std::vector<Vec3>& points) : impl_(std::make_unique<Impl>(points)) {}
KdTree::~KdTree() = default;

std::size_t KdTree::size() const { return impl_->adaptor.pts.size(); }

std::vector<std::size_t> KdTree::knn(Vec3 q, std::size_t k, std::vector<double>* sq) const {
    k = std::min(k, size());
    std::vector<std::size_t> idx(k);
    std::vector<double> d(k);
    if (k == 0) return idx;
    const double qp[3] = {q.x, q.y, q.z};
    const auto found = impl_->tree.knnSearch(qp, k, idx.data(), d.data());
    idx.resize(found);
    d.resize(found);
    if (sq) *sq = std::move(d);
    return idx;
}

std::vector<std::size_t> KdTree::radius(Vec3 q, double r, std::vector<double>* sq) const {
    const double qp[3] = {q.x, q.y, q.z};
    std::vector<nanoflann::ResultItem<std::size_t, double>> matches;
    nanoflann::SearchParameters params;
    params.sorted = true;
    impl_->tree.radiusSearch(qp, r * r, matches, params);
    std::vector<std::size_t> idx;
    idx.reserve(matches.size());
    if (sq) sq->clear();
    for (const auto& m : matches) {
        idx.push_back(m.first);
        if (sq) sq->push_back(m.second);
    }
    return idx;
}

std::optional<std::pair<std::size_t, double>> KdTree::nearest(Vec3 q) const {
    if (size() == 0) return std::nullopt;
    std::vector<double> d;
    const auto i = knn(q, 1, &d);
    if (i.empty()) return std::nullopt;
    return std::make_pair(i[0], std::sqrt(d[0]));
}

} // namespace openmine::survey
