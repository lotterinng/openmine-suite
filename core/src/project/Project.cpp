// SPDX-License-Identifier: Apache-2.0
#include "openmine/project/Project.h"

#include <cstdlib>

namespace openmine {

const char* toString(EntityKind k) {
    switch (k) {
    case EntityKind::ProjectInfo: return "Project";
    case EntityKind::Layer: return "CadLayer";
    case EntityKind::Geometry: return "GeometryObject";
    case EntityKind::Development: return "MineDevelopment";
    case EntityKind::Calendar: return "WorkCalendar";
    case EntityKind::Activity: return "ScheduleActivity";
    case EntityKind::Dependency: return "ActivityDependency";
    case EntityKind::DesignLink: return "ActivityDesignLink";
    case EntityKind::Baseline: return "ScheduleBaseline";
    case EntityKind::AttributeDefinition: return "AttributeDefinition";
    }
    return "?";
}

std::vector<Uuid> ProjectModel::developmentsForActivity(const Uuid& activityId) const {
    std::vector<Uuid> out;
    for (const auto& l : designLinks)
        if (l.activityId == activityId) out.push_back(l.developmentId);
    return out;
}

std::vector<Uuid> ProjectModel::activitiesForDevelopment(const Uuid& developmentId) const {
    std::vector<Uuid> out;
    for (const auto& l : designLinks)
        if (l.developmentId == developmentId) out.push_back(l.activityId);
    return out;
}

std::vector<Uuid> ProjectModel::developmentsUsingGeometry(const Uuid& geometryId) const {
    std::vector<Uuid> out;
    for (const auto& d : developments.all())
        if (d.geometryId == geometryId) out.push_back(d.id);
    return out;
}

const MineDevelopment* ProjectModel::developmentForGeometry(const Uuid& geometryId) const {
    for (const auto& d : developments.all())
        if (d.geometryId == geometryId) return &d;
    return nullptr;
}

Project::Project() : commands_(*this) {
    if (const char* u = std::getenv("USERNAME")) userName_ = u;
    else if (const char* u2 = std::getenv("USER")) userName_ = u2;
    else userName_ = "unknown";
}

Project::~Project() = default;

std::unique_ptr<Project> Project::createNew(const std::string& name, Date scheduleStart) {
    auto p = std::make_unique<Project>();
    const std::string now = utcTimestampNow();
    auto& m = p->model_;
    m.info.id = Uuid::generate();
    m.info.name = name;
    m.info.scheduleStart = scheduleStart;
    m.info.meta = {now, now, 1};

    CadLayer layer;
    layer.id = Uuid::generate();
    layer.name = "0";
    layer.colorRgb = 0xD0D0D0;
    layer.meta = {now, now, 1};
    m.layers.upsert(layer);

    CadLayer dev;
    dev.id = Uuid::generate();
    dev.name = "Development Centrelines";
    dev.colorRgb = 0x3FA9F5;
    dev.meta = {now, now, 1};
    m.layers.upsert(dev);

    WorkCalendar cal;
    cal.id = Uuid::generate();
    cal.name = "Standard 5-day";
    cal.meta = {now, now, 1};
    m.calendars.upsert(cal);

    WorkCalendar cal7;
    cal7.id = Uuid::generate();
    cal7.name = "Continuous 7-day";
    cal7.workingWeekdays = 0b1111111;
    cal7.meta = {now, now, 1};
    m.calendars.upsert(cal7);
    m.info.defaultCalendarId = cal.id;

    p->appendAudit("Create", "Project", m.info.id, "Project '" + name + "' created");
    return p;
}

void Project::appendAudit(std::string action, std::string entityType, Uuid id, std::string detail) {
    model_.audit.push_back({utcTimestampNow(), userName_, std::move(action), std::move(entityType), id, std::move(detail)});
}

} // namespace openmine
