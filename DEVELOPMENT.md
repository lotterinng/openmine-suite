# Development guide

## Toolchain and dependencies

| Dependency | Tested | Required | CMake option | Notes |
|---|---|---|---|---|
| C++ compiler | GCC 13.3 (Linux); MSVC 2022 17.8+ (Windows, target) | yes | | C++20 |
| CMake / Ninja | 3.28 / 1.11 | yes | | presets need CMake ≥ 3.25 |
| Qt | 6.4.2 (Widgets, OpenGLWidgets, Svg, PrintSupport; Pdf/PdfWidgets optional) | app | `OPENMINE_BUILD_APP` | |
| VTK | 9.1 (distribution build) | app | | no Qt module needed (ADR 0005) |
| SQLite, Eigen, nlohmann_json, nanoflann, zlib, libxlsxwriter | 3.45, 3.4, 3.11, 1.5, 1.3, 1.1.5 | yes | | |
| Clipper2 | 1.4.0 | yes | | built from source on Ubuntu 24.04 |
| Open CASCADE | 7.6.3 | optional | `OPENMINE_WITH_OCCT` | solids, STEP/IGES |
| PDAL (+ E57 plugin) | 2.8.4 | optional | `OPENMINE_WITH_PDAL` | built from source (Ubuntu's 2.6 lacks E57) |
| Open3D | 0.18.0 | optional | `OPENMINE_WITH_OPEN3D` | official devel binary (cxx11 ABI) |
| PROJ | 9.4 | optional | `OPENMINE_WITH_PROJ` | |
| GDAL | 3.8 | optional | `OPENMINE_WITH_GDAL` | ≥ 3.4 |
| OR-Tools | 9.12 | optional | `OPENMINE_WITH_ORTOOLS` | official binary release |
| libpq | 16 | optional | `OPENMINE_WITH_POSTGRES` | tests need a PostgreSQL + PostGIS server |
| Python + pybind11 | 3.13 / 3.1 (≥ 3.10 / ≥ 2.13) | optional | `OPENMINE_BUILD_PYTHON` | NumPy, pytest |
| GoogleTest | 1.14 | tests | `OPENMINE_BUILD_TESTS` | |

A missing optional dependency is reported by CMake ("… will be reported as unavailable") and the
feature is disabled in the application with that reason; nothing else breaks.

## Linux (Ubuntu 24.04)

```bash
sudo apt-get install -y build-essential ninja-build cmake git pkg-config \
  qt6-base-dev qt6-svg-dev qt6-pdf-dev libqt6opengl6-dev libgl-dev libvtk9-dev \
  libsqlite3-dev libeigen3-dev nlohmann-json3-dev libnanoflann-dev zlib1g-dev libxlsxwriter-dev \
  libocct-foundation-dev libocct-modeling-data-dev libocct-modeling-algorithms-dev \
  libocct-data-exchange-dev libproj-dev libgdal-dev libpq-dev libgtest-dev \
  libgeotiff-dev liblaszip-dev libxerces-c-dev libcurl4-openssl-dev \
  python3-dev python3-pip xvfb doxygen postgresql-16 postgresql-16-postgis-3
python3 -m pip install --break-system-packages "pybind11>=2.13" numpy pytest \
  ezdxf trimesh omf mplstereonet pypdf pdfplumber openpyxl mkdocs mkdocs-material
```

Dependencies Ubuntu 24.04 does not package in the needed version (prefix `/opt`, adjust as you
like):

```bash
# Clipper2 1.4.0
git clone --depth 1 --branch Clipper2_1.4.0 https://github.com/AngusJohnson/Clipper2 clipper2-src
cmake -S clipper2-src/CPP -B clipper2-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/opt/clipper2 -DCLIPPER2_TESTS=OFF -DCLIPPER2_EXAMPLES=OFF -DCLIPPER2_UTILS=OFF
ninja -C clipper2-build install

# PDAL 2.8.4 with the E57 plugin
git clone --depth 1 --branch 2.8.4 https://github.com/PDAL/PDAL pdal-src
cmake -S pdal-src -B pdal-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/pdal \
  -DWITH_TESTS=OFF -DBUILD_PLUGIN_E57=ON -DWITH_COMPLETION=OFF
ninja -C pdal-build install          # ~40 min on 2 cores

# Open3D 0.18.0 (binary, needs libc++1 and libc++abi1 at run time)
sudo apt-get install -y libc++1 libc++abi1 libgomp1
curl -L https://github.com/isl-org/Open3D/releases/download/v0.18.0/open3d-devel-linux-x86_64-cxx11-abi-0.18.0.tar.xz | sudo tar -xJ -C /opt

# OR-Tools 9.12 (binary release for Ubuntu 24.04)
curl -L https://github.com/google/or-tools/releases/download/v9.12/or-tools_amd64_ubuntu-24.04_cpp_v9.12.4544.tar.gz \
  | sudo tar -xz -C /opt && sudo mv /opt/or-tools_* /opt/ortools
```

Build and test:

```bash
cmake --preset linux-debug \
  -DCMAKE_PREFIX_PATH="/opt/pdal;/opt/clipper2;/opt/ortools" \
  -DOpen3D_DIR=/opt/open3d-devel-linux-x86_64-cxx11-abi-0.18.0/lib/cmake/Open3D \
  -DOPENMINE_TEST_PG_DSN="host=127.0.0.1 port=54329 dbname=openmine_test user=postgres"
cmake --build --preset linux-debug
export LD_LIBRARY_PATH=/opt/pdal/lib:/opt/clipper2/lib:/opt/ortools/lib:/opt/open3d-devel-linux-x86_64-cxx11-abi-0.18.0/lib
ctest --preset linux-debug -LE gui                    # everything headless
xvfb-run -a -s "-screen 0 1920x1080x24" ctest --preset linux-debug -L gui   # GUI smoke test
./build/linux-debug/bin/openmine-desktop
```

Headless only (no Qt/VTK): `-DOPENMINE_BUILD_APP=OFF`. Benchmarks: `bin/openmine_bench`
(see `benchmarks/README.md`; use a Release build for meaningful numbers).

### PostgreSQL test server

The repository tests (`Fixture.*`, `test_repository_round_trip`) run when
`OPENMINE_TEST_PG_DSN` (CMake) / `OPENMINE_PG_DSN` (pytest) points at a server; otherwise they are
skipped with that reason. A throw-away server:

```bash
sudo -u postgres /usr/lib/postgresql/16/bin/initdb -D /var/lib/postgresql/openmine-test/data
sudo -u postgres /usr/lib/postgresql/16/bin/postgres -D /var/lib/postgresql/openmine-test/data \
  -k /tmp -p 54329 -h 127.0.0.1 &
sudo -u postgres psql -h 127.0.0.1 -p 54329 -c "CREATE DATABASE openmine_test"
sudo -u postgres psql -h 127.0.0.1 -p 54329 -d openmine_test -c "CREATE EXTENSION postgis"
```

## Windows 10/11 x64 (primary target)

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg; C:\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"
# from a "x64 Native Tools Command Prompt for VS 2022":
cmake --preset windows-msvc            # vcpkg.json: Qt, VTK, OCCT, PDAL, PROJ, GDAL, libpq, Eigen, …
cmake --build --preset windows-msvc
ctest --preset windows-msvc -LE gui
ctest --preset windows-msvc -L gui     # smoke test (needs a desktop session)
build\windows-msvc\bin\openmine-desktop.exe
```

vcpkg builds Qt, VTK, OCCT, GDAL and PDAL from source on the first run (several hours; use a
binary cache). Open3D and OR-Tools are not in vcpkg's manifest here: install their official
Windows binaries and pass `-DOpen3D_DIR=…` and `-Dortools_DIR=…`, or leave them out (the features
are then reported unavailable). **Status:** the Windows path is written but has not been executed
(see `IMPLEMENTATION_STATUS.md`).

## The GUI smoke test

`openmine-desktop --smoke-test <dir>` drives the real application through every workspace: it
generates data, draws with viewport clicks, edits with grips, imports and processes scans, builds
as-built surfaces, designs headings, schedules, optimises, plays 4D, runs Python and plugins,
saves, reopens and compacts — checking each result. It writes `<dir>/smoke.log` (ending with
"N passed, 0 failed") and screenshots; the `gui_smoke_test` ctest runs it under Xvfb.

## Generated documentation

| Reference | Regenerate | Checked by |
|---|---|---|
| `docs/console-commands.md` table | `cmake -DEXE=<build>/bin/openmine-desktop -DDOC=docs/console-commands.md -DOUT=/tmp/c.md -DUPDATE=ON -P tests/tools/check_command_reference.cmake` | `console_reference_up_to_date` |
| `docs/reference/project-schema.md` | `PYTHONPATH=<build>/python python3 docs/tools/generate_schema_reference.py` | `schema_reference_up_to_date` |
| `docs/reference/python-api.md` | `PYTHONPATH=<build>/python python3 docs/tools/generate_python_reference.py` | `python_reference_up_to_date` (also requires a docstring on every public member) |
| C++ API (Doxygen) | `doxygen Doxyfile` → `build/doxygen/html` | `doxygen_reference_builds` (no warnings) |
| Documentation site (MkDocs) | `python3 docs/tools/build_site.py [--serve]` → `build/docs-site/html` | `docs_site_builds` (strict: every link resolves) |
| Python guide examples | — | `test_guide.py` executes every `python` block of `docs/python.md` |

Run all of them with `ctest -L docs`.

## Version

The version is set once, in the top-level `project(OpenMineSuite VERSION x.y.z)`. It is generated
into `openmine/Version.h` (C++), the Python package (`openmine.__version__`) and the project file
metadata. Update `vcpkg.json` and the `Doxyfile` `PROJECT_NUMBER` with it
(`doxygen_reference_builds` fails when the Doxyfile differs).

## Repository map

```
core/                    objects, geometry, coordinates, commands, project/editor, persistence, assets, jobs
modules/cad              curve algorithms, editing, snaps, grips, annotation
modules/mine_design      headings, layouts, rules, stopes, revisions, project-level edits
modules/survey           point clouds, octree, processing, registration, meshing, as-built, conformance
modules/scheduling       calendars, CPM, levelling, cycles, periods, scenarios
modules/planning         4D
modules/reporting        PDF writer, HTML reports, Gantt PDF, XLSX
modules/geology          drillholes, compositing, block models, estimation
modules/geotechnical     stereonets, rock-mass classification
integrations/*           opencascade, pdal, open3d, proj, gdal, ortools, postgres, file_formats, omf
sdk/                     plugin C ABI header and host; CMake helpers for plugins
plugins/samples          native (station import) and Python (levels) sample plugins
scripting/python         pybind11 bindings, openmine package, embedded console, pytest suite
apps/desktop             Qt application, smoke test
tests/                   GoogleTest suites, integration validations, tools
benchmarks/              engine benchmarks and their CI budgets
docs/                    guides, architecture, ADRs, references, formats, licensing
samples/                 CC0 sample project script
```

## Conventions

* C++20, `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor` clean (CI builds with
  `-DOPENMINE_WARNINGS_AS_ERRORS=ON`); `.clang-format` style.
* No third-party types in public `core`/`modules` headers; integrations inject engines.
* Every model change goes through `ProjectEditor` and is one undo step; long work takes a
  `jobs::Progress&` and runs off the UI thread.
* A feature is "done" when it has a test and a row in `IMPLEMENTATION_STATUS.md`.
