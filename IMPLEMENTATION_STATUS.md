# Implementation status — v0.1.0 (milestone: Phase 1 foundation + Step 4 vertical slice)

Legend: ✅ implemented and covered by automated tests · 🟡 implemented, partially tested or with
known limitations · ⛔ not implemented (UI shows it disabled / API raises `NotImplementedError`).

Evidence: `ctest` → **36/36 passed** on Ubuntu 24.04 (GCC 13.3, Qt 6.4.2, VTK 9.3.1 built for
Qt 6, OCCT 7.6.3, Python 3.13): 30 core/module tests, 4 OCCT tests, 7 pytest cases (one ctest
entry), and the GUI smoke test (57 checks, run under Xvfb). Log and screenshots:
`docs/screenshots/`.

## Environment note
Development and validation ran on **Linux**, not the Windows 10/11 target. The Windows build path
(MSVC + vcpkg preset, CI job) is written but **has not been executed**. Treat Windows as
unverified until the `windows` CI job passes.

## Foundation (Phase 1)

| Feature | Status | Evidence |
|---|---|---|
| CMake project, presets, options, warnings clean (`-Wall -Wextra -Wpedantic -Wshadow`) | ✅ | build log: 0 warnings in project code |
| Qt 6 main window, menu bar (File/Edit/View/Tools/Window/Settings/Help) | ✅ | screenshots |
| Ribbon with 10 workspaces; planned tools disabled with phase tooltip | ✅ | screenshots |
| Dockable Project Explorer, Properties, Command Console, Scheduler | ✅ | screenshots |
| Status bar (tool, snaps, CRS, cursor X/Y/Z) | ✅ | screenshots |
| Dark and light themes, programmatic original icons | ✅ | `05_light_theme.png` |
| VTK 3D viewport: perspective/parallel, top/front/side/iso, fit, axis triad, grid | ✅ | smoke test |
| Project new / open / save / save-as / close with unsaved-changes prompt | ✅ | smoke test, `Persistence.*` |

## Vertical slice (Step 4)

| Requirement | Status | Evidence |
|---|---|---|
| Create a project | ✅ | smoke: `NEW` |
| Create a line/polyline (viewport clicks with grid + endpoint snap; console with absolute/relative points) | ✅ | smoke: 3 picked vertices; `POLYLINE` |
| View it in the 3D viewport | ✅ | screenshots 02–06 |
| Select it (viewport pick, explorer, console) | ✅ | smoke: pick selected the centreline |
| Edit supported properties; invalid input rejected without change | ✅ | smoke: rename, layer, vertex; `not-a-number` rejected |
| Persistent unique identifier | ✅ | v4 UUIDs; `Uuid.*`; persisted |
| Create linked schedule activity | ✅ | smoke: `ACTIVITY 5`; `MineDesign.*` |
| Display in Gantt view | ✅ | smoke: Gantt row with start/finish |
| Save, close, reopen | ✅ | smoke |
| Geometry and activity links persist | ✅ | smoke: UUIDs, bit-identical vertices, links, baseline, CRS, audit |

## Engine features delivered early

| Feature | Status | Notes |
|---|---|---|
| Undo/redo for all edits, macros (one undo step), rollback on failure | ✅ | `Commands.*` |
| Append-only audit log, persisted | ✅ | |
| Schema-driven custom attributes (GeometryObject, MineDevelopment) | 🟡 | engine + properties editing ✅; no UI yet to *define* attributes (API/console only via Python) |
| Layers: create, colour, visibility, lock | ✅ | |
| Move / copy / rotate / delete, distance, length, plan area | ✅ | console commands |
| Development heading domain object, derived length/gradient/chainage/volume | ✅ | |
| OCCT swept-solid volume and area | 🟡 | exact for straight headings (tested); multi-segment sweeps with right-corner transitions report "solid not valid" from `BRepCheck` — shown honestly, not used for quantities |
| CPM: FS/SS/FF/SF, lags, SNET, milestones, WBS roll-up, TF/FF, critical path | ✅ | hand-calculated reference network |
| Working calendars (weekdays + holidays) | 🟡 | project default calendar only; per-activity calendars produce a warning |
| Rate-driven durations; actual quantity; status-date reforecast | ✅ | single actual value per activity |
| Baselines (immutable) and baseline bars in Gantt | ✅ | |
| Geometry change → flag → controlled recalculation → audit, baseline preserved | ✅ | ADR 0004 |
| Planned-vs-actual advance and flagged headings coloured in 3D | ✅ | |
| Schedule CSV export | ✅ | |
| `.openmine` SQLite container: FKs, CHECKs, migrations, atomic verified save, `.bak`, newer-schema refusal | ✅ | |
| Python API (`openmine` package) for the workflow | ✅ | 7 pytest cases; `samples/create_sample_project.py` |

## Not implemented (⛔)

Point clouds and all survey processing (LAS/LAZ/E57 import, filters, registration, meshing,
conformance); CRS transformations (PROJ); arcs/circles/splines/text/dimensions/hatch; offset, trim,
extend, fillet, chamfer, mirror, scale, split, join; advanced snaps; box selection and vertex grips;
DXF and every other exchange format; 3D solids in the viewport; stopes, levels, declines as
dedicated types; resources, resource levelling, OR-Tools optimisation; 4D playback; geology and
geotechnical modules; PDF/XLSX export and reports; PostgreSQL/PostGIS; plugin loading; in-app
Python console; background workers/progress for long jobs (no long jobs exist yet).

## Files

111 tracked files; ~9 200 lines of C++/Python. See `DEVELOPMENT.md` for the repository map.
