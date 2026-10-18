#pragma once
// SPDX-License-Identifier: Apache-2.0
// Builds SceneItems (render-local VTK poly data plus display properties) from project entities.
// Pure functions of the model: no VTK rendering objects, no Qt widgets.

#include "SceneTypes.h"

#include "openmine/geology/BlockModel.h"
#include "openmine/project/Project.h"

namespace openmine::app::scene {

struct BuildContext {
    const Project& project;
    const SceneSettings& settings;
    const coordinates::RenderOrigin& origin;
    /// Drillhole colouring range of settings.drillholeField over all holes (computed by the cache).
    std::optional<std::pair<double, double>> drillholeRange;
    /// Loaded block model values (by entity id), provided by the cache.
    const geology::BlockModelData* blockData = nullptr;
};

/// The entity whose selection a click on geometry `geometryId` produces (a development for its
/// centreline, a stope for its shape, …), or the geometry object itself.
SelectionItem ownerOfGeometry(const ProjectModel& m, const Uuid& geometryId);

std::vector<SceneItem> buildGeometry(const BuildContext& c, const GeometryObject& g);
std::vector<SceneItem> buildDevelopment(const BuildContext& c, const MineDevelopment& d);
std::vector<SceneItem> buildControlPoint(const BuildContext& c, const SurveyControlPoint& p);
std::vector<SceneItem> buildDrillhole(const BuildContext& c, const Drillhole& h);
std::vector<SceneItem> buildBlockModel(const BuildContext& c, const BlockModel& b);
std::vector<SceneItem> buildStructure(const BuildContext& c, const StructuralMeasurement& s);
std::vector<SceneItem> buildClassification(const BuildContext& c, const GeotechClassification& g);
/// Outline box of a point-cloud dataset (shown while its points stream in, and when selected).
std::vector<SceneItem> buildPointCloudBox(const BuildContext& c, const PointCloudDataset& d);

// ---- poly data helpers (also used by the viewport for overlays) ---------------------------------
vtkSmartPointer<vtkPolyData> polylinesData(const std::vector<std::vector<Vec3>>& lines, const coordinates::RenderOrigin& o);
vtkSmartPointer<vtkPolyData> segmentsData(const std::vector<std::pair<Vec3, Vec3>>& segments, const coordinates::RenderOrigin& o);
vtkSmartPointer<vtkPolyData> meshPolyData(const MeshData& mesh, const coordinates::RenderOrigin& o, bool computeNormals = true,
                                          double featureAngleDeg = 180.0);
vtkSmartPointer<vtkPolyData> pointsData(const std::vector<Vec3>& pts, const coordinates::RenderOrigin& o);
/// Vector-font text in the horizontal plane through `insertion` (world), cap height `height`.
vtkSmartPointer<vtkPolyData> textData(const std::string& text, Vec3 insertion, double height, double rotation, TextAlign align,
                                      const coordinates::RenderOrigin& o);
/// Splits polylines into dash segments for a CAD linetype ("Dashed", "Dotted", "DashDot";
/// "Continuous" returns the input).
std::vector<std::vector<Vec3>> applyLinetype(const std::vector<std::vector<Vec3>>& lines, const std::string& lineType, double scale);

} // namespace openmine::app::scene
