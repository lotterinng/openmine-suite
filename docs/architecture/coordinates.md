# Coordinates and engineering accuracy

Implementation: `integrations/proj` (`crs::`), `core/geometry/LocalGrid`, `core/coordinates/
RenderOrigin`. Tests: `Crs.*`, `LocalGrid.*`, `Units.*`, `Polyline.PrecisionAtLargeMineCoordinates`,
`SurveyProject.DeclaredCrsIsComparedNeverApplied`, `test_gis_exchange_crs_transforms_and_local_grids`.

## Rules

1. **Declared, never inferred.** The project CRS is a `CoordinateReferenceSystem` entity (name,
   authority code, WKT2, kind, datums, unit, axes, area of use) chosen by the user — from an EPSG
   code, WKT, a PROJ string or a name search (Tools ▸ Coordinate systems…). OpenMine never guesses
   a CRS from coordinate magnitudes or file extensions. Project coordinates must be linear (metres
   or another length unit); a geographic CRS is refused as the project CRS.
2. **Declaring changes nothing.** Setting the project CRS records what the stored coordinates
   are; it does not touch them.
3. **No silent transformations.** Imports store coordinates exactly as read. A file's declared
   CRS is recorded (point clouds: `source_crs`) and compared with the project CRS; a difference is
   reported to the user ("coordinates were not transformed"), in the desktop and as a Python
   `UserWarning`. WKT is compared through its outermost `AUTHORITY`/`ID`.
4. **Transformations are explicit operations.** Tools ▸ Transform objects to another CRS… (and
   `project.crs.transform_objects`) lists PROJ's candidate operations best first with their
   accuracy and required grid files; the chosen one creates **transformed copies** (originals
   untouched) and a `TransformationRecord` with the PROJ pipeline, accuracy and grids — one undo
   step, audited, and not deletable. *Ballpark* operations (datum shift ignored) are refused
   unless explicitly allowed. Points can be checked against the CRS's area of use, which catches
   swapped axes (`Crs.AreaOfUseCatchesSwappedAxes`).
5. **Axis order is the CRS's own.** Coordinates are always in the axis order of their CRS as
   defined by the authority: EPSG:22285 (Cape / Lo25) is Y westing, X southing; EPSG:4326 is
   latitude, longitude. Axes are never swapped or negated implicitly; the display can label Lo
   axes as *Y westing, X southing* (Preferences ▸ Project, stored per project).

## South African Lo systems

`Crs.DescribesSouthAfricanLoSystemsWithTheirAxes` checks that Cape / Lo25 (EPSG:22285) resolves
on the Cape datum with axes pointing west and south in metres, that Hartebeesthoek94 / Lo25
(EPSG:2051) resolves on its datum, and that unknown codes and non-CRS codes are refused. `Crs.TransformsAgreeWithIndependentFormulaAndRoundTrip` compares WGS 84 →
Hartebeesthoek94 / Lo25 against an independent transverse-Mercator implementation (Snyder) and
checks round trips. Cape ↔ Hartebeesthoek94 needs a datum transformation; PROJ offers the
published operations with their stated accuracy (and grid-based ones when the grid files are
installed) — the surveyor chooses.

## Local mine grids

Tools ▸ Local mine grid… fits a local grid to a base CRS from control pairs (local ↔ base):

| Method | Parameters |
|---|---|
| Helmert 2D | translation E, N, Z; scale; rotation |
| Affine 2D | six plan coefficients + Z shift (mirrored grids, different axis scales) |
| Helmert 3D | 3D similarity (Umeyama): translation, scale, rotation vector |

Pairs marked as *check* pairs do not enter the fit; their residuals are reported separately as an
independent accuracy estimate. The fit is stored as a `LocalGridDefinition` and a CRS of kind
*LocalGrid* related to its base CRS, so transformations to and from the local grid go through the
same explicit, recorded operation as any other (`Crs.ProjectLocalGridAndExplicitObjectTransformation`,
`LocalGrid.HelmertAffineAnd3DFitsRecoverParameters`).

## Precision

* All geometry is stored and computed in double precision in project coordinates; vertices
  round-trip bit-exactly through the project file, the repository and the octree format.
* Lengths are reproducible to < 1e-9 m at Lo-system magnitudes (X ≈ 2 900 000 m)
  (`Polyline.PrecisionAtLargeMineCoordinates`); registrations recover transforms at mine
  coordinates (`SurveyRegistration.ClosedFormFitRecoversTransformAtMineCoordinates`).
* **Render origin.** The viewports subtract an origin near the data before handing coordinates to
  the GPU (32-bit floats) and add it back for picking, snapping and the status bar.
* Exchange formats that are single-precision by definition (binary STL) are written with a
  warning; OBJ/PLY/OMF/DXF keep doubles.
* Tolerances are explicit: 1e-6 m geometric degeneracy, 1 mm design-change detection, 0.001
  working minutes CPM float.

## Units

The project's linear unit is the CRS's; lengths typed with units are converted at the input
boundary (`crs::parseLength`: "12.5 m", "3 ft", "1200mm", "5'"), US survey feet included
(`Units.ParsingAndConversion`). Stored values are never rescaled implicitly.
