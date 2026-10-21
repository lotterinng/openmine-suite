# Third-party notices

Components used by OpenMine Suite v0.4.0, with the versions it was built and tested with. Full
licence texts must accompany any binary distribution; the audit, obligations and distribution rules
are in [docs/licensing.md](docs/licensing.md).

## Linked into the application, libraries or Python extension

| Component | Version tested | Licence | Use | Linked as |
|---|---|---|---|---|
| Qt (Core, Gui, Widgets, OpenGL, OpenGLWidgets, Svg, PrintSupport, Pdf, PdfWidgets) | 6.4.2 | LGPL-3.0-only | user interface, printing, PDF preview | shared |
| VTK | 9.1.0 | BSD-3-Clause | 3D viewports | shared |
| Open CASCADE Technology | 7.6.3 | LGPL-2.1-only WITH OCCT-exception-1.0 | solids, STEP/IGES | shared |
| PDAL | 2.8.4 | BSD-3-Clause | LAS/LAZ/E57/PLY I/O, SMRF, Poisson | shared |
| └ LAZperf (vendored in PDAL) | PDAL 2.8.4 | Apache-2.0 | LAZ compression | inside PDAL |
| └ PoissonRecon (Kazhdan, vendored in PDAL) | PDAL 2.8.4 | MIT | Poisson reconstruction | inside PDAL |
| └ libE57Format (PDAL E57 plugin) | 3.2.0 | BSL-1.0 | E57 | static in the PDAL E57 plugin |
| └ Xerces-C (libE57Format dependency) | 3.2 | Apache-2.0 | XML in E57 | shared |
| Open3D | 0.18.0 | MIT | alternative point-cloud engines | shared |
| └ libc++ / libc++abi (Open3D binary runtime) | 18 | Apache-2.0 WITH LLVM-exception | C++ runtime of the Open3D binary | shared |
| Google OR-Tools | 9.12 | Apache-2.0 | schedule optimisation | shared |
| └ Abseil | 2024.07 | Apache-2.0 | OR-Tools dependency | shared |
| └ Protocol Buffers | 29.3 | BSD-3-Clause | OR-Tools dependency | shared |
| └ RE2 | 2024 | BSD-3-Clause | OR-Tools dependency | shared |
| └ COIN-OR Cbc, Clp, Cgl, CoinUtils, Osi | (OR-Tools 9.12) | EPL-2.0 | OR-Tools MIP back-ends (not used by CP-SAT, but shipped with it) | shared |
| └ SCIP | 9 (static in OR-Tools) | Apache-2.0 | OR-Tools MIP back-end | static in OR-Tools |
| PROJ | 9.4.0 | MIT | CRS transformations | shared |
| GDAL/OGR | 3.8.4 | MIT (+ driver dependencies, see licensing.md) | rasters and vector GIS formats | shared |
| libpq (PostgreSQL client) | 16.15 | PostgreSQL License | shared repository | shared |
| SQLite | 3.45.1 | Public domain | project container | shared |
| Eigen | 3.4.0 | MPL-2.0 | linear algebra | header-only |
| nanoflann | 1.5.4 | BSD-2-Clause | k-d trees | header-only |
| Clipper2 | 1.4.0 | BSL-1.0 | polygon booleans (conformance) | shared |
| nlohmann/json | 3.11.3 | MIT | JSON | header-only |
| libxlsxwriter | 1.1.5 | BSD-2-Clause (bundles minizip: Zlib; tmpfileplus: MPL-2.0; md5: public domain) | XLSX export | shared |
| zlib | 1.3 | Zlib | PDF/OMF compression | shared |
| pybind11 | 3.1.0 | BSD-3-Clause | Python bindings | header-only |
| CPython | 3.13 | PSF-2.0 | embedded interpreter, `openmine` package | shared |
| NumPy | 2.x | BSD-3-Clause | Python API arrays | Python package |
| libgomp (GCC OpenMP runtime) | 13 | GPL-3.0 WITH GCC-exception-3.1 | OpenMP runtime of the Open3D binary | shared |

VTK, Qt, GDAL, PROJ and Open3D bundle further third-party code (for example FreeType, libpng,
expat, GLEW, fmt, Filament, Assimp). Their notices are installed with those libraries (VTK:
`share/licenses/VTK`; Open3D: its `LICENSE` and third-party list) and must be included in a binary
distribution.

## Server-side, separate process (not linked)

| Component | Version tested | Licence | Use |
|---|---|---|---|
| PostgreSQL | 16.15 | PostgreSQL License | shared repository server |
| PostGIS | 3.4.2 | GPL-2.0-or-later | spatial queries in the repository server |

## Development, test and documentation tools (not distributed)

| Component | Licence | Use |
|---|---|---|
| GoogleTest 1.14 | BSD-3-Clause | C++ tests |
| pytest | MIT | Python tests |
| ezdxf, trimesh, omf (GMG), mplstereonet, pypdf, pdfplumber, openpyxl | MIT / MIT / MIT / MIT / BSD-3-Clause / MIT / MIT | independent validation of exports |
| MkDocs 1.6, Material for MkDocs 9.7, pymdown-extensions | BSD-2-Clause / MIT / MIT | documentation site |
| Doxygen 1.9.8 | GPL-2.0 (tool only; generated output is not covered) | C++ API reference |
| CMake, Ninja | BSD-3-Clause / Apache-2.0 | build |
