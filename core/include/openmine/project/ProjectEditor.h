#pragma once
// SPDX-License-Identifier: Apache-2.0
// Validated, undoable editing operations on the unified model. This is the only API
// the UI, console, Python bindings and modules use to change project data.
// Every method either fully succeeds (one undo step) or throws ValidationError
// without changing anything.

#include "openmine/project/Project.h"

#include <memory>
#include <string>
#include <vector>

namespace openmine {

class Command;

class ProjectEditor {
public:
    explicit ProjectEditor(Project& p) : p_(p) {}

    // ---- Generic entity operations (all entity types in OPENMINE_ENTITY_TYPES) -------------
    /// Validates and adds a new entity (assigns an id if nil and stamps metadata). One undo step.
    template <class T> Uuid add(T entity, const std::string& text = {});
    /// Validates and replaces an existing entity (revision is incremented). One undo step.
    template <class T> void update(T entity, const std::string& text = {});
    /// Removes an entity applying the documented deletion policy (restrict / cascade / set-null).
    template <class T> void remove(const Uuid& id);
    /// Validates without changing anything (throws ValidationError).
    template <class T> void validate(const T& entity) const;
    /// Builds (without executing) the command that removes `id` with its cascades.
    template <class T> std::unique_ptr<Command> removalCommand(const Uuid& id) const;

    // ---- Project -------------------------------------------------------------------------
    void setProjectInfo(const ProjectInfo& info, const std::string& text = "Edit project settings");

    // ---- Layers ----------------------------------------------------------------------------
    Uuid addLayer(const std::string& name, std::uint32_t colorRgb);
    void updateLayer(const CadLayer& layer) { update(layer); }
    const CadLayer* findLayerByName(const std::string& name) const;

    // ---- Convenience wrappers (kept for readability at call sites) ---------------------------
    Uuid addGeometry(GeometryObject g, const std::string& text = {}) { return add(std::move(g), text); }
    void updateGeometry(GeometryObject g, const std::string& text = {}) { update(std::move(g), text); }
    void removeGeometry(const Uuid& id) { remove<GeometryObject>(id); }
    Uuid addDevelopment(MineDevelopment d) { return add(std::move(d)); }
    void updateDevelopment(MineDevelopment d, const std::string& text = {}) { update(std::move(d), text); }
    void removeDevelopment(const Uuid& id) { remove<MineDevelopment>(id); }
    Uuid addCalendar(WorkCalendar c) { return add(std::move(c)); }
    void updateCalendar(WorkCalendar c) { update(std::move(c)); }
    Uuid addActivity(ScheduleActivity a) { return add(std::move(a)); }
    void updateActivity(ScheduleActivity a, const std::string& text = {}) { update(std::move(a), text); }
    void removeActivity(const Uuid& id) { remove<ScheduleActivity>(id); }
    Uuid addDependency(ActivityDependency d) { return add(std::move(d)); }
    void removeDependency(const Uuid& id) { remove<ActivityDependency>(id); }
    void addBaseline(ScheduleBaseline b) { add(std::move(b)); }
    Uuid addAttributeDefinition(AttributeDefinition def) { return add(std::move(def)); }

    // ---- Design links ------------------------------------------------------------------------
    /// Links an activity to a MineDevelopment, Stope or MiningBlock.
    void linkActivityToDesign(const Uuid& activityId, const Uuid& designId);
    void unlinkActivityFromDesign(const Uuid& activityId, const Uuid& designId);
    void linkActivityToDevelopment(const Uuid& a, const Uuid& d) { linkActivityToDesign(a, d); }
    void unlinkActivityFromDevelopment(const Uuid& a, const Uuid& d) { unlinkActivityFromDesign(a, d); }

    // ---- Custom attribute schema -------------------------------------------------------------
    /// Validates every custom attribute against the definitions for `entityType`.
    std::vector<std::string> validateAttributes(const std::string& entityType, const AttributeMap& attrs) const;

    /// Entity types that carry custom attributes.
    static const std::vector<std::string>& attributeEntityTypes();

    static void stampNew(EntityMetadata& m);
    static void stampModified(EntityMetadata& m);

private:
    Project& p_;
};

#define OPENMINE_DECLARE_EDITOR_SPECIALISATIONS(T, member, Kind)                         \
    template <> void ProjectEditor::validate<T>(const T&) const;                         \
    template <> std::unique_ptr<Command> ProjectEditor::removalCommand<T>(const Uuid&) const;
OPENMINE_ENTITY_TYPES(OPENMINE_DECLARE_EDITOR_SPECIALISATIONS)
#undef OPENMINE_DECLARE_EDITOR_SPECIALISATIONS

} // namespace openmine
