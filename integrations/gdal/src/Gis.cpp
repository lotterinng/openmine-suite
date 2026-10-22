// SPDX-License-Identifier: Apache-2.0
#include "openmine/gis/Gis.h"

#include "openmine/commands/CommandStack.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/objects/AttributeSchema.h"
#include "openmine/project/ProjectEditor.h"

#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <cstdio>

namespace openmine::gis {

namespace fs = std::filesystem;

namespace {
void init() {
    static std::once_flag once;
    std::call_once(once, [] {
        GDALAllRegister();
        CPLSetErrorHandler(CPLQuietErrorHandler);
    });
}

struct DsDeleter {
    void operator()(GDALDataset* d) const { GDALClose(d); }
};
using DsPtr = std::unique_ptr<GDALDataset, DsDeleter>;

DsPtr open(const fs::path& f, unsigned flags) {
    init();
    if (!fs::exists(f)) throw GisError("File not found: " + f.string());
    if (flags & GDAL_OF_RASTER) {
        // ESRI ASCII grids are read as Float32 unless asked: keep the written decimals.
        static const char* const aai[] = {"AAIGrid", nullptr};
        static const char* const f64[] = {"DATATYPE=Float64", nullptr};
        if (DsPtr g{static_cast<GDALDataset*>(GDALOpenEx(f.string().c_str(), flags, aai, f64, nullptr))}) return g;
    }
    DsPtr ds(static_cast<GDALDataset*>(GDALOpenEx(f.string().c_str(), flags | GDAL_OF_VERBOSE_ERROR, nullptr, nullptr, nullptr)));
    if (!ds) throw GisError("GDAL cannot open " + f.filename().string() + ": " + CPLGetLastErrorMsg());
    return ds;
}

void crsOf(const OGRSpatialReference* sr, std::string& wkt, std::string& auth) {
    if (!sr) return;
    char* w = nullptr;
    const char* opts[] = {"FORMAT=WKT2_2019", nullptr};
    if (sr->exportToWkt(&w, opts) == OGRERR_NONE && w) wkt = w;
    CPLFree(w);
    const char* an = sr->GetAuthorityName(nullptr);
    const char* ac = sr->GetAuthorityCode(nullptr);
    if (an && ac) {
        auth = std::string(an) + ":" + ac;
        return;
    }
    // Formats such as Shapefile (.prj in ESRI WKT) carry no authority: identify it from the PROJ
    // database, accepting only an equivalent definition (confidence ≥ 90 — same CRS, possibly
    // different names).
    int n = 0;
    int* confidence = nullptr;
    OGRSpatialReferenceH* matches = sr->FindMatches(nullptr, &n, &confidence);
    if (matches && n > 0 && confidence && confidence[0] >= 90) {
        const auto* m = OGRSpatialReference::FromHandle(matches[0]);
        const char* man = m->GetAuthorityName(nullptr);
        const char* mac = m->GetAuthorityCode(nullptr);
        if (man && mac) auth = std::string(man) + ":" + mac;
    }
    if (matches) OSRFreeSRSArray(matches);
    CPLFree(confidence);
}

std::string lowerExt(const fs::path& f) {
    std::string e = f.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string sanitizeKey(std::string k) {
    for (auto& c : k)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    if (k.empty() || std::isdigit(static_cast<unsigned char>(k[0]))) k = "f_" + k;
    return k;
}
} // namespace

std::string gdalVersion() { return GDALVersionInfo("RELEASE_NAME"); }

RasterInfo rasterInfo(const fs::path& file) {
    auto ds = open(file, GDAL_OF_RASTER);
    RasterInfo i;
    i.width = ds->GetRasterXSize();
    i.height = ds->GetRasterYSize();
    i.bands = ds->GetRasterCount();
    double gt[6];
    if (ds->GetGeoTransform(gt) != CE_None) throw GisError(file.filename().string() + " has no georeferencing (geotransform)");
    std::copy(gt, gt + 6, i.geoTransform.begin());
    if (i.bands > 0) {
        int has = 0;
        const double nd = ds->GetRasterBand(1)->GetNoDataValue(&has);
        if (has) i.noData = nd;
    }
    crsOf(ds->GetSpatialRef(), i.crsWkt, i.crsAuthority);
    i.driver = ds->GetDriver() ? ds->GetDriver()->GetDescription() : "";
    return i;
}

MeshData importDem(const fs::path& file, const DemImportOptions& o, RasterInfo* infoOut) {
    const auto info = rasterInfo(file);
    if (infoOut) *infoOut = info;
    if (o.band < 1 || o.band > info.bands) throw GisError("Raster band " + std::to_string(o.band) + " does not exist");
    if (o.step < 1) throw GisError("Step must be at least 1");
    auto ds = open(file, GDAL_OF_RASTER);
    GDALRasterBand* band = ds->GetRasterBand(o.band);
    int hasNd = 0;
    const double nd = band->GetNoDataValue(&hasNd);
    const int W = info.width, H = info.height, step = o.step;
    const int nx = (W - 1) / step + 1, ny = (H - 1) / step + 1;
    std::vector<double> row(static_cast<std::size_t>(W));
    MeshData m;
    std::vector<std::uint32_t> id(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0xFFFFFFFFu);
    const auto& gt = info.geoTransform;
    const double off = o.pixelIsPoint ? 0.0 : 0.5;
    for (int jj = 0; jj < ny; ++jj) {
        const int j = jj * step;
        if (band->RasterIO(GF_Read, 0, j, W, 1, row.data(), W, 1, GDT_Float64, 0, 0) != CE_None) throw GisError("Cannot read raster row " + std::to_string(j));
        for (int ii = 0; ii < nx; ++ii) {
            const int i = ii * step;
            const double z = row[static_cast<std::size_t>(i)];
            if (!std::isfinite(z) || (hasNd && z == nd)) continue;
            const double px = i + off, py = j + off;
            id[static_cast<std::size_t>(jj) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(ii)] = static_cast<std::uint32_t>(m.vertices.size());
            m.vertices.push_back({gt[0] + px * gt[1] + py * gt[2], gt[3] + px * gt[4] + py * gt[5], z * o.zScale});
        }
    }
    const bool flip = gt[1] * gt[5] - gt[2] * gt[4] > 0; // north-up rasters have dy < 0 → keep counter-clockwise in plan
    for (int jj = 0; jj + 1 < ny; ++jj)
        for (int ii = 0; ii + 1 < nx; ++ii) {
            auto at = [&](int a, int b) { return id[static_cast<std::size_t>(b) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(a)]; };
            const auto a = at(ii, jj), b = at(ii + 1, jj), c = at(ii + 1, jj + 1), d = at(ii, jj + 1);
            constexpr auto none = 0xFFFFFFFFu;
            auto tri = [&](std::uint32_t p, std::uint32_t q, std::uint32_t r) {
                if (p == none || q == none || r == none) return;
                if (flip) m.triangles.push_back({p, r, q});
                else m.triangles.push_back({p, q, r});
            };
            if (a != none && b != none && c != none && d != none) {
                tri(a, d, c);
                tri(a, c, b);
            } else { // partial cell: keep the triangle that is complete
                tri(a, d, c);
                tri(a, c, b);
                if (a == none) tri(b, d, c);
                if (c == none) tri(a, d, b);
            }
        }
    if (m.triangles.empty()) throw GisError("The raster has no valid elevation cells");
    // Ensure upward normals.
    const auto fn = geometry::faceNormals(m);
    if (!fn.empty() && fn.front().z < 0)
        for (auto& t : m.triangles) std::swap(t[1], t[2]);
    // Remove unreferenced vertices left by partial cells.
    std::vector<std::uint32_t> remap(m.vertices.size(), 0xFFFFFFFFu);
    MeshData out;
    for (auto t : m.triangles) {
        for (auto& v : t) {
            if (remap[v] == 0xFFFFFFFFu) {
                remap[v] = static_cast<std::uint32_t>(out.vertices.size());
                out.vertices.push_back(m.vertices[v]);
            }
            v = remap[v];
        }
        out.triangles.push_back(t);
    }
    return out;
}

RasterInfo exportDem(const MeshData& s, const fs::path& file, const DemExportOptions& o) {
    init();
    if (s.triangles.empty()) throw GisError("The surface has no triangles");
    if (!(o.cellSize > 0)) throw GisError("Cell size must be positive");
    BoundingBox b = BoundingBox::empty();
    for (const auto& v : s.vertices) b.expand(v);
    const int W = std::max(1, static_cast<int>(std::ceil((b.max.x - b.min.x) / o.cellSize)));
    const int H = std::max(1, static_cast<int>(std::ceil((b.max.y - b.min.y) / o.cellSize)));
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName(o.driver.c_str());
    if (!drv) throw GisError("GDAL driver " + o.driver + " is not available");
    // Drivers such as AAIGrid (ESRI ASCII grid) only copy a complete dataset: build it in memory first.
    const bool direct = drv->GetMetadataItem(GDAL_DCAP_CREATE) != nullptr;
    if (!direct && !drv->GetMetadataItem(GDAL_DCAP_CREATECOPY)) throw GisError("GDAL driver " + o.driver + " cannot write rasters");
    GDALDriver* mem = direct ? drv : GetGDALDriverManager()->GetDriverByName("MEM");
    if (!mem) throw GisError("GDAL MEM driver is not available");
    if (fs::exists(file)) fs::remove(file);
    DsPtr ds(mem->Create(direct ? file.string().c_str() : "", W, H, 1, GDT_Float64, nullptr));
    if (!ds) throw GisError("Cannot create " + file.string() + ": " + CPLGetLastErrorMsg());
    double gt[6] = {b.min.x, o.cellSize, 0, b.min.y + H * o.cellSize, 0, -o.cellSize};
    ds->SetGeoTransform(gt);
    if (!o.crsWkt.empty()) {
        OGRSpatialReference sr;
        if (sr.SetFromUserInput(o.crsWkt.c_str()) != OGRERR_NONE) throw GisError("Invalid CRS for the raster");
        ds->SetSpatialRef(&sr);
    }
    GDALRasterBand* band = ds->GetRasterBand(1);
    band->SetNoDataValue(o.noData);
    const geometry::MeshBvh bvh(s);
    const double top = b.max.z + 1;
    std::vector<double> row(static_cast<std::size_t>(W));
    for (int j = 0; j < H; ++j) {
        for (int i = 0; i < W; ++i) {
            const double x = gt[0] + (i + 0.5) * o.cellSize, y = gt[3] - (j + 0.5) * o.cellSize;
            const auto h = bvh.raycast({x, y, top}, {0, 0, -1});
            row[static_cast<std::size_t>(i)] = h ? h->point.z : o.noData;
        }
        if (band->RasterIO(GF_Write, 0, j, W, 1, row.data(), W, 1, GDT_Float64, 0, 0) != CE_None) throw GisError("Cannot write raster row");
    }
    if (!direct) {
        DsPtr copy(drv->CreateCopy(file.string().c_str(), ds.get(), FALSE, nullptr, nullptr, nullptr));
        if (!copy) throw GisError("Cannot write " + file.string() + ": " + CPLGetLastErrorMsg());
    }
    ds.reset();
    return rasterInfo(file);
}

std::vector<VectorLayerInfo> vectorInfo(const fs::path& file) {
    auto ds = open(file, GDAL_OF_VECTOR);
    std::vector<VectorLayerInfo> out;
    for (int k = 0; k < ds->GetLayerCount(); ++k) {
        OGRLayer* l = ds->GetLayer(k);
        VectorLayerInfo i;
        i.name = l->GetName();
        i.geometryType = OGRGeometryTypeToName(l->GetGeomType());
        i.featureCount = l->GetFeatureCount(true);
        OGRFeatureDefn* d = l->GetLayerDefn();
        for (int f = 0; f < d->GetFieldCount(); ++f)
            i.fields.push_back({d->GetFieldDefn(f)->GetNameRef(), OGRFieldDefn::GetFieldTypeName(d->GetFieldDefn(f)->GetType())});
        crsOf(l->GetSpatialRef(), i.crsWkt, i.crsAuthority);
        out.push_back(std::move(i));
    }
    return out;
}

namespace {
void collect(const OGRGeometry* g, double z0, std::vector<GeometryObject>& out, std::vector<std::string>& warnings) {
    if (!g) return;
    const auto t = wkbFlatten(g->getGeometryType());
    const bool hasZ = g->Is3D();
    auto pt = [&](double x, double y, double z) { return Vec3{x, y, hasZ ? z : z0}; };
    switch (t) {
    case wkbPoint: {
        const auto* p = g->toPoint();
        GeometryObject o;
        o.type = GeometryType::Point;
        o.vertices = {pt(p->getX(), p->getY(), p->getZ())};
        out.push_back(o);
        break;
    }
    case wkbLineString:
    case wkbLinearRing: {
        const auto* l = g->toSimpleCurve();
        GeometryObject o;
        o.type = GeometryType::Polyline;
        for (int i = 0; i < l->getNumPoints(); ++i) o.vertices.push_back(pt(l->getX(i), l->getY(i), l->getZ(i)));
        if (o.vertices.size() > 2 && o.vertices.front() == o.vertices.back()) {
            o.vertices.pop_back();
            o.closed = true;
        }
        if (o.vertices.size() >= 2) out.push_back(o);
        break;
    }
    case wkbPolygon: {
        const auto* p = g->toPolygon();
        collect(p->getExteriorRing(), z0, out, warnings);
        for (int i = 0; i < p->getNumInteriorRings(); ++i) collect(p->getInteriorRing(i), z0, out, warnings);
        break;
    }
    case wkbMultiPoint:
    case wkbMultiLineString:
    case wkbMultiPolygon:
    case wkbGeometryCollection: {
        const auto* c = g->toGeometryCollection();
        for (int i = 0; i < c->getNumGeometries(); ++i) collect(c->getGeometryRef(i), z0, out, warnings);
        break;
    }
    case wkbTIN:
    case wkbPolyhedralSurface: {
        const auto* ps = g->toPolyhedralSurface();
        auto m = std::make_shared<MeshData>();
        std::map<std::array<double, 3>, std::uint32_t> index;
        for (int i = 0; i < ps->getNumGeometries(); ++i) {
            const OGRPolygon* poly = ps->getGeometryRef(i); // the parts of a TIN / polyhedral surface are polygons
            const auto* r = poly->getExteriorRing();
            std::vector<std::uint32_t> ids;
            for (int k = 0; k + 1 < r->getNumPoints(); ++k) {
                const Vec3 v = pt(r->getX(k), r->getY(k), r->getZ(k));
                auto [it, ins] = index.emplace(std::array<double, 3>{v.x, v.y, v.z}, static_cast<std::uint32_t>(m->vertices.size()));
                if (ins) m->vertices.push_back(v);
                ids.push_back(it->second);
            }
            for (std::size_t k = 1; k + 1 < ids.size(); ++k) m->triangles.push_back({ids[0], ids[k], ids[k + 1]});
        }
        GeometryObject o;
        o.type = GeometryType::Mesh;
        o.mesh = m;
        if (!m->triangles.empty()) out.push_back(o);
        break;
    }
    default: {
        // Curved geometries (arcs, compound curves): linearise.
        std::unique_ptr<OGRGeometry> lin(g->getLinearGeometry(0.0, nullptr));
        if (lin && wkbFlatten(lin->getGeometryType()) != t) collect(lin.get(), z0, out, warnings);
        else warnings.push_back(std::string("Unsupported geometry type ") + OGRGeometryTypeToName(g->getGeometryType()));
    }
    }
}
} // namespace

VectorImport importVector(const fs::path& file, const std::string& layerName, double z0) {
    auto ds = open(file, GDAL_OF_VECTOR);
    OGRLayer* l = layerName.empty() ? ds->GetLayer(0) : ds->GetLayerByName(layerName.c_str());
    if (!l) throw GisError("Layer '" + layerName + "' not found in " + file.filename().string());
    VectorImport r;
    crsOf(l->GetSpatialRef(), r.crsWkt, r.crsAuthority);
    OGRFeatureDefn* d = l->GetLayerDefn();
    // KML drivers expose presentation settings as fields: they are not feature data.
    const std::string drvName = ds->GetDriver() ? ds->GetDriver()->GetDescription() : "";
    const bool kml = drvName == "KML" || drvName == "LIBKML";
    static const std::set<std::string> kmlPresentation{"tessellate", "extrude", "visibility", "draworder", "icon", "altitudemode", "begin", "end", "timestamp", "snippet"};
    std::vector<std::string> keys; // empty = field not imported
    for (int f = 0; f < d->GetFieldCount(); ++f) {
        const auto* fd = d->GetFieldDefn(f);
        std::string lower = fd->GetNameRef();
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (kml && kmlPresentation.count(lower)) {
            keys.emplace_back();
            continue;
        }
        std::string type = "Text";
        if (fd->GetType() == OFTReal) type = "Real";
        else if (fd->GetType() == OFTInteger || fd->GetType() == OFTInteger64) type = "Integer";
        else if (fd->GetType() == OFTDate) type = "Date";
        // "Name"/"NAME" (KML, many shapefiles) is the feature name.
        keys.push_back(lower == "name" ? std::string("name") : sanitizeKey(fd->GetNameRef()));
        r.fields.push_back({keys.back(), type});
    }
    l->ResetReading();
    for (auto& feat : *l) {
        std::vector<GeometryObject> geoms;
        collect(feat->GetGeometryRef(), z0, geoms, r.warnings);
        if (geoms.empty()) {
            ++r.skipped;
            continue;
        }
        AttributeMap attrs;
        for (int f = 0; f < d->GetFieldCount(); ++f) {
            if (keys[static_cast<std::size_t>(f)].empty() || !feat->IsFieldSetAndNotNull(f)) continue;
            if (d->GetFieldDefn(f)->GetType() == OFTDate) {
                int y, mo, dd, h, mi, tz;
                float s;
                feat->GetFieldAsDateTime(f, &y, &mo, &dd, &h, &mi, &s, &tz);
                char buf[16];
                std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, mo, dd);
                attrs[keys[static_cast<std::size_t>(f)]] = buf;
            } else attrs[keys[static_cast<std::size_t>(f)]] = feat->GetFieldAsString(f);
        }
        for (auto& g : geoms) {
            if (g.name.empty()) {
                const auto it = attrs.find("name");
                g.name = it != attrs.end() ? it->second : (std::string(l->GetName()) + " " + std::to_string(feat->GetFID()));
            }
            r.features.push_back({std::move(g), attrs});
        }
    }
    return r;
}

std::vector<Uuid> addVectorFeatures(Project& p, const VectorImport& data, const Uuid& layerId) {
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Import GIS features");
    for (const auto& [key, type] : data.fields)
        ed.ensureAttributeDefinition("GeometryObject", key, key, attributeValueTypeFromString(type).value_or(AttributeValueType::Text));
    std::vector<Uuid> ids;
    for (const auto& f : data.features) {
        GeometryObject g = f.geometry;
        g.layerId = layerId;
        g.attributes = f.attributes;
        ids.push_back(ed.addGeometry(std::move(g)));
    }
    macro.commit();
    return ids;
}

VectorExportReport exportVector(const std::vector<const GeometryObject*>& objs, const fs::path& file, const VectorExportOptions& o, bool closedAsPolygons) {
    init();
    VectorExportReport report;
    auto exportable = [](const GeometryObject& g) {
        return (g.type == GeometryType::Point && !g.vertices.empty()) || (geometry::isCurve(g.type) && g.vertices.size() >= 1);
    };
    for (const auto* g : objs) {
        if (!g || exportable(*g)) continue;
        const std::string why = g->type == GeometryType::Point ? "a point without a position"
                                                               : "meshes, solids, text, dimensions and hatches have no vector-feature form (use DXF, OBJ/PLY/STL or a DEM)";
        report.skipped.push_back((g->name.empty() ? std::string("(unnamed)") : g->name) + " (" + toString(g->type) + "): " + why);
    }
    if (std::none_of(objs.begin(), objs.end(), [&](const GeometryObject* g) { return g && exportable(*g); }))
        throw GisError("Nothing to export: vector files hold points and curves" +
                       (report.skipped.empty() ? std::string() : " — " + report.skipped.front()));
    std::string drvName = o.driver;
    if (drvName.empty()) {
        const auto e = lowerExt(file);
        drvName = e == ".shp" ? "ESRI Shapefile" : e == ".gpkg" ? "GPKG" : (e == ".geojson" || e == ".json") ? "GeoJSON" : e == ".kml" ? "KML" : "";
        if (drvName.empty()) throw GisError("Unknown vector format " + e);
    }
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName(drvName.c_str());
    if (!drv) throw GisError("GDAL driver " + drvName + " is not available");
    if (fs::exists(file)) fs::remove(file);
    DsPtr ds(drv->Create(file.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!ds) throw GisError("Cannot create " + file.string() + ": " + CPLGetLastErrorMsg());
    std::unique_ptr<OGRSpatialReference> sr;
    if (!o.crsWkt.empty()) {
        sr = std::make_unique<OGRSpatialReference>();
        if (sr->SetFromUserInput(o.crsWkt.c_str()) != OGRERR_NONE) throw GisError("Invalid CRS for the vector file");
    }
    // Shapefiles hold one geometry type per file: use the most general needed.
    bool anyPoint = false, anyPoly = false, anyLine = false;
    for (const auto* g : objs) {
        if (!g || !exportable(*g)) continue;
        if (g->type == GeometryType::Point) anyPoint = true;
        else if (geometry::isCurve(g->type)) ((g->closed || g->type == GeometryType::Circle) && closedAsPolygons ? anyPoly : anyLine) = true;
    }
    const OGRwkbGeometryType gt = (anyPoint && !anyLine && !anyPoly) ? wkbPoint25D : (anyPoly && !anyLine && !anyPoint) ? wkbPolygon25D : (anyLine && !anyPoly && !anyPoint) ? wkbLineString25D : wkbUnknown;
    if (gt == wkbUnknown && drvName == "ESRI Shapefile") throw GisError("A shapefile holds one geometry type: export points, lines and polygons separately");
    OGRLayer* l = ds->CreateLayer(o.layerName.c_str(), sr.get(), gt, nullptr);
    if (!l) throw GisError("Cannot create layer " + o.layerName);
    std::set<std::string> keys;
    for (const auto* g : objs)
        if (g)
            for (const auto& [k, v] : g->attributes) keys.insert(k);
    OGRFieldDefn nameField("name", OFTString);
    l->CreateField(&nameField);
    for (const auto& k : keys) {
        OGRFieldDefn f(k.c_str(), OFTString);
        l->CreateField(&f);
    }
    for (const auto* g : objs) {
        if (!g || !exportable(*g)) continue;
        std::unique_ptr<OGRGeometry> geom;
        if (g->type == GeometryType::Point && !g->vertices.empty()) geom = std::make_unique<OGRPoint>(g->vertices[0].x, g->vertices[0].y, g->vertices[0].z);
        else if (geometry::isCurve(g->type)) {
            auto pts = geometry::curvePolyline(*g, 0.001);
            const bool closed = g->closed || g->type == GeometryType::Circle;
            if (closed && closedAsPolygons) {
                auto ring = std::make_unique<OGRLinearRing>();
                for (const auto& v : pts) ring->addPoint(v.x, v.y, v.z);
                ring->closeRings();
                auto poly = std::make_unique<OGRPolygon>();
                poly->addRingDirectly(ring.release());
                geom = std::move(poly);
            } else {
                auto ls = std::make_unique<OGRLineString>();
                for (const auto& v : pts) ls->addPoint(v.x, v.y, v.z);
                geom = std::move(ls);
            }
        } else continue;
        OGRFeature f(l->GetLayerDefn());
        f.SetField("name", g->name.c_str());
        for (const auto& [k, v] : g->attributes) f.SetField(k.c_str(), v.c_str());
        f.SetGeometryDirectly(geom.release());
        if (l->CreateFeature(&f) != OGRERR_NONE) throw GisError("Cannot write a feature: " + std::string(CPLGetLastErrorMsg()));
        ++report.written;
    }
    return report;
}

} // namespace openmine::gis
