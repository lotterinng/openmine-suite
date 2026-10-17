// SPDX-License-Identifier: Apache-2.0
#include "openmine/objects/Entities.h"

namespace openmine {

const char* toString(GeometryType t) {
    switch (t) {
    case GeometryType::Point: return "Point";
    case GeometryType::Line: return "Line";
    case GeometryType::Polyline: return "Polyline";
    }
    return "Polyline";
}

std::optional<GeometryType> geometryTypeFromString(const std::string& s) {
    if (s == "Point") return GeometryType::Point;
    if (s == "Line") return GeometryType::Line;
    if (s == "Polyline") return GeometryType::Polyline;
    return std::nullopt;
}

const char* toString(ActivityKind k) {
    switch (k) {
    case ActivityKind::Task: return "Task";
    case ActivityKind::Milestone: return "Milestone";
    case ActivityKind::Summary: return "Summary";
    }
    return "Task";
}

std::optional<ActivityKind> activityKindFromString(const std::string& s) {
    if (s == "Task") return ActivityKind::Task;
    if (s == "Milestone") return ActivityKind::Milestone;
    if (s == "Summary") return ActivityKind::Summary;
    return std::nullopt;
}

const char* toString(DependencyType t) {
    switch (t) {
    case DependencyType::FinishToStart: return "FS";
    case DependencyType::StartToStart: return "SS";
    case DependencyType::FinishToFinish: return "FF";
    case DependencyType::StartToFinish: return "SF";
    }
    return "FS";
}

std::optional<DependencyType> dependencyTypeFromString(const std::string& s) {
    if (s == "FS") return DependencyType::FinishToStart;
    if (s == "SS") return DependencyType::StartToStart;
    if (s == "FF") return DependencyType::FinishToFinish;
    if (s == "SF") return DependencyType::StartToFinish;
    return std::nullopt;
}

} // namespace openmine
