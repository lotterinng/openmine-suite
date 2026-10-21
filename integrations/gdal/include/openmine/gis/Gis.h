#pragma once
// SPDX-License-Identifier: Apache-2.0
// GDAL/OGR (MIT) adapter: elevation rasters (GeoTIFF and every GDAL raster driver) to and from
// surfaces, and GIS vector layers (Shapefile, GeoPackage, GeoJSON, KML, DXF via OGR, …) to and
// from geometry objects with attributes. No GDAL type appears in this header.
//
// Coordinates are never reprojected implicitly: the dataset's declared CRS is reported and the
// caller decides (crs::transformObjects) what to do with it.

#include "openmine/objects/CadEntities.h"
#include "openmine/project/Project.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openmine::gis {

class GisError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string gdalVersion();

struct RasterInfo {
    int width = 0, height = 0, bands = 0;
    std::array<double, 6> geoTransform{}; ///< GDAL convention (x0, dx, rx, y0, ry, dy)
    std::optional<double> noData;
    std::string crsWkt, crsAuthority;     ///< as declared by the file
    std::string driver;
};
RasterInfo rasterInfo(const std::filesystem::path& file);

struct DemImportOptions {
    int band = 1;
    int step = 1;              ///< take every n-th cell (decimation)
    double zScale = 1.0;       ///< e.g. 0.3048 for feet
    bool pixelIsPoint = false; ///< sample positions at pixel corners instead of centres
};
/// Elevation raster → triangulated surface (two triangles per cell of four valid samples; no-data
/// cells leave holes).
MeshData importDem(const std::filesystem::path& file, const DemImportOptions& options = {}, RasterInfo* info = nullptr);

struct DemExportOptions {
    double cellSize = 1.0;
    double noData = -9999.0;
    std::string crsWkt;        ///< written as the raster's spatial reference when given
    std::string driver = "GTiff";
};
/// Surface → elevation raster by vertical sampling of the topmost surface at cell centres.
RasterInfo exportDem(const MeshData& surface, const std::filesystem::path& file, const DemExportOptions& options);

struct VectorLayerInfo {
    std::string name;
    std::string geometryType;
    long long featureCount = 0;
    std::vector<std::pair<std::string, std::string>> fields; ///< name, type
    std::string crsWkt, crsAuthority;
};
std::vector<VectorLayerInfo> vectorInfo(const std::filesystem::path& file);

struct VectorFeature {
    GeometryObject geometry;   ///< Point, Polyline (open or closed rings), or Mesh for TINs
    AttributeMap attributes;   ///< field values as text
};
struct VectorImport {
    std::vector<VectorFeature> features;
    std::vector<std::pair<std::string, std::string>> fields; ///< key, type ("Text", "Real", "Integer", "Date")
    std::string crsWkt, crsAuthority;
    std::size_t skipped = 0;   ///< features without usable geometry
    std::vector<std::string> warnings;
};
/// Reads one layer (by name; empty = first layer). Polygons become closed polylines (one per
/// ring); multi-geometries are split; 2D geometries get z = `defaultZ`.
VectorImport importVector(const std::filesystem::path& file, const std::string& layer = {}, double defaultZ = 0.0);

/// Adds imported features to a project layer, creating attribute definitions for unknown fields
/// (keys sanitised) — one undo step. Returns the new object ids.
std::vector<Uuid> addVectorFeatures(Project& project, const VectorImport& data, const Uuid& layerId);

struct VectorExportOptions {
    std::string driver;        ///< empty = from extension (.shp ESRI Shapefile, .gpkg GPKG, .geojson GeoJSON, .kml KML)
    std::string layerName = "openmine";
    std::string crsWkt;
};
struct VectorExportReport {
    int written = 0;                  ///< features written
    std::vector<std::string> skipped; ///< "<name> (<type>): <reason>" for every object not written
};
/// Writes points/curves (as line strings, closed curves as polygons when `closedAsPolygons`)
/// with their attributes. Curves with arcs are densified to 1 mm. Objects without a vector-feature
/// form (meshes, solids, text, dimensions, hatches, empty points) are not written and are listed in
/// the report — never dropped silently. Throws GisError when nothing can be written.
VectorExportReport exportVector(const std::vector<const GeometryObject*>& objects, const std::filesystem::path& file,
                                const VectorExportOptions& options, bool closedAsPolygons = true);

} // namespace openmine::gis
