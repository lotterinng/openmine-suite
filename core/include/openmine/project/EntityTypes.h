#pragma once
// SPDX-License-Identifier: Apache-2.0
// The single list of persistent entity types. It drives the model stores, change-event
// kinds, generic undoable commands and editor template instantiations, so adding an
// entity type is one line here plus its persistence mapping and validation.
//
//   X(Type, storeMember, EntityKindName)

#include "openmine/objects/AttributeSchema.h"
#include "openmine/objects/Entities.h"

#define OPENMINE_ENTITY_TYPES(X)                                                   \
    X(CoordinateReferenceSystem, crs, Crs)                                         \
    X(LocalGridDefinition, localGrids, LocalGrid)                                  \
    X(TransformationRecord, transformations, Transformation)                       \
    X(Asset, assets, Asset)                                                        \
    X(Attachment, attachments, Attachment)                                         \
    X(Material, materials, Material)                                               \
    X(AnalysisResult, analysisResults, AnalysisResult)                             \
    X(Report, reports, Report)                                                     \
    X(AttributeDefinition, attributeDefinitions, AttributeDefinition)              \
    X(CadLayer, layers, Layer)                                                     \
    X(GeometryObject, geometry, Geometry)                                          \
    X(SurveyControlPoint, controlPoints, ControlPoint)                             \
    X(PointCloudDataset, pointClouds, PointCloud)                                  \
    X(Registration, registrations, Registration)                                   \
    X(AsBuiltModel, asBuiltModels, AsBuilt)                                        \
    X(DesignConformanceResult, conformanceResults, Conformance)                    \
    X(TunnelProfile, profiles, Profile)                                            \
    X(MineLevel, levels, Level)                                                    \
    X(Decline, declines, Decline)                                                  \
    X(GroundSupportClass, supportClasses, SupportClass)                            \
    X(MineDevelopment, developments, Development)                                  \
    X(Stope, stopes, Stope)                                                        \
    X(MiningBlock, miningBlocks, MiningBlock)                                      \
    X(DesignRevision, designRevisions, DesignRevision)                             \
    X(WorkCalendar, calendars, Calendar)                                           \
    X(ScheduleActivity, activities, Activity)                                      \
    X(ActivityDependency, dependencies, Dependency)                                \
    X(Resource, resources, Resource)                                               \
    X(ActivityResourceAssignment, assignments, Assignment)                         \
    X(DevelopmentCycle, cycles, Cycle)                                             \
    X(ProductionActual, actuals, Actual)                                           \
    X(ProductionTarget, targets, Target)                                           \
    X(ScheduleBaseline, baselines, Baseline)                                       \
    X(Scenario, scenarios, Scenario)                                               \
    X(Drillhole, drillholes, Drillhole)                                            \
    X(GeologicalObject, geologicalObjects, GeologicalObject)                       \
    X(BlockModel, blockModels, BlockModel)                                         \
    X(StructuralMeasurement, structures, Structure)                                \
    X(GeotechClassification, geotechClassifications, GeotechClassification)
