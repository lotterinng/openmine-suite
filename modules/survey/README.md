# modules/survey

Point clouds and survey analysis (toolkit-independent; PDAL I/O lives in integrations/pdal).

| Area | Header | Notes |
|---|---|---|
| Buffers, sources, sinks | `PointBuffer.h` | SoA attributes: intensity, RGB, class, normals, returns, GPS time, float scalars |
| ASCII formats | `TextFormats.h` | XYZ/CSV/TXT (header and delimiter detection), Leica PTS, PTX (per-scan pose, no-return skip) |
| Octree | `Octree.h` | out-of-core LOD build (3 streaming passes), bit-exact float64 storage, box/LOD queries |
| Project datasets | `PointCloudProject.h` | import, derived datasets, export, format registry |
| Processing | `Processing.h` | crop masks, voxel/random/spacing subsampling, SOR/radius outliers, PCA normals, clustering, region growing, RANSAC planes, C2C / C2M distances |
| Registration | `Registration.h` | Umeyama point-pair/control fit (± scale), ICP point-to-point/point-to-plane, fit vs. check statistics, apply/revert |
| Meshing | `Meshing.h` | 2.5D Delaunay (sweep-hull + Lawson), QEM decimation, Taubin smoothing, repair/hole filling, exact TIN-to-plane and grid surface-to-surface volumes |
| Conformance | `Conformance.h` | overbreak/underbreak by station (Clipper2), clearance envelope, face position, stored results |

Tests: `tests/unit/test_survey_io.cpp`, `tests/unit/test_survey_processing.cpp`, `tests/unit/test_pdal.cpp`.
