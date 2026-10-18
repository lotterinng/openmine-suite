// SPDX-License-Identifier: Apache-2.0
#include "EntityInfo.h"

#include "openmine/commands/EntityCommands.h"
#include "openmine/objects/EnumNames.h"

#include <QCoreApplication>

#include <map>
#include <set>

namespace openmine::app {

namespace {
template <class E> QStringList names() {
    QStringList l;
    for (const auto& n : enumNames<E>()) l << qstr(n);
    return l;
}
QString shortId(const Uuid& id) { return qstr(id.toString().substr(0, 8)); }
} // namespace

std::string documentType(EntityKind k) {
#define OPENMINE_DOC_TYPE(T, member, Kind) \
    if (k == EntityKind::Kind) return #T;
    OPENMINE_ENTITY_TYPES(OPENMINE_DOC_TYPE)
#undef OPENMINE_DOC_TYPE
    if (k == EntityKind::ProjectInfo) return "ProjectInfo";
    if (k == EntityKind::DesignLink) return "ActivityDesignLink";
    return {};
}

std::optional<EntityKind> kindForDocument(const std::string& type) {
#define OPENMINE_DOC_KIND(T, member, Kind) \
    if (type == #T) return EntityKind::Kind;
    OPENMINE_ENTITY_TYPES(OPENMINE_DOC_KIND)
#undef OPENMINE_DOC_KIND
    if (type == "ProjectInfo") return EntityKind::ProjectInfo;
    if (type == "ActivityDesignLink") return EntityKind::DesignLink;
    return std::nullopt;
}

const std::vector<EntityKind>& entityKinds() {
    static const std::vector<EntityKind> v = [] {
        std::vector<EntityKind> out;
#define OPENMINE_KIND_LIST(T, member, Kind) out.push_back(EntityKind::Kind);
        OPENMINE_ENTITY_TYPES(OPENMINE_KIND_LIST)
#undef OPENMINE_KIND_LIST
        return out;
    }();
    return v;
}

QString kindLabel(EntityKind k, bool plural) {
    static const std::map<EntityKind, std::pair<const char*, const char*>> labels = {
        {EntityKind::ProjectInfo, {"Project", "Project"}},
        {EntityKind::DesignLink, {"Design link", "Design links"}},
        {EntityKind::Crs, {"Coordinate system", "Coordinate systems"}},
        {EntityKind::LocalGrid, {"Local grid", "Local grids"}},
        {EntityKind::Transformation, {"Transformation record", "Transformation records"}},
        {EntityKind::Asset, {"Asset", "Assets"}},
        {EntityKind::Attachment, {"Attachment", "Attachments"}},
        {EntityKind::Material, {"Material", "Materials"}},
        {EntityKind::AnalysisResult, {"Analysis result", "Analysis results"}},
        {EntityKind::Report, {"Report", "Reports"}},
        {EntityKind::AttributeDefinition, {"Attribute definition", "Attribute definitions"}},
        {EntityKind::Layer, {"Layer", "Layers"}},
        {EntityKind::Geometry, {"Object", "Objects"}},
        {EntityKind::ControlPoint, {"Control point", "Control points"}},
        {EntityKind::PointCloud, {"Point cloud", "Point clouds"}},
        {EntityKind::Registration, {"Registration", "Registrations"}},
        {EntityKind::AsBuilt, {"As-built model", "As-built models"}},
        {EntityKind::Conformance, {"Conformance result", "Conformance results"}},
        {EntityKind::Profile, {"Tunnel profile", "Tunnel profiles"}},
        {EntityKind::Level, {"Level", "Levels"}},
        {EntityKind::Decline, {"Decline", "Declines"}},
        {EntityKind::SupportClass, {"Support class", "Support classes"}},
        {EntityKind::Development, {"Development", "Developments"}},
        {EntityKind::Stope, {"Stope", "Stopes"}},
        {EntityKind::MiningBlock, {"Mining block", "Mining blocks"}},
        {EntityKind::DesignRevision, {"Design revision", "Design revisions"}},
        {EntityKind::Calendar, {"Calendar", "Calendars"}},
        {EntityKind::Activity, {"Activity", "Activities"}},
        {EntityKind::Dependency, {"Dependency", "Dependencies"}},
        {EntityKind::Resource, {"Resource", "Resources"}},
        {EntityKind::Assignment, {"Resource assignment", "Resource assignments"}},
        {EntityKind::Cycle, {"Development cycle", "Development cycles"}},
        {EntityKind::Actual, {"Production actual", "Production actuals"}},
        {EntityKind::Target, {"Production target", "Production targets"}},
        {EntityKind::Baseline, {"Baseline", "Baselines"}},
        {EntityKind::Scenario, {"Scenario", "Scenarios"}},
        {EntityKind::Drillhole, {"Drillhole", "Drillholes"}},
        {EntityKind::GeologicalObject, {"Geological object", "Geological objects"}},
        {EntityKind::BlockModel, {"Block model", "Block models"}},
        {EntityKind::Structure, {"Structure", "Structures"}},
        {EntityKind::GeotechClassification, {"Rock-mass classification", "Rock-mass classifications"}},
    };
    auto it = labels.find(k);
    if (it == labels.end()) return QStringLiteral("Entity");
    return QCoreApplication::translate("EntityInfo", plural ? it->second.second : it->second.first);
}

QString kindIcon(EntityKind k) {
    switch (k) {
    case EntityKind::Layer: return "layer";
    case EntityKind::Geometry: return "polyline";
    case EntityKind::ControlPoint: return "control";
    case EntityKind::PointCloud: return "cloud";
    case EntityKind::Registration: return "register";
    case EntityKind::AsBuilt: return "mesh";
    case EntityKind::Conformance: return "conformance";
    case EntityKind::Development: return "heading";
    case EntityKind::Stope: return "stope";
    case EntityKind::MiningBlock: return "stope";
    case EntityKind::Activity: return "task";
    case EntityKind::Dependency: return "dependency";
    case EntityKind::Resource: return "resource";
    case EntityKind::Calendar: return "calendar";
    case EntityKind::Baseline: return "baseline";
    case EntityKind::Scenario: return "scenario";
    case EntityKind::Drillhole: return "drillhole";
    case EntityKind::BlockModel: return "blocks";
    case EntityKind::Structure: return "structure";
    case EntityKind::GeotechClassification: return "geotech";
    case EntityKind::Report: return "report";
    case EntityKind::Crs: return "crs";
    default: return "entity";
    }
}

std::optional<EntityKind> findKind(const ProjectModel& m, const Uuid& id) {
#define OPENMINE_FIND_KIND(T, member, Kind) \
    if (m.member.contains(id)) return EntityKind::Kind;
    OPENMINE_ENTITY_TYPES(OPENMINE_FIND_KIND)
#undef OPENMINE_FIND_KIND
    return std::nullopt;
}

QString displayName(const ProjectModel& m, const SelectionItem& s) {
    const Uuid& id = s.id;
    auto nameOr = [&](const std::string& n, const QString& fallback) { return n.empty() ? fallback : qstr(n); };
    switch (s.kind) {
    case EntityKind::ProjectInfo: return qstr(m.info.name);
    case EntityKind::Crs: if (auto* e = m.crs.find(id)) return nameOr(e->name, qstr(e->authorityCode)); break;
    case EntityKind::LocalGrid: if (auto* e = m.localGrids.find(id)) return qstr(e->name); break;
    case EntityKind::Transformation: if (auto* e = m.transformations.find(id)) return qstr(e->sourceCrs + " → " + e->targetCrs); break;
    case EntityKind::Asset: if (auto* e = m.assets.find(id)) return qstr(e->relativePath); break;
    case EntityKind::Attachment: if (auto* e = m.attachments.find(id)) return qstr(e->fileName); break;
    case EntityKind::Material: if (auto* e = m.materials.find(id)) return qstr(e->name); break;
    case EntityKind::AnalysisResult: if (auto* e = m.analysisResults.find(id)) return nameOr(e->name, qstr(toString(e->kind))); break;
    case EntityKind::Report: if (auto* e = m.reports.find(id)) return qstr(e->name + " (" + e->format + ")"); break;
    case EntityKind::AttributeDefinition: if (auto* e = m.attributeDefinitions.find(id)) return qstr(e->entityType + "." + e->key); break;
    case EntityKind::Layer: if (auto* e = m.layers.find(id)) return qstr(e->name); break;
    case EntityKind::Geometry:
        if (auto* e = m.geometry.find(id)) return nameOr(e->name, QStringLiteral("%1 %2").arg(qstr(toString(e->type)), shortId(id)));
        break;
    case EntityKind::ControlPoint: if (auto* e = m.controlPoints.find(id)) return qstr(e->name); break;
    case EntityKind::PointCloud: if (auto* e = m.pointClouds.find(id)) return qstr(e->name); break;
    case EntityKind::Registration:
        if (auto* e = m.registrations.find(id)) {
            const auto* d = m.pointClouds.find(e->datasetId);
            return QStringLiteral("%1 — %2%3").arg(qstr(toString(e->method)), d ? qstr(d->name) : shortId(e->datasetId), e->applied ? QString() : QStringLiteral(" (not applied)"));
        }
        break;
    case EntityKind::AsBuilt: if (auto* e = m.asBuiltModels.find(id)) return qstr(e->name); break;
    case EntityKind::Conformance:
        if (auto* e = m.conformanceResults.find(id)) {
            const auto* d = m.developments.find(e->developmentId);
            return QStringLiteral("%1 %2–%3 m").arg(d ? qstr(d->headingId) : shortId(e->developmentId)).arg(e->chainageFrom, 0, 'f', 1).arg(e->chainageTo, 0, 'f', 1);
        }
        break;
    case EntityKind::Profile: if (auto* e = m.profiles.find(id)) return qstr(e->name); break;
    case EntityKind::Level: if (auto* e = m.levels.find(id)) return qstr(e->name); break;
    case EntityKind::Decline: if (auto* e = m.declines.find(id)) return qstr(e->name); break;
    case EntityKind::SupportClass: if (auto* e = m.supportClasses.find(id)) return qstr(e->name); break;
    case EntityKind::Development:
        if (auto* e = m.developments.find(id)) return e->name.empty() || e->name == e->headingId ? qstr(e->headingId) : qstr(e->headingId + " — " + e->name);
        break;
    case EntityKind::Stope: if (auto* e = m.stopes.find(id)) return qstr(e->name); break;
    case EntityKind::MiningBlock: if (auto* e = m.miningBlocks.find(id)) return qstr(e->name); break;
    case EntityKind::DesignRevision:
        if (auto* e = m.designRevisions.find(id)) return QStringLiteral("Rev %1%2").arg(qstr(e->revisionCode), e->approved ? QStringLiteral(" (approved)") : QString());
        break;
    case EntityKind::Calendar: if (auto* e = m.calendars.find(id)) return qstr(e->name); break;
    case EntityKind::Activity:
        if (auto* e = m.activities.find(id)) return e->wbsCode.empty() ? qstr(e->name) : qstr(e->wbsCode + "  " + e->name);
        break;
    case EntityKind::Dependency:
        if (auto* e = m.dependencies.find(id)) {
            const auto* a = m.activities.find(e->predecessorId);
            const auto* b = m.activities.find(e->successorId);
            static const char* t[] = {"FS", "SS", "FF", "SF"};
            return QStringLiteral("%1 → %2 (%3%4)")
                .arg(a ? qstr(a->name) : QStringLiteral("?"), b ? qstr(b->name) : QStringLiteral("?"), t[static_cast<int>(e->type)],
                     e->lagHours != 0 ? QStringLiteral(" %1%2 h").arg(e->lagHours > 0 ? "+" : "").arg(e->lagHours) : QString());
        }
        break;
    case EntityKind::Resource: if (auto* e = m.resources.find(id)) return qstr(e->name); break;
    case EntityKind::Assignment:
        if (auto* e = m.assignments.find(id)) {
            const auto* r = m.resources.find(e->resourceId);
            const auto* a = m.activities.find(e->activityId);
            return QStringLiteral("%1 × %2 → %3").arg(r ? qstr(r->name) : QStringLiteral("?")).arg(e->units).arg(a ? qstr(a->name) : QStringLiteral("?"));
        }
        break;
    case EntityKind::Cycle: if (auto* e = m.cycles.find(id)) return qstr(e->name); break;
    case EntityKind::Actual:
        if (auto* e = m.actuals.find(id)) return QStringLiteral("%1  %2 %3 (%4)").arg(qstr(e->date.toString())).arg(e->quantity).arg(qstr(e->unit), qstr(toString(e->source)));
        break;
    case EntityKind::Target: if (auto* e = m.targets.find(id)) return qstr(e->name); break;
    case EntityKind::Baseline: if (auto* e = m.baselines.find(id)) return qstr(e->name); break;
    case EntityKind::Scenario: if (auto* e = m.scenarios.find(id)) return qstr(e->name); break;
    case EntityKind::Drillhole: if (auto* e = m.drillholes.find(id)) return qstr(e->holeId); break;
    case EntityKind::GeologicalObject: if (auto* e = m.geologicalObjects.find(id)) return qstr(e->name); break;
    case EntityKind::BlockModel: if (auto* e = m.blockModels.find(id)) return qstr(e->name); break;
    case EntityKind::Structure:
        if (auto* e = m.structures.find(id))
            return QStringLiteral("%1 %2/%3").arg(e->setName.empty() ? qstr(toString(e->type)) : qstr(e->setName)).arg(e->dip, 2, 'f', 0, QChar('0')).arg(e->dipDirection, 3, 'f', 0, QChar('0'));
        break;
    case EntityKind::GeotechClassification:
        if (auto* e = m.geotechClassifications.find(id))
            return QStringLiteral("%1 %2 (%3)").arg(e->method == GeotechMethod::QSystem ? "Q" : "RMR").arg(e->value, 0, 'g', 3).arg(qstr(e->rockClass));
        break;
    default: break;
    }
    return shortId(id);
}

QString describe(const ProjectModel& m, const SelectionItem& item) { return kindLabel(item.kind) + " " + displayName(m, item); }

QString humanize(const std::string& column) {
    std::string c = column;
    for (const char* suffix : {"_uuid", "_json"})
        if (c.size() > std::string(suffix).size() && c.ends_with(suffix)) c.resize(c.size() - std::string(suffix).size());
    QString s = qstr(c);
    s.replace('_', ' ');
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    s.replace(QStringLiteral(" m2"), QStringLiteral(" m²"));
    s.replace(QStringLiteral(" m3"), QStringLiteral(" m³"));
    return s;
}

FieldInfo fieldInfo(const std::string& doc, const std::string& col) {
    using K = FieldInfo::Kind;
    FieldInfo f;
    f.label = humanize(col);
    f.group = 1;
    auto ro = [&](const QString& why) {
        f.readOnly = true;
        f.readOnlyReason = why;
    };

    // Generic kinds by column name ----------------------------------------------------------------
    static const std::set<std::string> bools = {"closed", "visible", "locked", "applied", "allow_scale", "icp_converged", "approved",
                                                "design_impact_flag", "duration_from_rate", "use_cycle_rate", "mesh", "drives_rate",
                                                "is_directory", "required"};
    static const std::set<std::string> dates = {"survey_date", "scan_date", "date", "period_start", "period_end", "drilled_date", "mapped_date"};
    static const std::set<std::string> dateTimes = {"actual_start", "actual_finish", "constraint_date"};
    static const std::set<std::string> multi = {"description", "notes", "comment", "note", "definition", "shifts", "exceptions",
                                                "custom_points", "parameters", "type_data", "attributes", "transform", "previous_transform",
                                                "local_to_base", "stored_bounds"};
    static const std::map<std::string, std::string> refs = {
        {"layer_uuid", "CadLayer"},          {"material_uuid", "Material"},           {"geometry_uuid", "GeometryObject"},
        {"brep_asset_uuid", "Asset"},        {"asset_uuid", "Asset"},                 {"payload_asset_uuid", "Asset"},
        {"data_asset_uuid", "Asset"},        {"local_grid_uuid", "LocalGridDefinition"}, {"base_crs_uuid", "CoordinateReferenceSystem"},
        {"dataset_uuid", "PointCloudDataset"}, {"source_dataset_uuid", "PointCloudDataset"}, {"target_dataset_uuid", "PointCloudDataset"},
        {"parent_dataset_uuid", "PointCloudDataset"}, {"survey_dataset_uuid", "PointCloudDataset"}, {"registration_uuid", "Registration"},
        {"as_built_uuid", "AsBuiltModel"},   {"development_uuid", "MineDevelopment"}, {"analysis_result_uuid", "AnalysisResult"},
        {"profile_uuid", "TunnelProfile"},   {"level_uuid", "MineLevel"},             {"decline_uuid", "Decline"},
        {"support_class_uuid", "GroundSupportClass"}, {"current_revision_uuid", "DesignRevision"}, {"stope_uuid", "Stope"},
        {"calendar_uuid", "WorkCalendar"},   {"parent_uuid", "ScheduleActivity"},     {"production_model_uuid", "DevelopmentCycle"},
        {"predecessor_uuid", "ScheduleActivity"}, {"successor_uuid", "ScheduleActivity"}, {"activity_uuid", "ScheduleActivity"},
        {"resource_uuid", "Resource"},       {"project_crs_uuid", "CoordinateReferenceSystem"}, {"default_calendar_uuid", "WorkCalendar"},
    };
    if (bools.count(col)) f.kind = K::Bool;
    else if (dates.count(col)) f.kind = K::Date;
    else if (dateTimes.count(col)) f.kind = K::DateTime;
    else if (col == "color_rgb" || col == "color_override") f.kind = K::Color;
    else if (col.ends_with("_json") || multi.count(col)) f.kind = col.ends_with("_json") || col == "type_data" || col == "shifts" || col == "exceptions" || col == "custom_points" ? K::Json : K::MultiLine;
    else if (auto it = refs.find(col); it != refs.end()) {
        f.kind = K::Reference;
        f.referenceType = it->second;
        f.group = 3;
    }
    if (col == "entity_uuid") {
        f.kind = K::ReadOnly;
        ro(QStringLiteral("Set by the owning entity"));
    }
    if (col == "created_at" || col == "modified_at" || col == "revision" || col == "captured_at") {
        f.group = 9;
        ro(QStringLiteral("Maintained automatically"));
    }
    if (col.find("uuid") != std::string::npos && f.referenceType == "Asset") ro(QStringLiteral("Assets are immutable files managed by the operation that created them"));

    // Units ------------------------------------------------------------------------------------------
    auto ends = [&](const char* s) { return col.ends_with(s); };
    if (ends("_hours") || col == "standard_hours_per_day" || col == "shift_hours") f.unit = "h";
    else if (col.find("minutes") != std::string::npos) f.unit = "min";
    else if (ends("_percent") || col == "utilisation" || col == "advance_efficiency") f.unit = col == "utilisation" || col == "advance_efficiency" ? "fraction" : "%";
    else if (col == "density") f.unit = "t/m³";
    else if (ends("_deg") || col == "dip" || col == "dip_direction") f.unit = "°";
    else if (col == "elevation" || col == "width" || col == "height" || col == "design_width" || col == "design_height" || col.find("chainage") != std::string::npos ||
             col == "length" || col == "round_length" || col == "bolt_length" || col == "bolt_spacing" || col == "shoulder_radius" || col == "arch_rise" ||
             col == "min_curve_radius" || col == "persistence" || ends("_sigma") || col.starts_with("position_") || col.starts_with("collar_") ||
             col.starts_with("location_") || col.starts_with("origin_") || col.starts_with("block_size_") || col == "approved_design_length" ||
             ends("_rms") || ends("_max") || ends("_depth") || col == "min_clearance" || col == "station_interval" || col == "accuracy_m" ||
             col == "icp_max_correspondence" || col == "rms_residual" || col == "max_residual" || col == "lag_hours")
        f.unit = col == "lag_hours" ? "h" : "m";
    else if (ends("_volume")) f.unit = "m³";
    else if (col == "shotcrete_thickness") f.unit = "mm";
    else if (col == "bucket_capacity") f.unit = "m³";
    else if (col == "drill_penetration_rate") f.unit = "m/min";
    else if (col == "cost_per_hour") f.unit = "/h";
    else if (col == "line_width") f.unit = "px";
    else if (col == "size_bytes") f.unit = "bytes";
    if (col == "transparency") f.tooltip = QStringLiteral("0 = opaque … 1 = invisible");

    // Enums -------------------------------------------------------------------------------------------
    auto en = [&](QStringList l) {
        f.kind = K::Enum;
        f.choices = std::move(l);
    };
    if (doc == "CoordinateReferenceSystem" && col == "kind") en(names<CrsKind>());
    if (doc == "LocalGridDefinition" && col == "method") en(names<LocalGridMethod>());
    if (doc == "Asset" && col == "kind") en(names<AssetKind>());
    if (doc == "AnalysisResult" && col == "kind") en(names<AnalysisKind>());
    if (doc == "GeometryObject" && col == "geometry_type") en(names<GeometryType>());
    if (doc == "SurveyControlPoint" && col == "role") en(names<ControlPointRole>());
    if (doc == "Registration" && col == "method") en(names<RegistrationMethod>());
    if (doc == "TunnelProfile" && col == "shape") en(names<ProfileShape>());
    if (doc == "TunnelProfile" && col == "centreline_reference") en(names<CentrelineReference>());
    if (doc == "Stope" && col == "method") en(names<StopeMethod>());
    if (doc == "WorkCalendar" && col == "kind") en(names<CalendarKind>());
    if (doc == "ScheduleActivity" && col == "kind") en(names<ActivityKind>());
    if (doc == "ScheduleActivity" && col == "constraint_type") en(names<ConstraintType>());
    if (doc == "ScheduleActivity" && col == "rate_unit") en(names<RateUnit>());
    if (doc == "ActivityDependency" && col == "dependency_type") en(names<DependencyType>());
    if (doc == "Resource" && col == "kind") en(names<ResourceKind>());
    if (doc == "ProductionActual" && col == "source") en(names<ActualSource>());
    if (doc == "Scenario" && col == "levelling") en(names<LevellingMode>());
    if (doc == "Scenario" && col == "objective") en({"Makespan", "WeightedCompletion", "Lateness"});
    if (doc == "GeologicalObject" && col == "kind") en(names<GeologicalObjectKind>());
    if (doc == "StructuralMeasurement" && col == "type") en(names<StructureType>());
    if (doc == "GeotechClassification" && col == "method") en(names<GeotechMethod>());
    if (doc == "AttributeDefinition" && col == "value_type") en({"Text", "Real", "Integer", "Boolean", "Date"});
    if (doc == "CadLayer" && col == "line_type") en({"Continuous", "Dashed", "Dotted", "DashDot"});
    if (doc == "PointCloudDataset" && col == "color_mode") {
        f.kind = K::Text;
        f.choices = {"Elevation", "Intensity", "RGB", "Classification", "Single"};
        f.editableChoices = true;
        f.tooltip = QStringLiteral("Elevation | Intensity | RGB | Classification | Scalar:<field> | Single");
    }
    if (col == "status") {
        f.kind = K::Text;
        f.choices = {"Planned", "In progress", "Complete", "Suspended"};
        f.editableChoices = true;
    }
    if (doc == "MineDevelopment" && col == "activity_type") {
        f.kind = K::Text;
        f.choices = {"Decline", "Ramp", "Level access", "Crosscut", "Ore drive", "Access drive", "Stockpile", "Sump", "Ventilation drive", "Development"};
        f.editableChoices = true;
    }

    if (doc == "ProjectInfo") {
        if (col == "project_crs") {
            f.kind = K::Reference;
            f.referenceType = "CoordinateReferenceSystem";
            f.tooltip = QStringLiteral("Declares the CRS of all stored coordinates; it never transforms them");
        }
        if (col == "default_calendar") {
            f.kind = K::Reference;
            f.referenceType = "WorkCalendar";
        }
        if (col == "schedule_start" || col == "status_date") f.kind = K::DateTime;
        if (col == "south_orientated_axes") f.kind = K::Bool;
        if (col == "uuid" || col == "meta") ro(QStringLiteral("Maintained automatically"));
        if (col == "status_date") f.tooltip = QStringLiteral("Data date for progress reforecasts (empty = none)");
    }

    // Identity first ----------------------------------------------------------------------------------
    if (col == "name" || col == "heading_id" || col == "hole_id" || col == "wbs_code" || col == "code" || col == "key") f.group = 0;

    // Protection rules (survey data, computed results, approved designs, baselines) --------------
    if (doc == "TransformationRecord") ro(QStringLiteral("Transformation records are an audit trail"));
    if (doc == "Asset") ro(QStringLiteral("Assets are immutable files managed by the operation that created them"));
    if (doc == "ScheduleBaseline") ro(QStringLiteral("Approved baselines are immutable"));
    if (doc == "DesignConformanceResult") ro(QStringLiteral("Computed result — rerun the conformance analysis to change it"));
    if (doc == "AnalysisResult" && col != "name") ro(QStringLiteral("Computed result"));
    if (doc == "Report" && col != "name") ro(QStringLiteral("Issued report file"));
    if (doc == "Registration" && col != "notes") ro(QStringLiteral("Registration results are computed; create a new registration to change them"));
    if (doc == "DesignRevision" && col != "comment") ro(QStringLiteral("Revisions are immutable snapshots (approval through Design ▸ Approve revision)"));
    if (doc == "PointCloudDataset" &&
        (col == "point_count" || col == "stored_bounds" || col == "attributes" || col == "transform" || col == "source_file" || col == "source_format" ||
         col == "source_crs" || col == "parent_dataset_uuid" || col == "derivation"))
        ro(col == "transform" ? QStringLiteral("Changed only by applying a registration (stored coordinates are never altered)")
                              : QStringLiteral("Provenance recorded at import"));
    if (doc == "AsBuiltModel" && (col == "source_dataset_uuid" || col == "registration_uuid" || col == "geometry_uuid" || col == "method"))
        ro(QStringLiteral("Provenance of the as-built model"));
    if (doc == "GeometryObject" && (col == "geometry_type" || col == "vertices" || col == "bulges" || col.starts_with("mesh_")))
        ro(col == "vertices" ? QStringLiteral("Edit vertices in the vertex table") : QStringLiteral("Geometry data — use the CAD tools"));
    if (doc == "MineDevelopment" && col == "approved_design_length") ro(QStringLiteral("Set only by the audited 'Apply design quantities' recalculation"));
    if ((doc == "MineDevelopment" || doc == "Stope") && (col == "current_revision_uuid" || col == "design_revision"))
        ro(QStringLiteral("Maintained by Design ▸ Capture revision"));
    if (doc == "ScheduleActivity" && (col == "design_impact_flag" || col == "design_impact_note"))
        ro(QStringLiteral("Raised by design changes; cleared only by 'Apply design quantities'"));
    if (doc == "BlockModel" && (col == "attributes" || col == "data_asset_uuid")) ro(QStringLiteral("Block values are managed by the block-model tools"));
    if (doc == "LocalGridDefinition" && (col == "parameters" || col == "local_to_base" || col == "rms_residual" || col == "max_residual" ||
                                         col == "control_pair_count" || col == "control_pairs_json"))
        ro(QStringLiteral("Results of the local-grid fit"));
    if (doc == "PointCloudDataset" && col == "asset_uuid") f.group = 3;
    if (f.kind == K::Json && !f.readOnly) f.tooltip = QStringLiteral("JSON — validated when committed");
    return f;
}

} // namespace openmine::app
