// SPDX-License-Identifier: Apache-2.0
#include "openmine/project/ProjectEditor.h"

#include "openmine/commands/EntityCommands.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace openmine {

namespace {
template <class T> std::string displayName(const T& e) {
    if constexpr (requires { e.headingId; }) {
        if (!e.headingId.empty()) return e.headingId;
    }
    if constexpr (requires { e.holeId; }) {
        if (!e.holeId.empty()) return e.holeId;
    }
    if constexpr (requires { e.name; }) {
        if (!e.name.empty()) return e.name;
    }
    if constexpr (requires { e.key; }) return e.key;
    return e.id.toString().substr(0, 8);
}

template <class T> std::unique_ptr<Command> setCmd(std::optional<T> before, std::optional<T> after, std::string text,
                                                   std::optional<std::size_t> pos = std::nullopt) {
    return std::make_unique<SetEntityCommand<T>>(std::move(before), std::move(after), std::move(text), pos);
}

// ---- Defaults applied before validation on add/update ----------------------------------------
template <class T> void normalize(const Project&, T&) {}

void normalize(const Project& p, GeometryObject& g) {
    if (g.name.empty()) g.name = std::string(toString(g.type)) + " " + std::to_string(p.model().geometry.size() + 1);
    if (g.type == GeometryType::Circle) g.arc.endAngle = g.arc.startAngle + 2 * 3.14159265358979323846;
}

void normalize(const Project& p, ScheduleActivity& a) {
    if (a.kind == ActivityKind::Milestone) {
        a.durationHours = 0.0;
        a.durationFromRate = false;
        a.useCycleRate = false;
    }
    if (a.useCycleRate) a.durationFromRate = true;
    if (a.sortOrder == 0) {
        int maxOrder = 0;
        for (const auto& x : p.model().activities.all()) maxOrder = std::max(maxOrder, x.sortOrder);
        a.sortOrder = maxOrder + 10;
    }
}

void normalize(const Project&, MineDevelopment& d) {
    if (d.name.empty()) d.name = d.headingId;
}

void normalize(const Project&, ScheduleBaseline& b) {
    if (b.capturedAt.empty()) b.capturedAt = utcTimestampNow();
}

void normalize(const Project& p, TransformationRecord& t) {
    if (t.createdAt.empty()) t.createdAt = utcTimestampNow();
    if (t.user.empty()) t.user = p.userName();
}

void normalize(const Project& p, AnalysisResult& a) {
    if (a.createdAt.empty()) a.createdAt = utcTimestampNow();
    if (a.user.empty()) a.user = p.userName();
}

void normalize(const Project&, Report& r) {
    if (r.createdAt.empty()) r.createdAt = utcTimestampNow();
}

void normalize(const Project& p, DesignRevision& r) {
    if (r.createdAt.empty()) r.createdAt = utcTimestampNow();
    if (r.author.empty()) r.author = p.userName();
    if (r.approved && r.approvedAt.empty()) {
        r.approvedAt = utcTimestampNow();
        if (r.approvedBy.empty()) r.approvedBy = p.userName();
    }
}

void normalize(const Project&, AttributeDefinition& d) {
    if (d.displayName.empty()) d.displayName = d.key;
}

void normalize(const Project& p, ProductionActual& a) {
    if (a.enteredBy.empty()) a.enteredBy = p.userName();
}

void normalize(const Project&, Drillhole& h) {
    std::sort(h.surveys.begin(), h.surveys.end(), [](auto& a, auto& b) { return a.depth < b.depth; });
    std::sort(h.intervals.begin(), h.intervals.end(), [](auto& a, auto& b) { return a.from < b.from; });
}
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

const std::vector<std::string>& ProjectEditor::attributeEntityTypes() {
    static const std::vector<std::string> v{"GeometryObject", "MineDevelopment", "Stope", "ScheduleActivity"};
    return v;
}

// ---- Generic operations ---------------------------------------------------------------------
template <class T> Uuid ProjectEditor::add(T e, const std::string& text) {
    auto& store = EntityTraits<T>::store(p_.mutableModel());
    if (e.id.isNil()) e.id = Uuid::generate();
    if (store.contains(e.id)) throw ValidationError(std::string("Duplicate ") + EntityTraits<T>::name + " id");
    normalize(p_, e);
    validate(e);
    if constexpr (requires { e.meta; }) stampNew(e.meta);
    const Uuid id = e.id;
    const std::string label = text.empty() ? "Create " + displayName(e) : text;
    p_.commands().execute(setCmd<T>(std::nullopt, std::move(e), label));
    return id;
}

template <class T> void ProjectEditor::update(T e, const std::string& text) {
    const auto& store = EntityTraits<T>::store(p_.model());
    const T* old = store.find(e.id);
    if (!old) throw ValidationError(std::string(EntityTraits<T>::name) + " does not exist");
    normalize(p_, e);
    validate(e);
    if constexpr (requires { e.meta; }) {
        e.meta.createdAt = old->meta.createdAt;
        e.meta.revision = old->meta.revision;
        stampModified(e.meta);
    }
    const std::string label = text.empty() ? "Edit " + displayName(e) : text;
    p_.commands().execute(setCmd<T>(*old, std::move(e), label));
}

template <class T> void ProjectEditor::remove(const Uuid& id) { p_.commands().execute(removalCommand<T>(id)); }

#define OPENMINE_INSTANTIATE_EDITOR(T, member, Kind)                                     \
    template Uuid ProjectEditor::add<T>(T, const std::string&);                          \
    template void ProjectEditor::update<T>(T, const std::string&);                       \
    template void ProjectEditor::remove<T>(const Uuid&);
OPENMINE_ENTITY_TYPES(OPENMINE_INSTANTIATE_EDITOR)
#undef OPENMINE_INSTANTIATE_EDITOR

// ---- Project ----------------------------------------------------------------------------------
Uuid ProjectEditor::ensureLayer(const std::string& name, std::uint32_t color) {
    if (const auto* l = findLayerByName(name); l && !l->locked) return l->id;
    return addLayer(name, color);
}

Uuid ProjectEditor::addAnalysis(AnalysisKind kind, const std::string& name, const std::string& inputsJson, const std::string& parametersJson,
                                const std::string& summaryJson, const std::string& payloadJson) {
    AnalysisResult r;
    r.kind = kind;
    r.name = name;
    r.inputsJson = inputsJson;
    r.parametersJson = parametersJson;
    r.summaryJson = summaryJson;
    r.payloadJson = payloadJson;
    r.createdAt = utcTimestampNow();
    r.user = p_.userName();
    return add(r, "Store analysis: " + name);
}

bool ProjectEditor::ensureAttributeDefinition(const std::string& entityType, const std::string& key, const std::string& displayName, AttributeValueType type,
                                              const std::string& unit) {
    for (const auto& d : p_.model().attributeDefinitions.all())
        if (d.entityType == entityType && d.key == key) return false;
    AttributeDefinition def;
    def.entityType = entityType;
    def.key = key;
    def.displayName = displayName.empty() ? key : displayName;
    def.valueType = type;
    def.unit = unit;
    addAttributeDefinition(def);
    return true;
}

void ProjectEditor::setProjectInfo(const ProjectInfo& info, const std::string& text) {
    const auto& m = p_.model();
    if (info.name.empty()) throw ValidationError("Project name must not be empty");
    if (info.defaultCalendarId && !m.calendars.contains(info.defaultCalendarId))
        throw ValidationError("Default calendar does not exist");
    if (info.projectCrsId && !m.crs.contains(info.projectCrsId)) throw ValidationError("Project CRS does not exist");
    if (info.coordinateDisplayDecimals < 0 || info.coordinateDisplayDecimals > 6)
        throw ValidationError("Coordinate display precision must be 0–6 decimals");
    ProjectInfo after = info;
    after.id = m.info.id;
    after.meta.createdAt = m.info.meta.createdAt;
    after.meta.revision = m.info.meta.revision;
    stampModified(after.meta);
    p_.commands().execute(std::make_unique<ProjectInfoCommand>(m.info, after, text));
}

// ---- Layers -----------------------------------------------------------------------------------
Uuid ProjectEditor::addLayer(const std::string& name, std::uint32_t colorRgb) {
    CadLayer l;
    l.name = name;
    l.colorRgb = colorRgb & 0xFFFFFF;
    return add(l, "Create layer '" + name + "'");
}

const CadLayer* ProjectEditor::findLayerByName(const std::string& name) const {
    for (const auto& l : p_.model().layers.all())
        if (l.name == name) return &l;
    return nullptr;
}

// ---- Links ------------------------------------------------------------------------------------
void ProjectEditor::linkActivityToDesign(const Uuid& activityId, const Uuid& designId) {
    const auto& m = p_.model();
    const auto* a = m.activities.find(activityId);
    if (!a) throw ValidationError("Activity does not exist");
    if (a->kind == ActivityKind::Summary) throw ValidationError("Link design objects to task activities, not WBS summaries");
    std::string type;
    if (m.developments.contains(designId)) type = "MineDevelopment";
    else if (m.stopes.contains(designId)) type = "Stope";
    else if (m.miningBlocks.contains(designId)) type = "MiningBlock";
    else throw ValidationError("Design object does not exist");
    for (const auto& l : m.designLinks)
        if (l.activityId == activityId && l.designId == designId) throw ValidationError("Already linked");
    p_.commands().execute(std::make_unique<DesignLinkCommand>(ActivityDesignLink{activityId, designId, type}, true));
}

void ProjectEditor::unlinkActivityFromDesign(const Uuid& activityId, const Uuid& designId) {
    for (const auto& l : p_.model().designLinks)
        if (l.activityId == activityId && l.designId == designId) {
            p_.commands().execute(std::make_unique<DesignLinkCommand>(l, false));
            return;
        }
    throw ValidationError("Not linked");
}

// ---- Attributes -------------------------------------------------------------------------------
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

// ======================================================================================================
// Deletion policies. Each builds one MacroCommand (one undo step). Policies are documented in
// docs/architecture/database-schema.md and mirrored by SQL foreign keys.
// ======================================================================================================
namespace {
class RemovalBuilder {
public:
    RemovalBuilder(const Project& p, std::string text) : p_(p), macro_(std::make_unique<MacroCommand>(std::move(text))) {}

    template <class T> void erase(const T& e) {
        if (!erased_.insert(e.id).second) return;
        const auto& store = EntityTraits<T>::store(p_.model());
        macro_->add(setCmd<T>(e, std::nullopt, "Delete " + displayName(e), store.positionOf(e.id)));
    }
    /// Replaces an entity with a modified copy (set-null cascades).
    template <class T, class F> void modify(const T& e, F&& f) {
        T copy = e;
        f(copy);
        ProjectEditor::stampModified(copy.meta);
        macro_->add(setCmd<T>(e, copy, "Update " + displayName(e)));
    }
    void unlink(const ActivityDesignLink& l) { macro_->add(std::make_unique<DesignLinkCommand>(l, false)); }
    void unlinkAllFor(const Uuid& id) {
        for (const auto& l : p_.model().designLinks)
            if (l.activityId == id || l.designId == id) unlink(l);
    }
    /// The command; attachments of every erased entity go with it (and come back on undo).
    std::unique_ptr<Command> take() {
        std::vector<const Attachment*> owned;
        for (const auto& a : p_.model().attachments.all())
            if (!a.entityId.isNil() && erased_.count(a.entityId) && !erased_.count(a.id)) owned.push_back(&a);
        for (const auto* a : owned) erase(*a);
        return std::move(macro_);
    }

private:
    const Project& p_;
    std::unique_ptr<MacroCommand> macro_;
    std::set<Uuid> erased_;
};

template <class T> const T& requireExisting(const Project& p, const Uuid& id) {
    const auto* e = EntityTraits<T>::store(p.model()).find(id);
    if (!e) throw ValidationError(std::string(EntityTraits<T>::name) + " does not exist");
    return *e;
}

[[noreturn]] void restrict(const std::string& m) { throw ValidationError(m); }

template <class T> std::unique_ptr<Command> simpleRemoval(const Project& p, const Uuid& id) {
    const auto& e = requireExisting<T>(p, id);
    RemovalBuilder b(p, "Delete " + displayName(e));
    b.erase(e);
    return b.take();
}

void requireAssetUnreferenced(const Project& p, const Uuid& id) {
    const auto refs = p.model().referencedAssets();
    if (std::binary_search(refs.begin(), refs.end(), id)) restrict("The asset is still used by a project object");
}
} // namespace

#define OPENMINE_SIMPLE_REMOVAL(T)                                                                    \
    template <> std::unique_ptr<Command> ProjectEditor::removalCommand<T>(const Uuid& id) const {   \
        return simpleRemoval<T>(p_, id);                                                             \
    }
OPENMINE_SIMPLE_REMOVAL(Attachment)
OPENMINE_SIMPLE_REMOVAL(Report)
OPENMINE_SIMPLE_REMOVAL(ActivityDependency)
OPENMINE_SIMPLE_REMOVAL(ActivityResourceAssignment)
OPENMINE_SIMPLE_REMOVAL(ProductionActual)
OPENMINE_SIMPLE_REMOVAL(ProductionTarget)
OPENMINE_SIMPLE_REMOVAL(Scenario)
OPENMINE_SIMPLE_REMOVAL(Drillhole)
OPENMINE_SIMPLE_REMOVAL(GeologicalObject)
OPENMINE_SIMPLE_REMOVAL(BlockModel)
OPENMINE_SIMPLE_REMOVAL(StructuralMeasurement)
OPENMINE_SIMPLE_REMOVAL(GeotechClassification)
OPENMINE_SIMPLE_REMOVAL(DesignConformanceResult)
#undef OPENMINE_SIMPLE_REMOVAL

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<CoordinateReferenceSystem>(const Uuid& id) const {
    const auto& c = requireExisting<CoordinateReferenceSystem>(p_, id);
    if (p_.model().info.projectCrsId == id) restrict("The project CRS cannot be deleted; declare another project CRS first");
    return simpleRemoval<CoordinateReferenceSystem>(p_, c.id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<LocalGridDefinition>(const Uuid& id) const {
    requireExisting<LocalGridDefinition>(p_, id);
    for (const auto& c : p_.model().crs.all())
        if (c.localGridId == id) restrict("The local grid is used by CRS '" + c.name + "'");
    return simpleRemoval<LocalGridDefinition>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<TransformationRecord>(const Uuid&) const {
    restrict("Transformation records are part of the audit trail and cannot be deleted");
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<Asset>(const Uuid& id) const {
    requireExisting<Asset>(p_, id);
    requireAssetUnreferenced(p_, id);
    return simpleRemoval<Asset>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<Material>(const Uuid& id) const {
    const auto& mat = requireExisting<Material>(p_, id);
    RemovalBuilder b(p_, "Delete material " + mat.name);
    const auto& m = p_.model();
    for (const auto& g : m.geometry.all())
        if (g.materialId == id) b.modify(g, [](GeometryObject& x) { x.materialId = {}; });
    for (const auto& d : m.developments.all())
        if (d.materialId == id) b.modify(d, [](MineDevelopment& x) { x.materialId = {}; });
    for (const auto& s : m.stopes.all())
        if (s.materialId == id) b.modify(s, [](Stope& x) { x.materialId = {}; });
    for (const auto& g : m.geologicalObjects.all())
        if (g.materialId == id) b.modify(g, [](GeologicalObject& x) { x.materialId = {}; });
    b.erase(mat);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<AnalysisResult>(const Uuid& id) const {
    requireExisting<AnalysisResult>(p_, id);
    for (const auto& c : p_.model().conformanceResults.all())
        if (c.analysisResultId == id) restrict("The analysis result belongs to a design-conformance result; delete that instead");
    return simpleRemoval<AnalysisResult>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<AttributeDefinition>(const Uuid& id) const {
    const auto& d = requireExisting<AttributeDefinition>(p_, id);
    const auto& m = p_.model();
    auto used = [&](const AttributeMap& a) { return a.count(d.key) != 0; };
    bool inUse = false;
    if (d.entityType == "GeometryObject")
        for (const auto& g : m.geometry.all()) inUse = inUse || used(g.attributes);
    if (d.entityType == "MineDevelopment")
        for (const auto& g : m.developments.all()) inUse = inUse || used(g.attributes);
    if (d.entityType == "Stope")
        for (const auto& g : m.stopes.all()) inUse = inUse || used(g.attributes);
    if (d.entityType == "ScheduleActivity")
        for (const auto& g : m.activities.all()) inUse = inUse || used(g.attributes);
    if (inUse) restrict("Attribute '" + d.key + "' still has values; clear them before deleting the definition");
    return simpleRemoval<AttributeDefinition>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<CadLayer>(const Uuid& id) const {
    const auto& l = requireExisting<CadLayer>(p_, id);
    if (p_.model().layers.size() <= 1) restrict("A project needs at least one layer");
    for (const auto& g : p_.model().geometry.all())
        if (g.layerId == id) restrict("Layer '" + l.name + "' still contains objects");
    return simpleRemoval<CadLayer>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<GeometryObject>(const Uuid& id) const {
    const auto& g = requireExisting<GeometryObject>(p_, id);
    const auto refs = p_.model().referencesToGeometry(id);
    if (!refs.empty()) {
        if (const auto* dev = p_.model().developmentForGeometry(id))
            restrict("'" + g.name + "' is the centreline of a development heading (" + dev->headingId + "); delete the heading first");
        restrict("'" + g.name + "' is used by " + refs.front().first + "; delete that object first");
    }
    return simpleRemoval<GeometryObject>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<SurveyControlPoint>(const Uuid& id) const {
    const auto& c = requireExisting<SurveyControlPoint>(p_, id);
    RemovalBuilder b(p_, "Delete control point " + c.name);
    // Registration pairs keep their copied target coordinates; only the reference is cleared.
    for (const auto& r : p_.model().registrations.all()) {
        bool refs = false;
        for (const auto& pr : r.pairs) refs = refs || pr.targetControlId == id;
        if (refs)
            b.modify(r, [&](Registration& x) {
                for (auto& pr : x.pairs)
                    if (pr.targetControlId == id) pr.targetControlId = {};
            });
    }
    b.erase(c);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<PointCloudDataset>(const Uuid& id) const {
    const auto& d = requireExisting<PointCloudDataset>(p_, id);
    const auto& m = p_.model();
    for (const auto& a : m.asBuiltModels.all())
        if (a.sourceDatasetId == id) restrict("Dataset '" + d.name + "' is the source of as-built model '" + a.name + "'");
    for (const auto& r : m.registrations.all())
        if (r.targetDatasetId == id && r.datasetId != id) restrict("Dataset '" + d.name + "' is the ICP target of another dataset's registration");
    RemovalBuilder b(p_, "Delete point cloud " + d.name);
    for (const auto& r : m.registrations.all())
        if (r.datasetId == id) b.erase(r); // transformation history of this dataset
    for (const auto& c : m.pointClouds.all())
        if (c.parentDatasetId == id) b.modify(c, [](PointCloudDataset& x) { x.parentDatasetId = {}; });
    for (const auto& a : m.actuals.all())
        if (a.surveyDatasetId == id) b.modify(a, [](ProductionActual& x) { x.surveyDatasetId = {}; });
    b.erase(d);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<Registration>(const Uuid& id) const {
    const auto& r = requireExisting<Registration>(p_, id);
    if (r.applied) restrict("Applied registrations are part of the dataset's transformation history and cannot be deleted");
    for (const auto& a : p_.model().asBuiltModels.all())
        if (a.registrationId == id) restrict("The registration is referenced by as-built model '" + a.name + "'");
    return simpleRemoval<Registration>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<AsBuiltModel>(const Uuid& id) const {
    const auto& a = requireExisting<AsBuiltModel>(p_, id);
    RemovalBuilder b(p_, "Delete as-built model " + a.name);
    for (const auto& c : p_.model().conformanceResults.all())
        if (c.asBuiltId == id) {
            if (const auto* r = p_.model().analysisResults.find(c.analysisResultId)) b.erase(*r);
            b.erase(c);
        }
    b.erase(a);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<TunnelProfile>(const Uuid& id) const {
    const auto& t = requireExisting<TunnelProfile>(p_, id);
    for (const auto& d : p_.model().developments.all())
        if (d.profileId == id) restrict("Profile '" + t.name + "' is used by heading " + d.headingId);
    for (const auto& d : p_.model().declines.all())
        if (d.profileId == id) restrict("Profile '" + t.name + "' is used by decline " + d.name);
    return simpleRemoval<TunnelProfile>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<MineLevel>(const Uuid& id) const {
    const auto& l = requireExisting<MineLevel>(p_, id);
    RemovalBuilder b(p_, "Delete level " + l.name);
    for (const auto& d : p_.model().developments.all())
        if (d.levelId == id) b.modify(d, [](MineDevelopment& x) { x.levelId = {}; });
    for (const auto& s : p_.model().stopes.all())
        if (s.levelId == id) b.modify(s, [](Stope& x) { x.levelId = {}; });
    b.erase(l);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<Decline>(const Uuid& id) const {
    const auto& dc = requireExisting<Decline>(p_, id);
    RemovalBuilder b(p_, "Delete decline " + dc.name);
    for (const auto& d : p_.model().developments.all())
        if (d.declineId == id) b.modify(d, [](MineDevelopment& x) { x.declineId = {}; });
    b.erase(dc);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<GroundSupportClass>(const Uuid& id) const {
    const auto& s = requireExisting<GroundSupportClass>(p_, id);
    RemovalBuilder b(p_, "Delete support class " + s.name);
    for (const auto& d : p_.model().developments.all())
        if (d.supportClassId == id) b.modify(d, [](MineDevelopment& x) { x.supportClassId = {}; });
    b.erase(s);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<MineDevelopment>(const Uuid& id) const {
    const auto& d = requireExisting<MineDevelopment>(p_, id);
    const auto& m = p_.model();
    RemovalBuilder b(p_, "Delete heading " + d.headingId);
    b.unlinkAllFor(id); // links CASCADE; activities are kept
    for (const auto& a : m.actuals.all())
        if (a.developmentId == id) b.modify(a, [](ProductionActual& x) { x.developmentId = {}; });
    for (const auto& a : m.asBuiltModels.all())
        if (a.developmentId == id) b.modify(a, [](AsBuiltModel& x) { x.developmentId = {}; });
    for (const auto& c : m.conformanceResults.all())
        if (c.developmentId == id) {
            if (const auto* r = m.analysisResults.find(c.analysisResultId)) b.erase(*r);
            b.erase(c);
        }
    for (const auto& s : m.structures.all())
        if (s.developmentId == id) b.modify(s, [](StructuralMeasurement& x) { x.developmentId = {}; });
    for (const auto& g : m.geotechClassifications.all())
        if (g.developmentId == id) b.modify(g, [](GeotechClassification& x) { x.developmentId = {}; });
    for (const auto& pc : m.pointClouds.all())
        if (pc.developmentId == id) b.modify(pc, [](PointCloudDataset& x) { x.developmentId = {}; });
    b.erase(d); // design revisions are history and are kept
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<Stope>(const Uuid& id) const {
    const auto& s = requireExisting<Stope>(p_, id);
    for (const auto& blk : p_.model().miningBlocks.all())
        if (blk.stopeId == id) restrict("Stope '" + s.name + "' still has mining blocks; delete them first");
    RemovalBuilder b(p_, "Delete stope " + s.name);
    b.unlinkAllFor(id);
    b.erase(s);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<MiningBlock>(const Uuid& id) const {
    const auto& blk = requireExisting<MiningBlock>(p_, id);
    RemovalBuilder b(p_, "Delete mining block " + blk.name);
    b.unlinkAllFor(id);
    b.erase(blk);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<DesignRevision>(const Uuid& id) const {
    const auto& r = requireExisting<DesignRevision>(p_, id);
    if (r.approved) restrict("Approved design revisions cannot be deleted");
    return simpleRemoval<DesignRevision>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<WorkCalendar>(const Uuid& id) const {
    const auto& c = requireExisting<WorkCalendar>(p_, id);
    const auto& m = p_.model();
    if (m.info.defaultCalendarId == id) restrict("'" + c.name + "' is the project default calendar");
    for (const auto& a : m.activities.all())
        if (a.calendarId == id) restrict("'" + c.name + "' is used by activity '" + a.name + "'");
    for (const auto& r : m.resources.all())
        if (r.calendarId == id) restrict("'" + c.name + "' is used by resource '" + r.name + "'");
    return simpleRemoval<WorkCalendar>(p_, id);
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<ScheduleActivity>(const Uuid& id) const {
    const auto& a = requireExisting<ScheduleActivity>(p_, id);
    const auto& m = p_.model();
    for (const auto& x : m.activities.all())
        if (x.parentId == id) restrict("Delete or move the WBS children of '" + a.name + "' first");
    for (const auto& x : m.actuals.all())
        if (x.activityId == id) restrict("'" + a.name + "' has recorded production actuals; they must be removed first");
    RemovalBuilder b(p_, "Delete activity '" + a.name + "'");
    for (const auto& d : m.dependencies.all())
        if (d.predecessorId == id || d.successorId == id) b.erase(d);
    for (const auto& r : m.assignments.all())
        if (r.activityId == id) b.erase(r);
    b.unlinkAllFor(id);
    for (const auto& s : m.scenarios.all()) {
        bool refs = false;
        for (const auto& o : s.overrides) refs = refs || o.entityId == id;
        if (refs) b.modify(s, [&](Scenario& x) { std::erase_if(x.overrides, [&](const auto& o) { return o.entityId == id; }); });
    }
    b.erase(a);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<Resource>(const Uuid& id) const {
    const auto& r = requireExisting<Resource>(p_, id);
    RemovalBuilder b(p_, "Delete resource " + r.name);
    for (const auto& a : p_.model().assignments.all())
        if (a.resourceId == id) b.erase(a);
    for (const auto& s : p_.model().scenarios.all()) {
        bool refs = false;
        for (const auto& o : s.overrides) refs = refs || o.entityId == id;
        if (refs) b.modify(s, [&](Scenario& x) { std::erase_if(x.overrides, [&](const auto& o) { return o.entityId == id; }); });
    }
    b.erase(r);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<DevelopmentCycle>(const Uuid& id) const {
    const auto& c = requireExisting<DevelopmentCycle>(p_, id);
    for (const auto& a : p_.model().activities.all())
        if (a.productionModelId == id && a.useCycleRate)
            restrict("Cycle '" + c.name + "' drives the rate of '" + a.name + "'");
    RemovalBuilder b(p_, "Delete cycle " + c.name);
    for (const auto& a : p_.model().activities.all())
        if (a.productionModelId == id) b.modify(a, [](ScheduleActivity& x) { x.productionModelId = {}; });
    b.erase(c);
    return b.take();
}

template <> std::unique_ptr<Command> ProjectEditor::removalCommand<ScheduleBaseline>(const Uuid& id) const {
    const auto& bl = requireExisting<ScheduleBaseline>(p_, id);
    if (bl.approved) restrict("Approved baselines are preserved and cannot be deleted");
    return simpleRemoval<ScheduleBaseline>(p_, id);
}

} // namespace openmine
