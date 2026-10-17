# modules/cad — planned (Phase 2)

Today, CAD entities (layers, points, lines, polylines), validation and undoable editing live in
`core` (`ProjectEditor`) and the desktop tools. This module will hold advanced 2D/3D editing
algorithms (offset, trim, extend, fillet, arcs/splines, snapping engine) on top of the OCCT adapter.
