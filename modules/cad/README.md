# modules/cad

CAD algorithms as pure functions on `GeometryObject`s; callers (desktop tools, console, Python)
apply results through `ProjectEditor` / `mine_design::GeometryEdits` so every edit is validated,
undoable and audited.

| Area | Header | Notes |
|---|---|---|
| Primitives | `Primitives.h` | curves decomposed into line and plan-arc primitives (bulged polylines, arcs, circles, splines); plan intersections with elevation interpolated on each curve |
| Construction and editing | `Edit.h` | lines, polylines, rectangles, polygons, circles, arcs (3-point), splines, text, hatches, dimensions; transforms (uniform for arcs); offset (miter/round, collapsed segments removed); trim, extend, split, join, explode; fillet and chamfer (two curves or a polyline corner); grips |
| Snapping | `Snap.h` | endpoint, midpoint, centre, intersection, perpendicular, tangent, nearest, node, grid — view-independent, with priorities and an aperture |
| Annotation | `Annotation.h` | display geometry of dimensions (7 kinds) and hatch fills (SOLID, ANSI31, LINES, CROSS, DOTS) |

Tests: `tests/unit/test_cad.cpp` (`Cad*` suites), `tests/unit/test_geometry.cpp` (`Curves.*`).
