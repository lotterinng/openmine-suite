#pragma once
// SPDX-License-Identifier: Apache-2.0
// Project-level entities: project info, coordinate systems, assets, attachments,
// materials, analysis results, reports and the audit log.

#include "openmine/objects/Common.h"

namespace openmine {

enum class CrsKind { Projected, Geographic, Geocentric, LocalGrid, Engineering, Unknown };

/// A coordinate reference system known to the project. The *project* CRS is the one
/// referenced from ProjectInfo. CRSs are always declared by the user (or by an
/// authoritative definition such as EPSG); OpenMine never infers a CRS from coordinates.
struct CoordinateReferenceSystem {
    Uuid id;
    std::string name;            ///< e.g. "Cape / Lo25"
    std::string authorityCode;   ///< e.g. "EPSG:22285"; empty for local grids
    std::string definition;      ///< PROJ string / WKT2 as resolved when declared (documentation)
    CrsKind kind = CrsKind::Unknown;
    std::string horizontalDatum; ///< e.g. "Cape", "Hartebeesthoek94"
    std::string verticalDatum = "Unspecified";
    std::string linearUnit = "metre";
    std::string axisDescription; ///< e.g. "Y westing, X southing" (from the definition)
    Uuid localGridId;            ///< when kind == LocalGrid
    EntityMetadata meta;
};

enum class LocalGridMethod { Helmert2D, Helmert3D, Affine2D };

/// A local mine grid related to a base CRS by a fitted transformation
/// (local → base). Parameters and residuals are kept so the fit can be audited.
struct LocalGridDefinition {
    Uuid id;
    std::string name;
    Uuid baseCrsId;
    LocalGridMethod method = LocalGridMethod::Helmert2D;
    Transform3 localToBase;      ///< evaluated transform
    std::vector<double> parameters; ///< method parameters (documented per method)
    double rmsResidual = 0;      ///< m, over control pairs used in the fit
    double maxResidual = 0;
    int controlPairCount = 0;
    std::string controlPairsJson; ///< [{name, local:[x,y,z], base:[x,y,z], residual:[dx,dy,dz]}]
    EntityMetadata meta;
};

/// A record of a coordinate transformation applied to project data (never applied implicitly).
struct TransformationRecord {
    Uuid id;
    std::string sourceCrs;   ///< authority code or local grid name
    std::string targetCrs;
    std::string operation;   ///< PROJ operation name / pipeline
    std::string pipeline;
    double accuracyMetres = -1; ///< stated accuracy of the operation; -1 = unknown
    std::string gridsUsed;
    std::string appliedTo;   ///< description of the entities produced
    std::string createdAt;
    std::string user;
};

enum class AssetKind { PointCloud, Mesh, Brep, BlockModel, Attachment, Report, Other };

/// A file (or folder with a manifest) in the sidecar asset store.
struct Asset {
    Uuid id;
    AssetKind kind = AssetKind::Other;
    std::string relativePath; ///< relative to <project>.openmine-assets/
    std::string sha256;       ///< of the file, or of the manifest for folder assets
    std::int64_t sizeBytes = 0;
    bool isDirectory = false;
    std::string createdAt;
};

struct Attachment {
    Uuid id;
    Uuid entityId; ///< any entity
    std::string entityType;
    std::string fileName;
    std::string mimeType;
    std::string description;
    AssetRef asset;
    EntityMetadata meta;
};

struct Material {
    Uuid id;
    std::string name;
    double density = 2.7;     ///< t/m³ (in situ)
    double swellFactor = 1.4; ///< broken / in-situ volume
    std::uint32_t colorRgb = 0x8C8C8C;
    std::string description;
    EntityMetadata meta;
};

enum class AnalysisKind {
    DistanceCloudToCloud,
    DistanceCloudToMesh,
    SurfaceComparison,
    VolumeCalculation,
    DesignConformance,
    Clearance,
    RegistrationReport,
    GeotechClassification,
    BlockModelEstimate,
    ScheduleScenarioComparison,
    Other
};

/// The stored, reproducible result of an analysis: inputs, parameters and summary
/// as JSON; large per-element payloads (e.g. per-point distances) live in an asset.
struct AnalysisResult {
    Uuid id;
    AnalysisKind kind = AnalysisKind::Other;
    std::string name;
    std::string inputsJson;
    std::string parametersJson;
    std::string summaryJson;
    std::string payloadJson; ///< modest-size detailed results (e.g. per-station table)
    AssetRef payloadAsset;
    std::string createdAt;
    std::string user;
};

struct Report {
    Uuid id;
    std::string name;
    std::string kind; ///< "Survey", "Schedule", "Reconciliation", ...
    std::string format; ///< "html" | "pdf"
    AssetRef asset;
    std::string createdAt;
};

struct AuditEntry {
    std::string timestamp;
    std::string user;
    std::string action;     ///< e.g. "Create", "Modify", "Delete", "Recalculate"
    std::string entityType; ///< e.g. "GeometryObject"
    Uuid entityId;
    std::string detail;
};

/// Project-wide settings.
struct ProjectInfo {
    Uuid id;
    std::string name;
    std::string description;
    DateTime scheduleStart;          ///< project/schedule start
    std::optional<DateTime> statusDate; ///< data date for progress reforecasts
    Uuid projectCrsId;               ///< CoordinateReferenceSystem of all stored coordinates
    Uuid defaultCalendarId;
    int coordinateDisplayDecimals = 3;
    std::string linearUnit = "metre";
    bool southOrientatedAxes = false; ///< display Lo-system labels (Y westing, X southing)
    EntityMetadata meta;
};

} // namespace openmine
