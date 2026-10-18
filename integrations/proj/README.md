# integrations/proj

[PROJ](https://proj.org/) (MIT) for coordinate reference systems. API: `openmine/crs/Crs.h`.

- **CRS catalogue:** describe a definition (EPSG code, WKT or PROJ string) with its axes and area
  of use; search the PROJ database.
- **Project CRS:** `declareCrs` and `setProjectCrs` record the CRS of the stored coordinates. They
  never transform anything.
- **Explicit transformation:**
  - `candidateOperations` lists the available operations with their accuracy;
  - `transformObjects` writes transformed copies and a TransformationRecord, so originals are
    never altered;
  - `transformPoints` does the same for raw points.
- **Area of use:** `checkAreaOfUse` catches coordinates outside a CRS's area of use, for example
  swapped axes in south-orientated systems.
- **Local mine grids:** Helmert 2D/3D and affine fits to control pairs, defined as derived CRSs.
- **Units:** length parsing and conversion.

Validation: `tests/unit/test_crs.cpp`:

- South African Lo systems are described with their axes;
- transforms agree with an independent formula and round-trip;
- the area of use catches swapped axes;
- Helmert, affine and 3D fits recover their parameters;
- local grids and explicit object transformation in a project;
- unit parsing and conversion.
