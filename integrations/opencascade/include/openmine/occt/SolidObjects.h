#pragma once
// SPDX-License-Identifier: Apache-2.0
// Solid objects in a project: B-rep stored as an immutable BREP asset, with a display
// tessellation in the GeometryObject. All edits go through ProjectEditor (one undo step each).

#include "openmine/occt/Solid.h"
#include "openmine/project/Project.h"

#include <filesystem>

namespace openmine::occt {

struct TessellationSettings {
    double linearDeflection = 0.01;   ///< m
    double angularDeflection = 0.35;  ///< rad
};

/// Stores `shape` as a BREP asset and adds a Solid geometry object. Returns the object id.
Uuid addSolidObject(Project& project, const Shape& shape, const std::string& name, const Uuid& layerId,
                    const TessellationSettings& t = {}, const std::string& commandText = "Add solid");

/// The exact shape of a Solid object (from its BREP asset).
Shape loadSolid(const Project& project, const GeometryObject& solid);

/// Shape of any solid-like object: Solid (exact), Mesh (sewn), closed planar curve (face).
Shape shapeOfObject(const Project& project, const GeometryObject& g);

/// Boolean of two objects; the result is a new Solid object. Inputs are removed when
/// `removeInputs` (same undo step).
Uuid booleanObjects(Project& project, const Uuid& a, const Uuid& b, BooleanOp op, bool removeInputs, const std::string& name,
                    double fuzzy = 0.0);

/// Applies a transform to a Solid object exactly (B-rep transformed by the kernel, re-tessellated).
/// With `copy` a new object is added and returned; otherwise the object is updated in place (new
/// immutable BREP asset, one undo step) and its id returned.
Uuid transformSolidObject(Project& project, const Uuid& solidId, const Transform3& t, bool copy, const TessellationSettings& ts = {});

/// Replaces the shape of a Solid object (new immutable BREP asset, re-tessellated; one undo step),
/// e.g. after healing. Returns the analysis of the new shape.
ShapeInfo replaceSolidShape(Project& project, const Uuid& solidId, const Shape& shape, const std::string& commandText,
                            const TessellationSettings& ts = {});

/// Converts a mesh object into a Solid object (sewn, coplanar faces merged). Throws if the mesh is
/// not closed and `requireClosed`.
Uuid solidFromMeshObject(Project& project, const Uuid& meshId, bool requireClosed = true);

/// Exact B-rep excavation solid of a development: its profile swept along its centreline.
Uuid developmentSolid(Project& project, const Uuid& developmentId, const Uuid& layerId,
                      SweepTransition transition = SweepTransition::RightCorner);

/// Section of an object by a plane, added as polylines on `layerId`.
std::vector<Uuid> sectionObject(Project& project, const Uuid& objectId, Vec3 planePoint, Vec3 planeNormal, const Uuid& layerId);

/// STEP/IGES exchange of solid objects.
std::vector<Uuid> importSolids(Project& project, const std::filesystem::path& file, const Uuid& layerId);
void exportSolids(const Project& project, const std::vector<Uuid>& ids, const std::filesystem::path& file);

} // namespace openmine::occt
