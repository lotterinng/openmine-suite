# integrations/pdal

[PDAL](https://pdal.io/) (BSD-3-Clause) for point-cloud formats and two algorithms. API:
`openmine/pdalio/Pdal.h`.

- `pdalio::registerFormats()` registers LAS/LAZ (LASzip), E57 (libE57Format) and PLY readers and
  writers with `survey::openPointFile` / `survey::exportDataset`. The application and the Python
  module call it at start-up.
- `pdalio::smrfGround` classifies ground with the Simple Morphological Filter (`filters.smrf`).
- `pdalio::poissonSurface` reconstructs a surface with Poisson (`filters.poisson`).

Points stream through a custom PDAL reader stage, so imports are out of core and cancellable.

Validation (`tests/unit/test_pdal.cpp`):

- LAS/LAZ round trips preserve every attribute.
- PLY and E57 round trip.
- Project import goes through the reader registry and SMRF.
- Poisson of a sphere.

`tests/unit/test_open3d.cpp` also cross-checks the Poisson volume against Open3D.
