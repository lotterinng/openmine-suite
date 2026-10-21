# As-built surfaces and design conformance

Implementation: `modules/survey` — `AsBuilt` (surface pipeline), `Conformance` (analysis,
storage, face advance). Surface engines: `integrations/pdal` (Poisson, PDAL 2.8) and
`integrations/open3d` (Poisson, Open3D 0.18), injected so the survey module depends on neither.
Used identically by the desktop (Survey ▸ As-built / Conformance), the console and Python
(`project.survey.build_as_built`, `conformance`, `compare_design`).

Tests: `tests/unit/test_survey_processing.cpp` (`SurveyAsBuilt.*`, `SurveyConformance.*`) and
`scripting/python/tests/test_api.py::test_compare_design_against_a_scan`.

## 1. As-built surface of a heading

1. **Corridor points.** Scan points within `corridor` (default 6 m) of the heading's centreline,
   in project coordinates (the dataset's registration in force is applied; the stored points are
   never modified), optionally voxel-thinned (`voxel`, default 0.05 m, nearest-to-centre).
2. **Normals** by PCA, oriented towards the **inside of the excavation**: towards the design
   profile's centroid carried along the centreline (the *interior line*, sampled every 0.5 m), not
   towards the centreline itself — a centreline usually runs at floor or grade level, where
   "towards the line" is ambiguous for floor points.
3. **Reconstruction** by the chosen engine (`depth`, default 9): a watertight implicit surface.
4. **Trimming.** Triangles whose centroid lies farther from the scan than
   `max(3 × voxel, 2 × median spacing, 0.05 m)` are removed — Poisson closes unscanned ends and
   shadows with invented surface that must not be measured.
5. **Repair.** Consistent orientation (not forced outward — a tunnel surface is open), and holes
   left by trimming with at most `fillHolesUpTo` (default 32) boundary edges are filled; the open
   heading ends are far larger and stay open.

The result is stored as a mesh geometry object plus an `AsBuiltModel` recording the dataset, the
registration in force, the development and the method text (`Poisson depth=<n> (<engine and version>)`).
One undo step.

## 2. Station sections

Stations are placed every `interval` (default 1 m) along the centreline between optional
`fromChainage`/`toChainage`. At each station a plane with the design section frame is used:
*u* to the right of travel, *v* up, orientation perpendicular to the axis (or vertical, per
option).

* **Mesh as-built:** the plane section of the mesh gives segment chains. Chains broken by holes
  in the as-built surface are joined end to end across gaps up to `maxGap` (default 0.5 m) — the
  report says "gaps up to X m bridged". A section still open after assembly (an unscanned floor)
  is closed by the chord between its ends and flagged `closedByChord`. An open section that does
  not enclose the design centroid is **partial** (typically at an unscanned end): the station is
  reported with the issue "— partial section, station not used" and excluded from volumes and
  depths.
* **Point-cloud as-built (no meshing):** points in a slab of ±`slabHalfWidth` (0.05 m) around the
  plane; the outermost point in each of `angularBins` (720) bins around the design centroid forms
  the section. Fewer than `minSlabPoints` (50) points make the station invalid.

## 3. Quantities

With design section *D* and as-built section *A* (polygons in *u, v*; booleans by Clipper2):

| Quantity | Definition |
|---|---|
| Overbreak area | area(*A* − *D*) |
| Underbreak area | area(*D* − *A*) |
| Max overbreak depth | largest distance of *A*'s boundary outside *D* |
| Max underbreak depth | largest distance of *A*'s boundary inside *D* |
| Clearance | smallest distance from an optional equipment envelope to *A*'s boundary; negative where *A* intrudes into the envelope |
| Volumes | average-end-area rule over consecutive **valid** stations |
| Overbreak % | overbreak volume / design volume × 100 |
| Mean overbreak depth | mean over valid stations of their maximum overbreak depth |

Reference case (`SurveyConformance.OverbreakUnderbreakClearanceFromMeshAndPoints`): a 5 × 5 m
design along 50 m, as-built 6 × 5.5 m (0.5 m overbreak on each wall and the back), stations every
5 m → 11 valid stations, each 25 m² design, 33 m² as-built, 8 m² overbreak, max overbreak depth
√0.5 m (the corner), clearance 0.5 m to a 4 × 3.5 m envelope; overbreak volume 400 m³ of 1250 m³
(32 %). A 4.5 m-wide as-built gives 2.5 m² underbreak per station, 0.25 m depth, 125 m³. The
same analysis from 200 000 points sampled on the as-built surface gives 8 ± 0.25 m² per station.

End-to-end (`test_compare_design_against_a_scan`): a synthetic scan of a 30 m heading whose north
wall is 0.3 m out over its 5 m height → overbreak 45 m³ ± 5 % and underbreak < 2 m³, with the PDAL
and Open3D engines agreeing within 3 %.

## 4. Storage

`commitConformance` stores the per-station table as an `AnalysisResult` and the summary as a
`DesignConformanceResult` (as-built, development, parameters as JSON including `interval` and
`max_gap`, volumes, depths, valid stations). Results are new records; neither the design nor the
scan is changed. CSV export (`conformanceCsv`) formats chainages as `1+234.500`.

## 5. Face position and advance reconciliation

* `facePosition`: scan points within a corridor (5 m) are binned along the centreline (0.5 m
  bins, ≥ 20 points); the face is the end of the continuously covered stretch, allowing gaps up to
  2 m.
* `reconcileAdvance`: surveyed advance (current face − previous survey face or heading start)
  against reported non-survey actuals of the linked activity since the previous survey;
  `correction = surveyed − reported`.
* `commitSurveyActual` stores the correction as a `ProductionActual` with source *Survey*, the
  dataset id and the face chainage, so reported + survey actuals equal the surveyed advance. The
  CPM then reforecasts from it (one undo step).

## 6. Limitations

* Sections are planar; strongly curved headings with short radii are sampled at the station
  interval (use a smaller interval on curves).
* The angular-bin method for point clouds assumes the section is star-shaped around the design
  centroid; for irregular shapes use a mesh as-built.
* Surface engines need adequate coverage: the pipeline refuses (with a message) when too few
  corridor points remain after trimming.
