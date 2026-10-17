#pragma once
// SPDX-License-Identifier: Apache-2.0
// Validated, undoable editing operations on the unified model. This is the only
// API the UI, console and Python bindings use to change project data.
// Every method either fully succeeds (one undo step) or throws ValidationError
// without changing anything.

#include "openmine/project/Project.h"

#include <string>
#include <vector>

namespace openmine {

class ProjectEditor {
public:
    explicit ProjectEditor(Project& p) : p_(p) {}

    // Project ----------------------------------------------------------------
    void setProjectInfo(const ProjectInfo& info, const std::string& text = "Edit project settings");

    // Layers -----------------------------------------------------------------
    Uuid addLayer(const std::string& name, std::uint32_t colorRgb);
    void updateLayer(const CadLayer& layer);
    const CadLayer* findLayerByName(const std::string& name) const;

    // Geometry ---------------------------------------------------------------
    Uuid addGeometry(GeometryObject g, const std::string& text = "");
    void updateGeometry(GeometryObject g, const std::string& text = "");
    void removeGeometry(const Uuid& id);

    // Mine design --------------------------------------------------------------
    Uuid addDevelopment(MineDevelopment d);
    void updateDevelopment(MineDevelopment d, const std::string& text = "");
    void removeDevelopment(const Uuid& id);

    // Calendars ----------------------------------------------------------------
    Uuid addCalendar(WorkCalendar c);
    void updateCalendar(WorkCalendar c);

    // Scheduling ---------------------------------------------------------------
    Uuid addActivity(ScheduleActivity a);
    void updateActivity(ScheduleActivity a, const std::string& text = "");
    void removeActivity(const Uuid& id);
    Uuid addDependency(ActivityDependency d);
    void removeDependency(const Uuid& id);
    void linkActivityToDevelopment(const Uuid& activityId, const Uuid& developmentId);
    void unlinkActivityFromDevelopment(const Uuid& activityId, const Uuid& developmentId);
    void addBaseline(ScheduleBaseline b);

    // Custom attribute schema --------------------------------------------------
    Uuid addAttributeDefinition(AttributeDefinition def);
    /// Validates every custom attribute against the definitions for `entityType`.
    std::vector<std::string> validateAttributes(const std::string& entityType, const AttributeMap& attrs) const;

private:
    void validateGeometry(const GeometryObject& g) const;
    void validateDevelopment(const MineDevelopment& d) const;
    void validateActivity(const ScheduleActivity& a) const;
    static void stampNew(EntityMetadata& m);
    static void stampModified(EntityMetadata& m);
    Project& p_;
};

} // namespace openmine
