# Requirements traceability

Source: the OpenMine Suite master development prompt. Each requirement is restated as a testable
statement with where it is verified. IDs are stable; status symbols as in
[IMPLEMENTATION_STATUS.md](../IMPLEMENTATION_STATUS.md) (✅ tested · 🟡 limited · ⛔ not done).

## Application (spec §3)

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| UI-01 | Menus File/Edit/View/Tools/Window/Settings/Help (plus domain menus) | ✅ | smoke: menus |
| UI-02 | Ribbon with the 10 workspaces Home … Automation | ✅ | smoke: ribbon pages |
| UI-03 | Project Explorer with every object category | ✅ | smoke screenshots |
| UI-04 | 3D viewport: perspective/orthographic, plan/section/3D, multiple views, picking, large coordinates | ✅ | smoke (every view renders, click selects), `Polyline.PrecisionAtLargeMineCoordinates` |
| UI-05 | Property editor: typed, validated, undoable; derived values read-only; schema attributes | ✅ | smoke, `Commands.ValidationFailureChangesNothing` |
| UI-06 | Command console: LINE, POLYLINE, ARC, CIRCLE, OFFSET, TRIM, EXTEND, MOVE, COPY, ROTATE, SCALE, MIRROR, EXTRUDE, VOLUME, SECTION, REGISTER, FILTER, MESH, SAVE … | ✅ | `console_reference_up_to_date`, smoke |

## Survey (spec §4)

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| S-01 | LAS, LAZ, E57, PLY read/write | ✅ | `Pdal.LasAndLazRoundTripPreserveAttributes`, `Pdal.PlyAndE57RoundTrip` |
| S-02 | XYZ / ASCII / CSV read/write; PTS read/write; PTX read | ✅ | `SurveyText.*` |
| S-03 | OBJ, STL, DXF, OMF | ✅ | `MeshFormats.*`, `Dxf.*`, `Omf.*` + independent validations |
| S-04 | Octree indexing, streaming and caching of large clouds | ✅ | `SurveyOctree.ChunkedBuildStoresEveryPointOnceBitExact`, smoke |
| S-05 | Point selection, manual (polygon) and box cropping | ✅ | `SurveyProcessing.SelectionsAndSubsampling`, smoke |
| S-06 | Statistical and radius outlier removal, voxel downsampling | ✅ | `SurveyProcessing.OutlierRemoval`, `Open3d.*Agrees*` |
| S-07 | Ground/non-ground classification | ✅ | `Pdal.ProjectImportThroughRegistryAndSmrf` |
| S-08 | Normals, segmentation, connected components | ✅ | `SurveyProcessing.NormalsSegmentationPlanes` |
| S-09 | Point-to-point and point-to-mesh distances | ✅ | `SurveyProcessing.CloudAndMeshDistances` |
| S-10 | Colour mapping and intensity display; export of processed clouds | ✅ | smoke, `SurveyProject.ImportTransformDeriveExport` |
| S-11 | Point-pair, control and ICP registration; rigid transforms; history | ✅ | `SurveyRegistration.*`, `Open3d.Icp*` |
| S-12 | Residuals vs fit vs independent check-point accuracy reported separately | ✅ | `SurveyRegistration.ControlAndIcpRegistrationInProject`, registration report |
| S-13 | Triangulation, reconstruction, decimation, smoothing, repair, normals | ✅ | `SurveyMeshing.*`, `SurveySurfaceTools.*`, Poisson tests |
| S-14 | Cross-sections, surface-to-surface and surface-to-design comparison, volumes | ✅ | `Mesh.PlaneSectionOfCubeIsSquareLoop`, `SurveyMeshing.VolumesExact`, `SurveyConformance.*` |
| S-15 | As-built tunnel models from development scans | ✅ | `SurveyAsBuilt.CorridorNormalsTrimmingAndHoleFilling`, `test_compare_design_against_a_scan` |
| S-16 | Overbreak/underbreak/clearance with documented definitions and validation data | ✅ | [conformance.md](architecture/conformance.md), `SurveyConformance.OverbreakUnderbreakClearanceFromMeshAndPoints` |
| S-17 | Chainage sections, face position, advance, design vs as-built reconciliation, reports | ✅ | `SurveyConformance.AdvanceReconciliationWithShiftActuals`, `Reporting.*` |

## CAD and mine design (spec §5)

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| C-01 | Points, lines, polylines, arcs, circles, splines, rectangles, polygons, hatching, dimensions, text | ✅ | `CadConstruction.*`, `CadAnnotation.*`, `Curves.*` |
| C-02 | Select, move, rotate, scale, copy, mirror, offset, trim, extend, split, join, explode, fillet, chamfer, delete | ✅ | `Cad*`, `MineDesign.ProjectGeometryEditsAreUndoableAndFlagLinkedActivities`, smoke |
| C-03 | Endpoint, midpoint, centre, intersection, perpendicular, tangent, nearest, grid, object snaps | ✅ | `CadSnap.ModesAndPriorities` |
| C-04 | Extrusions, sweeps, lofts, booleans, validation, mesh-to-solid, volume, area, sections | ✅ | `OcctSolid.*`, `OcctModelling.*` |
| C-05 | Centrelines, declines, profiles, crosscuts, levels, ramps, ore/access drives | ✅ | `MineDesign.HorizontalAlignmentWithCurves`, `MineDesign.SwitchbackDeclineReachesTargetAtConstantGrade`, `Headings.*` |
| C-06 | Stopes, mining blocks, chainage/stationing, gradients, curves, envelopes, quantities, revisions | ✅ | `MineDesign.*`, `Headings.*`, `Profiles.*` |
| C-07 | Heading is a domain object with the specified attributes (ID, name, zone, decline, level, type, chainages, length, size, gradient, status, geometry, activity, dates, revision) | ✅ | `MineDesign.MetricsDerivedFromGeometry`, schema reference |
| C-08 | Custom attributes schema-driven and extensible | ✅ | `Commands.CustomAttributesAreSchemaValidated` |

## Scheduling (spec §6)

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| P-01 | Activities, WBS, hierarchies, durations, milestones | ✅ | `Cpm.SummaryRollUpAndCycleRejection` |
| P-02 | Working, shift and resource calendars | ✅ | `CalendarTime.*`, `Levelling.ResourceCalendarRestrictsWork` |
| P-03 | FS/SS/FF/SF with lags; CPM early/late dates, total/free float | ✅ | `Cpm.ReferenceNetworkAllRelationshipTypes` (hand-calculated) |
| P-04 | Baselines, progress updates, forecasts, scenario comparison | ✅ | `Cpm.ReforecastFromDatedActualsAndStatusDate`, `Scenario.*`, `FourD.PlaybackFramesAndScenarioComparison` |
| P-05 | Development, drilling, blasting, mucking and support rates; equipment productivity; crews | ✅ | `Production.DevelopmentCycleRate`, `Scenario.OverridesChangeRatesAndCapacity` |
| P-06 | Resource constraints, levelling, priorities | ✅ | `Levelling.*` |
| P-07 | Quantities: scheduled, actual, remaining metres; targets | ✅ | `Periods.ScheduledActualTargetMetresPerWeek` |
| P-08 | OR-Tools: capacity, availability, shift calendars, precedence, mutually exclusive resources, restricted work areas, priorities, access (as precedence/release constraints); separate from CPM; infeasibility explained | ✅ | `Optimiser.*` |
| P-09 | Scheduler UI: grid, WBS, Gantt, arrows, critical path, resource/duration/date columns, progress, baseline vs forecast, timeline, filter, group, CSV/XLSX/PDF | ✅ | smoke screenshots, `report_validation` |
| P-10 | Geometry-linked scheduling steps 1–7; approved schedules never changed silently | ✅ | `MineDesign.GeometryChangeFlagsButNeverSilentlyChangesSchedule`, ADR 0004 |
| P-11 | 4D: time colouring, sequence, playback, equipment, completed vs planned, future, scenarios | ✅ | `FourD.*`, smoke |

## Data, coordinates, interoperability (spec §7–9)

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| D-01 | Unified object model with persistent UUIDs and relationships | ✅ | `Persistence.RoundTripPreservesEveryEntityType`, `Persistence.SchemaHasExplicitRelationships` |
| D-02 | SQLite local projects; PostgreSQL/PostGIS shared | ✅ | `Persistence.*`, `Fixture.*` |
| D-03 | Audit history | ✅ | `Commands.AuditLogIsAppendOnly` |
| K-01 | Declared CRS; no silent transformation; Lo systems; local grids; unit handling | ✅ | `Crs.*`, `LocalGrid.*`, `Units.*`, `SurveyProject.DeclaredCrsIsComparedNeverApplied` |
| I-01 | Python API (`project.survey.import_pointcloud`, `scan.remove_outliers()`, `project.cad.create_development`, `project.scheduler.create_activity`, `project.survey.compare_design`, `project.save()`) | ✅ | `test_specification_workflow_and_bit_exact_round_trip`, `test_compare_design_against_a_scan` |
| I-02 | Plugin architecture | ✅ | `Plugins.*`, Python plugin tests |
| I-03 | Documented open exchange formats only; no proprietary formats | ✅ | [licensing.md](licensing.md) |

## Development standards (spec §12)

| ID | Requirement | Approach / evidence |
|---|---|---|
| N-01 | No pseudocode, no fake results, no buttons pretending to work | optional components disabled with the reason; formats claimed only with tests |
| N-02 | Unit and integration tests; reference validation | ctest suite; independent-library validations |
| N-03 | Error handling and diagnostic logging | named exceptions; `diagnostics::Log` sinks (`Logging.SinksReceiveRecords`) |
| N-04 | Background workers, progress, cancellation | `Jobs.*`, Jobs panel |
| N-05 | Undo/redo | `Commands.*` |
| N-06 | Data integrity on save | `Persistence.FailedSaveLeavesExistingFileUntouched` |
| N-07 | Never silently alter survey coordinates, approved plans or baselines | derived datasets, transformation records, revision/baseline immutability |
| N-08 | Documented architecture decisions | `docs/adr/` |
| N-09 | Sample data with clear licences | test data generated by the tests; `samples/` CC0 |
| N-10 | Windows 10/11 x64 | 🟡 MSVC + vcpkg preset and CI job defined, not yet executed |
