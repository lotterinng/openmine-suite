#pragma once
// SPDX-License-Identifier: Apache-2.0
// One 3D view of the shared scene (VTK). Several views can be open at once (ViewportArea); each
// has its own camera, projection, section slab, display options and point-cloud level of detail,
// and they share the scene items built by SceneCache.
//
// Navigation (CAD/mining conventions, Z up):
//   left drag      orbit (turntable about the point under the cursor) — plan/section: box select
//   middle drag    pan                     shift + left drag   pan
//   wheel          zoom about the cursor   right drag          zoom
//   ctrl + left drag (3D)  box select      click               select (ctrl toggles, shift adds)
// Box selection follows CAD rules: dragged left→right selects objects entirely inside the box
// (window); right→left also selects objects crossing it.

#include "SceneCache.h"

#include <QImage>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

class vtkActor;
class vtkProp;
class vtkRenderer;
class vtkCamera;

namespace openmine::app {

class QtVtkWidget;
class PointCloudStore;
class PointCloudView;
class ToolController;
class ViewInteractorStyle;

enum class ViewMode { Perspective, Plan, Section, Elevation };

struct SectionDef {
    bool enabled = false;
    Vec3 point;               ///< world
    Vec3 normal{0, 1, 0};     ///< unit, horizontal for vertical sections
    double halfWidth = 2.5;   ///< m either side of the plane
    double step = 5.0;        ///< m per step (page up / page down)
    std::string label;        ///< e.g. "Section 1+250 on LEG2_675_D3"
};

struct ViewOptions {
    bool grid = true;
    bool axes = true;
    bool scaleBar = true;
    bool legend = true;
    bool labels = true;
    bool pointClouds = true;
    bool ghostFuture = true;        ///< 4D: show not-yet-mined design as ghosts
    bool sectionOutlines = true;    ///< section mode: highlight cut lines of surfaces
    std::size_t pointBudget = 3'000'000;
    double pointScreenSpacing = 1.5;
};

/// Temporary graphics drawn by tools (rubber bands, previews, snap markers).
struct Overlay {
    std::vector<std::vector<Vec3>> lines; ///< world
    std::vector<Vec3> points;             ///< world
    std::uint32_t color = 0x00B4FF;
    double lineWidth = 2.0;
    double pointSize = 9.0;
    bool dashed = false;
    std::vector<std::pair<Vec3, std::string>> labels;
};

struct PickHit {
    SelectionItem owner;
    Uuid geometryId;
    Vec3 world;               ///< picked surface/curve point (project coordinates)
    bool onPointCloud = false;
    Vec3 stored;              ///< point clouds: stored coordinates of the picked point
    std::string part;
};

class Viewport3D : public QWidget {
    Q_OBJECT
public:
    Viewport3D(AppContext& ctx, SceneCache& scene, PointCloudStore& clouds, int id, QWidget* parent = nullptr);
    ~Viewport3D() override;

    int id() const { return id_; }
    AppContext& context() { return ctx_; }
    SceneCache& scene() { return scene_; }
    QtVtkWidget* vtkWidget() const { return vtk_; }
    vtkRenderer* renderer() const;

    // ---- camera ------------------------------------------------------------------------------
    ViewMode mode() const { return mode_; }
    void setMode(ViewMode m);
    /// "top", "bottom", "north", "south", "east", "west", "iso", "iso-ne", "iso-nw", "iso-sw".
    void setNamedView(const QString& name);
    QString viewName() const;
    void fitAll();
    void fitBounds(const BoundingBox& world, double margin = 1.15);
    void fitSelection();
    void zoomBy(double factor);
    void setPerspective(bool on);
    bool perspective() const;
    /// Camera as text (position, focal point, view up, parallel scale) and back (for saved views).
    QString cameraState() const;
    bool restoreCamera(const QString& state);

    // ---- section --------------------------------------------------------------------------------
    const SectionDef& section() const { return section_; }
    void setSection(const SectionDef& s);
    void stepSection(int steps);
    /// Section across a development at a chainage (perpendicular to the centreline).
    bool sectionAtChainage(const Uuid& developmentId, double chainage, double halfWidth = 2.5);

    // ---- work plane -----------------------------------------------------------------------------
    /// Plane new points are placed on: horizontal at `workPlaneZ` in plan/3D views, the section
    /// plane in section views, the vertical plane through the focal point in elevation views.
    std::pair<Vec3, Vec3> workPlane() const;
    double workPlaneZ() const { return workPlaneZ_; }
    void setWorkPlaneZ(double z);

    // ---- display -------------------------------------------------------------------------------
    ViewOptions& options() { return options_; }
    void applyOptions();
    void refreshTheme();
    /// Requests a repaint (coalesced).
    void render();
    /// Renders now and returns the image (screenshots, smoke test).
    QImage grab();

    // ---- picking / coordinates (Qt logical widget coordinates) --------------------------------
    std::optional<PickHit> pickAt(QPoint p, bool includePointClouds = true) const;
    std::optional<Vec3> workPlanePoint(QPoint p) const;
    /// World → widget (logical px); nullopt behind the camera.
    std::optional<QPointF> worldToWidget(Vec3 world) const;
    /// World size of one logical pixel at the focal plane.
    double pixelSize() const;
    /// Items (owners) inside / crossing a widget rectangle.
    std::vector<SelectionItem> itemsInRect(const QRect& r, bool crossing) const;

    // ---- tools -----------------------------------------------------------------------------------
    void setToolController(ToolController* tc) { tools_ = tc; }
    ToolController* toolController() const { return tools_; }
    void setOverlay(const std::string& key, const Overlay& o);
    void clearOverlay(const std::string& key);
    void clearOverlays();

    /// Statistics for the status bar.
    std::size_t displayedPoints() const;
    std::size_t pendingPointNodes() const;
    bool isActiveView() const { return active_; }
    void setActiveView(bool a);

    // Simulated input (automation and the GUI smoke test drive the same code paths as a user).
    void simulateClick(QPoint p, Qt::KeyboardModifiers mods = Qt::NoModifier, Qt::MouseButton button = Qt::LeftButton);
    void simulateMove(QPoint p);
    void simulateDrag(QPoint from, QPoint to, Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers mods = Qt::NoModifier);

signals:
    void cursorMoved(openmine::Vec3 world, bool valid);
    void activated(Viewport3D* view);
    void viewChanged();
    void contextMenuRequested(QPoint globalPos, std::optional<openmine::app::PickHit> hit);

private:
    friend class ViewInteractorStyle;
    struct Impl;
    std::unique_ptr<Impl> d_;

    // events (VTK display coordinates)
    void onPress(int button, int x, int y, bool ctrl, bool shift);
    void onRelease(int button, int x, int y, bool ctrl, bool shift);
    void onMove(int x, int y, bool ctrl, bool shift);
    void onWheel(int delta, int x, int y);
    void onKey(const std::string& key, bool ctrl, bool shift);
    void onLeave();

    void syncScene(const QSet<QString>& changed, bool all);
    void addEntityActors(const Uuid& id);
    void removeEntityActors(const Uuid& id);
    void restyleAll();
    void restyleEntity(const Uuid& id);
    void update4D();
    void updateGrid();
    void updateLegend();
    void updateAnnotation();
    void updatePointClouds();
    void updateSectionOutlines();
    void applyClipping();
    void cameraChanged();
    void orbit(double dxPx, double dyPx);
    void pan(double dxPx, double dyPx);
    void zoomAt(double factor, int x, int y);
    std::optional<Vec3> rayPlane(int x, int y, Vec3 planePoint, Vec3 planeNormal) const;
    std::optional<Vec3> pickLocal(int x, int y) const;
    QPoint toQt(int x, int y) const;
    void setRubberBand(const std::optional<QRect>& r, bool crossing);

    AppContext& ctx_;
    SceneCache& scene_;
    PointCloudStore& clouds_;
    int id_;
    QtVtkWidget* vtk_ = nullptr;
    ToolController* tools_ = nullptr;
    ViewMode mode_ = ViewMode::Perspective;
    SectionDef section_;
    ViewOptions options_;
    double workPlaneZ_ = 0;
    bool workPlaneSet_ = false;
    bool active_ = false;
    bool firstFit_ = true;
};

} // namespace openmine::app

