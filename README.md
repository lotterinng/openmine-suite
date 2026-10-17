# OpenMine Suite

**Open-source, integrated mine survey, CAD/mine design and scheduling — one application, one
project, one model.**

OpenMine Suite is an original desktop application (C++20 · Qt 6 · VTK · Open CASCADE · SQLite)
that aims to cover, in one shared environment, the workflow mining teams currently spread across
point-cloud survey tools, mine CAD and mine schedulers:

> survey scan → registration → as-built → design → attributes → schedule activities → resources →
> schedule → actuals → reconciliation → reforecast → reports

It is pre-1.0. **What works today is listed precisely in [`IMPLEMENTATION_STATUS.md`](IMPLEMENTATION_STATUS.md);**
everything else is roadmap. Unfinished tools are visible in the UI but disabled and labelled with
their planned phase — nothing pretends to work.

## What v0.1 does

* Native Qt 6 desktop shell: menu bar, 10 ribbon workspaces, dockable Project Explorer,
  Properties editor, Command Console and Scheduler, status bar, dark and light themes.
* VTK 3D viewport: draw lines/polylines on a work plane with grid and endpoint snapping, pick to
  select, standard views, perspective/parallel, development envelopes, planned-vs-actual advance,
  large-coordinate render origin.
* Development headings as domain objects (heading ID, level, zone, profile, chainage, status…)
  with derived length, gradient and volume (Open CASCADE swept solid).
* Linked, rate-driven schedule activities; CPM with FS/SS/FF/SF + lags, floats, critical path,
  WBS, calendars, baselines and status-date reforecast; Gantt with dependency arrows.
* Design changes **flag** linked activities instead of silently changing quantities;
  recalculation is explicit and audited; baselines are immutable.
* `.openmine` project files: documented SQLite schema, atomic verified saves, migrations,
  audit log, bit-exact coordinates.
* Python API (`from openmine import Project`) for the same workflow.

## Quick start

See [`DEVELOPMENT.md`](DEVELOPMENT.md) for Windows (MSVC + vcpkg) and Linux builds.

```bash
cmake --preset linux-debug -DVTK_DIR=/opt/vtk-qt6/lib/cmake/vtk-9.3
cmake --build --preset linux-debug
ctest --preset linux-debug
./build/linux-debug/apps/desktop/openmine-desktop
```

Console example:

```
POLYLINE -25000,2900000,1200 @-90,0,-11.25 @0,-60,-7.5
HEADING LEG2_675_D3_DEC 5.5 5.5 675
ACTIVITY 5
SAVE C:/Mines/Jupiter.openmine
```

## Documentation

| Document | Contents |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | layers, state model, object model, UI, risks |
| [docs/architecture/database-schema.md](docs/architecture/database-schema.md) | ERD, constraints, deletion policies, migrations |
| [docs/architecture/scheduling.md](docs/architecture/scheduling.md) | CPM definitions and validation |
| [docs/architecture/project-format.md](docs/architecture/project-format.md) | `.openmine` container |
| [docs/architecture/coordinates.md](docs/architecture/coordinates.md) | CRS policy, precision |
| [docs/licensing.md](docs/licensing.md) | dependency licence audit |
| [ROADMAP.md](ROADMAP.md) · [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) | plan and status |

## Licence

Apache-2.0 (see [`LICENSE`](LICENSE), [`NOTICE`](NOTICE), [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)).
OpenMine Suite is independent and not affiliated with Maptek or Deswik; it does not use their
code, assets or proprietary file formats.
