#pragma once
// SPDX-License-Identifier: Apache-2.0
// Out-of-core level-of-detail octree for point clouds (own format, documented in
// docs/formats/octree.md).
//
//   <folder>/octree.json        metadata: cube, schema, node hierarchy (name, count, tight bounds)
//   <folder>/nodes/<name>.bin   points owned by the node, column by column
//
// Every point is stored exactly once, bit-exact (float64 coordinates as read from the source).
// A node owns a spatially uniform sample of its region (one point per grid cell, the one nearest
// to the cell centre); the remaining points go to its eight children. Rendering a node set where
// each selected node's parent is also selected therefore shows a cloud whose density increases
// with depth, and reading all nodes returns the full cloud.
//
// Building streams the source three times (bounds, per-cell counts, distribution into chunks
// that fit in memory), so clouds larger than RAM can be converted.

#include "openmine/survey/PointBuffer.h"

#include <filesystem>
#include <functional>
#include <map>

namespace openmine::survey {

struct OctreeBuildOptions {
    std::size_t maxNodePoints = 20000;   ///< a node with more points is split
    int grid = 128;                      ///< sampling grid per node (cells per axis)
    std::size_t maxChunkPoints = 4'000'000; ///< points processed in memory at once
    int maxDepth = 24;
};

struct OctreeNodeInfo {
    std::string name;      ///< "r", "r0", "r07", ... (child digit = 4·(x half) + 2·(y half) + (z half))
    int level = 0;
    std::int64_t count = 0;
    BoundingBox cube;      ///< node cube
    BoundingBox bounds;    ///< tight bounds of the points it owns
    double spacing = 0;    ///< sampling cell size of this node
};

struct OctreeInfo {
    std::int64_t pointCount = 0;
    BoundingBox bounds;    ///< tight bounds of all points
    BoundingBox cube;      ///< root cube
    PointSchema schema;
    std::vector<OctreeNodeInfo> nodes;
};

/// Converts a source into an octree in `folder` (created; must be empty or absent).
OctreeInfo buildOctree(PointSource& source, const std::filesystem::path& folder, const OctreeBuildOptions& options = {},
                       jobs::Progress& progress = jobs::nullProgress());

class OctreeReader {
public:
    explicit OctreeReader(const std::filesystem::path& folder);
    const OctreeInfo& info() const { return info_; }
    const OctreeNodeInfo* node(const std::string& name) const;
    std::vector<const OctreeNodeInfo*> children(const std::string& name) const;
    /// Points owned by one node.
    PointBuffer readNode(const std::string& name) const;
    /// Depth-first traversal: `descend(node)` decides whether to visit its children (LOD/frustum
    /// selection for display, spatial queries). Returns the visited nodes.
    std::vector<const OctreeNodeInfo*> select(const std::function<bool(const OctreeNodeInfo&)>& descend) const;
    /// All points (optionally only those inside `box`).
    PointBuffer readAll(const std::optional<BoundingBox>& box = std::nullopt, jobs::Progress& progress = jobs::nullProgress()) const;
    /// Streaming source over all nodes.
    std::unique_ptr<PointSource> source() const;
    const std::filesystem::path& folder() const { return folder_; }

private:
    std::filesystem::path folder_;
    OctreeInfo info_;
    std::map<std::string, std::size_t> index_;
};

} // namespace openmine::survey
