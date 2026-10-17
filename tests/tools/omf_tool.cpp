// SPDX-License-Identifier: Apache-2.0
// Test helper for the OMF validation suite (tests/integration/test_omf.py).
//   omf_tool write-sample <file.omf>   build a project, export it, print what was written
//   omf_tool read <file.omf>           parse and print the document
//   omf_tool import <file.omf>         import into a new project and print the result
#include "openmine/geology/BlockModel.h"
#include "openmine/geology/Desurvey.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/omf/OmfProject.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/PointCloudProject.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>

using namespace openmine;
using nlohmann::json;

namespace {
json v3(Vec3 v) { return {v.x, v.y, v.z}; }
json nanToNull(double x) { return std::isnan(x) ? json(nullptr) : json(x); }

MeshData box(Vec3 lo, Vec3 hi) {
    MeshData m;
    for (int i = 0; i < 8; ++i) m.vertices.push_back({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
    const std::uint32_t f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                    {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (auto& t : f) m.triangles.push_back({t[0], t[1], t[2]});
    return m;
}

json writeSample(const std::string& file) {
    auto pp = Project::createNew("OMF sample", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid layer = ed.addLayer("Design", 0x3366CC);
    GeometryObject pl;
    pl.name = "Drive CL";
    pl.layerId = layer;
    pl.vertices = {{1000, 2000, 100}, {1050, 2000, 99}, {1050, 2040, 98}};
    pl.bulges = {0.2, 0, 0};
    ed.addGeometry(pl);
    GeometryObject ring;
    ring.name = "Pillar";
    ring.layerId = layer;
    ring.vertices = {{0, 0, 5}, {10, 0, 5}, {10, 10, 5}, {0, 10, 5}};
    ring.closed = true;
    ed.addGeometry(ring);
    for (int i = 0; i < 2; ++i) {
        GeometryObject pt;
        pt.type = GeometryType::Point;
        pt.name = "STN" + std::to_string(i + 1);
        pt.layerId = layer;
        pt.vertices = {{500.0 + i, 600.0, 7.0}};
        ed.addGeometry(pt);
    }
    GeometryObject mesh;
    mesh.type = GeometryType::Mesh;
    mesh.name = "Stope";
    mesh.layerId = layer;
    mesh.mesh = std::make_shared<MeshData>(box({0, 0, 0}, {20, 10, 30}));
    ed.addGeometry(mesh);
    Drillhole h1;
    h1.holeId = "DD001";
    h1.collar = {1010, 2010, 120};
    h1.length = 60;
    h1.surveys = {{0, 90, -60}, {30, 95, -55}, {60, 100, -50}};
    h1.intervals = {{0, 10, {{"au", 0.5}}, {{"lith", "BAS"}}}, {10, 12, {{"au", 4.25}}, {{"lith", "QV"}}}, {12, 30, {}, {{"lith", "BAS"}}}};
    ed.add(h1);
    Drillhole h2;
    h2.holeId = "DD002";
    h2.collar = {1030, 2010, 121};
    h2.length = 40;
    h2.intervals = {{5, 6, {{"au", 1.5}}, {}}};
    ed.add(h2);
    BlockModel b;
    b.name = "Resource";
    b.origin = {1000, 2000, 50};
    b.blockSize = {10, 5, 2};
    b.nx = 3;
    b.ny = 2;
    b.nz = 2;
    b.rotationDeg = 30;
    const Uuid bid = geology::createBlockModel(p, b);
    auto data = geology::loadBlockModel(p, bid);
    const geology::BlockGrid g(data.header);
    auto& idx = data.attribute("ijk");
    auto& cx = data.attribute("cx");
    for (std::size_t i = 0; i < data.count(); ++i) {
        const auto c = g.ijk(i);
        idx[i] = c[0] + 10 * c[1] + 100 * c[2];
        cx[i] = g.centroid(i).x;
    }
    saveBlockModel(p, data);

    std::vector<std::string> notes;
    omf::OmfExportOptions o;
    o.author = "omf_tool";
    const auto doc = omf::exportProjectOmf(p, o, &notes);
    omf::writeOmf(doc, file);

    json out;
    out["notes"] = notes;
    out["elements"] = json::array();
    for (const auto& e : doc.elements) {
        json je{{"name", e.name}, {"vertices", e.vertices.size()}, {"segments", e.segments.size()}, {"triangles", e.triangles.size()}};
        out["elements"].push_back(je);
    }
    // Expected drillhole geometry (minimum curvature) and values.
    json holes = json::array();
    for (const auto* h : {&h1, &h2}) {
        const geology::DesurveyedHole d(*h);
        for (const auto& i : h->intervals) {
            const auto it = i.values.find("au");
            holes.push_back({{"hole", h->holeId}, {"from", v3(d.at(i.from))}, {"to", v3(d.at(i.to))}, {"au", it == i.values.end() ? json(nullptr) : json(it->second)},
                             {"lith", i.codes.count("lith") ? json(i.codes.at("lith")) : json(nullptr)}});
        }
    }
    out["intervals"] = holes;
    json cells = json::array();
    for (std::size_t i = 0; i < data.count(); ++i) cells.push_back({{"centroid", v3(g.centroid(i))}, {"ijk", idx[i]}});
    out["block_model"] = {{"origin", v3(b.origin)}, {"axis_u", v3(g.axisU())}, {"axis_v", v3(g.axisV())}, {"size", v3(b.blockSize)},
                          {"n", {b.nx, b.ny, b.nz}},  {"cells", cells}};
    out["mesh_volume"] = geometry::meshSignedVolume(*mesh.mesh);
    out["arc_length"] = geometry::length3dBulged(pl.vertices, pl.bulges, false);
    return out;
}

json dataJson(const omf::Data& d) {
    json j{{"name", d.name}, {"location", d.location}};
    switch (d.kind) {
    case omf::Data::Kind::Scalar: {
        j["kind"] = "scalar";
        json a = json::array();
        for (double x : d.scalars) a.push_back(nanToNull(x));
        j["values"] = a;
        break;
    }
    case omf::Data::Kind::Mapped: {
        j["kind"] = "mapped";
        j["indices"] = d.indices;
        json ls = json::array();
        for (const auto& l : d.legends) {
            json lj{{"name", l.name}};
            if (l.kind == omf::Legend::Kind::Color) lj["colors"] = l.colors;
            else if (l.kind == omf::Legend::Kind::Scalar) lj["scalars"] = l.scalars;
            else lj["strings"] = l.strings;
            ls.push_back(lj);
        }
        j["legends"] = ls;
        break;
    }
    case omf::Data::Kind::String:
        j["kind"] = "string";
        j["values"] = d.strings;
        break;
    case omf::Data::Kind::DateTime:
        j["kind"] = "datetime";
        j["values"] = d.strings;
        break;
    case omf::Data::Kind::Color:
        j["kind"] = "color";
        j["values"] = d.colors;
        break;
    case omf::Data::Kind::Vector2:
    case omf::Data::Kind::Vector3: {
        j["kind"] = d.kind == omf::Data::Kind::Vector2 ? "vector2" : "vector3";
        json a = json::array();
        for (const auto& v : d.vectors) a.push_back(v3(v));
        j["values"] = a;
        break;
    }
    }
    return j;
}

json readDoc(const std::string& file) {
    const auto doc = omf::readOmf(file);
    json out{{"name", doc.name}, {"author", doc.author}, {"units", doc.units}, {"origin", v3(doc.origin)}, {"elements", json::array()}};
    for (const auto& e : doc.elements) {
        const char* kinds[] = {"pointset", "lineset", "surface", "surfacegrid", "volume"};
        json je{{"name", e.name}, {"kind", kinds[static_cast<int>(e.kind)]}, {"origin", v3(e.origin)}, {"color", e.color}, {"subtype", e.subtype}};
        json vs = json::array();
        for (const auto& v : e.vertices) vs.push_back(v3(v));
        je["vertices"] = vs;
        je["segments"] = e.segments;
        je["triangles"] = e.triangles;
        je["tensor_u"] = e.tensorU;
        je["tensor_v"] = e.tensorV;
        je["tensor_w"] = e.tensorW;
        je["axis_u"] = v3(e.axisU);
        je["axis_v"] = v3(e.axisV);
        je["axis_w"] = v3(e.axisW);
        je["offset_w"] = e.offsetW;
        json ds = json::array();
        for (const auto& d : e.data) ds.push_back(dataJson(d));
        je["data"] = ds;
        je["notes"] = e.notes;
        out["elements"].push_back(je);
    }
    return out;
}

json importDoc(const std::string& file) {
    const auto doc = omf::readOmf(file);
    auto pp = Project::createNew("Import", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    omf::OmfImportOptions o;
    o.sourceFile = file;
    const auto s = omf::importOmf(p, doc, o);
    json out{{"notes", s.notes}, {"geometry", json::array()}, {"block_models", json::array()}, {"point_clouds", json::array()}};
    for (const auto& id : s.geometry) {
        const auto& g = *p.model().geometry.find(id);
        json gj{{"name", g.name}, {"type", toString(g.type)}, {"vertices", g.vertices.size()}, {"closed", g.closed}};
        if (g.mesh) {
            gj["triangles"] = g.mesh->triangles.size();
            gj["area"] = geometry::meshArea(*g.mesh);
            gj["scalar"] = g.mesh->scalarName;
        }
        if (!g.vertices.empty()) gj["first"] = v3(g.vertices.front());
        out["geometry"].push_back(gj);
    }
    for (const auto& id : s.blockModels) {
        const auto d = geology::loadBlockModel(p, id);
        const geology::BlockGrid g(d.header);
        json bj{{"name", d.header.name},
                {"origin", v3(d.header.origin)},
                {"size", v3(d.header.blockSize)},
                {"n", {d.header.nx, d.header.ny, d.header.nz}},
                {"rotation", d.header.rotationDeg},
                {"attributes", d.header.attributes},
                {"description", d.header.description}};
        json cents = json::array();
        for (std::size_t i = 0; i < d.count(); ++i) cents.push_back(v3(g.centroid(i)));
        bj["centroids"] = cents;
        json vals = json::object();
        for (const auto& [k, v] : d.values) {
            json a = json::array();
            for (double x : v) a.push_back(nanToNull(x));
            vals[k] = a;
        }
        bj["values"] = vals;
        out["block_models"].push_back(bj);
    }
    for (const auto& id : s.pointClouds) {
        const auto& ds = survey::dataset(p, id);
        const auto buf = survey::datasetPoints(p, id);
        json pj{{"name", ds.name}, {"points", ds.pointCount}, {"fields", ds.attributes.scalarFields}};
        json pts = json::array();
        for (const auto& v : buf.xyz) pts.push_back(v3(v));
        pj["xyz"] = pts;
        out["point_clouds"].push_back(pj);
    }
    p.commands().undo();
    out["after_undo"] = {{"geometry", p.model().geometry.size()}, {"block_models", p.model().blockModels.size()}, {"point_clouds", p.model().pointClouds.size()}};
    return out;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: omf_tool write-sample|read|import <file.omf>\n";
        return 2;
    }
    try {
        const std::string cmd = argv[1];
        json out;
        if (cmd == "write-sample") out = writeSample(argv[2]);
        else if (cmd == "read") out = readDoc(argv[2]);
        else if (cmd == "import") out = importDoc(argv[2]);
        else throw std::runtime_error("unknown command " + cmd);
        std::cout << out.dump() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
