#pragma once
// SPDX-License-Identifier: Apache-2.0
// Project-level solid modelling, shared by the desktop dialogs, the command console and the Python
// API: solids made from the project's curves (extrusion with holes, revolution, sweeps of tunnel
// profiles, lofts), solid checks and healing. Every call is validated and one undo step; results
// are exact B-rep Solid objects (immutable BREP assets with a display tessellation).

#include "openmine/geometry/Profile.h"
#include "openmine/occt/Solid.h"
#include "openmine/project/Project.h"

#include <string>
#include <vector>

namespace openmine::occt {

struct ExtrusionSpec {
    enum class Direction {
        Vertical, ///< along +Z by `distance`
        Normal,   ///< along each curve's plane normal by `distance`
        Vector,   ///< along `vector`
    } direction = Direction::Vertical;
    double distance = 5.0;
    Vec3 vector{0, 0, 5};
    bool symmetric = false; ///< half the extrusion on each side of the curve's plane
    bool holes = true;      ///< closed curves inside an outer curve (same plane) become holes
};

/// Extrudes closed planar curves (closed polylines, circles, closed splines) into solids: one solid
/// per outer curve (largest first), with the curves inside it as holes. Returns the new solids.
std::vector<Uuid> extrudeCurves(Project& project, const std::vector<Uuid>& curves, const ExtrusionSpec& spec, const std::string& name,
                                const Uuid& layerId);
/// Revolves closed planar curves about an axis by `angleRad` (2π: a full solid of revolution).
std::vector<Uuid> revolveCurves(Project& project, const std::vector<Uuid>& profiles, Vec3 axisPoint, Vec3 axisDirection, double angleRad,
                                const std::string& name, const Uuid& layerId);
/// Sweeps a profile (u right of travel, v up; arcs as bulges) along each path curve.
std::vector<Uuid> sweepCurves(Project& project, const std::vector<Uuid>& paths, const geometry::ProfileCurve& profile, SweepTransition transition,
                              const std::string& name, const Uuid& layerId);
/// Lofts through two or more closed sections, in order (a solid, or a surface shell).
Uuid loftCurves(Project& project, const std::vector<Uuid>& sections, bool solid, bool ruled, const std::string& name, const Uuid& layerId);

/// Analysis (validity, closure, volume, area, topology, centre of mass) of a Solid object.
ShapeInfo analyseSolidObject(const Project& project, const Uuid& solidId);

struct HealReport {
    ShapeInfo before, after;
    std::vector<std::string> fixes;
};
/// Heals a Solid object (shape fixing, orientation, face unification) in place — a new immutable
/// BREP asset, one undo step.
HealReport healSolidObject(Project& project, const Uuid& solidId);

/// Rounds every edge of a Solid object with `radius` (in place: a new immutable BREP asset, one
/// undo step). Throws when the kernel cannot fillet (radius too large for an edge).
ShapeInfo filletSolidEdges(Project& project, const Uuid& solidId, double radius);
/// Offsets the faces of a Solid object by `distance` (outwards > 0, inwards < 0), in place.
ShapeInfo offsetSolidObject(Project& project, const Uuid& solidId, double distance);

/// Sections solids (exact) and meshes by a plane; the curves are added on `layerId`. Throws when
/// the plane cuts none of them.
std::vector<Uuid> sectionObjects(Project& project, const std::vector<Uuid>& ids, Vec3 planePoint, Vec3 planeNormal, const Uuid& layerId);

} // namespace openmine::occt
