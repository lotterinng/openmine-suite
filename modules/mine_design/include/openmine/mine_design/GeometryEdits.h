#pragma once
// SPDX-License-Identifier: Apache-2.0
// Project-level CAD edits, shared by the desktop tools, the command console and the Python API.
// They apply the pure algorithms of modules/cad (Edit.h) to project objects through ProjectEditor:
// each call is validated and one undo step, and changes of a heading's centreline go through
// updateGeometryWithImpact, so linked schedule quantities are flagged — never silently changed.
// The first piece of a trimmed or split object keeps its identity (and so any development link).
// Solids are not handled here (their B-rep needs Open CASCADE: occt::transformSolidObject).

#include "openmine/geometry/Transform.h"
#include "openmine/project/Project.h"

#include <string>
#include <vector>

namespace openmine::mine_design {

struct EditResult {
    std::vector<Uuid> changed;        ///< objects modified in place
    std::vector<Uuid> created;        ///< new objects
    std::vector<Uuid> removed;        ///< objects deleted
    std::vector<std::string> notes;   ///< objects skipped and why
    int flaggedActivities = 0;        ///< linked schedule activities flagged for review
};

/// Moves grip `grip` (cad::grips order) of an object to `to` — a vertex, an arc's centre, radius or
/// end. One undo step; a heading centreline's linked activities are flagged.
EditResult moveGrip(Project& project, const Uuid& id, std::size_t grip, Vec3 to);

/// Moves/rotates/scales/mirrors curves, points, meshes and annotations (or copies them with
/// `copy`). Throws ValidationError for solids.
EditResult transformObjects(Project& project, const std::vector<Uuid>& ids, const Transform3& t, bool copy, const std::string& text);

/// Parallel copy of a curve at `distance` towards `side` (plan), on the source's layer.
Uuid offsetObject(Project& project, const Uuid& id, double distance, Vec3 side, bool roundJoins = false);

/// Trims `target` at its intersections with `cuttingEdges` (empty = every curve on a visible
/// layer), removing the part that contains `pick`.
EditResult trimObject(Project& project, const Uuid& target, const std::vector<Uuid>& cuttingEdges, Vec3 pick);
/// Extends the end of `target` nearest `pick` to the nearest boundary (empty = visible curves).
EditResult extendObject(Project& project, const Uuid& target, const std::vector<Uuid>& boundaries, Vec3 pick);
/// Splits a curve at the point nearest `at`.
EditResult splitObject(Project& project, const Uuid& id, Vec3 at);
/// Joins curves whose ends meet within `tolerance` into polylines; heading centrelines are kept
/// (noted). Throws ValidationError when nothing joins.
EditResult joinObjects(Project& project, const std::vector<Uuid>& ids, double tolerance = 1e-4);
/// Explodes polylines, hatches, dimensions and splines into their parts; centrelines are kept.
EditResult explodeObjects(Project& project, const std::vector<Uuid>& ids);
/// Fillet of two curves picked near their corner (a == b: the polyline vertex nearest `pickB`).
EditResult filletObjects(Project& project, const Uuid& a, Vec3 pickA, const Uuid& b, Vec3 pickB, double radius);
/// Chamfer of two different curves with distances d1 (on a) and d2 (on b).
EditResult chamferObjects(Project& project, const Uuid& a, Vec3 pickA, const Uuid& b, Vec3 pickB, double d1, double d2);

/// Section of a mesh object by a plane: one polyline per section chain (closed loops closed), named
/// "<mesh> section", on `layerId` (one undo step). Empty when the plane misses the mesh.
std::vector<Uuid> sectionMeshObject(Project& project, const Uuid& meshId, Vec3 planePoint, Vec3 planeNormal, const Uuid& layerId);

} // namespace openmine::mine_design
