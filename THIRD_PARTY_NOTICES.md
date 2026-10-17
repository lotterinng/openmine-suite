# Third-party notices

Components linked by the current build (v0.1.0). Full licence texts must accompany any binary
distribution; the audit and obligations are in `docs/licensing.md`.

| Component | Version tested | Licence | Linked as |
|---|---|---|---|
| Qt (Core, Gui, Widgets, OpenGL, OpenGLWidgets) | 6.4.2 | LGPL-3.0-only | shared libraries |
| VTK | 9.3.1 | BSD-3-Clause | shared libraries |
| Open CASCADE Technology | 7.6.3 | LGPL-2.1-only with OCCT exception | shared libraries |
| SQLite | 3.45.1 | Public domain | shared library |
| pybind11 | 3.1.0 | BSD-3-Clause | header-only (Python extension) |
| GoogleTest | 1.14.0 | BSD-3-Clause | test executables only (not distributed) |

VTK bundles third-party code (e.g. FreeType — FTL/GPL-2 dual, used under FTL; zlib; libpng;
expat; GLEW; fmt; loguru). Their notices are installed with VTK under `share/licenses/VTK` and
must be included in a binary distribution.
