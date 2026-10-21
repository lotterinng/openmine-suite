# Dependency and licence audit

Scope: every dependency OpenMine Suite v0.4.0 uses, as it would be distributed in a desktop
installer. Versions: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). This is an engineering
assessment, not legal advice; have counsel confirm before a commercial or public binary release.

## OpenMine's own licence: Apache-2.0

| Option | Fit | Reasoning |
|---|---|---|
| **Apache-2.0** ✅ | chosen | Permissive with an explicit patent grant (important in a field with commercial vendors); compatible with every linked dependency below when LGPL/EPL libraries are linked dynamically; lets mines and contractors build closed in-house plugins. |
| MPL-2.0 | good | File-level copyleft keeps core improvements open while allowing proprietary plugins; weaker patent language. |
| GPL-3.0 | poor fit | Would allow GPL-only libraries but forbid proprietary plugins and integrations most mining companies need. |

## Audit

| Component | Licence | Obligations in a binary distribution | Status |
|---|---|---|---|
| Qt 6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets, Svg, PrintSupport, Pdf, PdfWidgets) | LGPL-3.0 | link dynamically; ship licence text and a source offer; users must be able to replace the Qt libraries (no locked installer); no static linking | used |
| Qt Charts / Data Visualization / Graphs | GPL-3.0 (open-source edition) | incompatible with an Apache distribution | **not used** — Gantt, histograms and charts are custom-painted |
| VTK 9 | BSD-3-Clause | notices (incl. VTK's bundled third-party notices) | used (without its Qt module, ADR 0005) |
| Open CASCADE 7.6 | LGPL-2.1 + OCCT exception | dynamic linking; notice; source of any OCCT modification (none) | used |
| PDAL 2.8 | BSD-3-Clause | notice; vendored LAZperf (Apache-2.0), PoissonRecon (MIT), arbiter (MIT), utfcpp (BSL-1.0) | used |
| libE57Format 3.2 (+ Xerces-C 3.2) | BSL-1.0 (+ Apache-2.0) | notices | used through PDAL's E57 plugin |
| Open3D 0.18 | MIT | notice + Open3D's third-party notices; the official binary needs libc++/libc++abi (Apache-2.0 WITH LLVM-exception) and libgomp (GPL-3.0 WITH GCC Runtime Library Exception — permits distribution with non-GPL software) | used (optional) |
| Google OR-Tools 9.12 | Apache-2.0 | notice; bundled Abseil (Apache-2.0), protobuf and RE2 (BSD-3), SCIP 9 (Apache-2.0); **COIN-OR Cbc/Clp/Cgl/CoinUtils/Osi are EPL-2.0** — weak copyleft at file level: ship their licence and offer their source; they are used unmodified and dynamically linked | used (optional) |
| PROJ 9 | MIT | notice; datum grid files have their own licences — ship only grids whose licence allows it (proj-data lists them) | used (optional) |
| GDAL/OGR 3.8 | MIT | notice. **Distribution builds must exclude GPL drivers**: the Ubuntu GDAL used for development links Poppler (GPL-2/3, PDF driver) and MySQL client (GPL-2 with FOSS exception) — an installer must use a GDAL built without them (vcpkg `gdal` default features do not include Poppler or MySQL) | used (optional) |
| libpq | PostgreSQL License | notice | used (optional) |
| PostgreSQL / PostGIS server | PostgreSQL License / GPL-2.0+ | separate server process, not linked or distributed with OpenMine → no effect on OpenMine's licence | external |
| SQLite | Public domain | none | used |
| Eigen 3 | MPL-2.0 | file-level copyleft only for modified Eigen files (none) | used |
| nanoflann | BSD-2-Clause | notice | used |
| Clipper2 | BSL-1.0 | notice in source distributions; none required in binaries, included anyway | used |
| nlohmann/json | MIT | notice | used |
| libxlsxwriter | BSD-2-Clause (bundled minizip: Zlib; tmpfileplus: MPL-2.0) | notices | used |
| zlib | Zlib | none (notice appreciated) | used |
| pybind11 | BSD-3-Clause | notice | used |
| CPython (embedded) | PSF-2.0 | notice; the licence text | used |
| NumPy | BSD-3-Clause | notice | used by the Python API |
| GoogleTest, pytest, validation libraries (ezdxf, trimesh, omf, mplstereonet, pypdf, pdfplumber, openpyxl) | BSD-3 / MIT | not distributed | tests only |
| MkDocs, Material for MkDocs, Doxygen | BSD-2 / MIT / GPL-2 (tool) | not distributed; Doxygen's output is not covered by its licence | documentation only |
| LibreDWG | GPL-3.0 | incompatible with in-process use under Apache-2.0 | **not used**; DWG would need the ODA SDK (commercial) as a separately distributed plugin, or a separate GPL converter process exchanging files |

## Formats and intellectual property

* Only documented, open exchange formats are implemented: DXF (published by Autodesk), LAS/LAZ
  (ASPRS / LASzip), E57 (ASTM E2807), PLY, OBJ, STL, PTS/PTX, OMF (Global Mining Guidelines Group),
  STEP/IGES (ISO), GeoTIFF, Shapefile (ESRI white paper), GeoPackage (OGC), GeoJSON (RFC 7946),
  KML (OGC). The octree and project formats are OpenMine's own and documented.
* No Maptek or Deswik code, assets, branding, screenshots or undocumented file formats are used.
  Interoperability with their products is through the open formats above.
* All icons are drawn programmatically by OpenMine; no third-party icon sets are included.
* Test and sample data are generated by the tests or released as CC0 (`samples/`).

## Rules for contributors

1. No new dependency without a row in this table and in `THIRD_PARTY_NOTICES.md`, and an ADR if
   its licence is not permissive.
2. GPL-only components may not be linked into the application, libraries or plugins shipped with
   it.
3. LGPL and EPL components stay dynamically linked; the Windows installer must keep their DLLs
   replaceable.
4. Before a binary release, `THIRD_PARTY_NOTICES.md` must list every distributed component with
   its licence text in the installer.
5. Plugins may use any licence compatible with being loaded by an Apache-2.0 host.
