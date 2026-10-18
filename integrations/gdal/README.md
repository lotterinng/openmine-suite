# integrations/gdal

[GDAL/OGR](https://gdal.org/) (MIT) for GIS exchange. API: `openmine/gis/Gis.h`.

- **Vector** (Shapefile, GeoPackage, GeoJSON, KML and any OGR driver for reading):
  - `importVector` / `addVectorFeatures`: polygons become closed polylines, one per ring;
    multi-geometries are split; 2D features get a chosen elevation; attribute definitions are
    created for unknown fields.
  - `exportVector`: points and curves with their attributes; arcs are densified to 1 mm.
- **Rasters:** `importDem` turns an elevation raster into a triangulated surface (no-data cells
  leave holes); `exportDem` samples the topmost surface at cell centres.
- **CRS:** the declared CRS of a file is reported. It is never used to move coordinates silently:
  a mismatch with the project CRS is reported, and transformation is an explicit operation
  (`integrations/proj`).

Validation: `tests/unit/test_gis.cpp` covers a DEM export/import round trip, no-data holes, and a
vector round trip with attributes and CRS.
