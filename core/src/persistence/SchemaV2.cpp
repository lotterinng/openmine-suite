// SPDX-License-Identifier: Apache-2.0
// Project file schema version 2: table mappings for every entity of the unified model,
// child tables for explicit relationships, and model read/write.
#include "SchemaV2.h"

#include "TableMapping.h"
#include "openmine/project/Project.h"

#include <cmath>
#include <limits>
#include <set>

namespace openmine::persistence::detail {

namespace {
template <class T> struct Tag {};

// ---- Project level ------------------------------------------------------------------------------
Table<CoordinateReferenceSystem> makeTable(Tag<CoordinateReferenceSystem>) {
    using T = CoordinateReferenceSystem;
    Table<T> t{"coordinate_reference_system", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), text<T>("authority_code", &T::authorityCode),
                 text<T>("definition", &T::definition), enumCol<T>("kind", &T::kind),
                 text<T>("horizontal_datum", &T::horizontalDatum), text<T>("vertical_datum", &T::verticalDatum),
                 text<T>("linear_unit", &T::linearUnit, "TEXT NOT NULL CHECK (linear_unit = 'metre')"),
                 text<T>("axis_description", &T::axisDescription),
                 ref<T>("local_grid_uuid", &T::localGridId, "local_grid_definition", "RESTRICT")};
    addMeta(t);
    return t;
}

Table<LocalGridDefinition> makeTable(Tag<LocalGridDefinition>) {
    using T = LocalGridDefinition;
    Table<T> t{"local_grid_definition", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), ref<T>("base_crs_uuid", &T::baseCrsId, "coordinate_reference_system", "RESTRICT", true),
                 enumCol<T>("method", &T::method), transformCol<T>("local_to_base", &T::localToBase),
                 custom<T>("parameters", "TEXT NOT NULL", [](Stmt& s, int i, const T& e) { s.bind(i, json(e.parameters).dump()); },
                           [](const Stmt& s, int i, T& e) { e.parameters = parseJson(s.text(i), "parameters").get<std::vector<double>>(); }),
                 real<T>("rms_residual", &T::rmsResidual), real<T>("max_residual", &T::maxResidual),
                 integer<T>("control_pair_count", &T::controlPairCount), text<T>("control_pairs_json", &T::controlPairsJson)};
    addMeta(t);
    return t;
}

Table<TransformationRecord> makeTable(Tag<TransformationRecord>) {
    using T = TransformationRecord;
    Table<T> t{"transformation_record", {}, {}, {}};
    t.columns = {text<T>("source_crs", &T::sourceCrs), text<T>("target_crs", &T::targetCrs), text<T>("operation", &T::operation),
                 text<T>("pipeline", &T::pipeline), real<T>("accuracy_m", &T::accuracyMetres), text<T>("grids_used", &T::gridsUsed),
                 text<T>("applied_to", &T::appliedTo), text<T>("created_at", &T::createdAt), text<T>("user", &T::user)};
    return t;
}

Table<Asset> makeTable(Tag<Asset>) {
    using T = Asset;
    Table<T> t{"asset", {}, {"UNIQUE (relative_path)"}, {}};
    t.columns = {enumCol<T>("kind", &T::kind), text<T>("relative_path", &T::relativePath),
                 text<T>("sha256", &T::sha256, "TEXT NOT NULL CHECK (length(sha256) = 64)"),
                 int64<T>("size_bytes", &T::sizeBytes, "INTEGER NOT NULL CHECK (size_bytes >= 0)"),
                 boolean<T>("is_directory", &T::isDirectory), text<T>("created_at", &T::createdAt)};
    return t;
}

Table<Attachment> makeTable(Tag<Attachment>) {
    using T = Attachment;
    Table<T> t{"attachment", {}, {}, {"CREATE INDEX ix_attachment_entity ON attachment(entity_uuid)"}};
    t.columns = {uuidCol<T>("entity_uuid", &T::entityId), text<T>("entity_type", &T::entityType), text<T>("file_name", &T::fileName),
                 text<T>("mime_type", &T::mimeType), text<T>("description", &T::description), assetRef<T>("asset_uuid", &T::asset, true)};
    addMeta(t);
    return t;
}

Table<Material> makeTable(Tag<Material>) {
    using T = Material;
    Table<T> t{"material", {}, {"UNIQUE (name)"}, {}};
    t.columns = {text<T>("name", &T::name), real<T>("density", &T::density, "REAL NOT NULL CHECK (density > 0)"),
                 real<T>("swell_factor", &T::swellFactor, "REAL NOT NULL CHECK (swell_factor >= 1)"),
                 custom<T>("color_rgb", "INTEGER NOT NULL", [](Stmt& s, int i, const T& e) { s.bind(i, static_cast<std::int64_t>(e.colorRgb)); },
                           [](const Stmt& s, int i, T& e) { e.colorRgb = static_cast<std::uint32_t>(s.integer(i)); }),
                 text<T>("description", &T::description)};
    addMeta(t);
    return t;
}

Table<AnalysisResult> makeTable(Tag<AnalysisResult>) {
    using T = AnalysisResult;
    Table<T> t{"analysis_result", {}, {}, {}};
    t.columns = {enumCol<T>("kind", &T::kind), text<T>("name", &T::name), text<T>("inputs_json", &T::inputsJson),
                 text<T>("parameters_json", &T::parametersJson), text<T>("summary_json", &T::summaryJson),
                 text<T>("payload_json", &T::payloadJson), assetRef<T>("payload_asset_uuid", &T::payloadAsset),
                 text<T>("created_at", &T::createdAt), text<T>("user", &T::user)};
    return t;
}

Table<Report> makeTable(Tag<Report>) {
    using T = Report;
    Table<T> t{"report", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), text<T>("kind", &T::kind), text<T>("format", &T::format),
                 assetRef<T>("asset_uuid", &T::asset, true), text<T>("created_at", &T::createdAt)};
    return t;
}

Table<AttributeDefinition> makeTable(Tag<AttributeDefinition>) {
    using T = AttributeDefinition;
    Table<T> t{"attribute_definition", {}, {"UNIQUE (entity_type, key)"}, {}};
    t.columns = {text<T>("entity_type", &T::entityType,
                         "TEXT NOT NULL CHECK (entity_type IN ('GeometryObject','MineDevelopment','Stope','ScheduleActivity'))"),
                 text<T>("key", &T::key), text<T>("display_name", &T::displayName),
                 custom<T>("value_type", "TEXT NOT NULL CHECK (value_type IN ('Text','Real','Integer','Boolean','Date'))",
                           [](Stmt& s, int i, const T& e) { s.bind(i, toString(e.valueType)); },
                           [](const Stmt& s, int i, T& e) {
                               auto v = attributeValueTypeFromString(s.text(i));
                               if (!v) throw PersistenceError("Unknown attribute value type");
                               e.valueType = *v;
                           }),
                 text<T>("unit", &T::unit), boolean<T>("required", &T::required), text<T>("default_value", &T::defaultValue)};
    return t;
}

// ---- CAD --------------------------------------------------------------------------------------------
Table<CadLayer> makeTable(Tag<CadLayer>) {
    using T = CadLayer;
    Table<T> t{"cad_layer", {}, {"UNIQUE (name)"}, {}};
    t.columns = {text<T>("name", &T::name),
                 custom<T>("color_rgb", "INTEGER NOT NULL", [](Stmt& s, int i, const T& e) { s.bind(i, static_cast<std::int64_t>(e.colorRgb)); },
                           [](const Stmt& s, int i, T& e) { e.colorRgb = static_cast<std::uint32_t>(s.integer(i)); }),
                 boolean<T>("visible", &T::visible), boolean<T>("locked", &T::locked),
                 real<T>("transparency", &T::transparency, "REAL NOT NULL CHECK (transparency BETWEEN 0 AND 1)"),
                 real<T>("line_width", &T::lineWidth), text<T>("line_type", &T::lineType), text<T>("description", &T::description)};
    addMeta(t);
    return t;
}

Table<GeometryObject> makeTable(Tag<GeometryObject>) {
    using T = GeometryObject;
    Table<T> t{"geometry_object", {}, {}, {"CREATE INDEX ix_geometry_layer ON geometry_object(layer_uuid)"}};
    t.columns = {
        text<T>("name", &T::name), enumCol<T>("geometry_type", &T::type),
        ref<T>("layer_uuid", &T::layerId, "cad_layer", "RESTRICT", true),
        custom<T>("vertices", "BLOB NOT NULL CHECK (length(vertices) % 24 = 0)",
                  [](Stmt& s, int i, const T& e) {
                      const auto b = packVector(e.vertices);
                      s.bindBlob(i, b.data(), static_cast<int>(b.size()));
                  },
                  [](const Stmt& s, int i, T& e) { e.vertices = unpackVector<Vec3>(s.blob(i), "geometry vertices"); }),
        custom<T>("bulges", "BLOB",
                  [](Stmt& s, int i, const T& e) {
                      if (e.bulges.empty()) s.bindNull(i);
                      else {
                          const auto b = packVector(e.bulges);
                          s.bindBlob(i, b.data(), static_cast<int>(b.size()));
                      }
                  },
                  [](const Stmt& s, int i, T& e) {
                      if (!s.isNull(i)) e.bulges = unpackVector<double>(s.blob(i), "bulges");
                  }),
        boolean<T>("closed", &T::closed),
        custom<T>("type_data", "TEXT NOT NULL",
                  [](Stmt& s, int i, const T& e) {
                      json j = json::object();
                      if (e.type == GeometryType::Arc || e.type == GeometryType::Circle)
                          j["arc"] = {{"center", toJson(e.arc.center)}, {"radius", e.arc.radius}, {"normal", toJson(e.arc.normal)},
                                      {"refDir", toJson(e.arc.refDir)}, {"start", e.arc.startAngle}, {"end", e.arc.endAngle}};
                      if (e.type == GeometryType::Text)
                          j["text"] = {{"text", e.text.text}, {"height", e.text.height}, {"rotation", e.text.rotation},
                                       {"align", toString(e.text.align)}};
                      if (e.type == GeometryType::Dimension)
                          j["dimension"] = {{"kind", toString(e.dimension.kind)}, {"override", e.dimension.overrideText},
                                            {"precision", e.dimension.precision}, {"textHeight", e.dimension.textHeight}};
                      if (e.type == GeometryType::Hatch)
                          j["hatch"] = {{"pattern", e.hatch.pattern}, {"angle", e.hatch.angle}, {"spacing", e.hatch.spacing},
                                        {"loopStarts", e.hatch.loopStarts}};
                      s.bind(i, j.dump());
                  },
                  [](const Stmt& s, int i, T& e) {
                      const json j = parseJson(s.text(i), "geometry type data");
                      if (j.contains("arc")) {
                          const auto& a = j["arc"];
                          e.arc = {vec3FromJson(a["center"]), a["radius"].get<double>(), vec3FromJson(a["normal"]),
                                   vec3FromJson(a["refDir"]), a["start"].get<double>(), a["end"].get<double>()};
                      }
                      if (j.contains("text")) {
                          const auto& a = j["text"];
                          e.text.text = a["text"].get<std::string>();
                          e.text.height = a["height"].get<double>();
                          e.text.rotation = a["rotation"].get<double>();
                          e.text.align = enumFromString<TextAlign>(a["align"].get<std::string>()).value_or(TextAlign::Left);
                      }
                      if (j.contains("dimension")) {
                          const auto& a = j["dimension"];
                          e.dimension.kind = enumFromString<DimensionKind>(a["kind"].get<std::string>()).value_or(DimensionKind::Aligned);
                          e.dimension.overrideText = a["override"].get<std::string>();
                          e.dimension.precision = a["precision"].get<int>();
                          e.dimension.textHeight = a["textHeight"].get<double>();
                      }
                      if (j.contains("hatch")) {
                          const auto& a = j["hatch"];
                          e.hatch.pattern = a["pattern"].get<std::string>();
                          e.hatch.angle = a["angle"].get<double>();
                          e.hatch.spacing = a["spacing"].get<double>();
                          e.hatch.loopStarts = a["loopStarts"].get<std::vector<std::uint32_t>>();
                      }
                  }),
        custom<T>("mesh_vertices", "BLOB",
                  [](Stmt& s, int i, const T& e) {
                      if (!e.mesh) s.bindNull(i);
                      else {
                          const auto b = packVector(e.mesh->vertices);
                          s.bindBlob(i, b.data(), static_cast<int>(b.size()));
                      }
                  },
                  [](const Stmt& s, int i, T& e) {
                      if (s.isNull(i)) return;
                      auto m = std::make_shared<MeshData>();
                      m->vertices = unpackVector<Vec3>(s.blob(i), "mesh vertices");
                      e.mesh = m;
                  }),
        custom<T>("mesh_triangles", "BLOB",
                  [](Stmt& s, int i, const T& e) {
                      if (!e.mesh) s.bindNull(i);
                      else {
                          const auto b = packVector(e.mesh->triangles);
                          s.bindBlob(i, b.data(), static_cast<int>(b.size()));
                      }
                  },
                  [](const Stmt& s, int i, T& e) {
                      if (s.isNull(i) || !e.mesh) return;
                      auto m = std::const_pointer_cast<MeshData>(e.mesh);
                      m->triangles = unpackVector<std::array<std::uint32_t, 3>>(s.blob(i), "mesh triangles");
                  }),
        custom<T>("mesh_scalars", "BLOB",
                  [](Stmt& s, int i, const T& e) {
                      if (!e.mesh || e.mesh->vertexScalars.empty()) s.bindNull(i);
                      else {
                          const auto b = packVector(e.mesh->vertexScalars);
                          s.bindBlob(i, b.data(), static_cast<int>(b.size()));
                      }
                  },
                  [](const Stmt& s, int i, T& e) {
                      if (s.isNull(i) || !e.mesh) return;
                      std::const_pointer_cast<MeshData>(e.mesh)->vertexScalars = unpackVector<float>(s.blob(i), "mesh scalars");
                  }),
        custom<T>("mesh_scalar_name", "TEXT NOT NULL", [](Stmt& s, int i, const T& e) { s.bind(i, e.mesh ? e.mesh->scalarName : std::string()); },
                  [](const Stmt& s, int i, T& e) {
                      if (e.mesh) std::const_pointer_cast<MeshData>(e.mesh)->scalarName = s.text(i);
                  }),
        assetRef<T>("brep_asset_uuid", &T::brepAsset), ref<T>("material_uuid", &T::materialId, "material"),
        custom<T>("color_override", "INTEGER",
                  [](Stmt& s, int i, const T& e) {
                      if (e.colorOverride) s.bind(i, static_cast<std::int64_t>(*e.colorOverride));
                      else s.bindNull(i);
                  },
                  [](const Stmt& s, int i, T& e) {
                      if (!s.isNull(i)) e.colorOverride = static_cast<std::uint32_t>(s.integer(i));
                  }),
        custom<T>("transparency", "REAL CHECK (transparency IS NULL OR transparency BETWEEN 0 AND 1)",
                  [](Stmt& s, int i, const T& e) {
                      if (e.transparency) s.bind(i, *e.transparency);
                      else s.bindNull(i);
                  },
                  [](const Stmt& s, int i, T& e) {
                      if (!s.isNull(i)) e.transparency = s.real(i);
                  })};
    addMeta(t);
    return t;
}

// ---- Survey -------------------------------------------------------------------------------------------
Table<SurveyControlPoint> makeTable(Tag<SurveyControlPoint>) {
    using T = SurveyControlPoint;
    Table<T> t{"survey_control_point", {}, {"UNIQUE (name)"}, {}};
    t.columns = {text<T>("name", &T::name), text<T>("code", &T::code)};
    addXyz(t, "position", &T::position);
    t.columns.push_back(real<T>("horizontal_sigma", &T::horizontalSigma, "REAL NOT NULL CHECK (horizontal_sigma >= 0)"));
    t.columns.push_back(real<T>("vertical_sigma", &T::verticalSigma, "REAL NOT NULL CHECK (vertical_sigma >= 0)"));
    t.columns.push_back(enumCol<T>("role", &T::role));
    t.columns.push_back(text<T>("source", &T::source));
    t.columns.push_back(optDateCol<T>("survey_date", &T::surveyDate));
    t.columns.push_back(text<T>("description", &T::description));
    addMeta(t);
    return t;
}

Table<PointCloudDataset> makeTable(Tag<PointCloudDataset>) {
    using T = PointCloudDataset;
    Table<T> t{"point_cloud_dataset", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), assetRef<T>("asset_uuid", &T::asset, true),
                 int64<T>("point_count", &T::pointCount, "INTEGER NOT NULL CHECK (point_count >= 0)"),
                 custom<T>("stored_bounds", "TEXT NOT NULL",
                           [](Stmt& s, int i, const T& e) { s.bind(i, json{toJson(e.storedBounds.min), toJson(e.storedBounds.max)}.dump()); },
                           [](const Stmt& s, int i, T& e) {
                               const auto j = parseJson(s.text(i), "bounds");
                               e.storedBounds = {vec3FromJson(j[0]), vec3FromJson(j[1])};
                           }),
                 custom<T>("attributes", "TEXT NOT NULL",
                           [](Stmt& s, int i, const T& e) {
                               const auto& a = e.attributes;
                               s.bind(i, json{{"intensity", a.intensity}, {"rgb", a.rgb}, {"classification", a.classification},
                                              {"normals", a.normals}, {"returnInfo", a.returnInfo}, {"gpsTime", a.gpsTime},
                                              {"scalarFields", a.scalarFields}}
                                             .dump());
                           },
                           [](const Stmt& s, int i, T& e) {
                               const auto j = parseJson(s.text(i), "point attributes");
                               auto& a = e.attributes;
                               a.intensity = j.value("intensity", false);
                               a.rgb = j.value("rgb", false);
                               a.classification = j.value("classification", false);
                               a.normals = j.value("normals", false);
                               a.returnInfo = j.value("returnInfo", false);
                               a.gpsTime = j.value("gpsTime", false);
                               a.scalarFields = j.value("scalarFields", std::vector<std::string>{});
                           }),
                 transformCol<T>("transform", &T::transform), text<T>("source_file", &T::sourceFile),
                 text<T>("source_format", &T::sourceFormat), text<T>("source_crs", &T::sourceCrs),
                 ref<T>("parent_dataset_uuid", &T::parentDatasetId, "point_cloud_dataset"), text<T>("derivation", &T::derivation),
                 optDateCol<T>("scan_date", &T::scanDate), text<T>("scanner", &T::scanner), boolean<T>("visible", &T::visible),
                 real<T>("point_size", &T::pointSize), text<T>("color_mode", &T::colorMode),
                 ref<T>("development_uuid", &T::developmentId, "mine_development")};
    addMeta(t);
    return t;
}

Table<Registration> makeTable(Tag<Registration>) {
    using T = Registration;
    Table<T> t{"registration", {}, {}, {"CREATE INDEX ix_registration_dataset ON registration(dataset_uuid)"}};
    t.columns = {ref<T>("dataset_uuid", &T::datasetId, "point_cloud_dataset", "CASCADE", true),
                 ref<T>("target_dataset_uuid", &T::targetDatasetId, "point_cloud_dataset", "RESTRICT"),
                 enumCol<T>("method", &T::method), boolean<T>("allow_scale", &T::allowScale), transformCol<T>("transform", &T::transform),
                 transformCol<T>("previous_transform", &T::previousTransform), real<T>("fit_rms", &T::fitRms),
                 real<T>("fit_max", &T::fitMax), integer<T>("fit_count", &T::fitCount), real<T>("check_rms", &T::checkRms),
                 real<T>("check_max", &T::checkMax), integer<T>("check_count", &T::checkCount), real<T>("icp_fitness", &T::icpFitness),
                 real<T>("icp_inlier_rms", &T::icpInlierRms), integer<T>("icp_iterations", &T::icpIterations),
                 boolean<T>("icp_converged", &T::icpConverged), real<T>("icp_max_correspondence", &T::icpMaxCorrespondence),
                 boolean<T>("applied", &T::applied), text<T>("notes", &T::notes)};
    addMeta(t);
    return t;
}

Table<AsBuiltModel> makeTable(Tag<AsBuiltModel>) {
    using T = AsBuiltModel;
    Table<T> t{"as_built_model", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), ref<T>("source_dataset_uuid", &T::sourceDatasetId, "point_cloud_dataset", "RESTRICT"),
                 ref<T>("registration_uuid", &T::registrationId, "registration", "RESTRICT"),
                 ref<T>("geometry_uuid", &T::geometryId, "geometry_object", "RESTRICT", true),
                 ref<T>("development_uuid", &T::developmentId, "mine_development"), text<T>("method", &T::method),
                 optDateCol<T>("survey_date", &T::surveyDate)};
    addMeta(t);
    return t;
}

Table<DesignConformanceResult> makeTable(Tag<DesignConformanceResult>) {
    using T = DesignConformanceResult;
    Table<T> t{"design_conformance_result", {}, {}, {}};
    t.columns = {ref<T>("as_built_uuid", &T::asBuiltId, "as_built_model", "CASCADE", true),
                 ref<T>("development_uuid", &T::developmentId, "mine_development", "CASCADE", true),
                 real<T>("station_interval", &T::stationInterval, "REAL NOT NULL CHECK (station_interval > 0)"),
                 real<T>("chainage_from", &T::chainageFrom), real<T>("chainage_to", &T::chainageTo),
                 real<T>("design_volume", &T::designVolume), real<T>("as_built_volume", &T::asBuiltVolume),
                 real<T>("overbreak_volume", &T::overbreakVolume), real<T>("underbreak_volume", &T::underbreakVolume),
                 real<T>("mean_overbreak_depth", &T::meanOverbreakDepth), real<T>("max_underbreak_depth", &T::maxUnderbreakDepth),
                 real<T>("min_clearance", &T::minClearance), integer<T>("station_count", &T::stationCount),
                 ref<T>("analysis_result_uuid", &T::analysisResultId, "analysis_result", "RESTRICT")};
    addMeta(t);
    return t;
}

// ---- Design --------------------------------------------------------------------------------------------
Table<TunnelProfile> makeTable(Tag<TunnelProfile>) {
    using T = TunnelProfile;
    Table<T> t{"tunnel_profile", {}, {"UNIQUE (name)"}, {}};
    t.columns = {text<T>("name", &T::name), enumCol<T>("shape", &T::shape), real<T>("width", &T::width, "REAL NOT NULL CHECK (width > 0)"),
                 real<T>("height", &T::height, "REAL NOT NULL CHECK (height > 0)"), real<T>("shoulder_radius", &T::shoulderRadius),
                 real<T>("arch_rise", &T::archRise), real<T>("wall_batter", &T::wallBatter),
                 custom<T>("custom_points", "TEXT NOT NULL",
                           [](Stmt& s, int i, const T& e) {
                               json j = json::array();
                               for (auto p : e.customPoints) j.push_back(toJson(p));
                               s.bind(i, j.dump());
                           },
                           [](const Stmt& s, int i, T& e) {
                               for (const auto& p : parseJson(s.text(i), "profile points")) e.customPoints.push_back(vec2FromJson(p));
                           }),
                 enumCol<T>("centreline_reference", &T::reference), integer<T>("arc_segments", &T::arcSegments)};
    addMeta(t);
    return t;
}

Table<MineLevel> makeTable(Tag<MineLevel>) {
    using T = MineLevel;
    Table<T> t{"mine_level", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), text<T>("code", &T::code), real<T>("elevation", &T::elevation), text<T>("zone", &T::zone),
                 text<T>("status", &T::status), text<T>("description", &T::description)};
    addMeta(t);
    return t;
}

Table<Decline> makeTable(Tag<Decline>) {
    using T = Decline;
    Table<T> t{"decline", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), text<T>("code", &T::code), text<T>("zone", &T::zone), text<T>("number", &T::number),
                 real<T>("design_gradient_percent", &T::designGradientPercent), real<T>("min_curve_radius", &T::minCurveRadius),
                 ref<T>("profile_uuid", &T::profileId, "tunnel_profile", "RESTRICT"), text<T>("status", &T::status)};
    addMeta(t);
    return t;
}

Table<GroundSupportClass> makeTable(Tag<GroundSupportClass>) {
    using T = GroundSupportClass;
    Table<T> t{"ground_support_class", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), real<T>("bolt_length", &T::boltLength), real<T>("bolt_spacing", &T::boltSpacing),
                 text<T>("bolt_type", &T::boltType), boolean<T>("mesh", &T::mesh), real<T>("shotcrete_thickness", &T::shotcreteThickness),
                 real<T>("install_minutes_per_bolt", &T::installMinutesPerBolt),
                 real<T>("mesh_minutes_per_m2", &T::meshMinutesPerSquareMetre),
                 real<T>("shotcrete_minutes_per_m3", &T::shotcreteMinutesPerCubicMetre), text<T>("description", &T::description)};
    addMeta(t);
    return t;
}

Table<MineDevelopment> makeTable(Tag<MineDevelopment>) {
    using T = MineDevelopment;
    Table<T> t{"mine_development", {}, {"UNIQUE (heading_id)", "UNIQUE (geometry_uuid)"},
               {"CREATE INDEX ix_development_level ON mine_development(level_uuid)"}};
    t.columns = {text<T>("heading_id", &T::headingId), text<T>("name", &T::name), text<T>("zone", &T::zone),
                 text<T>("decline_number", &T::declineNumber), text<T>("level", &T::level),
                 ref<T>("level_uuid", &T::levelId, "mine_level"), ref<T>("decline_uuid", &T::declineId, "decline"),
                 text<T>("activity_type", &T::activityType), real<T>("chainage_from", &T::chainageFrom),
                 real<T>("design_width", &T::designWidth, "REAL NOT NULL CHECK (design_width > 0)"),
                 real<T>("design_height", &T::designHeight, "REAL NOT NULL CHECK (design_height > 0)"),
                 ref<T>("profile_uuid", &T::profileId, "tunnel_profile", "RESTRICT"), ref<T>("material_uuid", &T::materialId, "material"),
                 ref<T>("support_class_uuid", &T::supportClassId, "ground_support_class"), text<T>("status", &T::status),
                 ref<T>("geometry_uuid", &T::geometryId, "geometry_object", "RESTRICT", true),
                 real<T>("approved_design_length", &T::approvedDesignLength), text<T>("design_revision", &T::designRevision),
                 uuidCol<T>("current_revision_uuid", &T::currentRevisionId), integer<T>("priority", &T::priority)};
    addMeta(t);
    return t;
}

Table<Stope> makeTable(Tag<Stope>) {
    using T = Stope;
    Table<T> t{"stope", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), text<T>("code", &T::code), ref<T>("level_uuid", &T::levelId, "mine_level"),
                 text<T>("zone", &T::zone), enumCol<T>("method", &T::method),
                 ref<T>("geometry_uuid", &T::geometryId, "geometry_object", "RESTRICT", true),
                 ref<T>("material_uuid", &T::materialId, "material"), real<T>("grade", &T::grade), text<T>("grade_unit", &T::gradeUnit),
                 real<T>("dilution_percent", &T::dilutionPercent), real<T>("recovery_percent", &T::recoveryPercent),
                 text<T>("status", &T::status), text<T>("design_revision", &T::designRevision),
                 uuidCol<T>("current_revision_uuid", &T::currentRevisionId), integer<T>("priority", &T::priority)};
    addMeta(t);
    return t;
}

Table<MiningBlock> makeTable(Tag<MiningBlock>) {
    using T = MiningBlock;
    Table<T> t{"mining_block", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), ref<T>("stope_uuid", &T::stopeId, "stope", "RESTRICT", true),
                 integer<T>("sequence", &T::sequence), ref<T>("geometry_uuid", &T::geometryId, "geometry_object", "RESTRICT", true),
                 text<T>("status", &T::status)};
    addMeta(t);
    return t;
}

Table<DesignRevision> makeTable(Tag<DesignRevision>) {
    using T = DesignRevision;
    Table<T> t{"design_revision", {}, {"UNIQUE (entity_uuid, revision_code)"},
               {"CREATE INDEX ix_revision_entity ON design_revision(entity_uuid)"}};
    t.columns = {uuidCol<T>("entity_uuid", &T::entityId, true), text<T>("entity_type", &T::entityType),
                 text<T>("revision_code", &T::revisionCode), text<T>("snapshot_json", &T::snapshotJson), real<T>("length", &T::length),
                 real<T>("volume", &T::volume), boolean<T>("approved", &T::approved), text<T>("approved_by", &T::approvedBy),
                 text<T>("approved_at", &T::approvedAt), text<T>("comment", &T::comment), text<T>("created_at", &T::createdAt),
                 text<T>("author", &T::author)};
    return t;
}

// ---- Scheduling -----------------------------------------------------------------------------------------
Table<WorkCalendar> makeTable(Tag<WorkCalendar>) {
    using T = WorkCalendar;
    Table<T> t{"work_calendar", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), enumCol<T>("kind", &T::kind),
                 custom<T>("shifts", "TEXT NOT NULL",
                           [](Stmt& s, int i, const T& e) {
                               json j = json::array();
                               for (const auto& sh : e.shifts)
                                   j.push_back({{"name", sh.name}, {"start", sh.startMinute}, {"duration", sh.durationMinutes},
                                                {"weekdays", sh.weekdays}});
                               s.bind(i, j.dump());
                           },
                           [](const Stmt& s, int i, T& e) {
                               for (const auto& sh : parseJson(s.text(i), "shifts"))
                                   e.shifts.push_back({sh["name"].get<std::string>(), sh["start"].get<int>(), sh["duration"].get<int>(),
                                                       sh["weekdays"].get<std::uint8_t>()});
                           }),
                 custom<T>("exceptions", "TEXT NOT NULL",
                           [](Stmt& s, int i, const T& e) {
                               json j = json::array();
                               for (const auto& ex : e.exceptions) {
                                   json p = json::array();
                                   for (const auto& r : ex.periods) p.push_back({r.startMinute, r.endMinute});
                                   j.push_back({{"date", ex.date.toString()}, {"periods", p}, {"reason", ex.reason}});
                               }
                               s.bind(i, j.dump());
                           },
                           [](const Stmt& s, int i, T& e) {
                               for (const auto& ex : parseJson(s.text(i), "exceptions")) {
                                   CalendarException c;
                                   auto d = Date::parse(ex["date"].get<std::string>());
                                   if (!d) throw PersistenceError("Corrupt calendar exception date");
                                   c.date = *d;
                                   for (const auto& r : ex["periods"]) c.periods.push_back({r[0].get<int>(), r[1].get<int>()});
                                   c.reason = ex.value("reason", std::string());
                                   e.exceptions.push_back(c);
                               }
                           }),
                 real<T>("standard_hours_per_day", &T::standardHoursPerDay,
                         "REAL NOT NULL CHECK (standard_hours_per_day > 0 AND standard_hours_per_day <= 24)"),
                 text<T>("description", &T::description)};
    addMeta(t);
    return t;
}

Table<ScheduleActivity> makeTable(Tag<ScheduleActivity>) {
    using T = ScheduleActivity;
    Table<T> t{"schedule_activity", {}, {}, {"CREATE INDEX ix_activity_parent ON schedule_activity(parent_uuid)"}};
    t.columns = {ref<T>("parent_uuid", &T::parentId, "schedule_activity", "RESTRICT"), text<T>("wbs_code", &T::wbsCode),
                 text<T>("name", &T::name), enumCol<T>("kind", &T::kind),
                 real<T>("duration_hours", &T::durationHours, "REAL NOT NULL CHECK (duration_hours >= 0)"),
                 boolean<T>("duration_from_rate", &T::durationFromRate),
                 real<T>("quantity", &T::quantity, "REAL NOT NULL CHECK (quantity >= 0)"), text<T>("quantity_unit", &T::quantityUnit),
                 real<T>("rate", &T::rate), enumCol<T>("rate_unit", &T::rateUnit),
                 ref<T>("production_model_uuid", &T::productionModelId, "development_cycle"), boolean<T>("use_cycle_rate", &T::useCycleRate),
                 real<T>("actual_quantity", &T::actualQuantity), real<T>("manual_percent_complete", &T::manualPercentComplete),
                 optDateTimeCol<T>("actual_start", &T::actualStart), optDateTimeCol<T>("actual_finish", &T::actualFinish),
                 enumCol<T>("constraint_type", &T::constraint), optDateTimeCol<T>("constraint_date", &T::constraintDate),
                 ref<T>("calendar_uuid", &T::calendarId, "work_calendar", "RESTRICT"), integer<T>("priority", &T::priority),
                 text<T>("work_area", &T::workArea), integer<T>("sort_order", &T::sortOrder),
                 boolean<T>("design_impact_flag", &T::designImpactFlag), text<T>("design_impact_note", &T::designImpactNote)};
    addMeta(t);
    return t;
}

Table<ActivityDependency> makeTable(Tag<ActivityDependency>) {
    using T = ActivityDependency;
    Table<T> t{"activity_dependency", {}, {"UNIQUE (predecessor_uuid, successor_uuid)", "CHECK (predecessor_uuid <> successor_uuid)"},
               {"CREATE INDEX ix_dependency_successor ON activity_dependency(successor_uuid)"}};
    t.columns = {ref<T>("predecessor_uuid", &T::predecessorId, "schedule_activity", "CASCADE", true),
                 ref<T>("successor_uuid", &T::successorId, "schedule_activity", "CASCADE", true),
                 enumCol<T>("dependency_type", &T::type), real<T>("lag_hours", &T::lagHours)};
    addMeta(t);
    return t;
}

Table<Resource> makeTable(Tag<Resource>) {
    using T = Resource;
    Table<T> t{"resource", {}, {"UNIQUE (name)"}, {}};
    t.columns = {text<T>("name", &T::name), text<T>("code", &T::code), enumCol<T>("kind", &T::kind),
                 real<T>("capacity", &T::capacity, "REAL NOT NULL CHECK (capacity > 0)"),
                 ref<T>("calendar_uuid", &T::calendarId, "work_calendar", "RESTRICT"),
                 real<T>("productivity_factor", &T::productivityFactor, "REAL NOT NULL CHECK (productivity_factor > 0)"),
                 real<T>("cost_per_hour", &T::costPerHour), text<T>("description", &T::description)};
    addMeta(t);
    return t;
}

Table<ActivityResourceAssignment> makeTable(Tag<ActivityResourceAssignment>) {
    using T = ActivityResourceAssignment;
    Table<T> t{"activity_resource_assignment", {}, {"UNIQUE (activity_uuid, resource_uuid)"},
               {"CREATE INDEX ix_assignment_resource ON activity_resource_assignment(resource_uuid)"}};
    t.columns = {ref<T>("activity_uuid", &T::activityId, "schedule_activity", "CASCADE", true),
                 ref<T>("resource_uuid", &T::resourceId, "resource", "CASCADE", true),
                 real<T>("units", &T::units, "REAL NOT NULL CHECK (units > 0)"), boolean<T>("drives_rate", &T::drivesRate)};
    addMeta(t);
    return t;
}

Table<DevelopmentCycle> makeTable(Tag<DevelopmentCycle>) {
    using T = DevelopmentCycle;
    Table<T> t{"development_cycle", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), real<T>("round_length", &T::roundLength), real<T>("advance_efficiency", &T::advanceEfficiency),
                 integer<T>("holes_per_round", &T::holesPerRound), real<T>("drill_penetration_rate", &T::drillPenetrationRate),
                 integer<T>("drill_booms", &T::drillBooms), real<T>("drill_setup_minutes", &T::drillSetupMinutes),
                 real<T>("collar_minutes_per_hole", &T::collarMinutesPerHole), real<T>("charge_minutes_per_hole", &T::chargeMinutesPerHole),
                 real<T>("blast_reentry_minutes", &T::blastAndReentryMinutes), real<T>("swell_factor", &T::swellFactor),
                 real<T>("bucket_capacity", &T::bucketCapacity), real<T>("bucket_fill_factor", &T::bucketFillFactor),
                 real<T>("muck_cycle_minutes", &T::muckCycleMinutes), integer<T>("bolts_per_round", &T::boltsPerRound),
                 real<T>("minutes_per_bolt", &T::minutesPerBolt), real<T>("mesh_minutes", &T::meshMinutes),
                 real<T>("shotcrete_minutes", &T::shotcreteMinutes), real<T>("scaling_minutes", &T::scalingMinutes),
                 real<T>("survey_markup_minutes", &T::surveyMarkupMinutes), real<T>("service_minutes", &T::serviceMinutes),
                 real<T>("utilisation", &T::utilisation), real<T>("shift_hours", &T::shiftHours), integer<T>("shifts_per_day", &T::shiftsPerDay),
                 text<T>("notes", &T::notes)};
    addMeta(t);
    return t;
}

Table<ProductionActual> makeTable(Tag<ProductionActual>) {
    using T = ProductionActual;
    Table<T> t{"production_actual", {}, {}, {"CREATE INDEX ix_actual_activity ON production_actual(activity_uuid, date)"}};
    t.columns = {ref<T>("activity_uuid", &T::activityId, "schedule_activity", "RESTRICT", true),
                 ref<T>("development_uuid", &T::developmentId, "mine_development"), dateCol<T>("date", &T::date),
                 real<T>("quantity", &T::quantity), text<T>("unit", &T::unit), enumCol<T>("source", &T::source),
                 ref<T>("survey_dataset_uuid", &T::surveyDatasetId, "point_cloud_dataset"),
                 custom<T>("face_chainage", "REAL",
                           [](Stmt& s, int i, const T& e) {
                               if (e.faceChainage) s.bind(i, *e.faceChainage);
                               else s.bindNull(i);
                           },
                           [](const Stmt& s, int i, T& e) {
                               if (!s.isNull(i)) e.faceChainage = s.real(i);
                           }),
                 text<T>("note", &T::note), text<T>("entered_by", &T::enteredBy)};
    addMeta(t);
    return t;
}

Table<ProductionTarget> makeTable(Tag<ProductionTarget>) {
    using T = ProductionTarget;
    Table<T> t{"production_target", {}, {"CHECK (period_end >= period_start)"}, {}};
    t.columns = {text<T>("name", &T::name), dateCol<T>("period_start", &T::periodStart), dateCol<T>("period_end", &T::periodEnd),
                 real<T>("quantity", &T::quantity), text<T>("unit", &T::unit), text<T>("scope_zone", &T::scopeZone),
                 text<T>("scope_level", &T::scopeLevel)};
    addMeta(t);
    return t;
}

Table<ScheduleBaseline> makeTable(Tag<ScheduleBaseline>) {
    using T = ScheduleBaseline;
    Table<T> t{"schedule_baseline", {}, {"UNIQUE (name)"}, {}};
    t.columns = {text<T>("name", &T::name), text<T>("captured_at", &T::capturedAt), boolean<T>("approved", &T::approved)};
    return t;
}

Table<Scenario> makeTable(Tag<Scenario>) {
    using T = Scenario;
    Table<T> t{"scenario", {}, {"UNIQUE (name)"}, {}};
    t.columns = {text<T>("name", &T::name), text<T>("description", &T::description), enumCol<T>("levelling", &T::levelling),
                 text<T>("objective", &T::objective), real<T>("optimiser_time_limit_s", &T::optimiserTimeLimitSeconds)};
    addMeta(t);
    return t;
}

// ---- Geology ------------------------------------------------------------------------------------------
Table<Drillhole> makeTable(Tag<Drillhole>) {
    using T = Drillhole;
    Table<T> t{"drillhole", {}, {"UNIQUE (hole_id)"}, {}};
    t.columns = {text<T>("hole_id", &T::holeId)};
    addXyz(t, "collar", &T::collar);
    t.columns.push_back(real<T>("length", &T::length, "REAL NOT NULL CHECK (length > 0)"));
    t.columns.push_back(text<T>("hole_type", &T::holeType));
    t.columns.push_back(optDateCol<T>("drilled_date", &T::drilledDate));
    addMeta(t);
    return t;
}

Table<GeologicalObject> makeTable(Tag<GeologicalObject>) {
    using T = GeologicalObject;
    Table<T> t{"geological_object", {}, {}, {}};
    t.columns = {text<T>("name", &T::name), enumCol<T>("kind", &T::kind),
                 ref<T>("geometry_uuid", &T::geometryId, "geometry_object", "RESTRICT", true), text<T>("rock_unit", &T::rockUnit),
                 ref<T>("material_uuid", &T::materialId, "material"), text<T>("description", &T::description)};
    addMeta(t);
    return t;
}

Table<BlockModel> makeTable(Tag<BlockModel>) {
    using T = BlockModel;
    Table<T> t{"block_model", {}, {"CHECK (nx >= 1 AND ny >= 1 AND nz >= 1)"}, {}};
    t.columns = {text<T>("name", &T::name)};
    addXyz(t, "origin", &T::origin);
    addXyz(t, "block_size", &T::blockSize);
    t.columns.push_back(integer<T>("nx", &T::nx));
    t.columns.push_back(integer<T>("ny", &T::ny));
    t.columns.push_back(integer<T>("nz", &T::nz));
    t.columns.push_back(real<T>("rotation_deg", &T::rotationDeg));
    t.columns.push_back(custom<T>("attributes", "TEXT NOT NULL", [](Stmt& s, int i, const T& e) { s.bind(i, json(e.attributes).dump()); },
                                  [](const Stmt& s, int i, T& e) {
                                      e.attributes = parseJson(s.text(i), "block attributes").get<std::vector<std::string>>();
                                  }));
    t.columns.push_back(assetRef<T>("data_asset_uuid", &T::dataAsset));
    t.columns.push_back(text<T>("description", &T::description));
    addMeta(t);
    return t;
}

Table<StructuralMeasurement> makeTable(Tag<StructuralMeasurement>) {
    using T = StructuralMeasurement;
    Table<T> t{"structural_measurement", {}, {}, {}};
    t.columns = {text<T>("set_name", &T::setName), enumCol<T>("type", &T::type),
                 real<T>("dip", &T::dip, "REAL NOT NULL CHECK (dip BETWEEN 0 AND 90)"),
                 real<T>("dip_direction", &T::dipDirection, "REAL NOT NULL CHECK (dip_direction >= 0 AND dip_direction < 360)")};
    addXyz(t, "location", &T::location);
    t.columns.push_back(real<T>("persistence", &T::persistence));
    t.columns.push_back(text<T>("roughness", &T::roughness));
    t.columns.push_back(text<T>("infill", &T::infill));
    t.columns.push_back(ref<T>("development_uuid", &T::developmentId, "mine_development"));
    addMeta(t);
    return t;
}

Table<GeotechClassification> makeTable(Tag<GeotechClassification>) {
    using T = GeotechClassification;
    Table<T> t{"geotech_classification", {}, {}, {}};
    t.columns = {enumCol<T>("method", &T::method), ref<T>("development_uuid", &T::developmentId, "mine_development"),
                 real<T>("chainage", &T::chainage)};
    addXyz(t, "location", &T::location);
    t.columns.push_back(text<T>("inputs_json", &T::inputsJson));
    t.columns.push_back(real<T>("value", &T::value));
    t.columns.push_back(text<T>("rock_class", &T::rockClass));
    t.columns.push_back(text<T>("notes", &T::notes));
    t.columns.push_back(optDateCol<T>("mapped_date", &T::mappedDate));
    t.columns.push_back(text<T>("mapped_by", &T::mappedBy));
    addMeta(t);
    return t;
}

template <class T> const Table<T>& tableFor() {
    static const Table<T> t = makeTable(Tag<T>{});
    return t;
}

// ---- Entity documents ---------------------------------------------------------------------------------
Db& codecDb() {
    thread_local Db db(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    return db;
}

std::string base64(const std::vector<unsigned char>& b) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < b.size(); i += 3) {
        const unsigned v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == b.size()) {
        const unsigned v = b[i] << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == b.size()) {
        const unsigned v = (b[i] << 16) | (b[i + 1] << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::vector<unsigned char> unbase64(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> out;
    unsigned buf = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) throw PersistenceError("Corrupt base64 in entity document");
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

json realJson(double v) {
    if (std::isfinite(v)) return v;
    return json{{"$f", std::isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf")}};
}
double realFromJson(const json& j) {
    if (j.is_object()) {
        const std::string f = j.at("$f");
        if (f == "nan") return std::numeric_limits<double>::quiet_NaN();
        return f == "inf" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
    }
    return j.get<double>();
}

template <class T> json encodeChildren(const T& e) {
    json c = json::object();
    if constexpr (std::is_same_v<T, GeometryObject> || std::is_same_v<T, MineDevelopment> || std::is_same_v<T, Stope> ||
                  std::is_same_v<T, ScheduleActivity>)
        c["attributes"] = e.attributes;
    if constexpr (std::is_same_v<T, Registration>) {
        json a = json::array();
        for (const auto& p : e.pairs)
            a.push_back({{"name", p.name}, {"source", toJson(p.source)}, {"target", toJson(p.target)},
                         {"target_control", p.targetControlId.isNil() ? json(nullptr) : json(p.targetControlId.toString())},
                         {"role", toString(p.role)}, {"residual", toJson(p.residual)}});
        c["pairs"] = a;
    }
    if constexpr (std::is_same_v<T, ScheduleBaseline>) {
        json a = json::array();
        for (const auto& b : e.activities)
            a.push_back({{"activity", b.activityId.toString()}, {"name", b.name}, {"start", b.start.toString()}, {"finish", b.finish.toString()},
                         {"duration_hours", realJson(b.durationHours)}, {"quantity", realJson(b.quantity)}});
        c["activities"] = a;
    }
    if constexpr (std::is_same_v<T, Scenario>) {
        json a = json::array();
        for (const auto& o : e.overrides)
            a.push_back({{"entity", o.entityId.isNil() ? json(nullptr) : json(o.entityId.toString())}, {"field", o.field}, {"value", o.value}});
        c["overrides"] = a;
    }
    if constexpr (std::is_same_v<T, Drillhole>) {
        json sv = json::array(), iv = json::array();
        for (const auto& s : e.surveys) sv.push_back({realJson(s.depth), realJson(s.azimuth), realJson(s.dip)});
        for (const auto& i : e.intervals) {
            json vals = json::object();
            for (const auto& [k, v] : i.values) vals[k] = realJson(v);
            iv.push_back({{"from", realJson(i.from)}, {"to", realJson(i.to)}, {"values", vals}, {"codes", i.codes}});
        }
        c["surveys"] = sv;
        c["intervals"] = iv;
    }
    return c;
}

Uuid uuidOrNil(const json& j) {
    if (j.is_null()) return {};
    auto u = Uuid::parse(j.get<std::string>());
    if (!u) throw PersistenceError("Corrupt identifier in entity document");
    return *u;
}

template <class T> void decodeChildren(const json& c, T& e) {
    if constexpr (std::is_same_v<T, GeometryObject> || std::is_same_v<T, MineDevelopment> || std::is_same_v<T, Stope> ||
                  std::is_same_v<T, ScheduleActivity>) {
        if (c.contains("attributes")) e.attributes = c.at("attributes").get<AttributeMap>();
    }
    if constexpr (std::is_same_v<T, Registration>) {
        e.pairs.clear();
        for (const auto& p : c.at("pairs")) {
            RegistrationPair r;
            r.name = p.at("name");
            r.source = vec3FromJson(p.at("source"));
            r.target = vec3FromJson(p.at("target"));
            r.targetControlId = uuidOrNil(p.at("target_control"));
            r.role = enumFromString<PairRole>(p.at("role").get<std::string>()).value_or(PairRole::Fit);
            r.residual = vec3FromJson(p.at("residual"));
            e.pairs.push_back(r);
        }
    }
    if constexpr (std::is_same_v<T, ScheduleBaseline>) {
        e.activities.clear();
        for (const auto& b : c.at("activities")) {
            auto st = DateTime::parse(b.at("start").get<std::string>()), fi = DateTime::parse(b.at("finish").get<std::string>());
            if (!st || !fi) throw PersistenceError("Corrupt baseline date in entity document");
            e.activities.push_back({uuidOrNil(b.at("activity")), b.at("name"), *st, *fi, realFromJson(b.at("duration_hours")), realFromJson(b.at("quantity"))});
        }
    }
    if constexpr (std::is_same_v<T, Scenario>) {
        e.overrides.clear();
        for (const auto& o : c.at("overrides")) e.overrides.push_back({uuidOrNil(o.at("entity")), o.at("field"), o.at("value")});
    }
    if constexpr (std::is_same_v<T, Drillhole>) {
        e.surveys.clear();
        e.intervals.clear();
        for (const auto& s : c.at("surveys")) e.surveys.push_back({realFromJson(s.at(0)), realFromJson(s.at(1)), realFromJson(s.at(2))});
        for (const auto& i : c.at("intervals")) {
            DrillholeInterval d;
            d.from = realFromJson(i.at("from"));
            d.to = realFromJson(i.at("to"));
            for (const auto& [k, v] : i.at("values").items()) d.values[k] = realFromJson(v);
            d.codes = i.at("codes").get<std::map<std::string, std::string>>();
            e.intervals.push_back(std::move(d));
        }
    }
}
} // namespace

template <class T> std::string encodeEntityV2(const T& e) {
    const auto& t = tableFor<T>();
    std::string sql = "SELECT ";
    for (std::size_t i = 0; i < t.columns.size(); ++i) sql += (i ? ", ?" : "?") + std::to_string(i + 1);
    Stmt st(codecDb(), sql);
    for (std::size_t i = 0; i < t.columns.size(); ++i) t.columns[i].bind(st, static_cast<int>(i) + 1, e);
    if (!st.step()) throw PersistenceError("Entity encoding failed");
    json cols = json::object();
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        const int c = static_cast<int>(i);
        switch (st.type(c)) {
        case SQLITE_NULL: cols[t.columns[i].name] = nullptr; break;
        case SQLITE_INTEGER: cols[t.columns[i].name] = st.integer(c); break;
        case SQLITE_FLOAT: cols[t.columns[i].name] = realJson(st.real(c)); break;
        case SQLITE_TEXT: cols[t.columns[i].name] = st.text(c); break;
        default: cols[t.columns[i].name] = json{{"$b64", base64(st.blob(c))}}; break;
        }
    }
    json doc{{"uuid", e.id.toString()}, {"columns", cols}, {"children", encodeChildren(e)}};
    return doc.dump();
}

template <class T> T decodeEntityV2(const std::string& text) {
    const json doc = parseJson(text, "entity document");
    const auto& t = tableFor<T>();
    const auto& cols = doc.at("columns");
    std::string sql = "SELECT ";
    for (std::size_t i = 0; i < t.columns.size(); ++i) sql += (i ? ", ?" : "?") + std::to_string(i + 1);
    Stmt st(codecDb(), sql);
    std::vector<std::vector<unsigned char>> blobs(t.columns.size());
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        const int k = static_cast<int>(i) + 1;
        const auto it = cols.find(t.columns[i].name);
        if (it == cols.end()) throw PersistenceError("Entity document lacks column " + t.columns[i].name);
        const json& v = *it;
        if (v.is_null()) st.bindNull(k);
        else if (v.is_number_integer() || v.is_number_unsigned()) st.bind(k, v.get<std::int64_t>());
        else if (v.is_number_float()) st.bind(k, v.get<double>());
        else if (v.is_string()) st.bind(k, v.get<std::string>());
        else if (v.is_object() && v.contains("$b64")) {
            blobs[i] = unbase64(v.at("$b64").get<std::string>());
            st.bindBlob(k, blobs[i].data(), static_cast<int>(blobs[i].size()));
        } else if (v.is_object() && v.contains("$f")) st.bind(k, realFromJson(v));
        else throw PersistenceError("Unsupported value in entity document column " + t.columns[i].name);
    }
    if (!st.step()) throw PersistenceError("Entity decoding failed");
    T e;
    e.id = uuidOrNil(doc.at("uuid"));
    for (std::size_t i = 0; i < t.columns.size(); ++i) t.columns[i].read(st, static_cast<int>(i), e);
    decodeChildren(doc.at("children"), e);
    return e;
}

#define OPENMINE_INSTANTIATE_CODEC(T, member, Kind)                     \
    template std::string encodeEntityV2<T>(const T&);                   \
    template T decodeEntityV2<T>(const std::string&);
OPENMINE_ENTITY_TYPES(OPENMINE_INSTANTIATE_CODEC)
#undef OPENMINE_INSTANTIATE_CODEC

namespace {
// ---- Child tables (explicit relationships of nested collections) -------------------------------------------
constexpr const char* kChildTables = R"SQL(
CREATE TABLE project_info (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    uuid TEXT NOT NULL,
    name TEXT NOT NULL,
    description TEXT NOT NULL,
    schedule_start TEXT NOT NULL,
    status_date TEXT,
    project_crs_uuid TEXT REFERENCES coordinate_reference_system(uuid) ON DELETE RESTRICT,
    default_calendar_uuid TEXT REFERENCES work_calendar(uuid) ON DELETE RESTRICT,
    coordinate_display_decimals INTEGER NOT NULL CHECK (coordinate_display_decimals BETWEEN 0 AND 6),
    linear_unit TEXT NOT NULL CHECK (linear_unit = 'metre'),
    south_orientated_axes INTEGER NOT NULL,
    created_at TEXT NOT NULL, modified_at TEXT NOT NULL, revision INTEGER NOT NULL,
    app_version TEXT NOT NULL
);
CREATE TABLE attribute_value (
    entity_uuid TEXT NOT NULL,
    entity_type TEXT NOT NULL,
    key TEXT NOT NULL,
    value TEXT NOT NULL,
    PRIMARY KEY (entity_uuid, key)
);
CREATE TABLE registration_pair (
    registration_uuid TEXT NOT NULL REFERENCES registration(uuid) ON DELETE CASCADE,
    sort_index INTEGER NOT NULL,
    name TEXT NOT NULL,
    source_x REAL NOT NULL, source_y REAL NOT NULL, source_z REAL NOT NULL,
    target_x REAL NOT NULL, target_y REAL NOT NULL, target_z REAL NOT NULL,
    target_control_uuid TEXT REFERENCES survey_control_point(uuid) ON DELETE SET NULL,
    role TEXT NOT NULL CHECK (role IN ('Fit','Check')),
    residual_x REAL NOT NULL, residual_y REAL NOT NULL, residual_z REAL NOT NULL,
    PRIMARY KEY (registration_uuid, sort_index)
);
CREATE TABLE activity_design_link (
    activity_uuid TEXT NOT NULL REFERENCES schedule_activity(uuid) ON DELETE CASCADE,
    design_uuid TEXT NOT NULL,
    design_type TEXT NOT NULL CHECK (design_type IN ('MineDevelopment','Stope','MiningBlock')),
    sort_index INTEGER NOT NULL,
    PRIMARY KEY (activity_uuid, design_uuid)
);
CREATE INDEX ix_link_design ON activity_design_link(design_uuid);
CREATE TABLE baseline_activity (
    baseline_uuid TEXT NOT NULL REFERENCES schedule_baseline(uuid) ON DELETE CASCADE,
    activity_uuid TEXT NOT NULL,            -- intentionally no FK: baselines outlive activities
    sort_index INTEGER NOT NULL,
    name TEXT NOT NULL, start TEXT NOT NULL, finish TEXT NOT NULL,
    duration_hours REAL NOT NULL, quantity REAL NOT NULL,
    PRIMARY KEY (baseline_uuid, activity_uuid)
);
CREATE TABLE scenario_override (
    scenario_uuid TEXT NOT NULL REFERENCES scenario(uuid) ON DELETE CASCADE,
    sort_index INTEGER NOT NULL,
    entity_uuid TEXT,                       -- NULL for project-wide overrides (work-area limits)
    field TEXT NOT NULL,
    value TEXT NOT NULL,
    PRIMARY KEY (scenario_uuid, sort_index)
);
CREATE TABLE drillhole_survey (
    drillhole_uuid TEXT NOT NULL REFERENCES drillhole(uuid) ON DELETE CASCADE,
    depth REAL NOT NULL CHECK (depth >= 0),
    azimuth REAL NOT NULL CHECK (azimuth >= 0 AND azimuth < 360),
    dip REAL NOT NULL CHECK (dip BETWEEN -90 AND 90),
    PRIMARY KEY (drillhole_uuid, depth)
);
CREATE TABLE drillhole_interval (
    drillhole_uuid TEXT NOT NULL REFERENCES drillhole(uuid) ON DELETE CASCADE,
    depth_from REAL NOT NULL,
    depth_to REAL NOT NULL,
    numeric_values TEXT NOT NULL,
    code_values TEXT NOT NULL,
    PRIMARY KEY (drillhole_uuid, depth_from),
    CHECK (depth_to > depth_from)
);
CREATE TABLE audit_log (
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL, user TEXT NOT NULL, action TEXT NOT NULL,
    entity_type TEXT NOT NULL, entity_uuid TEXT, detail TEXT NOT NULL
);
)SQL";

std::string buildDdl() {
    std::string ddl = "CREATE TABLE schema_migrations (\n    version INTEGER PRIMARY KEY,\n    applied_at TEXT NOT NULL,\n    description TEXT NOT NULL\n);\n";
#define OPENMINE_APPEND_DDL(T, member, Kind) ddl += tableFor<T>().ddl();
    OPENMINE_ENTITY_TYPES(OPENMINE_APPEND_DDL)
#undef OPENMINE_APPEND_DDL
    ddl += kChildTables;
    return ddl;
}

template <class T> void writeAttributeValues(Db& db, const EntityStore<T>& store, const char* type) {
    Stmt s(db, "INSERT INTO attribute_value(entity_uuid, entity_type, key, value) VALUES (?,?,?,?)");
    for (const auto& e : store.all())
        for (const auto& [k, v] : e.attributes) s.bindUuid(1, e.id).bind(2, type).bind(3, k).bind(4, v).run();
}

template <class T> void readAttributeValues(Db& db, EntityStore<T>& store, const char* type) {
    Stmt s(db, "SELECT entity_uuid, key, value FROM attribute_value WHERE entity_type = ?");
    s.bind(1, type);
    std::map<Uuid, AttributeMap> values;
    while (s.step()) values[s.uuid(0)][s.text(1)] = s.text(2);
    for (auto& [id, attrs] : values) {
        const T* e = store.find(id);
        if (!e) throw PersistenceError(std::string("Orphaned attribute value for ") + type + " " + id.toString());
        T copy = *e;
        copy.attributes = std::move(attrs);
        store.upsert(copy);
    }
}
} // namespace

const std::string& schemaDdlV2() {
    static const std::string ddl = buildDdl();
    return ddl;
}

void writeModelV2(Db& db, const ProjectModel& m, const std::string& appVersion) {
#define OPENMINE_WRITE_TABLE(T, member, Kind) tableFor<T>().write(db, m.member);
    OPENMINE_ENTITY_TYPES(OPENMINE_WRITE_TABLE)
#undef OPENMINE_WRITE_TABLE
    {
        const auto& i = m.info;
        Stmt s(db, "INSERT INTO project_info VALUES (1,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        s.bindUuid(1, i.id).bind(2, i.name).bind(3, i.description).bind(4, i.scheduleStart.toString());
        if (i.statusDate) s.bind(5, i.statusDate->toString());
        else s.bindNull(5);
        s.bindUuid(6, i.projectCrsId).bindUuid(7, i.defaultCalendarId).bind(8, i.coordinateDisplayDecimals).bind(9, i.linearUnit)
            .bind(10, i.southOrientatedAxes).bind(11, i.meta.createdAt).bind(12, i.meta.modifiedAt).bind(13, i.meta.revision)
            .bind(14, appVersion);
        s.run();
    }
    writeAttributeValues(db, m.geometry, "GeometryObject");
    writeAttributeValues(db, m.developments, "MineDevelopment");
    writeAttributeValues(db, m.stopes, "Stope");
    writeAttributeValues(db, m.activities, "ScheduleActivity");
    {
        Stmt s(db, "INSERT INTO registration_pair VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        for (const auto& r : m.registrations.all()) {
            int k = 0;
            for (const auto& p : r.pairs) {
                s.bindUuid(1, r.id).bind(2, k++).bind(3, p.name).bind(4, p.source.x).bind(5, p.source.y).bind(6, p.source.z)
                    .bind(7, p.target.x).bind(8, p.target.y).bind(9, p.target.z).bindUuid(10, p.targetControlId)
                    .bind(11, toString(p.role)).bind(12, p.residual.x).bind(13, p.residual.y).bind(14, p.residual.z);
                s.run();
            }
        }
    }
    {
        Stmt s(db, "INSERT INTO activity_design_link VALUES (?,?,?,?)");
        int k = 0;
        for (const auto& l : m.designLinks) s.bindUuid(1, l.activityId).bindUuid(2, l.designId).bind(3, l.designType).bind(4, k++).run();
    }
    {
        Stmt s(db, "INSERT INTO baseline_activity VALUES (?,?,?,?,?,?,?,?)");
        for (const auto& b : m.baselines.all()) {
            int k = 0;
            for (const auto& a : b.activities)
                s.bindUuid(1, b.id).bindUuid(2, a.activityId).bind(3, k++).bind(4, a.name).bind(5, a.start.toString())
                    .bind(6, a.finish.toString()).bind(7, a.durationHours).bind(8, a.quantity).run();
        }
    }
    {
        Stmt s(db, "INSERT INTO scenario_override VALUES (?,?,?,?,?)");
        for (const auto& sc : m.scenarios.all()) {
            int k = 0;
            for (const auto& o : sc.overrides) s.bindUuid(1, sc.id).bind(2, k++).bindUuid(3, o.entityId).bind(4, o.field).bind(5, o.value).run();
        }
    }
    {
        Stmt sv(db, "INSERT INTO drillhole_survey VALUES (?,?,?,?)");
        Stmt iv(db, "INSERT INTO drillhole_interval VALUES (?,?,?,?,?)");
        for (const auto& h : m.drillholes.all()) {
            for (const auto& s : h.surveys) sv.bindUuid(1, h.id).bind(2, s.depth).bind(3, s.azimuth).bind(4, s.dip).run();
            for (const auto& i : h.intervals)
                iv.bindUuid(1, h.id).bind(2, i.from).bind(3, i.to).bind(4, json(i.values).dump()).bind(5, json(i.codes).dump()).run();
        }
    }
    {
        Stmt s(db, "INSERT INTO audit_log(timestamp, user, action, entity_type, entity_uuid, detail) VALUES (?,?,?,?,?,?)");
        for (const auto& e : m.audit)
            s.bind(1, e.timestamp).bind(2, e.user).bind(3, e.action).bind(4, e.entityType).bindUuid(5, e.entityId).bind(6, e.detail).run();
    }
}

void readModelV2(Db& db, ProjectModel& m) {
#define OPENMINE_READ_TABLE(T, member, Kind) tableFor<T>().read(db, m.member);
    OPENMINE_ENTITY_TYPES(OPENMINE_READ_TABLE)
#undef OPENMINE_READ_TABLE
    {
        Stmt s(db, "SELECT uuid,name,description,schedule_start,status_date,project_crs_uuid,default_calendar_uuid,"
                   "coordinate_display_decimals,linear_unit,south_orientated_axes,created_at,modified_at,revision FROM project_info WHERE id = 1");
        if (!s.step()) throw PersistenceError("Project file has no project_info record");
        auto& i = m.info;
        i.id = s.uuid(0);
        i.name = s.text(1);
        i.description = s.text(2);
        auto st = DateTime::parse(s.text(3));
        if (!st) throw PersistenceError("Corrupt schedule start");
        i.scheduleStart = *st;
        if (!s.isNull(4)) {
            auto sd = DateTime::parse(s.text(4));
            if (!sd) throw PersistenceError("Corrupt status date");
            i.statusDate = *sd;
        }
        i.projectCrsId = s.uuid(5);
        i.defaultCalendarId = s.uuid(6);
        i.coordinateDisplayDecimals = static_cast<int>(s.integer(7));
        i.linearUnit = s.text(8);
        i.southOrientatedAxes = s.integer(9) != 0;
        i.meta = {s.text(10), s.text(11), s.integer(12)};
    }
    readAttributeValues(db, m.geometry, "GeometryObject");
    readAttributeValues(db, m.developments, "MineDevelopment");
    readAttributeValues(db, m.stopes, "Stope");
    readAttributeValues(db, m.activities, "ScheduleActivity");
    {
        Stmt s(db, "SELECT registration_uuid,name,source_x,source_y,source_z,target_x,target_y,target_z,target_control_uuid,role,"
                   "residual_x,residual_y,residual_z FROM registration_pair ORDER BY registration_uuid, sort_index");
        std::map<Uuid, std::vector<RegistrationPair>> pairs;
        while (s.step()) {
            RegistrationPair p;
            p.name = s.text(1);
            p.source = {s.real(2), s.real(3), s.real(4)};
            p.target = {s.real(5), s.real(6), s.real(7)};
            p.targetControlId = s.uuid(8);
            p.role = enumFromString<PairRole>(s.text(9)).value_or(PairRole::Fit);
            p.residual = {s.real(10), s.real(11), s.real(12)};
            pairs[s.uuid(0)].push_back(p);
        }
        for (auto& [id, v] : pairs) {
            const auto* r = m.registrations.find(id);
            if (!r) throw PersistenceError("Registration pair without registration");
            Registration copy = *r;
            copy.pairs = std::move(v);
            m.registrations.upsert(copy);
        }
    }
    {
        Stmt s(db, "SELECT activity_uuid, design_uuid, design_type FROM activity_design_link ORDER BY sort_index");
        while (s.step()) m.designLinks.push_back({s.uuid(0), s.uuid(1), s.text(2)});
    }
    {
        Stmt s(db, "SELECT baseline_uuid,activity_uuid,name,start,finish,duration_hours,quantity FROM baseline_activity "
                   "ORDER BY baseline_uuid, sort_index");
        std::map<Uuid, std::vector<BaselineActivity>> rows;
        while (s.step()) {
            auto a = DateTime::parse(s.text(3)), b = DateTime::parse(s.text(4));
            if (!a || !b) throw PersistenceError("Corrupt baseline date");
            rows[s.uuid(0)].push_back({s.uuid(1), s.text(2), *a, *b, s.real(5), s.real(6)});
        }
        for (auto& [id, v] : rows) {
            const auto* bl = m.baselines.find(id);
            if (!bl) throw PersistenceError("Baseline rows without baseline");
            ScheduleBaseline copy = *bl;
            copy.activities = std::move(v);
            m.baselines.upsert(copy);
        }
    }
    {
        Stmt s(db, "SELECT scenario_uuid, entity_uuid, field, value FROM scenario_override ORDER BY scenario_uuid, sort_index");
        std::map<Uuid, std::vector<ScenarioOverride>> rows;
        while (s.step()) rows[s.uuid(0)].push_back({s.uuid(1), s.text(2), s.text(3)});
        for (auto& [id, v] : rows) {
            const auto* sc = m.scenarios.find(id);
            if (!sc) throw PersistenceError("Scenario override without scenario");
            Scenario copy = *sc;
            copy.overrides = std::move(v);
            m.scenarios.upsert(copy);
        }
    }
    {
        std::map<Uuid, Drillhole> holes;
        for (const auto& h : m.drillholes.all()) holes[h.id] = h;
        Stmt sv(db, "SELECT drillhole_uuid, depth, azimuth, dip FROM drillhole_survey ORDER BY drillhole_uuid, depth");
        while (sv.step()) holes.at(sv.uuid(0)).surveys.push_back({sv.real(1), sv.real(2), sv.real(3)});
        Stmt iv(db, "SELECT drillhole_uuid, depth_from, depth_to, numeric_values, code_values FROM drillhole_interval "
                    "ORDER BY drillhole_uuid, depth_from");
        while (iv.step()) {
            DrillholeInterval i;
            i.from = iv.real(1);
            i.to = iv.real(2);
            i.values = parseJson(iv.text(3), "interval values").get<std::map<std::string, double>>();
            i.codes = parseJson(iv.text(4), "interval codes").get<std::map<std::string, std::string>>();
            holes.at(iv.uuid(0)).intervals.push_back(std::move(i));
        }
        for (auto& [id, h] : holes) m.drillholes.upsert(h);
    }
    {
        Stmt s(db, "SELECT timestamp,user,action,entity_type,entity_uuid,detail FROM audit_log ORDER BY seq");
        while (s.step()) m.audit.push_back({s.text(0), s.text(1), s.text(2), s.text(3), s.uuid(4), s.text(5)});
    }
}

std::vector<std::string> verifyV2(Db& db) {
    std::vector<std::string> problems;
    {
        Stmt s(db, "PRAGMA integrity_check");
        while (s.step())
            if (s.text(0) != "ok") problems.push_back("integrity_check: " + s.text(0));
    }
    {
        Stmt s(db, "PRAGMA foreign_key_check");
        while (s.step()) problems.push_back("foreign key violation in table " + s.text(0) + " (references " + s.text(2) + ")");
    }
    {
        Stmt s(db, "SELECT entity_uuid FROM attribute_value WHERE entity_uuid NOT IN (SELECT uuid FROM geometry_object UNION "
                   "SELECT uuid FROM mine_development UNION SELECT uuid FROM stope UNION SELECT uuid FROM schedule_activity)");
        while (s.step()) problems.push_back("orphaned attribute value for " + s.text(0));
    }
    {
        Stmt s(db, "SELECT l.design_uuid FROM activity_design_link l WHERE l.design_uuid NOT IN (SELECT uuid FROM mine_development "
                   "UNION SELECT uuid FROM stope UNION SELECT uuid FROM mining_block)");
        while (s.step()) problems.push_back("design link to missing design object " + s.text(0));
    }
    return problems;
}

} // namespace openmine::persistence::detail
