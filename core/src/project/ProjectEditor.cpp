// SPDX-License-Identifier: Apache-2.0
#include "openmine/project/ProjectEditor.h"

#include "openmine/commands/EntityCommands.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace openmine {

namespace {
template <class T>
std::unique_ptr<Command> setCmd(std::optional<T> before, std::optional<T> after, std::string text,
                                std::optional<std::size_t> pos = std::nullopt) {
    return std::make_unique<SetEntityCommand<T>>(std::move(before), std::move(after), std::move(text), pos);
}
bool finite(double v) { return std::isfinite(v); }
} // namespace

void ProjectEditor::stampNew(EntityMetadata& m) {
    const auto now = utcTimestampNow();
    m.createdAt = now;
    m.modifiedAt = now;
    m.revision = 1;
}

void ProjectEditor::stampModified(EntityMetadata& m) {
    m.modifiedAt = utcTimestampNow();
    ++m.revision;
}

// ---- Project ---------------------------------------------------------------
void ProjectEditor::setProjectInfo(const ProjectInfo& info, const std::string& text) {
    if (info.name.empty()) throw ValidationError("Project name must not be empty");
    if (info.defaultCalendarId && !p_.model().calendars.contains(info.defaultCalendarId))
        throw ValidationError("Default calendar does not exist");
    ProjectInfo after = info;
    after.id = p_.model().info.id;
    stampModified(after.meta);
    p_.commands().execute(std::make_unique<ProjectInfoCommand>(p_.model().info, after, text));
}

// ---- Layers ----------------------------------------------------------------
Uuid ProjectEditor::addLayer(const std::string& name, std::uint32_t colorRgb) {
    if (name.empty()) throw ValidationError("Layer name must not be empty");
    if (findLayerByName(name)) throw ValidationError("Layer '" + name + "' already exists");
    CadLayer l;
    l.id = Uuid::generate();
    l.name = name;
    l.colorRgb = colorRgb & 0xFFFFFF;
    stampNew(l.meta);
    p_.commands().execute(setCmd<CadLayer>(std::nullopt, l, "Create layer '" + name + "'"));
    return l.id;
}

void ProjectEditor::updateLayer(const CadLayer& layer) {
    const auto* old = p_.model().layers.find(layer.id);
    if (!old) throw ValidationError("Layer does not exist");
    if (layer.name.empty()) throw ValidationError("Layer name must not be empty");
    if (const auto* other = findLayerByName(layer.name); other && other->id != layer.id)
        throw ValidationError("Layer '" + layer.name + "' already exists");
    CadLayer after = layer;
    stampModified(after.meta);
    p_.commands().execute(setCmd<CadLayer>(*old, after, "Edit layer '" + layer.name + "'"));
}

const CadLayer* ProjectEditor::findLayerByName(const std::string& name) const {
    for (const auto& l : p_.model().layers.all())
        if (l.name == name) return &l;
    return nullptr;
}

// ---- Geometry ----------------------------------------------------------------
void ProjectEditor::validateGeometry(const GeometryObject& g) const {
    const auto* layer = p_.model().layers.find(g.layerId);
    if (!layer) throw ValidationError("Geometry must be on an existing layer");
    if (layer->locked) throw ValidationError("Layer '" + layer->name + "' is locked");
    for (const auto& v : g.vertices)
        if (!finite(v.x) || !finite(v.y) || !finite(v.z)) throw ValidationError("Vertex coordinates must be finite");
    switch (g.type) {
    case GeometryType::Point:
        if (g.vertices.size() != 1) throw ValidationError("A point has exactly one vertex");
        break;
    case GeometryType::Line:
        if (g.vertices.size() != 2) throw ValidationError("A line has exactly two vertices");
        break;
    case GeometryType::Polyline:
        if (g.vertices.size() < 2) throw ValidationError("A polyline needs at least two vertices");
        break;
    }
    auto errs = validateAttributes("GeometryObject", g.attributes);
    if (!errs.empty()) throw ValidationError(errs.front());
}

Uuid ProjectEditor::addGeometry(GeometryObject g, const std::string& text) {
    if (g.id.isNil()) g.id = Uuid::generate();
    if (p_.model().geometry.contains(g.id)) throw ValidationError("Duplicate geometry id");
    validateGeometry(g);
    if (g.name.empty()) g.name = std::string(toString(g.type)) + " " + std::to_string(p_.model().geometry.size() + 1);
    stampNew(g.meta);
    const Uuid id = g.id;
    p_.commands().execute(setCmd<GeometryObject>(std::nullopt, g, text.empty() ? "Create " + g.name : text));
    return id;
}

void ProjectEditor::updateGeometry(GeometryObject g, const std::string& text) {
    const auto* old = p_.model().geometry.find(g.id);
    if (!old) throw ValidationError("Geometry does not exist");
    validateGeometry(g);
    stampModified(g.meta);
    p_.commands().execute(setCmd<GeometryObject>(*old, g, text.empty() ? "Edit " + g.name : text));
}

void ProjectEditor::removeGeometry(const Uuid& id) {
    const auto* old = p_.model().geometry.find(id);
    if (!old) throw ValidationError("Geometry does not exist");
    // Deletion policy: RESTRICT while a mine design object references the geometry.
    if (!p_.model().developmentsUsingGeometry(id).empty())
        throw ValidationError("'" + old->name + "' is the centreline of a development heading; delete the heading first");
    p_.commands().execute(setCmd<GeometryObject>(*old, std::nullopt, "Delete " + old->name,
                                                 p_.model().geometry.positionOf(id)));
}

// ---- Mine design ---------------------------------------------------------------
void ProjectEditor::validateDevelopment(const MineDevelopment& d) const {
    if (d.headingId.empty()) throw ValidationError("Heading ID must not be empty");
    const auto* g = p_.model().geometry.find(d.geometryId);
    if (!g) throw ValidationError("Development must reference existing centreline geometry");
    if (g->type == GeometryType::Point) throw ValidationError("A development centreline cannot be a point");
    if (!(d.designWidth > 0) || !(d.designHeight > 0) || !finite(d.designWidth) || !finite(d.designHeight))
        throw ValidationError("Design width and height must be positive");
    if (!finite(d.chainageFrom)) throw ValidationError("Chainage must be finite");
    for (const auto& other : p_.model().developments.all()) {
        if (other.id == d.id) continue;
        if (other.headingId == d.headingId) throw ValidationError("Heading ID '" + d.headingId + "' is already used");
        if (other.geometryId == d.geometryId)
            throw ValidationError("That geometry is already the centreline of heading '" + other.headingId + "'");
    }
    auto errs = validateAttributes("MineDevelopment", d.attributes);
    if (!errs.empty()) throw ValidationError(errs.front());
}

Uuid ProjectEditor::addDevelopment(MineDevelopment d) {
    if (d.id.isNil()) d.id = Uuid::generate();
    if (d.name.empty()) d.name = d.headingId;
    validateDevelopment(d);
    stampNew(d.meta);
    const Uuid id = d.id;
    p_.commands().execute(setCmd<MineDevelopment>(std::nullopt, d, "Create heading " + d.headingId));
    return id;
}

void ProjectEditor::updateDevelopment(MineDevelopment d, const std::string& text) {
    const auto* old = p_.model().developments.find(d.id);
    if (!old) throw ValidationError("Development does not exist");
    validateDevelopment(d);
    stampModified(d.meta);
    p_.commands().execute(setCmd<MineDevelopment>(*old, d, text.empty() ? "Edit heading " + d.headingId : text));
}

void ProjectEditor::removeDevelopment(const Uuid& id) {
    const auto* old = p_.model().developments.find(id);
    if (!old) throw ValidationError("Development does not exist");
    auto macro = std::make_unique<MacroCommand>("Delete heading " + old->headingId);
    // Deletion policy: links CASCADE; activities themselves are kept.
    for (const auto& a : p_.model().activitiesForDevelopment(id))
        macro->add(std::make_unique<DesignLinkCommand>(ActivityDesignLink{a, id}, false));
    macro->add(setCmd<MineDevelopment>(*old, std::nullopt, "Delete heading " + old->headingId,
                                       p_.model().developments.positionOf(id)));
    p_.commands().execute(std::move(macro));
}

// ---- Calendars -------------------------------------------------------------------
Uuid ProjectEditor::addCalendar(WorkCalendar c) {
    if (c.id.isNil()) c.id = Uuid::generate();
    if (c.name.empty()) throw ValidationError("Calendar name must not be empty");
    if ((c.workingWeekdays & 0x7F) == 0) throw ValidationError("A calendar needs at least one working weekday");
    stampNew(c.meta);
    const Uuid id = c.id;
    p_.commands().execute(setCmd<WorkCalendar>(std::nullopt, c, "Create calendar '" + c.name + "'"));
    return id;
}

void ProjectEditor::updateCalendar(WorkCalendar c) {
    const auto* old = p_.model().calendars.find(c.id);
    if (!old) throw ValidationError("Calendar does not exist");
    if ((c.workingWeekdays & 0x7F) == 0) throw ValidationError("A calendar needs at least one working weekday");
    stampModified(c.meta);
    p_.commands().execute(setCmd<WorkCalendar>(*old, c, "Edit calendar '" + c.name + "'"));
}

// ---- Scheduling --------------------------------------------------------------------
void ProjectEditor::validateActivity(const ScheduleActivity& a) const {
    if (a.name.empty()) throw ValidationError("Activity name must not be empty");
    if (!finite(a.durationDays) || a.durationDays < 0) throw ValidationError("Duration must be zero or positive");
    if (!finite(a.quantity) || a.quantity < 0) throw ValidationError("Quantity must be zero or positive");
    if (!finite(a.actualQuantity) || a.actualQuantity < 0) throw ValidationError("Actual quantity must be zero or positive");
    if (a.durationFromRate && (!finite(a.ratePerDay) || a.ratePerDay <= 0))
        throw ValidationError("A rate-driven activity needs a production rate greater than zero");
    if (a.calendarId && !p_.model().calendars.contains(a.calendarId)) throw ValidationError("Calendar does not exist");
    if (a.parentId) {
        if (a.parentId == a.id) throw ValidationError("An activity cannot be its own parent");
        const auto* parent = p_.model().activities.find(a.parentId);
        if (!parent || parent->kind != ActivityKind::Summary)
            throw ValidationError("WBS parent must be an existing summary activity");
        // Guard against WBS cycles.
        Uuid cur = parent->parentId;
        while (cur) {
            if (cur == a.id) throw ValidationError("WBS hierarchy would contain a cycle");
            const auto* x = p_.model().activities.find(cur);
            cur = x ? x->parentId : Uuid{};
        }
    }
}

Uuid ProjectEditor::addActivity(ScheduleActivity a) {
    if (a.id.isNil()) a.id = Uuid::generate();
    if (a.kind == ActivityKind::Milestone) {
        a.durationDays = 0.0;
        a.durationFromRate = false;
    }
    validateActivity(a);
    if (a.sortOrder == 0) {
        int maxOrder = 0;
        for (const auto& x : p_.model().activities.all()) maxOrder = std::max(maxOrder, x.sortOrder);
        a.sortOrder = maxOrder + 10;
    }
    stampNew(a.meta);
    const Uuid id = a.id;
    p_.commands().execute(setCmd<ScheduleActivity>(std::nullopt, a, "Create activity '" + a.name + "'"));
    return id;
}

void ProjectEditor::updateActivity(ScheduleActivity a, const std::string& text) {
    const auto* old = p_.model().activities.find(a.id);
    if (!old) throw ValidationError("Activity does not exist");
    if (a.kind == ActivityKind::Milestone) {
        a.durationDays = 0.0;
        a.durationFromRate = false;
    }
    validateActivity(a);
    if (old->kind == ActivityKind::Summary && a.kind != ActivityKind::Summary)
        for (const auto& x : p_.model().activities.all())
            if (x.parentId == a.id) throw ValidationError("Activity has WBS children and must remain a summary");
    stampModified(a.meta);
    p_.commands().execute(setCmd<ScheduleActivity>(*old, a, text.empty() ? "Edit activity '" + a.name + "'" : text));
}

void ProjectEditor::removeActivity(const Uuid& id) {
    const auto* old = p_.model().activities.find(id);
    if (!old) throw ValidationError("Activity does not exist");
    for (const auto& x : p_.model().activities.all())
        if (x.parentId == id) throw ValidationError("Delete or move the WBS children of '" + old->name + "' first");
    auto macro = std::make_unique<MacroCommand>("Delete activity '" + old->name + "'");
    for (const auto& d : p_.model().dependencies.all())
        if (d.predecessorId == id || d.successorId == id)
            macro->add(setCmd<ActivityDependency>(d, std::nullopt, "Delete dependency", p_.model().dependencies.positionOf(d.id)));
    for (const auto& dev : p_.model().developmentsForActivity(id))
        macro->add(std::make_unique<DesignLinkCommand>(ActivityDesignLink{id, dev}, false));
    macro->add(setCmd<ScheduleActivity>(*old, std::nullopt, "Delete activity '" + old->name + "'",
                                        p_.model().activities.positionOf(id)));
    p_.commands().execute(std::move(macro));
}

Uuid ProjectEditor::addDependency(ActivityDependency d) {
    const auto& m = p_.model();
    const auto* pred = m.activities.find(d.predecessorId);
    const auto* succ = m.activities.find(d.successorId);
    if (!pred || !succ) throw ValidationError("Both activities of a dependency must exist");
    if (d.predecessorId == d.successorId) throw ValidationError("An activity cannot depend on itself");
    if (pred->kind == ActivityKind::Summary || succ->kind == ActivityKind::Summary)
        throw ValidationError("Dependencies on summary (WBS) activities are not supported yet");
    if (!finite(d.lagDays)) throw ValidationError("Lag must be finite");
    for (const auto& e : m.dependencies.all())
        if (e.predecessorId == d.predecessorId && e.successorId == d.successorId)
            throw ValidationError("A dependency between these activities already exists");
    // Reject if successor already reaches predecessor (would close a cycle).
    std::set<Uuid> seen;
    std::function<bool(const Uuid&)> reaches = [&](const Uuid& from) {
        if (from == d.predecessorId) return true;
        if (!seen.insert(from).second) return false;
        for (const auto& e : m.dependencies.all())
            if (e.predecessorId == from && reaches(e.successorId)) return true;
        return false;
    };
    if (reaches(d.successorId)) throw ValidationError("Dependency would create a circular logic loop");
    if (d.id.isNil()) d.id = Uuid::generate();
    stampNew(d.meta);
    const Uuid id = d.id;
    p_.commands().execute(setCmd<ActivityDependency>(std::nullopt, d,
        std::string("Add ") + toString(d.type) + " dependency '" + pred->name + "' → '" + succ->name + "'"));
    return id;
}

void ProjectEditor::removeDependency(const Uuid& id) {
    const auto* old = p_.model().dependencies.find(id);
    if (!old) throw ValidationError("Dependency does not exist");
    p_.commands().execute(setCmd<ActivityDependency>(*old, std::nullopt, "Delete dependency",
                                                     p_.model().dependencies.positionOf(id)));
}

void ProjectEditor::linkActivityToDevelopment(const Uuid& activityId, const Uuid& developmentId) {
    if (!p_.model().activities.contains(activityId)) throw ValidationError("Activity does not exist");
    if (!p_.model().developments.contains(developmentId)) throw ValidationError("Development does not exist");
    const ActivityDesignLink link{activityId, developmentId};
    const auto& links = p_.model().designLinks;
    if (std::find(links.begin(), links.end(), link) != links.end()) throw ValidationError("Already linked");
    p_.commands().execute(std::make_unique<DesignLinkCommand>(link, true));
}

void ProjectEditor::unlinkActivityFromDevelopment(const Uuid& activityId, const Uuid& developmentId) {
    const ActivityDesignLink link{activityId, developmentId};
    const auto& links = p_.model().designLinks;
    if (std::find(links.begin(), links.end(), link) == links.end()) throw ValidationError("Not linked");
    p_.commands().execute(std::make_unique<DesignLinkCommand>(link, false));
}

void ProjectEditor::addBaseline(ScheduleBaseline b) {
    if (b.name.empty()) throw ValidationError("Baseline name must not be empty");
    for (const auto& x : p_.model().baselines)
        if (x.name == b.name) throw ValidationError("A baseline named '" + b.name + "' already exists");
    if (b.id.isNil()) b.id = Uuid::generate();
    if (b.capturedAt.empty()) b.capturedAt = utcTimestampNow();
    p_.commands().execute(std::make_unique<AddBaselineCommand>(std::move(b)));
}

// ---- Attribute schema ------------------------------------------------------------
Uuid ProjectEditor::addAttributeDefinition(AttributeDefinition def) {
    if (def.entityType != "GeometryObject" && def.entityType != "MineDevelopment")
        throw ValidationError("Custom attributes are supported on GeometryObject and MineDevelopment");
    if (!isValidAttributeKey(def.key)) throw ValidationError("Invalid attribute key '" + def.key + "'");
    for (const auto& d : p_.model().attributeDefinitions.all())
        if (d.entityType == def.entityType && d.key == def.key) throw ValidationError("Attribute '" + def.key + "' already defined");
    if (def.displayName.empty()) def.displayName = def.key;
    if (!def.defaultValue.empty()) {
        auto err = validateAttributeValue(def, def.defaultValue);
        if (!err.empty()) throw ValidationError("Default value: " + err);
    }
    if (def.id.isNil()) def.id = Uuid::generate();
    const Uuid id = def.id;
    p_.commands().execute(setCmd<AttributeDefinition>(std::nullopt, def, "Define attribute '" + def.key + "'"));
    return id;
}

std::vector<std::string> ProjectEditor::validateAttributes(const std::string& entityType, const AttributeMap& attrs) const {
    std::vector<std::string> errors;
    for (const auto& def : p_.model().attributeDefinitions.all()) {
        if (def.entityType != entityType) continue;
        auto it = attrs.find(def.key);
        const std::string value = it == attrs.end() ? std::string{} : it->second;
        auto err = validateAttributeValue(def, value);
        if (!err.empty()) errors.push_back(err);
    }
    for (const auto& [key, _] : attrs) {
        bool known = false;
        for (const auto& def : p_.model().attributeDefinitions.all())
            if (def.entityType == entityType && def.key == key) known = true;
        if (!known) errors.push_back("Attribute '" + key + "' is not defined in the project schema");
    }
    return errors;
}

} // namespace openmine
