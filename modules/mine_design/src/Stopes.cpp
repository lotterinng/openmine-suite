// SPDX-License-Identifier: Apache-2.0
#include "openmine/mine_design/Stopes.h"

#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/mine_design/Layout.h"
#include "openmine/project/ProjectEditor.h"

#include <cmath>

namespace openmine::mine_design {

namespace {
Uuid pickLayer(const Project& p, const Uuid& requested) {
    if (const auto* l = p.model().layers.find(requested); l && !l->locked) return l->id;
    for (const auto& l : p.model().layers.all())
        if (!l.locked) return l.id;
    throw ValidationError("All layers are locked");
}

Uuid addMeshGeometry(Project& p, MeshData mesh, const std::string& name, const Uuid& layer, const Uuid& material) {
    GeometryObject g;
    g.type = GeometryType::Mesh;
    g.name = name;
    g.layerId = pickLayer(p, layer);
    g.materialId = material;
    g.mesh = std::make_shared<MeshData>(std::move(mesh));
    return ProjectEditor(p).add(g);
}

Stope makeStope(const NewStope& s, const Uuid& geometryId) {
    Stope st;
    st.name = s.name;
    st.code = s.code;
    st.levelId = s.levelId;
    st.materialId = s.materialId;
    st.method = s.method;
    st.zone = s.zone;
    st.grade = s.grade;
    st.gradeUnit = s.gradeUnit;
    st.dilutionPercent = s.dilutionPercent;
    st.recoveryPercent = s.recoveryPercent;
    st.geometryId = geometryId;
    return st;
}
} // namespace

geometry::Ring clipHalfPlane(const geometry::Ring& ring, Vec2 o, Vec2 n) {
    geometry::Ring out;
    const std::size_t k = ring.size();
    for (std::size_t i = 0; i < k; ++i) {
        const Vec2 a = ring[i], b = ring[(i + 1) % k];
        const double da = dot(a - o, n), db = dot(b - o, n);
        if (da >= 0) out.push_back(a);
        if ((da >= 0) != (db >= 0)) {
            const double t = da / (da - db);
            out.push_back(a + (b - a) * t);
        }
    }
    return geometry::normalizeRing(out, 1e-9);
}

Uuid createStopeFromOutline(Project& p, const geometry::Ring& outline, double floorZ, double backZ, const NewStope& spec) {
    if (spec.name.empty()) throw ValidationError("Stope name must not be empty");
    CommandStack::MacroScope macro(p.commands(), "Create stope " + spec.name);
    const Uuid gid = addMeshGeometry(p, geometry::extrudeOutline(outline, floorZ, backZ), spec.name + " shape", spec.layerId, spec.materialId);
    const Uuid id = ProjectEditor(p).add(makeStope(spec, gid));
    macro.commit();
    return id;
}

Uuid createStopeFromGeometry(Project& p, const Uuid& geometryId, const NewStope& spec) {
    const auto* g = p.model().geometry.find(geometryId);
    if (!g || !g->mesh) throw ValidationError("Select a closed mesh or solid for the stope shape");
    if (!geometry::topology(*g->mesh).closed()) throw ValidationError("The stope shape must be a closed (watertight) mesh");
    return ProjectEditor(p).add(makeStope(spec, geometryId));
}

StopeMetrics stopeMetrics(const Project& p, const Stope& s) {
    StopeMetrics m;
    const auto* g = p.model().geometry.find(s.geometryId);
    if (!g || !g->mesh) return m;
    m.watertight = geometry::topology(*g->mesh).watertight();
    m.volume = std::abs(geometry::meshSignedVolume(*g->mesh));
    const auto* mat = p.model().materials.find(s.materialId);
    const double density = mat ? mat->density : 0.0;
    m.tonnes = m.volume * density;
    m.dilutedTonnes = m.tonnes * (1 + s.dilutionPercent / 100.0);
    m.recoveredTonnes = m.dilutedTonnes * s.recoveryPercent / 100.0;
    m.containedMetal = m.tonnes * s.grade;
    m.minedGrade = s.grade / (1 + s.dilutionPercent / 100.0);
    return m;
}

std::vector<Uuid> subdivideStope(Project& p, const Uuid& stopeId, const geometry::Ring& outlineIn, double floorZ, double backZ,
                                 double strikeAz, double panel, double lift) {
    const auto* st = p.model().stopes.find(stopeId);
    if (!st) throw ValidationError("Stope does not exist");
    if (!(panel > 0)) throw ValidationError("Panel length must be greater than zero");
    if (lift < 0) throw ValidationError("Lift height must not be negative");
    geometry::Ring outline = geometry::normalizeRing(outlineIn);
    const Vec2 strike = bearingVector(strikeAz);
    double smin = 1e300, smax = -1e300;
    for (auto v : outline) {
        smin = std::min(smin, dot(v, strike));
        smax = std::max(smax, dot(v, strike));
    }
    std::vector<double> zs{floorZ};
    if (lift > 0)
        for (double z = floorZ + lift; z < backZ - 1e-9; z += lift) zs.push_back(z);
    zs.push_back(backZ);

    const Uuid layer = p.model().geometry.find(st->geometryId)->layerId;
    std::vector<Uuid> ids;
    CommandStack::MacroScope macro(p.commands(), "Subdivide stope " + st->name);
    int seq = 0;
    const std::string stopeName = st->name;
    const Uuid material = st->materialId;
    for (double s0 = smin; s0 < smax - 1e-9; s0 += panel) {
        const double s1 = std::min(smax, s0 + panel);
        auto piece = clipHalfPlane(outline, strike * s0, strike);
        piece = clipHalfPlane(piece, strike * s1, strike * -1.0);
        if (piece.size() < 3 || geometry::area(piece) < 1e-6) continue;
        for (std::size_t k = 0; k + 1 < zs.size(); ++k) {
            ++seq;
            const std::string name = stopeName + " B" + std::to_string(seq);
            const Uuid gid = addMeshGeometry(p, geometry::extrudeOutline(piece, zs[k], zs[k + 1]), name, layer, material);
            MiningBlock b;
            b.name = name;
            b.stopeId = stopeId;
            b.sequence = seq;
            b.geometryId = gid;
            ids.push_back(ProjectEditor(p).add(b));
        }
    }
    macro.commit();
    return ids;
}

} // namespace openmine::mine_design
