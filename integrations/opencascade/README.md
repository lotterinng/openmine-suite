# integrations/opencascade

[Open CASCADE Technology](https://dev.opencascade.org/) (LGPL-2.1 with exception) for exact B-rep
solids. APIs:

- `openmine/occt/Solid.h` — kernel operations:
  - primitives; faces from curves; extrude, revolve, sweep (with corner transitions) and loft;
  - booleans; fillet and offset; mesh ↔ solid;
  - tessellation, sections, distances, point classification;
  - validity analysis; STEP/IGES/BREP exchange.
- `openmine/occt/SolidObjects.h` — the project layer:
  - Solid geometry objects whose B-rep is an immutable asset, so a change writes a new asset;
  - booleans of objects; `transformSolidObject`, which transforms exactly in the kernel and
    re-tessellates;
  - mesh to solid; development excavation solids; sections; STEP/IGES import and export.
- `openmine/occt/SweptSolid.h` — sweeping a development's profile along its centreline.

Every edit is one undo step.

Validation (`tests/unit/test_occt.cpp`):

- primitives have exact properties;
- sweeps match the core geometry (area × slope length);
- arc and corner transitions;
- booleans, fillet and offset;
- mesh conversion, tessellation and sections;
- transforms, including in place, copy, scale, mirror and undo;
- exchange formats;
- project persistence and undo of solid objects.
