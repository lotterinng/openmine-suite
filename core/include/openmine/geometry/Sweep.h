#pragma once
// SPDX-License-Identifier: Apache-2.0
// Profile sweeps (excavation envelopes), polygon triangulation and prism/loft meshes.

#include "openmine/geometry/Polygon2D.h"
#include "openmine/objects/CadEntities.h"

#include <span>
#include <vector>

namespace openmine::geometry {

/// Ear-clipping triangulation of a simple polygon (either orientation). Returns index triples
/// into `ring`, counter-clockwise. Throws std::invalid_argument for degenerate input.
std::vector<std::array<std::uint32_t, 3>> triangulatePolygon(std::span<const Vec2> ring);

enum class SectionOrientation {
    PerpendicularToAxis, ///< section planes ⟂ the 3D centreline; volume = area × slope length
    Vertical,            ///< vertical section planes ⟂ the plan direction; volume = area × plan length
};

/// Local section frame along a centreline: origin, right (u) and up (v) axes, tangent.
struct SectionFrame {
    Vec3 origin, right, up, tangent;
    double chainage = 0;
    /// Maps profile coordinates (u, v) to world coordinates.
    Vec3 toWorld(Vec2 uv) const { return origin + right * uv.x + up * uv.y; }
    /// Maps a world point to (u, v) in the section plane (component along tangent dropped).
    Vec2 toProfile(Vec3 p) const { return {dot(p - origin, right), dot(p - origin, up)}; }
};

SectionFrame sectionFrameAt(std::span<const Vec3> centreline, double chainage, SectionOrientation o = SectionOrientation::PerpendicularToAxis);

/// Watertight excavation envelope: the profile swept along the centreline with mitred joints and
/// triangulated end caps, outward-oriented. Centreline must have ≥ 2 distinct points.
MeshData sweepProfile(std::span<const Vec3> centreline, std::span<const Vec2> profile,
                      SectionOrientation o = SectionOrientation::PerpendicularToAxis);

/// Closed prism from a plan outline between two elevations (stopes, blocks, pillars).
MeshData extrudeOutline(std::span<const Vec2> outline, double bottomZ, double topZ);

/// Closed loft between two rings with the same number of vertices (e.g. stope floor and back outlines).
MeshData loftRings(std::span<const Vec3> bottom, std::span<const Vec3> top);

} // namespace openmine::geometry
