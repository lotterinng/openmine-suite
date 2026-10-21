# ADR 0003 — Build VTK against Qt 6 instead of using distro VTK

* Status: superseded by [ADR 0005](0005-own-qt6-vtk-adapter.md) (v0.2)

## Context
The specification requires Qt 6. Ubuntu 24.04's `libvtk9-qt-dev` (VTK 9.1) is linked against
Qt 5; CMake refuses to mix `Qt5::Core` and `Qt6::Core` in one target, and a single process cannot
load both.

## Options
1. Downgrade the application to Qt 5 on Linux — diverges from the Windows target, rejected.
2. Write a custom QOpenGLWidget ↔ VTK bridge without `GUISupportQt` — duplicates mature VTK code.
3. Build a minimal VTK 9.3.1 with `VTK_QT_VERSION=6` — chosen.

## Decision
Build VTK 9.3.1 from source with only the modules the viewport needs (see `DEVELOPMENT.md`).
On Windows, vcpkg's `vtk[qt]` already builds against Qt 6.

## Consequences
One-off ~45 min build on Linux dev machines/CI (cacheable). Same VTK major version on both
platforms.
