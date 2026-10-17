# Development guide

## Requirements

| Tool | Version used/tested | Notes |
|---|---|---|
| C++ compiler | GCC 13.3 (Linux); MSVC 2022 17.8+ (Windows, target) | C++20 |
| CMake | 3.28 (min 3.25) | presets |
| Ninja | 1.11 | |
| Qt | 6.4.2 (Linux distro) / 6.x via vcpkg | Widgets, OpenGLWidgets |
| VTK | 9.3.1 **built against Qt 6** | GUISupportQt, RenderingOpenGL2, InteractionStyle/Widgets, RenderingAnnotation/FreeType |
| Open CASCADE | 7.6.3 | ModelingAlgorithms/Data, Foundation |
| SQLite | 3.45 | |
| GoogleTest | 1.14 | |
| Python | 3.10+ (tested 3.13) | pybind11 ≥ 2.13, pytest |

## Windows 10/11 x64 (primary target)

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg; C:\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"
# from a "x64 Native Tools Command Prompt for VS 2022":
cmake --preset windows-msvc            # vcpkg installs qtbase, vtk[qt], opencascade, sqlite3, gtest, pybind11
cmake --build --preset windows-msvc
ctest --preset windows-msvc -LE gui     # headless tests
build\windows-msvc\apps\desktop\openmine-desktop.exe
```

The first vcpkg build of Qt + VTK + OCCT takes a long time (1–3 h); use a vcpkg binary cache.
**Status:** the Windows path is written but has not yet been executed in CI (no Windows host was
available when v0.1 was produced). See `IMPLEMENTATION_STATUS.md`.

### Windows GUI smoke test

```powershell
build\windows-msvc\apps\desktop\openmine-desktop.exe --smoke-test C:\temp\openmine-smoke
type C:\temp\openmine-smoke\smoke.log   # ends with "SMOKE TEST PASSED"; screenshots *.png alongside
```

Manual check (2 minutes): New project → CAD › Polyline, click 3 points in the viewport,
right-click → select it, rename in Properties → Mine Design › Create Heading → Linked Activity
(5 m/d) → Scheduler shows the bar → File › Save, Close, Open → the heading, activity and link are
still there.

## Linux (development / CI)

```bash
sudo apt-get install ninja-build qt6-base-dev libqt6opengl6-dev libsqlite3-dev libgtest-dev \
     libocct-modeling-algorithms-dev libocct-modeling-data-dev libocct-foundation-dev \
     python3-dev xvfb
python3 -m pip install "pybind11>=2.13" pytest

# Ubuntu's libvtk9-qt-dev is linked against Qt 5 and cannot be mixed with Qt 6.
# Build VTK against Qt 6 once (~45 min on 2 cores):
git clone --depth 1 --branch v9.3.1 https://github.com/Kitware/VTK.git vtk-src
cmake -S vtk-src -B vtk-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/vtk-qt6 \
  -DVTK_BUILD_TESTING=OFF -DVTK_WRAP_PYTHON=OFF -DVTK_QT_VERSION=6 \
  -DVTK_GROUP_ENABLE_Imaging=DONT_WANT -DVTK_GROUP_ENABLE_MPI=DONT_WANT -DVTK_GROUP_ENABLE_Qt=DONT_WANT \
  -DVTK_GROUP_ENABLE_Rendering=DONT_WANT -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT \
  -DVTK_GROUP_ENABLE_Views=DONT_WANT -DVTK_GROUP_ENABLE_Web=DONT_WANT \
  -DVTK_MODULE_ENABLE_VTK_GUISupportQt=YES -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES \
  -DVTK_MODULE_ENABLE_VTK_InteractionStyle=YES -DVTK_MODULE_ENABLE_VTK_InteractionWidgets=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingAnnotation=YES -DVTK_MODULE_ENABLE_VTK_RenderingFreeType=YES \
  -DVTK_MODULE_ENABLE_VTK_FiltersSources=YES -DVTK_MODULE_ENABLE_VTK_RenderingUI=YES
ninja -C vtk-build install

cmake --preset linux-debug -DVTK_DIR=/opt/vtk-qt6/lib/cmake/vtk-9.3
cmake --build --preset linux-debug
ctest --preset linux-debug -LE gui                              # headless tests
LD_LIBRARY_PATH=/opt/vtk-qt6/lib xvfb-run -a -s "-screen 0 1920x1080x24" \
  ctest --preset linux-debug -L gui                             # GUI smoke test
```

Headless only (no Qt/VTK needed): `-DOPENMINE_BUILD_APP=OFF`.

## Python package

```bash
export PYTHONPATH=build/linux-debug/python
python3 -c "from openmine import Project; p = Project.new('Demo', start='2026-11-02'); print(p.name)"
pytest scripting/python/tests
```

## Repository map

```
core/                 objects, geometry, coordinates, commands, project, persistence, events
modules/scheduling    calendars + CPM
modules/mine_design   development headings, design ↔ schedule impact
integrations/opencascade  OCCT solid adapter
apps/desktop          Qt application (ribbon, viewport, explorer, properties, console, scheduler)
scripting/python      pybind11 core + openmine facade + pytest
tests/unit            GoogleTest suites
docs/                 architecture, ADRs, licensing
```
