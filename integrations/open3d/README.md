# integrations/open3d

[Open3D](https://www.open3d.org/) (MIT) engines for point-cloud processing, offered as
alternatives to the built-in OpenMine implementations. Built when CMake finds Open3D
(`-DOpen3D_DIR=…/lib/cmake/Open3D`); `OPENMINE_WITH_OPEN3D=OFF` disables it.

API: `openmine/o3d/Open3d.h`.

| Function | Open3D component | Built-in counterpart |
|---|---|---|
| `o3d::icp` | `pipelines::registration::RegistrationICP` (point-to-point, point-to-plane, Tukey kernel) | `survey::icp` |
| `o3d::estimateNormals` | `PointCloud::EstimateNormals`, `EstimateCovariances` | `survey::estimateNormals` |
| `o3d::voxelDownsample` | `PointCloud::VoxelDownSampleAndTrace` | `survey::voxelDownsample(…, Centroid)` |
| `o3d::statisticalInliers`, `o3d::radiusInliers` | `RemoveStatisticalOutliers`, `RemoveRadiusOutliers` | `survey::statisticalInliers`, `survey::radiusInliers` |
| `o3d::poissonSurface` | `TriangleMesh::CreateFromPointCloudPoisson` (with densities) | `pdalio::poissonSurface` |

Design points:

- Everything runs in a local frame (coordinates minus a centre), so mine coordinates keep full
  precision; results come back in the input coordinates.
- ICP is iterated one Open3D step at a time. The convergence test on the increment, the RMS
  history, progress, cancellation, moving-point sampling (same seed) and the reported fitness /
  inlier RMS therefore have exactly the definitions of `survey::icp`. Open3D has no trimmed ICP:
  a trim fraction is refused unless a Tukey robust kernel is given.
- `o3d::registerEngines()` registers the ICP as engine `"Open3D"` (`survey::registerIcpEngine`).
  `survey::computeIcpRegistration` records the engine and version in the registration's notes.
- Voxels are anchored exactly as in the built-in filter, and attributes the Open3D cloud cannot
  carry (intensity, classification, returns, GPS time, scalar fields) are aggregated from the
  traced point indices with the built-in rules.
- Normals use the same orientation rules and the same curvature definition λ0 / Σλ.

Where it is used:

- In the desktop app, the ICP dialog and the point-cloud dialogs (normals, subsample, outliers,
  Poisson surface) have an *Engine* choice, as does the as-built dialog (Poisson). The engine is
  recorded in each derived dataset's derivation, in the surface's `method` attribute and in each
  registration's notes.
- In Python, `openmine.survey.icp_engines()` lists the engines;
  `project.survey.register_icp(…, engine="Open3D")` selects one.

Validation: `tests/unit/test_open3d.cpp` cross-validates against the built-in engines on data at
mine coordinates. The checks:

- ICP transforms agree to 1 cm, with both engines recovering a known misalignment.
- Normals agree to within 0.5°, and curvature to 1e-3.
- Voxel output matches point for point, including attributes.
- Radius outlier masks are identical; statistical masks agree on more than 99 % of points.
- Poisson recovers a sphere's volume to 3 %, and agrees with PDAL's reconstruction to 3 %.

Also tested: the engine registry, the project record, refusals, cancellation, and the Python
API (`test_icp_engines_agree`).

Runtime note: the Linux binary packages of Open3D 0.18 contain components built against libc++,
so the libc++ runtime (`libc++1`, `libc++abi1`) must be installed alongside them.
