# benchmarks

`openmine_bench` times the engine at realistic sizes, without the UI:

| Benchmark | Full size | What is timed |
|---|---|---|
| `cpm` | 10 000 activities, FS chains + SS cross links | `calculateSchedule` (input + passes) |
| `levelling` | 2 000 activities on 6 shared jumbos | `levelResources` |
| `project-save` / `project-load` | 20 000 polylines × 20 vertices | atomic verified save; verified load |
| `octree-build` / `octree-read` | 2 000 000 points | out-of-core build from a stream; full read |
| `kdtree-build` / `kdtree-knn16` | 2 000 000 points / 200 000 queries | nanoflann index; 16 nearest neighbours |
| `statistical-outliers`, `voxel-0.25m` | 2 000 000 points | filters used by the survey derivations |

```bash
build/<preset>/bin/openmine_bench --json results.json           # full sizes (use a Release build)
build/<preset>/bin/openmine_bench --check benchmarks/budgets.json  # fail when over budget
```

`--quick` runs small sizes; the `benchmarks_quick` ctest (label `benchmark`) uses it to keep the
benchmarks working. Each benchmark also checks its result (no lost points or objects).

Reference run (Release, GCC 13.3, 2026-10-21, 2-core cloud VM):

| Benchmark | Size | Seconds |
|---|---|---|
| cpm | 10 000 | 0.840 |
| levelling | 2 000 | 0.200 |
| project-save | 20 000 | 0.309 |
| project-load | 20 000 | 0.127 |
| octree-build | 2 000 000 | 0.554 |
| octree-read | 2 000 000 | 0.020 |
| kdtree-build | 2 000 000 | 0.693 |
| kdtree-knn16 | 200 000 | 1.143 |
| statistical-outliers | 2 000 000 | 5.125 |
| voxel-0.25m | 2 000 000 | 1.261 |

The budgets in `budgets.json` leave room for slower CI runners (at least 3.5× these times).
