#pragma once
// SPDX-License-Identifier: Apache-2.0
// Exchange of project content through OMF v1.
//
// Export: curve objects → LineSet (arcs/bulges tessellated to the chord tolerance), point objects
// → one PointSet with a "name" StringData, meshes and solids → Surface, drillholes → a "borehole"
// LineSet of desurveyed intervals with every numeric field as segment ScalarData (NaN = missing)
// and every code field as MappedData, plus a collar PointSet; block models → Volume with cell
// ScalarData; selected point clouds → PointSet with intensity/classification/scalar fields.
//
// Import: PointSet → point-cloud dataset (scalar data become float32 point fields; a note reports
// that precision change), LineSet → polylines (segments chained into the longest runs), Surface →
// mesh object (first vertex scalar kept as the mesh scalar), SurfaceGrid → triangulated mesh,
// Volume → block model when the grid is regular (uniform spacings) with a vertical w axis;
// otherwise the element is skipped and reported. Every skipped element or data array is listed
// in the summary notes — nothing is dropped silently. Everything is added in one undo step.

#include "openmine/geology/Desurvey.h"
#include "openmine/jobs/Job.h"
#include "openmine/omf/Omf.h"
#include "openmine/project/Project.h"

#include <string>
#include <vector>

namespace openmine::omf {

struct OmfExportOptions {
    bool allGeometry = true;
    std::vector<Uuid> geometry;          ///< used when allGeometry is false
    bool drillholes = true;
    bool allBlockModels = true;
    std::vector<Uuid> blockModels;
    std::vector<Uuid> pointClouds;       ///< exported only when listed (can be very large)
    geology::DesurveyMethod method = geology::DesurveyMethod::MinimumCurvature;
    double chordTolerance = 0.01;        ///< m, for arcs and bulges
    std::string author;
};

Document exportProjectOmf(const Project& project, const OmfExportOptions& options, std::vector<std::string>* notes = nullptr,
                          jobs::Progress& progress = jobs::nullProgress());

struct OmfImportOptions {
    Uuid layerId;                        ///< target layer; nil = a new layer named after the file
    std::string layerName = "OMF import";
    bool pointSetsAsPointClouds = true;  ///< otherwise point objects (only sensible for small sets)
    std::string sourceFile;
};

struct OmfImportSummary {
    std::vector<Uuid> geometry, blockModels, pointClouds;
    std::vector<std::string> notes;
};

OmfImportSummary importOmf(Project& project, const Document& doc, const OmfImportOptions& options = {},
                           jobs::Progress& progress = jobs::nullProgress());

/// Chains line segments into polylines (each vertex of degree 2 joins two segments; closed loops
/// are detected). Returns vertex index lists; a closed loop repeats its first index at the end.
std::vector<std::vector<std::int64_t>> chainSegments(const std::vector<std::array<std::int64_t, 2>>& segments, std::size_t vertexCount);

} // namespace openmine::omf
