// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Edit.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/gis/Gis.h"
#include "openmine/project/ProjectEditor.h"

#include <gtest/gtest.h>
#include <ogr_spatialref.h>

#include <filesystem>
#include <fstream>
#include <memory>

using namespace openmine;
using namespace openmine::gis;
namespace fs = std::filesystem;

namespace {
fs::path tempDir() {
    auto d = fs::temp_directory_path() / ("openmine_gis_" + Uuid::generate().toString());
    fs::create_directories(d);
    return d;
}
const char* kLo25 = "EPSG:2051";
} // namespace

TEST(Gis, DemExportImportRoundTrip) {
    const auto dir = tempDir();
    // Plane z = 1500 + 0.1x − 0.05y over 100 × 60 m at Lo25 coordinates.
    MeshData plane;
    const double x0 = 22000, y0 = 3180000;
    plane.vertices = {{x0, y0, 0}, {x0 + 100, y0, 0}, {x0 + 100, y0 + 60, 0}, {x0, y0 + 60, 0}};
    for (auto& v : plane.vertices) v.z = 1500 + 0.1 * (v.x - x0) - 0.05 * (v.y - y0);
    plane.triangles = {{0, 1, 2}, {0, 2, 3}};
    DemExportOptions eo;
    eo.cellSize = 2.0;
    eo.crsWkt = kLo25;
    const auto info = exportDem(plane, dir / "dem.tif", eo);
    EXPECT_EQ(info.width, 50);
    EXPECT_EQ(info.height, 30);
    EXPECT_EQ(info.crsAuthority, "EPSG:2051");
    EXPECT_EQ(info.driver, "GTiff");
    RasterInfo ri;
    const auto m = importDem(dir / "dem.tif", {}, &ri);
    EXPECT_EQ(m.vertices.size(), 50u * 30u);
    EXPECT_EQ(m.triangles.size(), 2u * 49u * 29u);
    for (const auto& v : m.vertices) EXPECT_NEAR(v.z, 1500 + 0.1 * (v.x - x0) - 0.05 * (v.y - y0), 1e-9);
    for (const auto& n : geometry::faceNormals(m)) EXPECT_GT(n.z, 0);
    // Decimation step and a vertical scale.
    DemImportOptions io;
    io.step = 5;
    io.zScale = 0.5;
    const auto coarse = importDem(dir / "dem.tif", io);
    EXPECT_EQ(coarse.vertices.size(), 10u * 6u);
    EXPECT_THROW(importDem(dir / "missing.tif"), GisError);
    // ESRI ASCII grid (written by copy from an in-memory raster), with its CRS in the .prj.
    DemExportOptions ao = eo;
    ao.driver = "AAIGrid";
    const auto ai = exportDem(plane, dir / "dem.asc", ao);
    EXPECT_EQ(ai.driver, "AAIGrid");
    EXPECT_EQ(ai.width, 50);
    EXPECT_EQ(ai.height, 30);
    EXPECT_TRUE(fs::exists(dir / "dem.prj"));
    RasterInfo ari;
    const auto am = importDem(dir / "dem.asc", {}, &ari);
    ASSERT_EQ(am.vertices.size(), m.vertices.size());
    for (std::size_t i = 0; i < am.vertices.size(); ++i) {
        EXPECT_NEAR(am.vertices[i].x, m.vertices[i].x, 1e-9);
        EXPECT_NEAR(am.vertices[i].y, m.vertices[i].y, 1e-9);
        EXPECT_NEAR(am.vertices[i].z, m.vertices[i].z, 1e-6); // decimal text
    }
    fs::remove_all(dir);
}

TEST(Gis, NoDataLeavesHoles) {
    const auto dir = tempDir();
    MeshData tri; // a triangle covering half of its bounding box → the other half is no-data
    tri.vertices = {{0, 0, 10}, {20, 0, 10}, {0, 20, 10}};
    tri.triangles = {{0, 1, 2}};
    DemExportOptions eo;
    eo.cellSize = 1.0;
    exportDem(tri, dir / "half.tif", eo);
    RasterInfo ri;
    const auto m = importDem(dir / "half.tif", {}, &ri);
    ASSERT_TRUE(ri.noData);
    EXPECT_EQ(*ri.noData, -9999.0);
    const double area = geometry::meshArea(m);
    EXPECT_GT(area, 150.0);
    EXPECT_LT(area, 200.0);
    fs::remove_all(dir);
}

TEST(Gis, VectorRoundTripWithAttributesAndCrs) {
    const auto dir = tempDir();
    GeometryObject road = cad::makePolyline({{22000, 3180000, 1500}, {22100, 3180050, 1495}, {22200, 3180020, 1490}});
    road.name = "Haul road";
    road.attributes = {{"surface", "gravel"}, {"width_m", "12.5"}};
    GeometryObject pit = cad::makePolyline({{22300, 3180000, 1480}, {22400, 3180000, 1480}, {22400, 3180100, 1480}, {22300, 3180100, 1480}}, true);
    pit.name = "Pit crest";
    GeometryObject bh;
    bh.type = GeometryType::Point;
    bh.vertices = {{22050, 3180010, 1501.25}};
    bh.name = "DDH001";
    for (const char* ext : {"geojson", "gpkg"}) {
        SCOPED_TRACE(ext);
        const auto f = dir / (std::string("out.") + ext);
        VectorExportOptions o;
        o.crsWkt = kLo25;
        GeometryObject mesh;
        mesh.type = GeometryType::Mesh;
        mesh.name = "Pit shell";
        const auto rep = exportVector({&road, &pit, &mesh, &bh}, f, o);
        EXPECT_EQ(rep.written, 3);
        ASSERT_EQ(rep.skipped.size(), 1u);
        EXPECT_EQ(rep.skipped[0].rfind("Pit shell (Mesh): ", 0), 0u) << rep.skipped[0];
        EXPECT_THROW(exportVector({&mesh}, dir / (std::string("nothing.") + ext), o), GisError);
        const auto info = vectorInfo(f);
        ASSERT_EQ(info.size(), 1u);
        EXPECT_EQ(info[0].featureCount, 3);
        const auto in = importVector(f);
        EXPECT_EQ(in.crsAuthority, "EPSG:2051");
        ASSERT_EQ(in.features.size(), 3u);
        EXPECT_EQ(in.features[0].geometry.vertices.size(), 3u);
        EXPECT_DOUBLE_EQ(in.features[0].geometry.vertices[1].z, 1495);
        EXPECT_EQ(in.features[0].attributes.at("surface"), "gravel");
        EXPECT_EQ(in.features[0].attributes.at("name"), "Haul road");
        EXPECT_TRUE(in.features[1].geometry.closed);
        EXPECT_EQ(in.features[1].geometry.vertices.size(), 4u);
        EXPECT_EQ(in.features[2].geometry.type, GeometryType::Point);
    }
    // Shapefiles take one geometry type.
    VectorExportOptions so;
    EXPECT_THROW(exportVector({&road, &bh}, dir / "mixed.shp", so), GisError);
    so.crsWkt = kLo25;
    exportVector({&road}, dir / "road.shp", so);
    {
        const auto shp = importVector(dir / "road.shp");
        // The .prj is ESRI WKT without an authority; PROJ cannot identify it as EPSG:2051 with
        // confidence (it is only a 25 % match), so no authority is claimed — but the definition
        // is the same south-orientated Transverse Mercator on 25° E.
        EXPECT_TRUE(shp.crsAuthority.empty() || shp.crsAuthority == "EPSG:2051") << shp.crsAuthority;
        OGRSpatialReference lo25, read;
        lo25.SetFromUserInput(kLo25);
        ASSERT_EQ(read.SetFromUserInput(shp.crsWkt.c_str()), OGRERR_NONE);
        const char* same[] = {"IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", "CRITERION=EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS", nullptr};
        EXPECT_TRUE(read.IsSame(&lo25, same) || (read.GetProjParm(SRS_PP_CENTRAL_MERIDIAN) == 25.0 && std::string(read.GetAttrValue("PROJECTION") ? read.GetAttrValue("PROJECTION") : "").find("South") != std::string::npos));
        ASSERT_EQ(shp.features.size(), 1u);
        EXPECT_EQ(shp.features[0].geometry.vertices.size(), 3u);
        EXPECT_DOUBLE_EQ(shp.features[0].geometry.vertices[2].z, 1490);
        EXPECT_EQ(shp.features[0].attributes.at("width_m"), "12.5");
    }
    // KML is geographic: features are written in WGS 84 (transformed by the driver) and come back
    // as longitude/latitude with heights — the importer reports the CRS, it does not transform.
    {
        VectorExportOptions ko;
        ko.crsWkt = kLo25;
        exportVector({&road, &bh}, dir / "out.kml", ko);
        const auto kml = importVector(dir / "out.kml");
        EXPECT_EQ(kml.crsAuthority, "EPSG:4326");
        ASSERT_EQ(kml.features.size(), 2u);
        OGRSpatialReference src, dst;
        src.SetFromUserInput(kLo25);
        dst.SetWellKnownGeogCS("WGS84");
        src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&src, &dst));
        ASSERT_TRUE(ct);
        for (std::size_t k = 0; k < road.vertices.size(); ++k) {
            double x = road.vertices[k].x, y = road.vertices[k].y;
            ASSERT_TRUE(ct->Transform(1, &x, &y));
            EXPECT_NEAR(kml.features[0].geometry.vertices[k].x, x, 1e-9);
            EXPECT_NEAR(kml.features[0].geometry.vertices[k].y, y, 1e-9);
            EXPECT_NEAR(kml.features[0].geometry.vertices[k].z, road.vertices[k].z, 1e-6);
        }
        EXPECT_EQ(kml.features[1].geometry.type, GeometryType::Point);
        EXPECT_EQ(kml.features[0].attributes.at("name"), "Haul road");
    }
    // Import into a project creates the attribute definitions; one undo step.
    auto pp = Project::createNew("GIS", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*pp);
    const Uuid layer = ed.addLayer("GIS", 0x00FF00);
    const auto data = importVector(dir / "out.gpkg");
    const auto ids = addVectorFeatures(*pp, data, layer);
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_EQ(pp->model().geometry.find(ids[0])->attributes.at("width_m"), "12.5");
    pp->commands().undo();
    EXPECT_FALSE(pp->model().geometry.find(ids[0]));
    fs::remove_all(dir);
}
