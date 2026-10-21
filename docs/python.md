# Python guide

The `openmine` package drives the same native core as the desktop application. Every change a
script makes is validated by the same rules, is undoable and is written to the audit log; a script
cannot put the project into a state the user interface would refuse.

Complete, generated reference (every public class, function and property, checked by
`ctest -L docs`): [reference/python-api.md](reference/python-api.md).

## Where Python runs

| Where | How |
|---|---|
| Stand-alone | `PYTHONPATH=<build>/python python3 script.py` (or an installed wheel) |
| Desktop: Python console | Tools ▸ Python console — an embedded interpreter; `project` is the open project |
| Desktop: scripts | Tools ▸ Run Python script…, or the console command `RUNSCRIPT <file.py>` |
| Desktop: one statement | console command `PYTHON <statement>` (alias `PY`) |
| Plugins | Python plugin commands ([plugins.md](plugins.md)) |

In the desktop, `openmine.current_project()` returns the application's open project. Changes made
from Python appear immediately in the views and in Edit ▸ Undo.

## Conventions

* Identifiers are uuid strings. Wrapper objects (`Development`, `Activity`, `PointCloud`) are
  also accepted wherever an id is expected; `PointCloud` *is* a `str` subclass.
* Dates are `"YYYY-MM-DD"`, date-times `"YYYY-MM-DD HH:MM"` in project time.
* Point sets are `numpy` arrays of shape `(n, 3)` (float64 — coordinates are never rounded to
  float32); meshes are `(vertices, triangles)` pairs.
* Errors: `ValidationError` (rejected input; nothing changed), `PersistenceError`,
  `PointCloudError`, `DxfError`, `MeshFormatError`, `OmfError`, `GisError`, `SolidError`,
  `CrsError` (a `ValueError`), `CancelledError`. Unknown ids raise `KeyError`.
* Optional components are reported in `openmine.FEATURES` (`pdal`, `open3d`, `solids`, `crs`,
  `gis`, `optimiser`, `repository`); functions that need a missing one raise a clear error.
* `openmine.__version__`, `openmine.SCHEMA_VERSION` (project schema), `openmine.PLUGIN_API_VERSION`.

## The specification workflow

```py
from openmine import Project

project = Project.new("Jupiter Mine", start="2026-11-02")
project.declare_crs("EPSG:2051")                     # declares the CRS; coordinates are not changed

design = project.cad.create_development(
    name="LEG2_675_D3_DEC", length=150, width=5.5, height=5.5,
    start=(-25000.0, 2900000.0, 1200.0), azimuth_deg=270, gradient_percent=-12.5, level="675")
activity = project.scheduler.create_activity(
    name="Develop LEG2_675_D3_DEC", geometry=design, production_rate=5.0)   # m per standard day

result = project.scheduler.calculate()
print(result.activity(activity)["finish"])

scan = project.survey.import_pointcloud("scan.laz")   # LAS/LAZ/E57/PLY via PDAL; XYZ/PTS/CSV built in
clean = scan.remove_outliers()                        # a new, derived dataset; `scan` is unchanged
report = project.survey.compare_design(design, clean) # builds the as-built surface, then compares
print(report["overbreak_volume"], report["underbreak_volume"], report["valid_stations"])

project.save("JUP_Mine.openmine")
```

This exact workflow is exercised by `scripting/python/tests/test_api.py`
(`test_specification_workflow_and_bit_exact_round_trip`, which reopens the file and checks the
vertices are bit-identical, and `test_compare_design_against_a_scan`, which checks a 0.3 m wall
overbreak over 5 m × 30 m is measured as 45 m³ ± 5 % with both surface engines).

## Domain tour

The examples in this section run as written, in order: `scripting/python/tests/test_guide.py`
executes every `python` block of this page, so they cannot drift from the API.

```python
import numpy as np
from openmine import Project, ValidationError, FEATURES

project = Project.new("Guide", start="2026-11-02")
design = project.cad.create_development(name="DEC_675_A", length=120, width=5.5, height=5.5,
                                        start=(1000.0, 5000.0, 675.0), azimuth_deg=90,
                                        gradient_percent=-12.5, level="675")
activity = project.scheduler.create_activity(name="Develop DEC_675_A", geometry=design, production_rate=5.0)
assert abs(design.metrics["volume"] - 120 * 5.5 * 5.5) < 1e-6   # 3630 m³
```

### Undo and grouping

```python
with project.edit("Reference lines"):                 # one undo step
    a = project.cad.add_polyline([(0, 0, 0), (50, 0, -5)], name="Ref A")
    b = project.cad.offset(a, 10.0, side_point=(0, 5, 0))
assert project.undo_text == "Reference lines"
project.undo()                                        # removes both lines
project.redo()

count = len(project.cad.ids())
try:
    with project.edit("Rolled back"):
        project.cad.add_line((0, 0, 0), (1, 0, 0))
        project.cad.add_layer("")                     # ValidationError: "Layer name must not be empty"
except ValidationError:
    pass
assert len(project.cad.ids()) == count                # the line was rolled back too
```

### CAD and solids

```python
pad = project.cad.add_rectangle((0, 0, 0), (10, 5, 0), name="Pad")
copy = project.cad.move([pad], (0, 20, 0), copy=True)["created"][0]
if FEATURES["solids"]:                                # Open CASCADE
    block = project.solids.extrude([pad], distance=4.0)[0]
    info = project.solids.info(block)
    assert info["valid"] and abs(info["volume"] - 200.0) < 1e-6
```

### Mine design

```python
project.design.add_level("675", elevation=675.0)
crosscuts = project.design.create_crosscuts(design, interval=30, length=40, side="left")
violations = project.design.check_design_rules(design, max_grade_percent=12.0)
assert violations[0]["rule"] == "Maximum grade"       # -12.5 % exceeds 12 %

flagged = project.design.grade(design, grade_percent=-11.0)
assert flagged == 1                                   # the linked activity is flagged, not changed
assert project.design.impacts()[0]["heading_id"] == "DEC_675_A"
project.design.apply_design_quantities(activity)      # explicit, audited, undoable
assert project.design.impacts() == []
```

### Scheduling

```python
crew = project.scheduler.add_calendar("Jumbo crew", hours_per_day=20, shifts=[
    {"name": "Day", "start": "06:00", "hours": 10}, {"name": "Night", "start": "18:00", "hours": 10}])
project.scheduler.set_calendar(activity, crew)
jumbo = project.scheduler.add_resource("Jumbo J01", capacity=1)
project.scheduler.assign(activity, jumbo)

activity.record_actual(15, date="2026-11-03")         # dated production actual
project.set_status_date("2026-11-04")                 # reforecast the remaining work from here
baseline = project.scheduler.create_baseline("BL-Nov-2026")
levelled = project.scheduler.add_scenario("Levelled", levelling="Heuristic")

result = project.scheduler.calculate(scenario=levelled)
timing = result.activity(activity)
assert timing["actual_quantity"] == 15 and timing["started"]
weekly = project.scheduler.production("week")         # scheduled / actual / target per week
assert weekly[0]["actual"] == 15
project.scheduler.export_xlsx("schedule.xlsx")
if FEATURES["optimiser"]:                             # OR-Tools CP-SAT, stored as a scenario
    best = project.scheduler.optimise(store_as="Optimised", time_limit_seconds=5)
    assert best["status"] in ("optimal", "feasible")
```

### Survey

```python
rng = np.random.default_rng(1)
xy = rng.uniform(0, 50, size=(20000, 2))
pts = np.column_stack([xy, 0.02 * xy[:, 0] + rng.normal(0, 0.01, len(xy))]) + [1000.0, 5000.0, 700.0]
scan = project.survey.add_point_cloud(pts, "Pad scan")

clean = scan.remove_outliers(k=16, std_ratio=2.0)     # new dataset; `scan` is never modified
thin = clean.voxel_downsample(0.25)                   # nearest-to-centre keeps real points
assert thin.parent == clean and thin.derivation == "voxel nearest-to-centre 0.250000 m"
surface = thin.surface(method="delaunay")             # {"id", "triangles", "method"}
if FEATURES["pdal"]:
    ground = scan.classify_ground()                   # PDAL SMRF
    assert ground.derivation == "SMRF ground classification"
```

Registrations (`project.survey.register_icp`, control-point and pair registration) change the
dataset's stored transformation, never its points, and are reverted with
`project.survey.revert_registration`.

### Geotechnical, exchange and GIS

```python
from openmine import geotech
q = geotech.q_system(rqd=75, jn=9, jr=1.5, ja=1, jw=1, srf=1)
assert q["q"] == 12.5 and q["class"] == "Good"

project.formats.export_dxf("design.dxf")
other = Project.new("Copy", start="2026-11-02")
assert other.formats.import_dxf("design.dxf")["objects"] > 0

if FEATURES["gis"]:
    out = project.formats.export_vector(project.cad.ids(), "design.gpkg")
    assert out["written"] > 0                         # meshes and text are listed in out["skipped"]
    assert len(other.formats.import_vector("design.gpkg")["ids"]) == out["written"]
```

### Attachments and compaction

```python
with open("face.txt", "w") as f:
    f.write("Face at 0+045: wet, extra bolting")
note = project.attachments.add("face.txt", to=design, description="Face inspection")
project.attachments.save(note, "copy.txt")            # verified against the stored SHA-256
assert open("copy.txt").read().startswith("Face at 0+045")

project.save("guide.openmine")
project.attachments.remove(note)                       # undoable; the stored copy is kept for undo
freed = project.compact()                              # removes unused stored files, saves, clears undo
assert len(freed["unreferenced_assets"]) == 1 and project.undo_text == ""
```

## Long operations: progress and cancellation

```python
from openmine import Progress, CancelledError

progress = Progress()          # cancel() may be called from another thread, e.g. a UI button
progress.cancel()
try:
    clean.voxel_downsample(0.1, progress=progress)
except CancelledError:
    pass                       # nothing was changed
```

Long native operations release the GIL, so other Python threads keep running; `progress.fraction`
and `progress.stage` can be polled from them.

## Documents: lossless access to every entity

```python
types = project.documents.types()                     # every entity type, in dependency order
doc = project.documents.template("ScheduleActivity")  # a default entity with a fresh uuid
doc["columns"]["name"] = "Ventilation raise"
key = project.documents.put("ScheduleActivity", doc)  # validated by the same editor as the UI
assert project.documents.check() == []                # referential-integrity problems
```

The document form is the same as the project file and the shared PostgreSQL repository use
([plugins.md](plugins.md#entity-documents)).

## Shared repository (PostgreSQL/PostGIS)

```py
from openmine.repository import Repository

repo = Repository("host=db.mine.local dbname=openmine user=planner")
wc = repo.create_project(project, author="planner", message="Initial import")   # revision 1
local, wc = repo.checkout(repo.projects()[0][0])      # a local Project and its working copy
...                                                   # edit offline
repo.pull(local, wc)                                  # policy "abort" (default), "take_remote", "keep_local"
repo.publish(local, wc, author="planner", message="Level 675 headings")   # refused when behind
boxes = repo.intersecting(repo.projects()[0][0], (900, 4900, 1200, 5100))   # PostGIS query
```

Publishing is refused when the working copy is behind the repository (pull first); conflicting
changes are reported and resolved by an explicit policy, never overwritten silently. Advisory
locks (`lock`/`unlock`) reserve entities while someone works on them. Tested against PostgreSQL 16
+ PostGIS 3.4 by `tests/unit/test_pg_repository.cpp` and `test_api.py::test_repository_round_trip`.

## Writing plugins

See [plugins.md](plugins.md#python-plugins): an entry point in the `openmine.plugins` group,
commands receive the project and keyword arguments, each run is one undo step.
