// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/AsBuilt.h"

#include "openmine/geometry/Polygon2D.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/mine_design/Development.h"
#include "openmine/survey/KdTree.h"
#include "openmine/survey/Meshing.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/Processing.h"

#include <algorithm>
#include <cmath>

namespace openmine::survey {

namespace {
/// Points inside the excavation every `step` metres: the centroid of the design profile carried
/// along the centreline. Normals are oriented towards the nearest of them. (The centreline itself is
/// usually the floor or grade line, from which floor normals would be ambiguous.)
std::vector<Vec3> interiorLine(const Project& p, const MineDevelopment& d, const std::vector<Vec3>& cl, double step) {
    Vec2 c{0, 0};
    try {
        const auto pc = mine_design::developmentProfileCurve(p, d);
        if (pc.points.size() >= 3) c = geometry::centroid(pc.points);
    } catch (const std::exception&) {
        // no profile: fall back to the centreline itself
    }
    double length = 0;
    for (std::size_t i = 1; i < cl.size(); ++i) length += distance(cl[i - 1], cl[i]);
    std::vector<Vec3> out;
    const int n = std::max(1, static_cast<int>(std::ceil(length / step)));
    for (int k = 0; k <= n; ++k) out.push_back(geometry::sectionFrameAt(cl, length * k / n).toWorld(c));
    return out;
}
} // namespace

PointBuffer corridorPoints(const Project& p, const Uuid& datasetId, const Uuid& developmentId, const AsBuiltOptions& o, jobs::Progress& progress) {
    if (!(o.corridor > 0)) throw PointCloudError("The corridor half-width must be positive");
    const auto* d = p.model().developments.find(developmentId);
    if (!d) throw PointCloudError("The development does not exist");
    const auto cl = mine_design::centrelinePoints(p, *d);
    if (cl.size() < 2) throw PointCloudError("The development's centreline has no length");
    auto box = BoundingBox::empty();
    for (const auto& q : cl) box.expand(q);
    box.min = box.min - Vec3{o.corridor, o.corridor, o.corridor};
    box.max = box.max + Vec3{o.corridor, o.corridor, o.corridor};
    progress.subRange(0, 0.5);
    auto pts = datasetPoints(p, datasetId, true, box, progress);
    std::vector<char> keep(pts.size(), 0);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const auto dist = geometry::pointPolylineDistance(pts.xyz[i], cl);
        keep[i] = dist && *dist <= o.corridor ? 1 : 0;
    }
    pts = pts.filtered(keep);
    if (o.voxel > 0) pts = voxelDownsample(pts, o.voxel);
    if (pts.size() < 100) throw PointCloudError("Fewer than 100 scan points lie within the corridor of the heading");
    progress.subRange(0.5, 1.0);
    NormalOptions no;
    no.orientation = NormalOptions::Orientation::TowardsNearestScanner;
    no.scanners = interiorLine(p, *d, cl, 0.5);
    no.storeCurvature = false;
    estimateNormals(pts, no, progress);
    return pts;
}

MeshData trimToScan(const MeshData& mesh, const PointBuffer& pts, double voxel, std::size_t fillHolesUpTo) {
    if (pts.empty()) throw PointCloudError("No scan points to trim against");
    const KdTree tree(pts.xyz);
    std::vector<double> spacing;
    const std::size_t stride = std::max<std::size_t>(1, pts.size() / 5000);
    for (std::size_t i = 0; i < pts.size(); i += stride) {
        std::vector<double> d2;
        tree.knn(pts.xyz[i], 2, &d2);
        if (d2.size() == 2) spacing.push_back(std::sqrt(d2[1]));
    }
    std::nth_element(spacing.begin(), spacing.begin() + static_cast<std::ptrdiff_t>(spacing.size() / 2), spacing.end());
    const double medianSpacing = spacing.empty() ? voxel : spacing[spacing.size() / 2];
    const double maxD = std::max({3 * voxel, 2.0 * medianSpacing, 0.05});
    MeshData trimmed;
    trimmed.vertices = mesh.vertices;
    for (const auto& t : mesh.triangles) {
        const Vec3 cen = (mesh.vertices[t[0]] + mesh.vertices[t[1]] + mesh.vertices[t[2]]) / 3.0;
        const auto n = tree.nearest(cen);
        if (n && n->second <= maxD) trimmed.triangles.push_back(t); // nearest() returns the distance
    }
    if (trimmed.triangles.empty()) throw PointCloudError("The reconstructed surface does not follow the scan");
    RepairOptions ro;
    ro.orientConsistently = true;
    ro.orientOutward = false;
    ro.fillHolesUpTo = fillHolesUpTo;
    return repairMesh(trimmed, ro);
}

MeshData buildAsBuiltSurface(const Project& p, const Uuid& datasetId, const Uuid& developmentId, const AsBuiltOptions& o, const SurfaceEngine& engine,
                             jobs::Progress& progress) {
    if (!engine) throw PointCloudError("No surface-reconstruction engine is available (PDAL or Open3D)");
    progress.subRange(0, 0.6);
    const auto pts = corridorPoints(p, datasetId, developmentId, o, progress);
    progress.checkCancelled();
    progress.subRange(0.6, 0.95);
    progress.update(0, "Surface reconstruction");
    MeshData surface = engine(pts, o.depth);
    surface.vertexScalars.clear();
    surface.scalarName.clear();
    if (surface.triangles.empty()) throw PointCloudError("The surface engine returned no surface");
    progress.subRange(0.95, 1.0);
    return trimToScan(surface, pts, o.voxel, o.fillHolesUpTo);
}

} // namespace openmine::survey
