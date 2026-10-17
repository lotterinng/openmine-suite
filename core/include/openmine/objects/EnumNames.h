#pragma once
// SPDX-License-Identifier: Apache-2.0
// Canonical text names of every persisted enum (used by persistence, UI, console and Python).
// Names are part of the file format: never rename an existing entry.

#include "openmine/objects/CadEntities.h"
#include "openmine/objects/DesignEntities.h"
#include "openmine/objects/GeologyEntities.h"
#include "openmine/objects/ProjectEntities.h"
#include "openmine/objects/ScheduleEntities.h"
#include "openmine/objects/SurveyEntities.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openmine {

template <class E> struct EnumTraits; // specialised below: static const std::vector<std::pair<E, const char*>>& items()

template <class E>
concept NamedEnum = requires { EnumTraits<E>::items(); };

template <NamedEnum E> const char* toString(E e) {
    for (const auto& [v, n] : EnumTraits<E>::items())
        if (v == e) return n;
    return "?";
}

template <NamedEnum E> std::optional<E> enumFromString(std::string_view s) {
    for (const auto& [v, n] : EnumTraits<E>::items())
        if (s == n) return v;
    return std::nullopt;
}

template <NamedEnum E> std::vector<std::string> enumNames() {
    std::vector<std::string> out;
    for (const auto& [v, n] : EnumTraits<E>::items()) out.emplace_back(n);
    return out;
}

#define OPENMINE_ENUM_NAMES(E, ...)                                                   \
    template <> struct EnumTraits<E> {                                                \
        static const std::vector<std::pair<E, const char*>>& items() {                \
            static const std::vector<std::pair<E, const char*>> v{__VA_ARGS__};       \
            return v;                                                                 \
        }                                                                             \
    };

OPENMINE_ENUM_NAMES(CrsKind, {CrsKind::Projected, "Projected"}, {CrsKind::Geographic, "Geographic"},
                    {CrsKind::Geocentric, "Geocentric"}, {CrsKind::LocalGrid, "LocalGrid"},
                    {CrsKind::Engineering, "Engineering"}, {CrsKind::Unknown, "Unknown"})
OPENMINE_ENUM_NAMES(LocalGridMethod, {LocalGridMethod::Helmert2D, "Helmert2D"}, {LocalGridMethod::Helmert3D, "Helmert3D"},
                    {LocalGridMethod::Affine2D, "Affine2D"})
OPENMINE_ENUM_NAMES(AssetKind, {AssetKind::PointCloud, "PointCloud"}, {AssetKind::Mesh, "Mesh"}, {AssetKind::Brep, "Brep"},
                    {AssetKind::BlockModel, "BlockModel"}, {AssetKind::Attachment, "Attachment"},
                    {AssetKind::Report, "Report"}, {AssetKind::Other, "Other"})
OPENMINE_ENUM_NAMES(AnalysisKind, {AnalysisKind::DistanceCloudToCloud, "DistanceCloudToCloud"},
                    {AnalysisKind::DistanceCloudToMesh, "DistanceCloudToMesh"},
                    {AnalysisKind::SurfaceComparison, "SurfaceComparison"},
                    {AnalysisKind::VolumeCalculation, "VolumeCalculation"},
                    {AnalysisKind::DesignConformance, "DesignConformance"}, {AnalysisKind::Clearance, "Clearance"},
                    {AnalysisKind::RegistrationReport, "RegistrationReport"},
                    {AnalysisKind::GeotechClassification, "GeotechClassification"},
                    {AnalysisKind::BlockModelEstimate, "BlockModelEstimate"},
                    {AnalysisKind::ScheduleScenarioComparison, "ScheduleScenarioComparison"},
                    {AnalysisKind::Other, "Other"})
OPENMINE_ENUM_NAMES(GeometryType, {GeometryType::Point, "Point"}, {GeometryType::Line, "Line"},
                    {GeometryType::Polyline, "Polyline"}, {GeometryType::Arc, "Arc"}, {GeometryType::Circle, "Circle"},
                    {GeometryType::Spline, "Spline"}, {GeometryType::Text, "Text"},
                    {GeometryType::Dimension, "Dimension"}, {GeometryType::Hatch, "Hatch"}, {GeometryType::Mesh, "Mesh"},
                    {GeometryType::Solid, "Solid"})
OPENMINE_ENUM_NAMES(TextAlign, {TextAlign::Left, "Left"}, {TextAlign::Center, "Center"}, {TextAlign::Right, "Right"})
OPENMINE_ENUM_NAMES(DimensionKind, {DimensionKind::Aligned, "Aligned"}, {DimensionKind::Horizontal, "Horizontal"},
                    {DimensionKind::Vertical, "Vertical"}, {DimensionKind::Angular, "Angular"},
                    {DimensionKind::Radius, "Radius"}, {DimensionKind::Diameter, "Diameter"},
                    {DimensionKind::Elevation, "Elevation"})
OPENMINE_ENUM_NAMES(ControlPointRole, {ControlPointRole::Control, "Control"}, {ControlPointRole::Check, "Check"},
                    {ControlPointRole::Temporary, "Temporary"}, {ControlPointRole::ScanTarget, "ScanTarget"})
OPENMINE_ENUM_NAMES(RegistrationMethod, {RegistrationMethod::PointPair, "PointPair"},
                    {RegistrationMethod::ControlPoints, "ControlPoints"},
                    {RegistrationMethod::IcpPointToPoint, "IcpPointToPoint"},
                    {RegistrationMethod::IcpPointToPlane, "IcpPointToPlane"}, {RegistrationMethod::Manual, "Manual"})
OPENMINE_ENUM_NAMES(PairRole, {PairRole::Fit, "Fit"}, {PairRole::Check, "Check"})
OPENMINE_ENUM_NAMES(ProfileShape, {ProfileShape::Rectangular, "Rectangular"}, {ProfileShape::ShoulderArch, "ShoulderArch"},
                    {ProfileShape::SemicircularArch, "SemicircularArch"},
                    {ProfileShape::SegmentalArch, "SegmentalArch"}, {ProfileShape::Horseshoe, "Horseshoe"},
                    {ProfileShape::Custom, "Custom"})
OPENMINE_ENUM_NAMES(CentrelineReference, {CentrelineReference::FloorCentre, "FloorCentre"},
                    {CentrelineReference::ProfileCentre, "ProfileCentre"})
OPENMINE_ENUM_NAMES(StopeMethod, {StopeMethod::LongholeOpenStope, "LongholeOpenStope"},
                    {StopeMethod::SublevelOpenStope, "SublevelOpenStope"}, {StopeMethod::CutAndFill, "CutAndFill"},
                    {StopeMethod::RoomAndPillar, "RoomAndPillar"}, {StopeMethod::SublevelCaving, "SublevelCaving"},
                    {StopeMethod::Other, "Other"})
OPENMINE_ENUM_NAMES(CalendarKind, {CalendarKind::Project, "Project"}, {CalendarKind::Shift, "Shift"},
                    {CalendarKind::Resource, "Resource"})
OPENMINE_ENUM_NAMES(ActivityKind, {ActivityKind::Task, "Task"}, {ActivityKind::Milestone, "Milestone"},
                    {ActivityKind::Summary, "Summary"})
OPENMINE_ENUM_NAMES(ConstraintType, {ConstraintType::None, "None"},
                    {ConstraintType::StartNoEarlierThan, "StartNoEarlierThan"},
                    {ConstraintType::FinishNoLaterThan, "FinishNoLaterThan"}, {ConstraintType::MustStartOn, "MustStartOn"})
OPENMINE_ENUM_NAMES(RateUnit, {RateUnit::PerHour, "PerHour"}, {RateUnit::PerShift, "PerShift"}, {RateUnit::PerDay, "PerDay"})
OPENMINE_ENUM_NAMES(DependencyType, {DependencyType::FinishToStart, "FS"}, {DependencyType::StartToStart, "SS"},
                    {DependencyType::FinishToFinish, "FF"}, {DependencyType::StartToFinish, "SF"})
OPENMINE_ENUM_NAMES(ResourceKind, {ResourceKind::Equipment, "Equipment"}, {ResourceKind::Crew, "Crew"},
                    {ResourceKind::Material, "Material"}, {ResourceKind::Other, "Other"})
OPENMINE_ENUM_NAMES(ActualSource, {ActualSource::Manual, "Manual"}, {ActualSource::Survey, "Survey"},
                    {ActualSource::Import, "Import"})
OPENMINE_ENUM_NAMES(LevellingMode, {LevellingMode::None, "None"}, {LevellingMode::Heuristic, "Heuristic"},
                    {LevellingMode::Optimised, "Optimised"})
OPENMINE_ENUM_NAMES(GeologicalObjectKind, {GeologicalObjectKind::Surface, "Surface"},
                    {GeologicalObjectKind::Solid, "Solid"}, {GeologicalObjectKind::Fault, "Fault"},
                    {GeologicalObjectKind::Contact, "Contact"}, {GeologicalObjectKind::Domain, "Domain"},
                    {GeologicalObjectKind::Other, "Other"})
OPENMINE_ENUM_NAMES(StructureType, {StructureType::Joint, "Joint"}, {StructureType::Fault, "Fault"},
                    {StructureType::Bedding, "Bedding"}, {StructureType::Foliation, "Foliation"},
                    {StructureType::Shear, "Shear"}, {StructureType::Other, "Other"})
OPENMINE_ENUM_NAMES(GeotechMethod, {GeotechMethod::QSystem, "QSystem"}, {GeotechMethod::Rmr89, "RMR89"})

#undef OPENMINE_ENUM_NAMES

// Backwards-compatible helpers used throughout the code base.
inline std::optional<GeometryType> geometryTypeFromString(const std::string& s) { return enumFromString<GeometryType>(s); }
inline std::optional<ActivityKind> activityKindFromString(const std::string& s) { return enumFromString<ActivityKind>(s); }
inline std::optional<DependencyType> dependencyTypeFromString(const std::string& s) {
    return enumFromString<DependencyType>(s);
}

} // namespace openmine
