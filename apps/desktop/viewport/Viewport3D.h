#pragma once
// SPDX-License-Identifier: Apache-2.0
// The single shared 3D viewport (VTK). Shows CAD geometry, development
// excavation envelopes and planned-vs-actual progress, supports picking and
// interactive line/polyline drawing on a horizontal work plane.

#include "../AppContext.h"
#include "openmine/coordinates/RenderOrigin.h"

#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

class QVTKOpenGLNativeWidget;

namespace openmine::app {

class ViewportInteractorStyle;

class Viewport3D : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, DrawLine, DrawPolyline };

    explicit Viewport3D(AppContext& ctx, QWidget* parent = nullptr);
    ~Viewport3D() override;

    Tool tool() const { return tool_; }
    void setTool(Tool t);
    void finishDrawing();
    void cancelDrawing();
    const std::vector<Vec3>& pendingPoints() const { return drawPoints_; }

    void setWorkPlaneZ(double z);
    double workPlaneZ() const { return workPlaneZ_; }
    void setGridSnap(double step) { gridSnap_ = step; } ///< 0 disables
    double gridSnap() const { return gridSnap_; }
    void setEndpointSnap(bool on) { endpointSnap_ = on; }
    bool endpointSnap() const { return endpointSnap_; }

    void setView(const QString& name); ///< "top", "front", "side", "iso"
    void fitAll();
    void setPerspective(bool on);
    bool perspective() const { return perspective_; }
    void refreshTheme();
    void render();

    /// Convert a widget-local (Qt) position to the work-plane point in world coordinates.
    std::optional<Vec3> pickWorkPlane(QPoint qtPos) const;

    // Test hooks used by the smoke test (simulate user clicks without an event loop).
    void simulateClick(QPoint qtPos);
    /// Project a world point to widget-local (Qt) coordinates.
    QPoint worldToWidget(Vec3 world) const;
    QVTKOpenGLNativeWidget* vtkWidget() const { return vtk_; }

signals:
    void drawingFinished(std::vector<openmine::Vec3> points, bool isLine);
    void cursorMoved(openmine::Vec3 world, bool onPlane);
    void toolChanged();

private:
    friend class ViewportInteractorStyle;
    void rebuildScene();
    void scheduleRebuild();
    void updateSelectionStyle();
    void updateRubberBand(std::optional<Vec3> cursor);
    void buildGrid();
    void onClick(int vtkX, int vtkY);
    void onMove(int vtkX, int vtkY);
    std::optional<Vec3> planePointFromVtk(int x, int y) const;
    Vec3 applySnaps(Vec3 world, int vtkX, int vtkY) const;
    void addDrawPoint(Vec3 world);

    AppContext& ctx_;
    QVTKOpenGLNativeWidget* vtk_ = nullptr;
    struct Impl;
    std::unique_ptr<Impl> d_;

    coordinates::RenderOrigin origin_;
    bool originSet_ = false;
    Tool tool_ = Tool::Select;
    std::vector<Vec3> drawPoints_;
    double workPlaneZ_ = 0.0;
    double gridSnap_ = 1.0;
    bool endpointSnap_ = true;
    bool perspective_ = true;
    bool rebuildPending_ = false;
    bool firstFit_ = true;
};

} // namespace openmine::app
