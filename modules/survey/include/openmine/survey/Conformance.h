#pragma once
// SPDX-License-Identifier: Apache-2.0
// Underground design conformance: overbreak / underbreak, clearance and face position of an
// as-built (scan) against a development's design profile, station by station along the
// centreline. Definitions (docs/architecture/conformance.md):
//
//  * At each station a section plane is placed with the design section frame (u to the right of
//    travel, v up). The as-built section is the plane section of the as-built mesh (or, for point
//    clouds, the outermost points of a thin slab by angular bins around the design centroid).
//  * Overbreak area = area(as-built − design); underbreak area = area(design − as-built)
//    (polygon booleans, Clipper2). Depths are the largest distances of the as-built boundary
//    outside / inside the design boundary.
//  * Volumes integrate station areas with the average-end-area rule over the station interval.
//  * Clearance (optional equipment envelope in section coordinates) is the smallest distance from
//    the envelope to the as-built boundary, negative where the as-built intrudes into it.
//  * An open as-built section (e.g. an unscanned floor) is closed with the chord between its ends;
//    such stations are flagged.

#include "openmine/geometry/Sweep.h"
#include "openmine/project/Project.h"
#include "openmine/survey/PointBuffer.h"

namespace openmine::survey {

struct ConformanceOptions {
    double interval = 1.0;                  ///< m between stations
    std::optional<double> fromChainage, toChainage; ///< default: whole centreline
    geometry::SectionOrientation orientation = geometry::SectionOrientation::PerpendicularToAxis;
    std::vector<Vec2> clearanceEnvelope;    ///< optional, section coordinates
    double slabHalfWidth = 0.05;            ///< point-cloud sections: ±m around the plane
    int angularBins = 720;                  ///< point-cloud sections
    std::size_t minSlabPoints = 50;         ///< point-cloud sections: fewer → station invalid
};

struct StationConformance {
    double chainage = 0;
    bool valid = false;
    bool closedByChord = false;
    std::string issue;
    double designArea = 0, asBuiltArea = 0, overbreakArea = 0, underbreakArea = 0;
    double maxOverbreakDepth = 0, maxUnderbreakDepth = 0;
    std::optional<double> clearance;
    std::vector<Vec2> asBuilt; ///< as-built section polygon (u, v)
};

struct ConformanceReport {
    std::vector<StationConformance> stations;
    double designVolume = 0, asBuiltVolume = 0, overbreakVolume = 0, underbreakVolume = 0;
    double overbreakPercent = 0;      ///< overbreak volume / design volume × 100
    double meanOverbreakDepth = 0;    ///< mean over valid stations of their maximum overbreak depth
    double maxOverbreakDepth = 0, maxUnderbreakDepth = 0;
    std::optional<double> minClearance;
    int validStations = 0;
};

/// Conformance of an as-built mesh against a design profile along a centreline.
ConformanceReport analyseConformance(const std::vector<Vec3>& centreline, const std::vector<Vec2>& designProfile, const MeshData& asBuilt,
                                     const ConformanceOptions& options = {}, jobs::Progress& progress = jobs::nullProgress());
/// The same against scan points directly (no meshing).
ConformanceReport analyseConformance(const std::vector<Vec3>& centreline, const std::vector<Vec2>& designProfile, const PointBuffer& points,
                                     const ConformanceOptions& options = {}, jobs::Progress& progress = jobs::nullProgress());

/// One-station overbreak/underbreak from polygons (exposed for tests and the section viewer).
StationConformance compareSection(const std::vector<Vec2>& design, const std::vector<Vec2>& asBuilt, const std::vector<Vec2>& envelope = {});

struct FacePosition {
    double faceChainage = 0;     ///< furthest chainage with continuous scan coverage
    double coveredFrom = 0;
    std::size_t pointsUsed = 0;
};
/// Face position from scan points within `corridor` m of the centreline: the end of the covered
/// stretch (bins of `bin` m with at least `minPointsPerBin` points, gaps up to `maxGap` m allowed).
FacePosition facePosition(const std::vector<Vec3>& centreline, const PointBuffer& points, double corridor = 5.0, double bin = 0.5,
                          std::size_t minPointsPerBin = 20, double maxGap = 2.0);

// ---- Project integration ----------------------------------------------------------------------------
/// Adds a mesh geometry object and an AsBuiltModel linked to the dataset, its registration in force
/// and an optional development (one undo step).
Uuid addAsBuiltModel(Project& project, const Uuid& datasetId, MeshData mesh, const std::string& name, const Uuid& layerId,
                     const Uuid& developmentId, const std::string& method);
/// Runs the analysis of an as-built model against its development's design.
ConformanceReport analyseAsBuilt(const Project& project, const Uuid& asBuiltId, const Uuid& developmentId, const ConformanceOptions& options = {},
                                 jobs::Progress& progress = jobs::nullProgress());
/// Stores the per-station table (AnalysisResult) and the summary (DesignConformanceResult).
Uuid commitConformance(Project& project, const Uuid& asBuiltId, const Uuid& developmentId, const ConformanceReport& report,
                       const ConformanceOptions& options);
/// CSV of the per-station table (chainage formatted as 1+234.500).
std::string conformanceCsv(const ConformanceReport& report);

} // namespace openmine::survey
