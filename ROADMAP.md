# Roadmap

Phases follow the master specification. A feature is "done" only when it has automated tests
and is listed as implemented in `IMPLEMENTATION_STATUS.md`.

## Phase 1 — Architecture and foundation ✅ (v0.1)
Architecture, ERD, licence audit, repository, CMake/presets, Qt 6 shell with ribbon (10
workspaces), dockable explorer/properties/console/scheduler, VTK viewport, project
create/open/save, dark/light themes.

## Phase 2 — Initial CAD (next)
- [x] Line / polyline (viewport and console), selection, grid + endpoint snap, layers, attributes,
      persistence, undo/redo, move/copy/rotate, delete *(delivered early in v0.1)*
- [ ] Arc, circle, spline, rectangle, polygon, text, dimensions, hatch
- [ ] Offset, trim, extend, split, join, explode, fillet, chamfer, mirror, scale
- [ ] Midpoint, centre, intersection, perpendicular, tangent, nearest snaps
- [ ] Vertex grip editing and box/multi-selection in the viewport
- [ ] DXF import/export (round-trip tested)

## Phase 3 — Initial survey
- [ ] PDAL integration: LAS/LAZ (LASzip) and E57 import; CSV/XYZ/PTS points
- [ ] Out-of-core octree tiles in the asset sidecar; background loading with progress/cancel
- [ ] Point-cloud rendering (LOD), crop (box/polygon), statistical/radius outlier, voxel filters
- [ ] Coordinate display and point picking; distances
- [ ] Point-pair and control registration with residual/QA reporting; ICP (Open3D)
- [ ] Surface generation (Delaunay 2.5D, Poisson) and mesh export
- [ ] PROJ CRS transformations (Lo-system ↔ WGS84 with verified parameters)

## Phase 4 — Scheduler completion
- [x] Activity model, WBS, Gantt, dependencies (FS/SS/FF/SF + lags), calendars, CPM, floats,
      critical path, geometry links, save/load, validation tests *(delivered early in v0.1)*
- [ ] Per-activity and resource calendars (calendar-time CPM)
- [ ] In-grid editing, drag-to-link in the Gantt, filtering/grouping
- [ ] Baseline vs forecast variance columns; scenario copies
- [ ] XLSX and PDF export

## Phase 5 — Integrated mine workflow
- [x] Heading → linked activity → rate/calendar → dates *(v0.1)*
- [x] Actual advance → remaining quantity → reforecast from status date *(v0.1, single actual value)*
- [x] Planned vs actual development in 3D *(v0.1, advance overlay)*
- [ ] Dated `ProductionActual` records; survey-derived face positions
- [ ] Reports (PDF/HTML) and dashboards
- [ ] `DesignRevision` history with geometry snapshots

## Phase 6 — Advanced
Tunnel profiles and swept solids in the viewport (OCCT adapter exists), stope solids, overbreak/
underbreak conformance, chainage sections, OR-Tools resource-constrained scheduling, 4D playback,
geology (drillholes, surfaces, block models), geotechnical tools, PostgreSQL/PostGIS backend,
plugin SDK, embedded Python console.
