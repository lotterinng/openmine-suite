// SPDX-License-Identifier: Apache-2.0
#include "openmine/project/Project.h"

#include <algorithm>
#include <cstdlib>

namespace openmine {

const char* toString(EntityKind k) {
    switch (k) {
    case EntityKind::ProjectInfo: return "Project";
    case EntityKind::DesignLink: return "ActivityDesignLink";
#define OPENMINE_KIND_NAME(T, member, Kind)                                                                            \
    case EntityKind::Kind: return #T;
        OPENMINE_ENTITY_TYPES(OPENMINE_KIND_NAME)
#undef OPENMINE_KIND_NAME
    }
    return "?";
}

std::vector<Uuid> ProjectModel::designsForActivity(const Uuid& activityId) const {
    std::vector<Uuid> out;
    for (const auto& l : designLinks)
        if (l.activityId == activityId) out.push_back(l.designId);
    return out;
}

std::vector<Uuid> ProjectModel::developmentsForActivity(const Uuid& activityId) const {
    std::vector<Uuid> out;
    for (const auto& l : designLinks)
        if (l.activityId == activityId && developments.contains(l.designId)) out.push_back(l.designId);
    return out;
}

std::vector<Uuid> ProjectModel::activitiesForDesign(const Uuid& designId) const {
    std::vector<Uuid> out;
    for (const auto& l : designLinks)
        if (l.designId == designId) out.push_back(l.activityId);
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

std::vector<const ActivityResourceAssignment*> ProjectModel::assignmentsForActivity(const Uuid& activityId) const {
    std::vector<const ActivityResourceAssignment*> out;
    for (const auto& a : assignments.all())
        if (a.activityId == activityId) out.push_back(&a);
    return out;
}

std::vector<const ProductionActual*> ProjectModel::actualsForActivity(const Uuid& activityId) const {
    std::vector<const ProductionActual*> out;
    for (const auto& a : actuals.all())
        if (a.activityId == activityId) out.push_back(&a);
    std::sort(out.begin(), out.end(), [](auto* x, auto* y) { return x->date < y->date; });
    return out;
}

std::vector<std::pair<std::string, Uuid>> ProjectModel::referencesToGeometry(const Uuid& gid) const {
    std::vector<std::pair<std::string, Uuid>> out;
    for (const auto& d : developments.all())
        if (d.geometryId == gid) out.emplace_back("MineDevelopment " + d.headingId, d.id);
    for (const auto& s : stopes.all())
        if (s.geometryId == gid) out.emplace_back("Stope " + s.name, s.id);
    for (const auto& b : miningBlocks.all())
        if (b.geometryId == gid) out.emplace_back("MiningBlock " + b.name, b.id);
    for (const auto& a : asBuiltModels.all())
        if (a.geometryId == gid) out.emplace_back("AsBuiltModel " + a.name, a.id);
    for (const auto& g : geologicalObjects.all())
        if (g.geometryId == gid) out.emplace_back("GeologicalObject " + g.name, g.id);
    return out;
}

std::vector<Uuid> ProjectModel::referencedAssets() const {
    std::vector<Uuid> out;
    auto add = [&](const AssetRef& r) {
        if (!r.isNull()) out.push_back(r.id);
    };
    for (const auto& g : geometry.all()) add(g.brepAsset);
    for (const auto& p : pointClouds.all()) add(p.asset);
    for (const auto& a : attachments.all()) add(a.asset);
    for (const auto& a : analysisResults.all()) add(a.payloadAsset);
    for (const auto& r : reports.all()) add(r.asset);
    for (const auto& b : blockModels.all()) add(b.dataAsset);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const WorkCalendar* ProjectModel::calendarFor(const ScheduleActivity& a) const {
    if (a.calendarId)
        if (const auto* c = calendars.find(a.calendarId)) return c;
    return calendars.find(info.defaultCalendarId);
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
    const EntityMetadata meta{now, now, 1};
    auto& m = p->model_;
    m.info.id = Uuid::generate();
    m.info.name = name;
    m.info.scheduleStart = DateTime::fromDate(scheduleStart);
    m.info.meta = meta;
    p->assets_ = AssetStore(AssetStore::temporaryRoot(m.info.id));

    CoordinateReferenceSystem crs;
    crs.id = Uuid::generate();
    crs.name = "Unspecified (declare the project CRS)";
    crs.kind = CrsKind::Unknown;
    crs.meta = meta;
    m.crs.upsert(crs);
    m.info.projectCrsId = crs.id;

    auto layer = [&](const char* n, std::uint32_t rgb) {
        CadLayer l;
        l.id = Uuid::generate();
        l.name = n;
        l.colorRgb = rgb;
        l.meta = meta;
        m.layers.upsert(l);
    };
    layer("0", 0xD0D0D0);
    layer("Development Centrelines", 0x3FA9F5);

    WorkCalendar cal;
    cal.id = Uuid::generate();
    cal.name = "Standard 5-day";
    cal.kind = CalendarKind::Project;
    cal.shifts = {ShiftDefinition{"Day", 0, 1440, 0b0011111}};
    cal.standardHoursPerDay = 24.0;
    cal.meta = meta;
    m.calendars.upsert(cal);
    m.info.defaultCalendarId = cal.id;

    WorkCalendar cal7;
    cal7.id = Uuid::generate();
    cal7.name = "Continuous 7-day";
    cal7.shifts = {ShiftDefinition{"Day", 0, 1440, 0b1111111}};
    cal7.standardHoursPerDay = 24.0;
    cal7.meta = meta;
    m.calendars.upsert(cal7);

    WorkCalendar shifts;
    shifts.id = Uuid::generate();
    shifts.name = "2 × 10 h shifts, 7 days";
    shifts.kind = CalendarKind::Shift;
    shifts.shifts = {ShiftDefinition{"Day shift", 6 * 60, 10 * 60, 0b1111111},
                     ShiftDefinition{"Night shift", 18 * 60, 10 * 60, 0b1111111}};
    shifts.standardHoursPerDay = 20.0;
    shifts.meta = meta;
    m.calendars.upsert(shifts);

    Material waste;
    waste.id = Uuid::generate();
    waste.name = "Waste rock";
    waste.density = 2.75;
    waste.swellFactor = 1.4;
    waste.meta = meta;
    m.materials.upsert(waste);

    Material ore;
    ore.id = Uuid::generate();
    ore.name = "Ore";
    ore.density = 2.9;
    ore.swellFactor = 1.45;
    ore.colorRgb = 0xC9A227;
    ore.meta = meta;
    m.materials.upsert(ore);

    TunnelProfile prof;
    prof.id = Uuid::generate();
    prof.name = "5.5 × 5.5 m shoulder arch";
    prof.shape = ProfileShape::ShoulderArch;
    prof.width = 5.5;
    prof.height = 5.5;
    prof.shoulderRadius = 1.5;
    prof.meta = meta;
    m.profiles.upsert(prof);

    p->appendAudit("Create", "Project", m.info.id, "Project '" + name + "' created");
    return p;
}

void Project::appendAudit(std::string action, std::string entityType, Uuid id, std::string detail) {
    model_.audit.push_back({utcTimestampNow(), userName_, std::move(action), std::move(entityType), id, std::move(detail)});
}

} // namespace openmine
