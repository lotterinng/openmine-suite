# Roadmap

Phases follow the master specification. A feature is "done" only when it has automated tests and
is listed as implemented in `IMPLEMENTATION_STATUS.md`. Version history: v0.1 (Phase 1 + vertical
slice), v0.2 (CAD, survey I/O and processing, calendar-time scheduling), v0.3 (solids, geology,
geotechnical, repository, plugins, reports), v0.4 (shared project-level operations behind the
desktop, console and Python; as-built conformance; scheduling libraries; generated references).

## Phase 1 — Architecture and foundation ✅
- [x] Architecture, ERD, licence audit, repository, CMake presets, CI definition
- [x] Qt 6 shell: menus, 10 ribbon workspaces, dockable panels, status bar, themes
- [x] VTK viewports, project create/open/save/close, undo/redo, audit log

## Phase 2 — CAD ✅
- [x] Lines, polylines with arcs, polygons, rectangles, circles, arcs, splines, text, dimensions, hatches
- [x] Offset, trim, extend, split, join, explode, fillet, chamfer, move/copy/rotate/mirror/scale
- [x] Object snaps (endpoint, midpoint, centre, intersection, perpendicular, tangent, nearest, node, grid, surface), ortho, typed input
- [x] Box/multi-selection, grip editing, layers, schema-driven attributes
- [x] DXF R2004 import/export (validated against ezdxf); solids with Open CASCADE; STEP/IGES

## Phase 3 — Survey ✅
- [x] LAS/LAZ/E57/PLY (PDAL), XYZ/CSV/PTS/PTX; out-of-core LOD octree with background loading
- [x] Crop, outlier, voxel/spacing filters, normals, segmentation, ground classification — as derived datasets
- [x] Point-pair/control registration with residual reports; ICP (built-in and Open3D)
- [x] Delaunay and Poisson surfaces, mesh tools, volumes; OBJ/PLY/STL
- [x] PROJ transformations, CRS identification, local grids

## Phase 4 — Scheduler ✅
- [x] Calendar-time CPM with shift calendars, resource calendars, constraints, negative float
- [x] Rates, development cycles, resources, levelling, OR-Tools optimisation as scenarios
- [x] Gantt with in-grid editing, dependencies, baselines and variances; CSV/XLSX/PDF exports

## Phase 5 — Integrated mine workflow ✅
- [x] Design → linked activity → rate/calendar → dates; geometry changes flag, never overwrite
- [x] Dated actuals, survey-derived face positions reconciled with reported advance, reforecast
- [x] Planned vs actual in 3D, 4D playback, scenario comparison
- [x] Design revisions; reports (HTML/PDF/XLSX)

## Phase 6 — Advanced ✅ (first versions)
- [x] Tunnel profiles and swept solids, stopes and mining blocks, as-built conformance
- [x] Geology (drillholes, compositing, block models, variography, kriging) and geotechnical tools
- [x] PostgreSQL/PostGIS shared repository; plugin SDK (C ABI + Python); embedded Python console

## Next (v0.5 and beyond)

Verification and delivery
- [ ] Execute the Windows CI job (MSVC + vcpkg), fix what it finds, publish an installer that keeps
      LGPL libraries replaceable
- [ ] Publish the Python package as wheels (Linux, Windows)
- [ ] Validate against production datasets (scans of real headings, a full mine schedule) with
      published comparisons against established tools' outputs
- [x] Engine benchmarks with budgets in CI (`benchmarks/`: CPM, levelling, save/load, octree, k-d tree, filters)
- [ ] Larger-scale budgets (10⁸-point import) and viewport frame-time measurements

Features
- [ ] Scheduling: progress override as an alternative to retained logic; activity splitting and
      partial resource units in levelling; cost tracking against `costPerHour`
- [ ] Survey: E57 embedded images and scan-position metadata; classification editing tools
- [ ] Geology: sub-blocked models; implicit surfaces from contacts
- [ ] Design: associative dimensions; parametric stope shapes optimised against block models
- [ ] Exchange: DWG through an optional, separately licensed plugin (ODA), never in-process GPL code
- [ ] Collaboration: repository web viewer; comments on entities

Documentation
- [ ] Desktop user guide with screenshots per workflow (survey → design → schedule → reconcile)
