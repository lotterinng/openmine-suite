# Implementation status — v0.4.0

Legend: ✅ implemented and covered by automated tests · 🟡 implemented with a stated limitation ·
⛔ not implemented (the UI shows nothing pretending otherwise).

**Evidence** (Linux, 2026-10-21): `ctest` → **226/226 passed** — 212 GoogleTest cases (incl. the
embedded-console tests), the Python suite (24 pytest cases: 23 API tests and the executed
`docs/python.md` guide), 5 validations against independent libraries (ezdxf, trimesh, GMG `omf`,
mplstereonet, pypdf/pdfplumber/openpyxl), 6 documentation and consistency checks (generated
references, Doxygen without warnings, strict MkDocs site, version strings), the quick benchmark run
and the GUI smoke test (**239 checks** under Xvfb; log and screenshots in `docs/screenshots/`).
PostgreSQL repository tests ran against PostgreSQL 16.15 + PostGIS 3.4.2. A minimal configuration
(no optional integrations, no app, `-Werror`) also builds and passes. Release benchmarks:
`benchmarks/README.md`.

Toolchain of that run: Ubuntu 24.04, GCC 13.3, CMake 3.28, Qt 6.4.2, VTK 9.1 (distribution build,
own Qt adapter), Open CASCADE 7.6.3, PDAL 2.8.4 (with E57), Open3D 0.18.0, PROJ 9.4.0, GDAL 3.8.4,
OR-Tools 9.12, SQLite 3.45, Python 3.13 / pybind11 3.1.

## Platform note
Development and validation ran on **Linux**. The Windows 10/11 build (MSVC 2022 + vcpkg preset,
CI job) is written but **has not been executed**; treat Windows as unverified until the `windows`
CI job passes. macOS is not a target.

## Foundation and application shell

| Feature | Status | Evidence |
|---|---|---|
| CMake presets, optional integrations, warnings clean (`-Wall -Wextra -Wpedantic -Wshadow`) | ✅ | build logs; CI `-DOPENMINE_WARNINGS_AS_ERRORS=ON` |
| Menu bar (File, Edit, View, Draw, Modify, Survey, Mine Design, Scheduler, Geology, Geotechnical, Tools, Window, Settings, Help) | ✅ | smoke: menus |
| Ribbon with the 10 workspaces; quick-access toolbar (customisable, persisted) | ✅ | smoke: workspace |
| Dockable panels: explorer, properties, layers, scheduler, console, Python console, jobs, reports, 4D playback, stereonet | ✅ | smoke |
| Dark and light themes; programmatic icons | ✅ | smoke screenshots |
| Viewports: 1/2/2H/4 layouts, named views, perspective/parallel, section views, render origin | ✅ | smoke: every view of the 2- and 4-view layouts renders |
| Background jobs with progress and cancellation; results applied as one undo step | ✅ | `Jobs.*`, smoke |
| Preferences (display, point clouds, drawing/snaps, plugins, project display) persisted | ✅ | smoke |
| Undo/redo for every edit; macros; rollback on failure; append-only audit log | ✅ | `Commands.*` |

## Project data

| Feature | Status | Evidence |
|---|---|---|
| `.openmine` SQLite container, schema v2, atomic verified save, `.bak`, newer-schema refusal | ✅ | `Persistence.*` |
| Migration of schema-1 (v0.1) projects | ✅ | `Persistence.MigratesSchemaVersion1Projects` |
| Bit-exact coordinates; every entity type round-trips | ✅ | `Persistence.RoundTripPreservesEveryEntityType`, Python spec test |
| Asset sidecar (immutable, SHA-256), Save As copies assets | ✅ | `Assets.*`, `Persistence.SaveAsCopiesSidecarAssets` |
| Attachments on any entity (verified extraction, follow their entity, undo) | ✅ | `Persistence.AttachmentsAreStoredVerifiedAndFollowTheirEntity`, smoke |
| Compaction of unused/unregistered asset files | ✅ | `Persistence.CompactRemovesUnreferencedAndUnregisteredAssetsOnly`, smoke |
| Entity documents (lossless JSON), referential check | ✅ | `EntityCodec.*` |
| Schema-driven custom attributes | ✅ | `Commands.CustomAttributesAreSchemaValidated`, `AttributeSchema.*` |
| Shared repository on PostgreSQL + PostGIS (checkout, publish, pull policies, locks, spatial query, merges) | ✅ | `Fixture.*` (6, need a server), `test_repository_round_trip` |

## CAD

| Feature | Status | Evidence |
|---|---|---|
| Point, line, polyline (bulges), polygon, rectangle, circle, 3-point arc, spline, text, dimensions (7 kinds), hatch | ✅ | `CadConstruction.*`, `CadAnnotation.*`, `Curves.*`, smoke |
| Move, copy, rotate, mirror, scale | ✅ | `CadTransform.*`, `Transform.*`, smoke |
| Offset (miter/round), trim, extend, split, join, explode, fillet, chamfer | ✅ | `CadOffset.*`, `CadTrim.*`, `CadExtend.*`, `CadSplitJoinExplode.*`, `CadCorners.*` |
| Snaps: endpoint, midpoint, centre, intersection, perpendicular, tangent, nearest, node, grid, surface/point cloud; ortho | ✅ | `CadSnap.ModesAndPriorities`, smoke |
| Grip editing (vertices; arc/circle centre, radius, ends) in the viewport, console and Python | ✅ | `CadGrips.*`, `MineDesign.GripEditsMoveDefiningPointsAndFlagLinkedActivities`, smoke |
| Window/crossing box selection, multi-selection, viewport picking | ✅ | smoke (click selects) |
| Layers (colour, visibility, lock), measuring distance/area | ✅ | smoke |
| Solids (Open CASCADE): primitives, extrude, revolve, sweep along declines, loft, booleans, fillet, offset, healing, sections, mesh ↔ solid, distances | ✅ | `OcctSolid.*`, `OcctModelling.*`, smoke |

## Mine design

| Feature | Status | Evidence |
|---|---|---|
| Development headings with derived length, gradient, chainage, section area, volume, tonnes | ✅ | `MineDesign.MetricsDerivedFromGeometry`, `Profiles.*` |
| Profiles (rectangular, shoulder arch, semicircular arch, segmental arch, horseshoe, custom bulged outlines), watertight envelopes | ✅ | `MineDesign.ExcavationEnvelopeIsWatertightWithExactVolume` |
| Layouts: horizontal with curves, switchback/spiral declines, ramps, access, ore drives, crosscuts, vertical curves | ✅ | `MineDesign.HorizontalAlignmentWithCurves`, `MineDesign.SwitchbackDeclineReachesTargetAtConstantGrade`, `Headings.*` |
| Design rules (grade, grade on curves, radius, grade change), gradient profiles, regrading | ✅ | `Headings.GradientProfileRulesAndGrading` |
| Levels, declines, stopes, mining blocks (tonnage/grade), quantities report | ✅ | `MineDesign.StopeTonnageAndBlocks`, `MineDesign.QuantitiesReport`, `Headings.ProjectCrosscutsAndLevels` |
| Design revisions: capture, approve, compare, restore | ✅ | `MineDesign.RevisionsCaptureApproveCompareRestore` |
| Geometry change → flag → explicit recalculation; baselines preserved | ✅ | `MineDesign.GeometryChangeFlagsButNeverSilentlyChangesSchedule`, ADR 0004 |

## Survey

| Feature | Status | Evidence |
|---|---|---|
| LAS/LAZ, E57, PLY read/write (PDAL, streamed) | ✅ | `Pdal.LasAndLazRoundTripPreserveAttributes`, `Pdal.PlyAndE57RoundTrip` |
| XYZ/CSV/TXT read/write, Leica PTS read/write, PTX read (scanner transforms applied) | ✅ | `SurveyText.*` |
| Out-of-core LOD octree (own format, bit-exact), background loading, point budget | ✅ | `SurveyOctree.*`, smoke; format: `docs/formats/octree.md` |
| Derived datasets (sources never modified): crop box/polygon, outliers (statistical/radius), voxel and spacing subsampling, normals, clusters, region growing, SMRF ground, distances | ✅ | `SurveyProcessing.*`, `SurveyDerivations.*`, Python tests |
| Registration: point pairs/control (Umeyama, residuals), ICP point-to-point/plane (built-in and Open3D), reversible | ✅ | `SurveyRegistration.*`, `Open3d.Icp*` |
| Surfaces: Delaunay 2.5D, Poisson (PDAL / Open3D), decimate, smooth, repair, compare, volumes | ✅ | `SurveyMeshing.*`, `SurveySurfaceTools.*`, `Pdal.PoissonSurfaceOfASphere`, `Open3d.Poisson*` |
| As-built surfaces of headings; overbreak/underbreak, clearance, face position, survey actuals | ✅ | `SurveyAsBuilt.*`, `SurveyConformance.*`, `test_compare_design_against_a_scan` (45 m³ ± 5 %) |
| Plane fitting for structures, control points, registration/point-cloud/conformance reports | ✅ | `SurveyProcessing.NormalsSegmentationPlanes`, `Reporting.*` |
| CRS: declaration, identification, PROJ transformations, local grids (Helmert 2D/3D, affine) | ✅ | `Crs.*`, `LocalGrid.*` |
| A scan's declared CRS recorded and compared with the project CRS (never applied implicitly) | ✅ | `SurveyProject.DeclaredCrsIsComparedNeverApplied`, `test_point_cloud_crs_is_recorded_and_mismatch_reported` |
| E57 embedded images, scan-position metadata beyond PTX headers | ⛔ | — |

## Scheduling

| Feature | Status | Evidence |
|---|---|---|
| Calendar-time CPM, FS/SS/FF/SF with lags, constraints (SNET, FNLT, MSO), negative float, WBS roll-up | ✅ | `Cpm.*` (hand-calculated reference network) |
| Shift calendars with exceptions; activity ∩ resource calendars | ✅ | `CalendarTime.*`, `Levelling.ResourceCalendarRestrictsWork` |
| Rates per hour/shift/day, development cycle model, resource productivity | ✅ | `Cpm.ShiftCalendarRates`, `Production.DevelopmentCycleRate` |
| Dated actuals, status-date reforecast, survey-reconciled actuals | ✅ | `Cpm.ReforecastFromDatedActualsAndStatusDate`, `SurveyConformance.AdvanceReconciliationWithShiftActuals` |
| Baselines and variances; scenarios with overrides | ✅ | `Scenario.*`, smoke |
| Heuristic levelling; resource histograms | ✅ | `Levelling.*` |
| OR-Tools CP-SAT optimisation (makespan, weighted completion, lateness), infeasibility explanations, stored as scenarios | ✅ | `Optimiser.*` |
| Production per period vs targets; 4D playback and scenario comparison | ✅ | `Periods.*`, `FourD.*`, smoke |
| Gantt (WBS grid, dependencies, critical path, baselines, progress, status line), resource and production charts | ✅ | smoke screenshots |
| Exports: CSV, XLSX (dates, WBS, weekly Gantt sheet), Gantt PDF | ✅ | `report_validation` (pypdf, openpyxl) |
| Activity splitting / pre-emption in levelling; progress override (vs retained logic) | ⛔ | see `docs/architecture/scheduling.md` §12 |

## Geology and geotechnical

| Feature | Status | Evidence |
|---|---|---|
| Drillhole import (collar/survey/intervals) with every rejected record reported; QA checks | ✅ | `Drillholes.*` |
| Desurveying (minimum curvature, balanced tangent, tangent), compositing (length, domains, benches), statistics, capping | ✅ | `Desurvey.*`, `Compositing.*` |
| Regular block models (rotated), domain proportions from solids, grade–tonnage, CSV | ✅ | `BlockModels.*` |
| Variograms (experimental, fitted, anisotropic), NN/IDW/OK/SK estimation, cross-validation | ✅ | `Estimation.*` |
| Stereonets, Fisher statistics, joint sets, kinematic analysis, Q (with support), RMR89, GSI, Hoek–Brown | ✅ | `Stereonet.*`, `QSystem.*`, `Rmr89.*`, `HoekBrown.*`, mplstereonet validation |
| Sub-blocked block models; implicit geological modelling | ⛔ | — |

## Data exchange

| Format | Import | Export | Evidence |
|---|---|---|---|
| DXF R2004 (lines, polylines, arcs, splines, text, dimensions, hatches, blocks, 3D faces, meshes) | ✅ | ✅ | `Dxf.*`, ezdxf validation |
| OBJ, PLY, STL (ASCII/binary) meshes | ✅ | ✅ | `MeshFormats.*`, trimesh validation |
| OMF v1 | ✅ | ✅ | `Omf.*`, GMG `omf` reference validation |
| STEP, IGES solids | ✅ | ✅ | `OcctSolid.TransformsAndExchangeFormats` |
| GeoTIFF / ASCII grid DEMs | ✅ | ✅ | `Gis.Dem*` |
| Shapefile, GeoPackage, GeoJSON, KML vectors (objects without a vector form are reported) | ✅ | ✅ | `Gis.VectorRoundTripWithAttributesAndCrs` |
| Point clouds: see Survey | | | |
| Drillhole/composite/block-model CSV | ✅ | ✅ | `Drillholes.*`, `BlockModels.*` |
| HTML and PDF reports | — | ✅ | `Reporting.*`, report validation |
| DWG; proprietary Deswik/Maptek formats | ⛔ | ⛔ | licensing: `docs/licensing.md` |

## Scripting and extension

| Feature | Status | Evidence |
|---|---|---|
| Python API over every domain (spec API: `import_pointcloud`, `remove_outliers`, `create_development`, `create_activity`, `compare_design`, `save`) | ✅ | `scripting/python/tests/test_api.py` (23 cases) |
| Python guide examples executed as a test | ✅ | `test_guide.py` |
| Embedded Python console and script runner on the live project | ✅ | `PythonConsole.*`, smoke |
| Command console: 97 commands, typed point input; generated, checked reference | ✅ | `console_reference_up_to_date`, smoke |
| Native plugins (C ABI v1): commands, importers, exporters; host protections; enable/disable | ✅ | `Plugins.*`, smoke |
| Python plugins (entry points) in the desktop | ✅ | `test_sample_python_plugin_discovery_and_desktop_host`, smoke |
| Generated references: console commands, project schema (with ER diagram), Python API | ✅ | `ctest -L docs` |
| Documentation site (MkDocs, strict) and C++ API reference (Doxygen, no warnings); consistent version strings | ✅ | `docs_site_builds`, `doxygen_reference_builds`, `version_strings_agree` |
| Engine benchmarks with budgets | ✅ | `benchmarks_quick`; CI `benchmarks` job (`--check benchmarks/budgets.json`) |

## Known limitations (beyond the ⛔ rows)

* All test data is synthetic or generated by the tests (scans of known shapes, hand-calculated
  networks); results have not yet been compared with production mine datasets.
* Poisson as-built surfaces depend on scan coverage; partial stations are excluded and reported,
  not estimated.
* The optimiser works at a slot resolution (default 1 h); the stored scenario is recalculated
  exactly by CPM.
* Windows (see above); the Python package is built from source (no published wheel yet).
