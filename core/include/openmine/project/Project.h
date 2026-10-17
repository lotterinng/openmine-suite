#pragma once
// SPDX-License-Identifier: Apache-2.0
// The Project owns the single, unified in-memory model shared by every module
// (survey, CAD, mine design, scheduling) plus the undo stack and event bus.

#include "openmine/commands/CommandStack.h"
#include "openmine/events/EventBus.h"
#include "openmine/objects/AttributeSchema.h"
#include "openmine/objects/Entities.h"
#include "openmine/project/EntityStore.h"

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
    EntityStore<CadLayer> layers;
    EntityStore<GeometryObject> geometry;
    EntityStore<MineDevelopment> developments;
    EntityStore<WorkCalendar> calendars;
    EntityStore<ScheduleActivity> activities;
    EntityStore<ActivityDependency> dependencies;
    EntityStore<AttributeDefinition> attributeDefinitions;
    std::vector<ActivityDesignLink> designLinks;
    std::vector<ScheduleBaseline> baselines;
    std::vector<AuditEntry> audit; ///< append-only

    // Relationship queries --------------------------------------------------
    std::vector<Uuid> developmentsForActivity(const Uuid& activityId) const;
    std::vector<Uuid> activitiesForDevelopment(const Uuid& developmentId) const;
    std::vector<Uuid> developmentsUsingGeometry(const Uuid& geometryId) const;
    const MineDevelopment* developmentForGeometry(const Uuid& geometryId) const;
};

class Project {
public:
    Project();
    ~Project();
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;

    /// A new project with a default layer and a default (Mon–Fri) calendar.
    static std::unique_ptr<Project> createNew(const std::string& name, Date scheduleStart);

    const ProjectModel& model() const { return model_; }
    /// Direct mutable access is reserved for commands and persistence.
    ProjectModel& mutableModel() { return model_; }

    EventBus& events() { return events_; }
    CommandStack& commands() { return commands_; }
    const CommandStack& commands() const { return commands_; }

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
    std::filesystem::path filePath_;
    std::string userName_;
};

} // namespace openmine
