# ADR 0005 — Own QOpenGLWidget adapter for VTK instead of VTK's Qt module

* Status: accepted (v0.2); supersedes [ADR 0003](0003-build-vtk-against-qt6.md)

## Context
ADR 0003 built VTK 9.3.1 from source with `VTK_QT_VERSION=6` because distribution VTK packages
(Ubuntu 24.04: VTK 9.1) build the `GUISupportQt` module against Qt 5, which cannot be mixed with
Qt 6 in one process. Experience since v0.1:

* the custom VTK build had to be repeated on every development and CI machine and for every
  additional VTK module the viewport needed (labels, extraction, images), and its cache was
  invalidated by each change of module list;
* `GUISupportQt` (`QVTKOpenGLNativeWidget`) is the only part of VTK that depends on Qt; the
  rendering, filtering and interaction modules do not;
* the application needs control over when rendering happens anyway: several viewports in one
  window (1/2/4-view layouts), point-cloud nodes arriving from background threads, and smoke-test
  screenshots must all render through Qt's paint cycle.

## Options
1. Keep building VTK against Qt 6 (ADR 0003) — works, but a heavy, fragile build dependency.
2. Use distribution VTK without its Qt module and host the VTK render window in our own
   `QOpenGLWidget` — about 300 lines, no Qt dependency inside VTK.
3. Drop VTK for a custom OpenGL renderer — loses VTK's picking, widgets, annotation and filters.

## Decision
Option 2. `apps/desktop/vtk/QtVtkWidget` hosts a `vtkGenericOpenGLRenderWindow` in a
`QOpenGLWidget`:

* the OpenGL context is Qt's (VTK never creates, makes current or swaps one); the surface format
  requests OpenGL 3.2 core with a 24-bit depth and 8-bit stencil buffer;
* VTK renders into its own offscreen framebuffers and the result is blitted into the framebuffer
  Qt binds for `paintGL()`;
* mouse, wheel and key events are forwarded to a `vtkGenericRenderWindowInteractor`, so standard
  and custom interactor styles work unchanged;
* rendering is always requested through Qt (`update()`), never by VTK directly.

`find_package(VTK)` asks only for non-Qt modules (`CommonCore`, `CommonDataModel`,
`CommonTransforms`, `FiltersCore`, `FiltersGeneral`, `FiltersSources`, `InteractionStyle`,
`InteractionWidgets`, `RenderingAnnotation`, `RenderingCore`, `RenderingFreeType`,
`RenderingOpenGL2`, `RenderingLabel`, `IOImage`), so any VTK ≥ 9.1 works: Ubuntu's `libvtk9-dev`
on Linux, vcpkg `vtk` (without the `qt` feature) on Windows.

## Consequences
* No VTK source build on Linux; CI installs packages only.
* VTK's own Qt widgets (e.g. `QVTKOpenGLStereoWidget`) are not available; none are used.
* The adapter is covered by the GUI smoke test under Xvfb with Mesa llvmpipe: every view of the
  1-, 2- and 4-view layouts is grabbed and must show drawn content (≥ 2 % of pixels differing from
  the background), and viewport picking selects the entity under the cursor.
