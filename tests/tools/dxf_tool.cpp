// SPDX-License-Identifier: Apache-2.0
// Test helper for validating the DXF reader/writer against ezdxf (tests/integration/test_dxf_ezdxf.py).
//   dxf_tool write-sample <out.dxf> <expected.json>   writes the shared sample drawing
//   dxf_tool read <in.dxf>                            prints the import result as JSON

#include "../support/DxfSample.h"
#include "openmine/cad/Annotation.h"
#include "openmine/formats/Dxf.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/objects/EnumNames.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>

using namespace openmine;
using json = nlohmann::json;

namespace {
json vec(Vec3 v) { return json::array({v.x, v.y, v.z}); }

json describe(const GeometryObject& g) {
    json j;
    j["type"] = toString(g.type);
    j["name"] = g.name;
    if (g.colorOverride) j["color"] = *g.colorOverride;
    if (geometry::isCurve(g.type)) {
        j["length"] = geometry::curveLength(g);
        j["start"] = vec(*geometry::curveStart(g));
        j["end"] = vec(*geometry::curveEnd(g));
        j["closed"] = g.closed || g.type == GeometryType::Circle;
    }
    if (g.type == GeometryType::Arc || g.type == GeometryType::Circle) {
        j["center"] = vec(g.arc.center);
        j["radius"] = g.arc.radius;
    }
    if (g.type == GeometryType::Spline || g.type == GeometryType::Polyline || g.type == GeometryType::Point || g.type == GeometryType::Line) {
        json pts = json::array();
        for (const auto& v : g.vertices) pts.push_back(vec(v));
        j["vertices"] = pts;
    }
    if (g.type == GeometryType::Hatch) {
        j["area"] = cad::hatchArea(g);
        j["pattern"] = g.hatch.pattern;
        j["loops"] = g.hatch.loopStarts.size();
    }
    if (g.type == GeometryType::Text) {
        j["text"] = g.text.text;
        j["height"] = g.text.height;
        j["position"] = vec(g.vertices[0]);
        j["rotation"] = g.text.rotation;
    }
    if (g.type == GeometryType::Dimension) {
        const auto d = cad::dimensionGraphics(g);
        j["kind"] = toString(g.dimension.kind);
        j["value"] = d.value;
        j["text"] = d.text;
    }
    if (g.type == GeometryType::Mesh && g.mesh) {
        j["vertexCount"] = g.mesh->vertices.size();
        j["triangleCount"] = g.mesh->triangles.size();
        j["area"] = geometry::meshArea(*g.mesh);
    }
    return j;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string cmd = argc > 1 ? argv[1] : "";
        if (cmd == "write-sample" && argc == 4) {
            const auto d = openmine::testing::sampleDrawing();
            const auto rep = formats::writeDxf(argv[2], d.layers, d.ptrs());
            json exp = json::object();
            for (const auto& o : d.objects) exp[o.name] = describe(o);
            json out;
            out["objects"] = exp;
            out["written"] = rep.written;
            out["warnings"] = rep.warnings;
            std::ofstream(argv[3]) << out.dump(2);
            return 0;
        }
        if (cmd == "read" && (argc == 3 || argc == 4)) {
            formats::DxfImportOptions opt;
            if (argc == 4) opt.chordTolerance = std::stod(argv[3]);
            const auto r = formats::readDxf(argv[2], opt);
            json out;
            out["acadVersion"] = r.acadVersion;
            out["insUnits"] = r.insUnits;
            out["unitScale"] = r.unitScale;
            out["imported"] = r.imported;
            out["skipped"] = r.skipped;
            out["warnings"] = r.warnings;
            json layers = json::object();
            std::map<Uuid, std::string> names;
            for (const auto& l : r.layers) {
                names[l.id] = l.name;
                layers[l.name] = {{"color", l.colorRgb}, {"visible", l.visible}, {"locked", l.locked}, {"lineType", l.lineType}};
            }
            out["layers"] = layers;
            json objs = json::array();
            for (const auto& g : r.objects) {
                auto j = describe(g);
                j["layer"] = names[g.layerId];
                objs.push_back(j);
            }
            out["objects"] = objs;
            std::cout << out.dump(2) << "\n";
            return 0;
        }
        std::cerr << "usage: dxf_tool write-sample <out.dxf> <expected.json> | read <in.dxf> [chordTolerance]\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
