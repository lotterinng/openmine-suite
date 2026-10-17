# Coordinates and engineering accuracy

## Implemented (v0.1)

* **Declared CRS.** `ProjectInfo.crs` stores name, authority code (e.g. `EPSG:2050`
  Hartebeesthoek94 / Lo25), vertical datum, linear unit (metre only), local-mine-grid flag and a
  south-orientated-axes flag. These are *declarations*; the software never infers a CRS from
  coordinate magnitudes or file extensions.
* **Axis convention.** Geometry is stored as `x, y, z` in the declared CRS axes; `z` is elevation.
  The Lo flag is metadata so that display/labels can follow the Westing/Southing convention; no
  stored value is ever negated or swapped implicitly.
* **Precision.** All computation is in `double`. Lengths are reproducible to < 1e-9 m at Lo-style
  magnitudes (`Polyline.PrecisionAtLargeMineCoordinates`). Vertices round-trip bit-exactly.
* **Render origin.** The viewport subtracts a 1 km-snapped origin before handing coordinates to VTK
  (GPU float precision), and adds it back for picking/snapping/status-bar display.
* **Tolerances.** 1 mm design-change tolerance; 1e-6 m degeneracy; documented in code next to each
  use.

## Planned (Phase 3)

* PROJ-based transformations with explicit pipelines; Cape Datum (Clarke 1880 modified) ↔
  Hartebeesthoek94/WGS84 via verified parameters or NTv2/grid files supplied by the surveyor.
  Transformations will record pipeline, grid files and stated accuracy in a `transformation`
  record and will never be applied implicitly on import.
* Local mine grid definitions (Helmert/affine from control) with residual reporting.
* Unit conversion at import/export boundaries only.
