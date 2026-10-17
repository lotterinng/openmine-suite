# Dependency and licence audit

Scope: the dependencies named in the master specification, as they would be distributed in a
Windows desktop installer. This is an engineering assessment, not legal advice; have counsel
confirm before a commercial or public binary release.

## Recommendation: Apache-2.0 for OpenMine Suite's own code

| Option | Fit | Reasoning |
|---|---|---|
| **Apache-2.0** ✅ | best | Permissive with an explicit patent grant (important in a field with commercial vendors); compatible with every *required* dependency below when LGPL libraries are linked dynamically; lets mines and contractors build closed in-house plugins, which drives adoption. |
| MPL-2.0 | good | File-level copyleft keeps core improvements open while allowing proprietary plugins; weaker patent language than Apache; slightly less familiar to industrial users. Reasonable alternative. |
| GPL-3.0 | poor fit | Would allow linking GPL-only libraries (LibreDWG, Qt Charts) but forbids proprietary plugins/integrations, which most mining companies need for site-specific systems. |

## Dependency audit

| Component | Licence | Use in OpenMine | Distribution obligations | Status |
|---|---|---|---|---|
| Qt 6 (Core, Gui, Widgets, OpenGLWidgets) | LGPL-3.0 (or commercial) | UI | Link **dynamically**; ship licence text + Qt source offer; allow users to replace Qt DLLs; do not lock the installer against relinking | used |
| Qt Charts / Data Visualization / Graphs | **GPL-3.0** only (open source) | — | Incompatible with Apache distribution → **not used**; Gantt is custom-painted | avoided |
| VTK 9 | BSD-3-Clause | 3D viewport | notice | used |
| Open CASCADE Technology 7.x | LGPL-2.1 + OCCT exception | solids/volumes | dynamic linking; notice; source of OCCT modifications (none) | used |
| SQLite | Public domain | project container | none | used |
| GoogleTest | BSD-3-Clause | tests only | not distributed | used |
| pybind11 | BSD-3-Clause | Python bindings | notice | used |
| Python 3 (embedded, planned) | PSF-2.0 | scripting | notice | planned |
| Eigen 3 | MPL-2.0 | numerics | file-level copyleft only for modified Eigen files | planned |
| PDAL | BSD-3-Clause | LAS/LAZ/E57 pipeline | notice | planned (Phase 3) |
| LASzip | Apache-2.0 (relicensed from LGPL by rapidlasso) | LAZ | notice | planned |
| libE57Format | Boost Software License 1.0 | E57 | notice | planned |
| Open3D | MIT | registration/ICP, meshing | notice; check bundled deps (e.g. some optional modules) | planned (Phase 3) |
| PROJ | MIT | CRS transforms | notice; grid files have their own licences (check each) | planned |
| GDAL/OGR | MIT (+ various driver deps) | GIS formats | notice; avoid GPL/proprietary optional drivers in the bundle | planned |
| Google OR-Tools | Apache-2.0 | resource-constrained scheduling | notice | planned (Phase 6) |
| PostgreSQL client (libpq) | PostgreSQL licence (permissive) | enterprise DB | notice | planned |
| PostGIS | GPL-2.0+ | server side only | runs as a separate server; no linking into OpenMine → no effect on OpenMine's licence | planned |
| LibreDWG | **GPL-3.0** | DWG | **Not compatible** with in-process use under Apache-2.0. Options: DXF only; Open Design Alliance SDK under commercial licence as an optional, separately distributed plugin; or a separate GPL converter process communicating via files | not used |
| NumPy / SciPy | BSD-3-Clause | Python scripting | notice | user-installed |

## Practical rules for contributors

1. No new dependency without a row in this table and an ADR if the licence is not permissive.
2. GPL-only components may not be linked into the application or core libraries.
3. LGPL components must stay dynamically linked; the Windows installer must keep their DLLs
   replaceable.
4. `THIRD_PARTY_NOTICES.md` must list every distributed component and its licence text before a
   binary release.
5. No proprietary Maptek/Deswik code, assets, branding or undocumented formats. Interoperability is
   via documented exchange formats only (DXF, CSV, LAS/LAZ, E57, OMF, GeoPackage, …).
