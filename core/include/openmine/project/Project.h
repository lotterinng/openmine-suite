#pragma once
// SPDX-License-Identifier: Apache-2.0
// The Project owns the single, unified in-memory model shared by every module
// (survey, CAD, mine design, scheduling, geology), the undo stack, the event bus
// and the sidecar asset store.

#include "openmine/assets/AssetStore.h"
#include "openmine/commands/CommandStack.h"
#include "openmine/events/EventBus.h"
#include "openmine/project/EntityStore.h"
#include "openmine/project/EntityTypes.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace openmine {

/// Raised when a requested edit would violate model integrity. Nothing is changed.
class ValidationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ProjectModel {
    ProjectInfo info;
#define OPENMINE_DECLARE_STORE(T, member, Kind) EntityStore<T> member;
    OPENMINE_ENTITY_TYPES(OPENMINE_DECLARE_STORE)
#undef OPENMINE_DECLARE_STORE
    std::vector<ActivityDesignLink> designLinks;
    std::vector<AuditEntry> audit; ///< append-only

    // Relationship queries --------------------------------------------------
    /// Design objects (any type) linked to an activity.
    std::vector<Uuid> designsForActivity(const Uuid& activityId) const;
    /// MineDevelopment ids linked to an activity.
    std::vector<Uuid> developmentsForActivity(const Uuid& activityId) const;
    std::vector<Uuid> activitiesForDesign(const Uuid& designId) const;
    std::vector<Uuid> activitiesForDevelopment(const Uuid& developmentId) const { return activitiesForDesign(developmentId); }
    std::vector<Uuid> developmentsUsingGeometry(const Uuid& geometryId) const;
    const MineDevelopment* developmentForGeometry(const Uuid& geometryId) const;
    std::vector<const ActivityResourceAssignment*> assignmentsForActivity(const Uuid& activityId) const;
    std::vector<const ProductionActual*> actualsForActivity(const Uuid& activityId) const;
    /// Which entities reference a geometry object (for deletion policies and UI).
    std::vector<std::pair<std::string, Uuid>> referencesToGeometry(const Uuid& geometryId) const;
    /// All asset ids referenced by entities.
    std::vector<Uuid> referencedAssets() const;
    const CoordinateReferenceSystem* projectCrs() const { return crs.find(info.projectCrsId); }
    const WorkCalendar* calendarFor(const ScheduleActivity& a) const;
};

class Project {
public:
    Project();
    ~Project();
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;

    /// A new project with default layers, calendars, material, profile and project CRS placeholder.
    static std::unique_ptr<Project> createNew(const std::string& name, Date scheduleStart);

    const ProjectModel& model() const { return model_; }
    /// Direct mutable access is reserved for commands and persistence.
    ProjectModel& mutableModel() { return model_; }

    EventBus& events() { return events_; }
    CommandStack& commands() { return commands_; }
    const CommandStack& commands() const { return commands_; }

    AssetStore& assets() { return assets_; }
    const AssetStore& assets() const { return assets_; }

    const std::filesystem::path& filePath() const { return filePath_; }
    void setFilePath(std::filesystem::path p) { filePath_ = std::move(p); }

    bool isModified() const { return !commands_.isClean(); }

    const std::string& userName() const { return userName_; }
    void setUserName(std::string u) { userName_ = std::move(u); }

    void appendAudit(std::string action, std::string entityType, Uuid id, std::string detail);

private:
    ProjectModel model_;
    EventBus events_;
    CommandStack commands_;
    AssetStore assets_;
    std::filesystem::path filePath_;
    std::string userName_;
};

} // namespace openmine
