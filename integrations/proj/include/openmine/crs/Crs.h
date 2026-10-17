#pragma once
// SPDX-License-Identifier: Apache-2.0
// Coordinate reference systems and transformations through PROJ (MIT). No PROJ type appears in
// this header.
//
// Axis order: coordinates are always in the CRS's own axis order as defined by its authority
// (e.g. EPSG:22285 Cape / Lo25 is Y (westing), X (southing); EPSG:4326 is latitude, longitude).
// OpenMine never swaps axes silently; `CrsInfo::axes` describes them for display.
// Transformations are explicit: choosing an operation records its accuracy and grids, and
// ballpark (datum-ignoring) operations are refused unless explicitly allowed.

#include "openmine/geometry/LocalGrid.h"
#include "openmine/project/Project.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openmine::crs {

class CrsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Axis {
    std::string name, abbreviation, direction, unit;
};

struct CrsInfo {
    std::string name;
    std::string authorityCode;      ///< "EPSG:22285" (empty for custom definitions)
    std::string projString;
    std::string wkt;                ///< WKT2:2019
    CrsKind kind = CrsKind::Unknown;
    std::string horizontalDatum;
    std::string verticalDatum;
    std::string linearUnit;         ///< "metre", "foot", ... (empty for geographic)
    double unitToMetre = 1.0;
    std::vector<Axis> axes;
    bool deprecated = false;
    std::string areaName;
    std::optional<std::array<double, 4>> areaOfUse; ///< west, south, east, north (degrees)
    std::string axisDescription() const; ///< e.g. "Y (westing) south-oriented? …" → "Y westing, X southing"
};

std::string projVersion();

/// Resolves "EPSG:22285", "epsg:4326", a WKT string or a PROJ string.
CrsInfo describe(const std::string& definition);
/// CRS entity for the project (definition = WKT2).
CoordinateReferenceSystem toEntity(const CrsInfo& info);
/// Database search by name fragment (case-insensitive), projected and geographic CRSs.
std::vector<CrsInfo> search(const std::string& nameFragment, std::size_t maxResults = 50);

struct OperationInfo {
    std::string name;
    std::string projPipeline;
    double accuracy = -1;            ///< m; -1 = unknown
    bool ballpark = false;           ///< datum shift ignored or approximate
    std::vector<std::string> grids;  ///< grid files the operation needs
    bool gridsAvailable = true;
};

/// Candidate operations between two CRSs, best first (PROJ ranking).
std::vector<OperationInfo> candidateOperations(const std::string& source, const std::string& target);

struct TransformerOptions {
    int operationIndex = 0;          ///< index into candidateOperations
    bool allowBallpark = false;
};

class Transformer {
public:
    Transformer(const std::string& source, const std::string& target, const TransformerOptions& options = {});
    ~Transformer();
    Transformer(const Transformer&) = delete;
    Transformer& operator=(const Transformer&) = delete;

    Vec3 forward(Vec3 p) const;
    Vec3 inverse(Vec3 p) const;
    std::vector<Vec3> forward(const std::vector<Vec3>& pts) const;
    const OperationInfo& operation() const { return op_; }
    const std::string& source() const { return src_; }
    const std::string& target() const { return dst_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    OperationInfo op_;
    std::string src_, dst_;
};

/// Checks points against a CRS's area of use (transformed to geographic coordinates); returns
/// human-readable warnings (empty when every point is inside, with a 10 % margin).
std::vector<std::string> checkAreaOfUse(const std::string& crs, const std::vector<Vec3>& points);

// ---- Project integration ----------------------------------------------------------------------------
/// Declares a CRS in the project (or returns the existing one with the same authority code).
Uuid declareCrs(Project& project, const std::string& definition);
/// Sets the project CRS (ProjectInfo.projectCrsId). Coordinates are not changed.
void setProjectCrs(Project& project, const Uuid& crsId);

/// Stores a fitted local grid (LocalGridDefinition + LocalGrid CRS) related to `baseCrsId`.
Uuid defineLocalGrid(Project& project, const std::string& name, const Uuid& baseCrsId, const geometry::LocalGridFit& fit);

/// Transforms geometry objects into copies in another CRS (originals are untouched) and stores a
/// TransformationRecord with the operation, its accuracy and grids. Local grid CRSs are handled
/// through their fitted transformation to the base CRS. Returns the new object ids.
std::vector<Uuid> transformObjects(Project& project, const std::vector<Uuid>& objectIds, const Uuid& sourceCrsId, const Uuid& targetCrsId,
                                   const Uuid& targetLayerId, const TransformerOptions& options = {});

/// Point transformation between two project CRSs (local grids included), with the record it would store.
std::vector<Vec3> transformPoints(const Project& project, const std::vector<Vec3>& pts, const Uuid& sourceCrsId, const Uuid& targetCrsId,
                                  const TransformerOptions& options = {}, TransformationRecord* record = nullptr);

// ---- Units ----------------------------------------------------------------------------------------
enum class LengthUnit { Metre, Millimetre, Centimetre, Kilometre, Foot, UsSurveyFoot, Inch, Yard };
double toMetres(double value, LengthUnit unit);
double fromMetres(double metres, LengthUnit unit);
/// Parses "12.5 m", "3 ft", "1200mm", "5'" (feet) etc.; plain numbers are metres.
double parseLength(const std::string& text);
std::optional<LengthUnit> unitFromName(const std::string& name);

} // namespace openmine::crs
