#pragma once
// SPDX-License-Identifier: Apache-2.0
// Survey entities: control, point-cloud datasets, registrations, as-built models
// and design-conformance results.

#include "openmine/objects/Common.h"

namespace openmine {

enum class ControlPointRole { Control, Check, Temporary, ScanTarget };

/// A surveyed point with stated accuracy. Coordinates are in the project CRS.
struct SurveyControlPoint {
    Uuid id;
    std::string name;
    std::string code;
    Vec3 position;
    double horizontalSigma = 0.005; ///< m, 1σ, as stated by the surveyor
    double verticalSigma = 0.005;   ///< m, 1σ
    ControlPointRole role = ControlPointRole::Control;
    std::string source;             ///< instrument / traverse / station file
    std::optional<Date> surveyDate;
    std::string description;
    EntityMetadata meta;
};

/// Per-point attributes present in a dataset.
struct PointAttributes {
    bool intensity = false;
    bool rgb = false;
    bool classification = false;
    bool normals = false;
    bool returnInfo = false;
    bool gpsTime = false;
    std::vector<std::string> scalarFields; ///< extra float32 fields (e.g. "distance")
    friend bool operator==(const PointAttributes&, const PointAttributes&) = default;
};

/// A point cloud stored out-of-core as an octree in the asset store (see modules/survey).
/// Points are stored in their *original* coordinates; `transform` (the product of the
/// applied registrations) maps stored → project coordinates. Original data is never altered.
struct PointCloudDataset {
    Uuid id;
    std::string name;
    AssetRef asset;                ///< octree folder
    std::int64_t pointCount = 0;
    BoundingBox storedBounds;      ///< bounds of stored coordinates
    PointAttributes attributes;
    Transform3 transform;          ///< stored → project coordinates
    std::string sourceFile;        ///< original file name (not path)
    std::string sourceFormat;      ///< LAS, LAZ, E57, PLY, XYZ, PTS, PTX, CSV
    std::string sourceCrs;         ///< declared CRS of the source file (as declared at import)
    Uuid parentDatasetId;          ///< for derived (processed) datasets
    std::string derivation;        ///< e.g. "statistical outlier removal k=16 n=2.0"
    std::optional<Date> scanDate;
    std::string scanner;
    bool visible = true;
    double pointSize = 2.0;
    std::string colorMode = "Elevation"; ///< Elevation | Intensity | RGB | Classification | `Scalar:<name>` | Single
    Uuid developmentId;            ///< optional: heading this scan covers
    EntityMetadata meta;
};

enum class RegistrationMethod { PointPair, ControlPoints, IcpPointToPoint, IcpPointToPlane, Manual };

enum class PairRole { Fit, Check };

/// One correspondence used by a registration. `targetControlId` refers to a control point
/// when registering to control; otherwise `target` holds the target coordinates.
struct RegistrationPair {
    std::string name;
    Vec3 source;          ///< in the moving dataset's stored coordinates
    Vec3 target;          ///< in project coordinates
    Uuid targetControlId; ///< optional
    PairRole role = PairRole::Fit;
    Vec3 residual;        ///< target − T(source) after the fit
    friend bool operator==(const RegistrationPair&, const RegistrationPair&) = default;
};

/// A registration of a point-cloud dataset. Statistics deliberately distinguish:
///  * fit residuals — misclosure of the points used to compute the transform (optimistic);
///  * ICP fit — inlier RMS distance between overlapping clouds (internal consistency);
///  * independent check — residuals at check points NOT used in the fit (positional accuracy estimate).
struct Registration {
    Uuid id;
    Uuid datasetId;
    Uuid targetDatasetId; ///< for cloud-to-cloud (ICP) registrations
    RegistrationMethod method = RegistrationMethod::PointPair;
    bool allowScale = false;
    Transform3 transform;  ///< increment: stored(current) → new project coordinates
    Transform3 previousTransform;
    std::vector<RegistrationPair> pairs;
    double fitRms = 0, fitMax = 0;
    int fitCount = 0;
    double checkRms = -1, checkMax = -1; ///< -1 when no check points
    int checkCount = 0;
    double icpFitness = -1;   ///< fraction of source points with a correspondence
    double icpInlierRms = -1; ///< m
    int icpIterations = 0;
    bool icpConverged = false;
    double icpMaxCorrespondence = 0;
    bool applied = false;
    std::string notes;
    EntityMetadata meta;
};

/// As-built geometry derived from a registered scan.
struct AsBuiltModel {
    Uuid id;
    std::string name;
    Uuid sourceDatasetId;
    Uuid registrationId;  ///< registration in force when the model was generated
    Uuid geometryId;      ///< Mesh geometry object
    Uuid developmentId;   ///< optional heading
    std::string method;   ///< "Poisson depth=9", "Delaunay2.5D", ...
    std::optional<Date> surveyDate;
    EntityMetadata meta;
};

/// Overbreak/underbreak and clearance results for a heading (definition: docs/architecture/conformance.md).
struct DesignConformanceResult {
    Uuid id;
    Uuid asBuiltId;
    Uuid developmentId;
    double stationInterval = 1.0;
    double chainageFrom = 0, chainageTo = 0;
    double designVolume = 0, asBuiltVolume = 0;
    double overbreakVolume = 0, underbreakVolume = 0;
    double meanOverbreakDepth = 0, maxUnderbreakDepth = 0;
    double minClearance = -1;
    int stationCount = 0;
    Uuid analysisResultId; ///< per-station table
    EntityMetadata meta;
};

} // namespace openmine
