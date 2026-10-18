// SPDX-License-Identifier: Apache-2.0
#include "openmine/occt/SolidObjects.h"

#include "openmine/commands/CommandStack.h"
#include "openmine/geometry/Curves.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace openmine::occt {

namespace {
const GeometryObject& object(const Project& p, const Uuid& id) {
    const auto* g = p.model().geometry.find(id);
    if (!g) throw ValidationError("Geometry object does not exist");
    return *g;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

Uuid addSolidObject(Project& p, const Shape& shape, const std::string& name, const Uuid& layerId, const TessellationSettings& t,
                    const std::string& text) {
    const auto info = analyse(shape);
    if (info.faces == 0) throw SolidError("The shape has no faces");
    auto mesh = std::make_shared<MeshData>(tessellate(shape, t.linearDeflection, t.angularDeflection));
    Asset asset = p.assets().writeBytes(toBrep(shape), AssetKind::Brep, "brep");
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), text);
    ed.add(asset);
    GeometryObject g;
    g.type = GeometryType::Solid;
    g.name = name;
    g.layerId = layerId;
    g.brepAsset.id = asset.id;
    g.mesh = std::move(mesh);
    const Uuid id = ed.add(std::move(g));
    macro.commit();
    return id;
}

Shape loadSolid(const Project& p, const GeometryObject& g) {
    if (g.type != GeometryType::Solid) throw SolidError("'" + g.name + "' is not a solid");
    const auto* a = p.model().assets.find(g.brepAsset.id);
    if (!a) throw SolidError("The B-rep asset of '" + g.name + "' is missing from the project");
    return fromBrep(p.assets().readBytes(*a));
}

Uuid transformSolidObject(Project& p, const Uuid& id, const Transform3& t, bool copy, const TessellationSettings& ts) {
    const auto& g = object(p, id);
    if (g.type != GeometryType::Solid) throw SolidError("Only solid objects are transformed through the kernel (" + g.name + " is not a solid)");
    if (std::abs(t.determinant3()) < 1e-12) throw SolidError("The transformation is singular");
    const Shape moved = transformed(loadSolid(p, g), t);
    if (copy) return addSolidObject(p, moved, g.name.empty() ? std::string("Solid copy") : g.name + " (copy)", g.layerId, ts, "Copy solid");
    replaceSolidShape(p, id, moved, "Transform solid", ts);
    return id;
}

ShapeInfo replaceSolidShape(Project& p, const Uuid& id, const Shape& shape, const std::string& text, const TessellationSettings& ts) {
    const auto& g = object(p, id);
    if (g.type != GeometryType::Solid) throw SolidError("'" + g.name + "' is not a solid");
    const auto info = analyse(shape);
    if (info.faces == 0) throw SolidError("The shape has no faces");
    auto mesh = std::make_shared<MeshData>(tessellate(shape, ts.linearDeflection, ts.angularDeflection));
    Asset asset = p.assets().writeBytes(toBrep(shape), AssetKind::Brep, "brep");
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), text);
    ed.add(asset);
    GeometryObject u = g;
    u.brepAsset.id = asset.id;
    u.mesh = std::move(mesh);
    ed.update(std::move(u));
    macro.commit();
    return info;
}

Shape shapeOfObject(const Project& p, const GeometryObject& g) {
    switch (g.type) {
    case GeometryType::Solid: return loadSolid(p, g);
    case GeometryType::Mesh:
        if (!g.mesh) throw SolidError("Mesh object has no data");
        return fromMesh(*g.mesh);
    default:
        if (geometry::isCurve(g.type)) return planarFace(g);
        throw SolidError("Objects of this type have no solid shape");
    }
}

Uuid booleanObjects(Project& p, const Uuid& a, const Uuid& b, BooleanOp op, bool removeInputs, const std::string& name, double fuzzy) {
    const auto& ga = object(p, a);
    const auto& gb = object(p, b);
    const Shape r = boolean(shapeOfObject(p, ga), shapeOfObject(p, gb), op, fuzzy);
    const Uuid layer = ga.layerId;
    const char* opName = op == BooleanOp::Union ? "Union" : (op == BooleanOp::Difference ? "Subtract" : "Intersect");
    CommandStack::MacroScope macro(p.commands(), std::string(opName) + " solids");
    const Uuid id = addSolidObject(p, r, name.empty() ? std::string(opName) + " of " + ga.name + " and " + gb.name : name, layer);
    if (removeInputs) {
        ProjectEditor ed(p);
        ed.remove<GeometryObject>(a);
        ed.remove<GeometryObject>(b);
    }
    macro.commit();
    return id;
}

Uuid solidFromMeshObject(Project& p, const Uuid& meshId, bool requireClosed) {
    const auto& g = object(p, meshId);
    if (g.type != GeometryType::Mesh || !g.mesh) throw ValidationError("The object is not a mesh");
    const Shape s = fromMesh(*g.mesh);
    const auto info = analyse(s);
    if (requireClosed && (info.solids == 0 || !info.closed)) throw SolidError("The mesh is not closed: it cannot become a solid");
    return addSolidObject(p, s, g.name.empty() ? "Solid" : g.name + " (solid)", g.layerId, {}, "Convert mesh to solid");
}

Uuid developmentSolid(Project& p, const Uuid& devId, const Uuid& layerId, SweepTransition t) {
    const auto* d = p.model().developments.find(devId);
    if (!d) throw ValidationError("Development does not exist");
    const auto& centre = object(p, d->geometryId);
    const auto profile = mine_design::developmentProfileCurve(p, *d);
    const Shape s = sweepProfile(profile.points, profile.bulges, centre, t);
    return addSolidObject(p, s, d->name + " excavation", layerId, {}, "Create excavation solid for " + d->name);
}

std::vector<Uuid> sectionObject(Project& p, const Uuid& objectId, Vec3 pp, Vec3 n, const Uuid& layerId) {
    const auto& g = object(p, objectId);
    const auto loops = section(shapeOfObject(p, g), pp, n);
    std::vector<Uuid> ids;
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Section " + g.name);
    for (const auto& l : loops) {
        GeometryObject s;
        s.type = GeometryType::Polyline;
        s.layerId = layerId;
        s.name = g.name + " section";
        s.vertices = l;
        if (s.vertices.size() > 2 && distance(s.vertices.front(), s.vertices.back()) < 1e-7) {
            s.vertices.pop_back();
            s.closed = true;
        }
        if (s.vertices.size() < 2) continue;
        ids.push_back(ed.add(std::move(s)));
    }
    macro.commit();
    return ids;
}

std::vector<Uuid> importSolids(Project& p, const std::filesystem::path& file, const Uuid& layerId) {
    const std::string ext = lower(file.extension().string());
    std::vector<Shape> shapes;
    if (ext == ".step" || ext == ".stp") shapes = readStep(file.string());
    else if (ext == ".iges" || ext == ".igs") shapes = readIges(file.string());
    else if (ext == ".brep" || ext == ".brp") {
        std::ifstream in(file, std::ios::binary);
        if (!in) throw SolidError("Cannot open " + file.string());
        std::ostringstream ss;
        ss << in.rdbuf();
        shapes = {fromBrep(ss.str())};
    } else throw SolidError("Unsupported solid file type " + ext + " (STEP, IGES, BREP)");
    CommandStack::MacroScope macro(p.commands(), "Import " + file.filename().string());
    std::vector<Uuid> ids;
    int k = 1;
    for (const auto& s : shapes) ids.push_back(addSolidObject(p, s, file.stem().string() + (shapes.size() > 1 ? " " + std::to_string(k++) : ""), layerId));
    macro.commit();
    return ids;
}

void exportSolids(const Project& p, const std::vector<Uuid>& ids, const std::filesystem::path& file) {
    std::vector<Shape> shapes;
    for (const auto& id : ids) shapes.push_back(shapeOfObject(p, object(p, id)));
    if (shapes.empty()) throw SolidError("Nothing to export");
    const std::string ext = lower(file.extension().string());
    if (ext == ".step" || ext == ".stp") writeStep(shapes, file.string());
    else if (ext == ".iges" || ext == ".igs") writeIges(shapes, file.string());
    else throw SolidError("Unsupported export type " + ext + " (STEP or IGES)");
}

} // namespace openmine::occt
