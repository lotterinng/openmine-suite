# integrations/file_formats

Own implementations of exchange formats. A format is listed as supported only when an automated
import or round-trip test passes.

## DXF (ASCII, written as AutoCAD R2004 / AC1018)

- `openmine/formats/Dxf.h` — reader and writer; `openmine/formats/DxfProject.h` — import into a
  project as one undoable command, export of project geometry.
- Validation: `tests/unit/test_dxf.cpp` (round trip of every geometry type, OCS, units, blocks,
  MINSERT, BYBLOCK, 3DFACE merging, hatch edge paths, errors) and
  `tests/integration/test_dxf_ezdxf.py`, which checks our files with the independent ezdxf
  library (auditor clean; arcs, splines, hatches, dimensions and meshes reconstructed equal) and
  imports ezdxf-written drawings with entity types we never write.
- Not supported: binary DXF, DWG, 3DSOLID/REGION/BODY (ACIS data), images, tables, MLEADER; they
  are counted in the import report as skipped.
