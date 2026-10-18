# integrations/vtk

VTK (BSD-3-Clause) rendering lives with the desktop application, which is its only user:

- `apps/desktop/vtk/QtVtkWidget` — a QOpenGLWidget adapter for `vtkGenericOpenGLRenderWindow`.
  VTK's own Qt module is not used, because distributions often build it against Qt 5.
- `apps/desktop/viewport/SceneBuilder`, `SceneCache` — project entities to VTK poly data, in
  render-local coordinates (a render origin keeps mine coordinates precise in float GPU buffers).
- `apps/desktop/viewport/PointCloudLod` — out-of-core octree streaming with a memory budget,
  screen-space-error node selection and a per-view point budget.
- `apps/desktop/viewport/Viewport3D` — views (3D, plan, section, elevation), picking, section
  clipping, 4D display and overlays.

It is validated by the headless GUI test (`gui_smoke_test`), which checks that rendered views show
the scene.
