#pragma once
// SPDX-License-Identifier: Apache-2.0
// Mine-design domain entities.

#include "openmine/objects/Common.h"

namespace openmine {

enum class ProfileShape {
    Rectangular,   ///< width × height
    ShoulderArch,  ///< vertical walls, circular shoulders of radius r, flat crown
    SemicircularArch, ///< vertical walls + semicircular arch (r = width/2)
    SegmentalArch, ///< vertical walls + circular arch of rise `archRise`
    Horseshoe,     ///< arched roof with walls inclined by `wallBatter`
    Custom,        ///< user polygon in profile coordinates
};

/// Where the design centreline sits on the profile.
enum class CentrelineReference { FloorCentre, ProfileCentre };

/// A reusable tunnel profile. Profile coordinates: u horizontal (right of travel, m),
/// v vertical (up, m), origin at the centreline reference point.
struct TunnelProfile {
    Uuid id;
    std::string name;
    ProfileShape shape = ProfileShape::Rectangular;
    double width = 5.0;
    double height = 5.0;
    double shoulderRadius = 1.0;
    double archRise = 1.0;
    double wallBatter = 0.0; ///< horizontal change per metre of height, each wall
    std::vector<Vec2> customPoints;
    CentrelineReference reference = CentrelineReference::FloorCentre;
    int arcSegments = 8; ///< tessellation of curved parts (per 90°)
    EntityMetadata meta;
};

struct MineLevel {
    Uuid id;
    std::string name;     ///< e.g. "675 Level"
    std::string code;     ///< e.g. "675"
    double elevation = 0; ///< nominal floor RL (m)
    std::string zone;
    std::string status = "Planned";
    std::string description;
    EntityMetadata meta;
};

/// A decline (ramp) system. Its design parameters drive the decline designer.
struct Decline {
    Uuid id;
    std::string name;
    std::string code;
    std::string zone;
    std::string number;
    double designGradientPercent = -12.5; ///< negative = down
    double minCurveRadius = 25.0;         ///< m (plan)
    Uuid profileId;
    std::string status = "Planned";
    EntityMetadata meta;
};

/// An underground development heading — a domain object referencing centreline geometry.
/// Length, chainage-to and gradient are derived from geometry (never typed in).
struct MineDevelopment {
    Uuid id;
    std::string headingId;      ///< mine-assigned code, e.g. "LEG2_675_D3_DEC"
    std::string name;
    std::string zone;
    std::string declineNumber;
    std::string level;          ///< free text (legacy / display)
    Uuid levelId;
    Uuid declineId;
    std::string activityType = "Development"; ///< Decline, Ramp, Crosscut, Ore drive, Access drive, ...
    double chainageFrom = 0.0;  ///< m
    double designWidth = 5.0;   ///< m (used when no profile is assigned)
    double designHeight = 5.0;  ///< m
    Uuid profileId;             ///< TunnelProfile; nil → rectangular width × height
    Uuid materialId;            ///< for tonnes (density) and swell
    Uuid supportClassId;        ///< GroundSupportClass
    std::string status = "Planned"; ///< Planned | In progress | Complete | Suspended
    Uuid geometryId;            ///< centreline (required)
    double approvedDesignLength = 0.0; ///< length the schedule quantities were approved against
    std::string designRevision = "A";
    Uuid currentRevisionId;     ///< latest DesignRevision snapshot
    int priority = 0;
    AttributeMap attributes;
    EntityMetadata meta;
};

enum class StopeMethod { LongholeOpenStope, SublevelOpenStope, CutAndFill, RoomAndPillar, SublevelCaving, Other };

struct Stope {
    Uuid id;
    std::string name;
    std::string code;
    Uuid levelId;
    std::string zone;
    StopeMethod method = StopeMethod::LongholeOpenStope;
    Uuid geometryId;   ///< closed mesh or solid
    Uuid materialId;
    double grade = 0;  ///< primary grade (unit in gradeUnit)
    std::string gradeUnit = "g/t";
    double dilutionPercent = 0;
    double recoveryPercent = 100;
    std::string status = "Planned";
    std::string designRevision = "A";
    Uuid currentRevisionId;
    int priority = 0;
    AttributeMap attributes;
    EntityMetadata meta;
};

/// A schedulable subdivision of a stope (e.g. a firing or a bench).
struct MiningBlock {
    Uuid id;
    std::string name;
    Uuid stopeId;
    int sequence = 0;
    Uuid geometryId;
    std::string status = "Planned";
    EntityMetadata meta;
};

/// An immutable snapshot of a design object at a revision.
struct DesignRevision {
    Uuid id;
    Uuid entityId;          ///< MineDevelopment or Stope
    std::string entityType; ///< "MineDevelopment" | "Stope"
    std::string revisionCode;
    std::string snapshotJson; ///< entity + geometry vertices at this revision
    double length = 0;      ///< m (developments)
    double volume = 0;      ///< m³
    bool approved = false;
    std::string approvedBy;
    std::string approvedAt;
    std::string comment;
    std::string createdAt;
    std::string author;
};

struct GroundSupportClass {
    Uuid id;
    std::string name;         ///< e.g. "GS2 — 2.4 m resin bolts @1.2 m + mesh"
    double boltLength = 2.4;  ///< m
    double boltSpacing = 1.2; ///< m (square pattern)
    std::string boltType = "Resin rebar";
    bool mesh = true;
    double shotcreteThickness = 0; ///< mm
    double installMinutesPerBolt = 4.0;
    double meshMinutesPerSquareMetre = 0.5;
    double shotcreteMinutesPerCubicMetre = 25.0;
    std::string description;
    EntityMetadata meta;
};

} // namespace openmine
