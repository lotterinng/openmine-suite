#pragma once
// SPDX-License-Identifier: Apache-2.0
// Geology and geotechnical entities.

#include "openmine/objects/Common.h"

namespace openmine {

struct DrillholeSurvey {
    double depth = 0;    ///< m along hole
    double azimuth = 0;  ///< degrees clockwise from grid north (+Y)
    double dip = -90;    ///< degrees, negative = downward
    friend bool operator==(const DrillholeSurvey&, const DrillholeSurvey&) = default;
};

/// A downhole interval with named numeric and text values (assays, lithology...).
struct DrillholeInterval {
    double from = 0;
    double to = 0;
    std::map<std::string, double> values;
    std::map<std::string, std::string> codes;
    friend bool operator==(const DrillholeInterval&, const DrillholeInterval&) = default;
};

struct Drillhole {
    Uuid id;
    std::string holeId;
    Vec3 collar;
    double length = 0;
    std::vector<DrillholeSurvey> surveys; ///< sorted by depth; first at 0 m
    std::vector<DrillholeInterval> intervals;
    std::string holeType = "DD";
    std::optional<Date> drilledDate;
    EntityMetadata meta;
};

enum class GeologicalObjectKind { Surface, Solid, Fault, Contact, Domain, Other };

struct GeologicalObject {
    Uuid id;
    std::string name;
    GeologicalObjectKind kind = GeologicalObjectKind::Solid;
    Uuid geometryId;
    std::string rockUnit;
    Uuid materialId;
    std::string description;
    EntityMetadata meta;
};

/// A regular (optionally Z-rotated) block model. Attribute arrays are stored in an asset
/// as little-endian float64, one array per attribute, in i-fastest order.
struct BlockModel {
    Uuid id;
    std::string name;
    Vec3 origin;          ///< corner of block (0,0,0)
    Vec3 blockSize{5, 5, 5};
    int nx = 1, ny = 1, nz = 1;
    double rotationDeg = 0; ///< about +Z through origin, CCW
    std::vector<std::string> attributes;
    AssetRef dataAsset;
    std::string description;
    EntityMetadata meta;
};

enum class StructureType { Joint, Fault, Bedding, Foliation, Shear, Other };

struct StructuralMeasurement {
    Uuid id;
    std::string setName;
    StructureType type = StructureType::Joint;
    double dip = 0;          ///< degrees 0..90
    double dipDirection = 0; ///< degrees 0..360 clockwise from grid north
    Vec3 location;
    double persistence = 0;  ///< m (0 = unknown)
    std::string roughness;
    std::string infill;
    Uuid developmentId;
    EntityMetadata meta;
};

enum class GeotechMethod { QSystem, Rmr89 };

/// A rock-mass classification at a location (inputs and result are kept for audit).
struct GeotechClassification {
    Uuid id;
    GeotechMethod method = GeotechMethod::QSystem;
    Uuid developmentId;
    double chainage = 0;
    Vec3 location;
    std::string inputsJson;
    double value = 0;        ///< Q or RMR
    std::string rockClass;   ///< e.g. "Fair", "III Fair rock"
    std::string notes;
    std::optional<Date> mappedDate;
    std::string mappedBy;
    EntityMetadata meta;
};

} // namespace openmine
