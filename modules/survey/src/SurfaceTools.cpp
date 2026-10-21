// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/SurfaceTools.h"

#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/Processing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace openmine::survey {

namespace {
std::string num(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}
} // namespace

MeshPtr surfaceOf(const Project& p, const Uuid& id) {
    const auto* g = p.model().geometry.find(id);
    if (!g) throw ValidationError("Object " + id.toString() + " does not exist");
    if (!g->mesh || g->mesh->triangles.empty() || (g->type != GeometryType::Mesh && g->type != GeometryType::Solid))
        throw ValidationError("'" + g->name + "' is not a surface");
    return g->mesh;
}

SurfaceToolResult runSurfaceTool(const MeshData& mesh, SurfaceOperation op, const SurfaceToolOptions& o, const MeshData* reference, jobs::Progress& pr) {
    if (mesh.triangles.empty()) throw ValidationError("The surface has no triangles");
    SurfaceToolResult r;
    switch (op) {
    case SurfaceOperation::Decimate: {
        if (o.targetTriangles == 0 && !(o.maxError > 0)) throw ValidationError("Give a target triangle count or a maximum error");
        DecimateOptions d;
        d.targetTriangles = o.targetTriangles;
        d.maxError = o.maxError > 0 ? o.maxError : 1e300;
        d.preserveBoundary = o.preserveBoundary;
        r.mesh = decimate(mesh, d, pr);
        r.summary = std::to_string(mesh.triangles.size()) + " → " + std::to_string(r.mesh.triangles.size()) + " triangles";
        r.method = "Quadric decimation target=" + std::to_string(o.targetTriangles) + " maxError=" + num(o.maxError, 4);
        break;
    }
    case SurfaceOperation::Smooth:
        if (o.iterations < 1) throw ValidationError("Smoothing needs at least one iteration");
        if (!(o.lambda > 0) || !(o.mu < -o.lambda)) throw ValidationError("Taubin smoothing needs λ > 0 and μ < −λ");
        r.mesh = smoothTaubin(mesh, o.iterations, o.lambda, o.mu, o.fixBoundary);
        r.summary = std::to_string(o.iterations) + " Taubin iterations (λ " + num(o.lambda, 3) + ", μ " + num(o.mu, 3) + ")";
        r.method = "Taubin smoothing n=" + std::to_string(o.iterations);
        break;
    case SurfaceOperation::Repair: {
        RepairOptions ro;
        ro.mergeTolerance = o.mergeTolerance;
        ro.fillHolesUpTo = o.fillHolesUpTo;
        ro.orientOutward = o.orientOutward;
        r.mesh = repairMesh(mesh, ro, &r.repair);
        const auto& rep = r.repair;
        r.summary = std::to_string(rep.mergedVertices) + " vertices merged, " + std::to_string(rep.degenerateRemoved) + " degenerate and " +
                    std::to_string(rep.duplicatesRemoved) + " duplicate triangles removed, " + std::to_string(rep.flipped) + " flipped, " +
                    std::to_string(rep.holesFilled) + " holes filled; " + std::to_string(rep.nonManifoldEdges) + " non-manifold edges and " +
                    std::to_string(rep.boundaryLoops) + " boundary loops remain";
        r.method = "Mesh repair";
        break;
    }
    case SurfaceOperation::Compare: {
        if (!reference || reference->triangles.empty()) throw ValidationError("Choose the reference surface");
        PointBuffer pts;
        pts.xyz = mesh.vertices;
        const auto d = cloudToMesh(pts, *reference, o.signedDistance, o.maxDistance > 0 ? o.maxDistance : 1e300, pr);
        r.mesh = mesh;
        r.mesh.vertexScalars = d;
        r.mesh.scalarName = "distance";
        std::vector<float> valid;
        for (float x : d)
            if (std::isfinite(x)) valid.push_back(x);
        if (valid.empty()) throw ValidationError("No vertex lies within the maximum distance of the reference");
        std::sort(valid.begin(), valid.end());
        double sum = 0, sq = 0;
        for (float x : valid) {
            sum += x;
            sq += static_cast<double>(x) * x;
        }
        auto& st = r.distances;
        st.vertices = d.size();
        st.measured = valid.size();
        st.mean = sum / static_cast<double>(valid.size());
        st.rms = std::sqrt(sq / static_cast<double>(valid.size()));
        st.min = valid.front();
        st.median = valid[valid.size() / 2];
        st.max = valid.back();
        r.summary = "distance to reference: mean " + num(st.mean, 4) + " m, RMS " + num(st.rms, 4) + " m, min " + num(st.min, 4) + " m, median " +
                    num(st.median, 4) + " m, max " + num(st.max, 4) + " m (" + std::to_string(st.measured) + " of " + std::to_string(st.vertices) + " vertices)";
        r.method = "Distance to reference surface";
        break;
    }
    }
    if (r.mesh.triangles.empty()) throw ValidationError("The operation produced an empty surface");
    return r;
}

Uuid addDerivedSurface(Project& p, const Uuid& sourceId, const SurfaceToolResult& result, const std::string& name) {
    const auto* src = p.model().geometry.find(sourceId);
    if (!src) throw ValidationError("The source surface no longer exists");
    CommandStack::MacroScope macro(p.commands(), "Add surface " + name);
    ProjectEditor ed(p);
    ed.ensureAttributeDefinition("GeometryObject", "method", "Method");
    ed.ensureAttributeDefinition("GeometryObject", "source_object", "Source object");
    GeometryObject obj;
    obj.type = GeometryType::Mesh;
    obj.name = name.empty() ? src->name + " (" + result.method + ")" : name;
    obj.layerId = src->layerId;
    obj.mesh = std::make_shared<MeshData>(result.mesh);
    obj.attributes["method"] = result.method;
    obj.attributes["source_object"] = sourceId.toString();
    const Uuid id = ed.addGeometry(std::move(obj));
    macro.commit();
    return id;
}

CloudSurface surfaceFromPoints(PointBuffer pts, const CloudSurfaceSpec& spec, const PoissonEngine& poisson, jobs::Progress& pr) {
    if (spec.voxel > 0) pts = voxelDownsample(pts, spec.voxel);
    if (pts.size() < 3) throw ValidationError("A surface needs at least three points");
    CloudSurface r;
    if (spec.method == CloudSurfaceSpec::Method::Delaunay) {
        DelaunayOptions o;
        o.maxEdgeLength = spec.maxEdgeLength;
        r.mesh = delaunay25D(pts.xyz, o, nullptr, pr);
        if (spec.targetTriangles > 0 && r.mesh.triangles.size() > spec.targetTriangles) {
            DecimateOptions d;
            d.targetTriangles = spec.targetTriangles;
            r.mesh = decimate(r.mesh, d, pr);
        }
        r.method = "Delaunay 2.5D";
    } else {
        if (!poisson.function) throw ValidationError("Poisson reconstruction needs an engine (PDAL or Open3D)");
        if (spec.depth < 4 || spec.depth > 14) throw ValidationError("The Poisson depth must be between 4 and 14");
        if (!pts.schema().normals) {
            NormalOptions no;
            if (spec.orientTowardsCentroid) {
                no.orientation = NormalOptions::Orientation::TowardsViewpoint;
                no.viewpoint = statistics(pts, 0).centroid;
            }
            estimateNormals(pts, no, pr);
        }
        r.mesh = poisson.function(pts, spec.depth);
        r.method = "Poisson depth=" + std::to_string(spec.depth) + (poisson.label.empty() ? std::string() : " (" + poisson.label + ")");
    }
    if (r.mesh.triangles.empty()) throw ValidationError("The surface has no triangles");
    return r;
}

Uuid addSurfaceObject(Project& p, MeshData mesh, const std::string& name, const Uuid& layerId, const std::string& method, const Uuid& sourceId) {
    if (mesh.triangles.empty()) throw ValidationError("The surface has no triangles");
    CommandStack::MacroScope macro(p.commands(), "Add surface " + name);
    ProjectEditor ed(p);
    ed.ensureAttributeDefinition("GeometryObject", "method", "Method");
    GeometryObject g;
    g.type = GeometryType::Mesh;
    g.name = name;
    g.layerId = layerId;
    g.mesh = std::make_shared<MeshData>(std::move(mesh));
    g.attributes["method"] = method;
    if (!sourceId.isNil()) {
        ed.ensureAttributeDefinition("GeometryObject", "source_object", "Source object");
        g.attributes["source_object"] = sourceId.toString();
    }
    const Uuid id = ed.addGeometry(std::move(g));
    macro.commit();
    return id;
}

} // namespace openmine::survey
