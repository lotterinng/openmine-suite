# Point-cloud octree format (`openmine-octree`, version 1)

OpenMine stores imported point clouds as an out-of-core, level-of-detail octree in the project's
asset sidecar. The format is OpenMine's own and fully documented here so any tool can read it.
Implementation: `modules/survey/src/Octree.cpp` (`buildOctree`, `OctreeReader`). Tests:
`SurveyOctree.*` in `tests/unit/test_survey_io.cpp` (120 000 points with intensity, RGB and a
scalar field: every point stored once, bit-exact; no orphan nodes; node bounds inside node cubes).

## Location

```
<project>.openmine-assets/pointcloud/<asset uuid>/
    manifest.json          asset manifest (format "openmine-asset-manifest", per-file SHA-256)
    octree.json            octree metadata
    nodes/<name>.bin       points owned by each node
```

The asset is immutable once finalised. Processing (filtering, registration, derivation) never
rewrites an octree; a derived dataset gets a new asset. Registrations are stored as
transformations on the dataset, so the stored coordinates are always the surveyed ones.

## `octree.json`

```json
{
  "format": "openmine-octree",
  "version": 1,
  "pointCount": 120000,
  "bounds": [xmin, ymin, zmin, xmax, ymax, zmax],
  "cube":   [xmin, ymin, zmin, xmax, ymax, zmax],
  "grid": 128,
  "schema": { "intensity": true, "rgb": true, "classification": false, "normals": false,
              "returnInfo": false, "gpsTime": false, "scalarFields": ["deviation"] },
  "nodes": [
    { "name": "r",  "level": 0, "count": 20000, "cube": [...], "bounds": [...], "spacing": 0.39 },
    { "name": "r0", "level": 1, "count": 18211, "cube": [...], "bounds": [...], "spacing": 0.195 }
  ]
}
```

| Key | Meaning |
|---|---|
| `format`, `version` | must be `"openmine-octree"` and ≤ 1; newer versions are refused with a message |
| `pointCount` | total points (sum of node counts) |
| `bounds` | tight bounds of all points |
| `cube` | root cube (equal edges, enclosing `bounds`) |
| `grid` | sampling grid per node (cells per axis) used when building |
| `schema` | which attribute columns every node file contains, in the order below |
| `nodes[]` | every node, sorted by name: `name`, `level`, `count`, node `cube`, tight `bounds` of the points it owns, sampling `spacing` (cell size) |

### Node names

The root is `r`. A child appends one digit `d = 4·x + 2·y + z`, where *x*, *y*, *z* are 1 for the
upper half of the parent cube along that axis: `r0` … `r7`, `r07`, `r073`, … The parent of a node
is its name without the last digit. A node's cube is derived from the root cube and its name;
the stored `cube` is redundant and lets readers check consistency.

## Node files `nodes/<name>.bin`

Little-endian, column by column:

| Field | Type | Present when |
|---|---|---|
| magic | 8 bytes `OMPNODE1` | always |
| count *n* | `uint64` | always |
| x, y, z | *n* × 3 × `float64` (interleaved per point) | always |
| intensity | *n* × `uint16` | `schema.intensity` |
| red, green, blue | *n* × 3 × `uint16` | `schema.rgb` |
| classification | *n* × `uint8` | `schema.classification` |
| normal x, y, z | *n* × 3 × `float32` | `schema.normals` |
| return number | *n* × `uint8` | `schema.returnInfo` |
| number of returns | *n* × `uint8` | `schema.returnInfo` |
| GPS time | *n* × `float64` | `schema.gpsTime` |
| each scalar field | *n* × `float32` per field, in `scalarFields` order | always (0 fields allowed) |

Coordinates are float64 exactly as read from the source — no offset/scale quantisation, so mine
coordinates (e.g. 2 900 000 m northings) keep sub-millimetre precision bit-exactly.

## Construction

* Every point is stored **exactly once**. A node owns a spatially uniform sample of its region —
  one point per occupied grid cell (`grid`³ cells over the node cube), the point nearest to the
  cell centre; the remaining points are passed to its eight children.
* A node with more than `maxNodePoints` (20 000) points is split, up to `maxDepth` (24).
* The source is streamed three times (bounds; per-cell counts; distribution into chunks of at most
  4 million points), so clouds larger than memory can be converted. Building is cancellable; an
  aborted build removes its temporary chunk files and the asset folder is discarded
  (`AssetStore::abortFolder`).

## Reading

* **Level of detail:** select nodes top-down; a set in which every selected node's parent is also
  selected shows a cloud whose density increases with depth. The desktop viewport descends into
  nodes inside the view frustum while their projected sampling spacing exceeds the target screen
  spacing (default 1.5 px), within a point budget (default 3 million per view, Preferences ▸ Point
  clouds); nodes are loaded in the background, most important first.
* **Complete cloud:** reading all nodes returns every point; `readAll(box)` reads only nodes whose
  tight bounds intersect the box and filters their points.
