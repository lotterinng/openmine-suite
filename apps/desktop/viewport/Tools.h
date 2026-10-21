#pragma once
// SPDX-License-Identifier: Apache-2.0
// Interactive CAD tools of the viewports: drawing (point, line, polyline, polygon, rectangle,
// circle, 3-point arc, spline, text, dimension), measuring (distance, area), editing (move, copy,
// rotate, mirror, scale, offset, trim, extend, fillet, split), grip editing of the selected
// objects' defining points (vertices; arc and circle centre, radius and ends) and picking (points
// for dialogs, section lines, crop polygons).
//
// Points come from object snaps (modules/cad/Snap: endpoint, midpoint, centre, intersection,
// perpendicular, tangent, nearest, node, grid), from surfaces and point clouds under the cursor
// (surface snap), or from the view's work plane; ortho mode constrains to plan right angles.
// Coordinates and values can also be typed in the command console while a tool runs
// (“x,y,z”, “@dx,dy[,dz]” relative, “@distance<bearing” polar, or a number). Every result is
// applied through ProjectEditor (validated, one undo step) — tools never write the model directly.

#include "../app/AppContext.h"

#include "openmine/cad/Edit.h"
#include "openmine/cad/Snap.h"

#include <QObject>
#include <QPointer>

#include <functional>
#include <optional>
#include <vector>

namespace openmine::app {

class Viewport3D;

enum class Tool {
    Select,
    Point, Line, Polyline, Polygon, Rectangle, Circle, Arc3, Spline, Text, Dimension,
    Measure, MeasureArea,
    Move, Copy, Rotate, Mirror, Scale, Offset, Trim, Extend, Fillet, Chamfer, Split,
    PickPoint, PickPoints, SectionLine, CropPolygon,
    Grip, ///< moving one grip of a selected object (entered by clicking a grip while selecting)
};
const char* toolName(Tool t);

class ToolController : public QObject {
    Q_OBJECT
public:
    explicit ToolController(AppContext& ctx, QObject* parent = nullptr);

    Tool tool() const { return tool_; }
    void setTool(Tool t);
    bool isActive() const { return tool_ != Tool::Select; }
    /// True while a tool needs left-button drags for itself (none do at present: drags navigate).
    bool capturesDrag() const { return false; }
    /// Finishes multi-point tools (Enter / right-click), cancels (Escape).
    void finish();
    void cancel();
    void undoLastPoint();

    /// One-shot point request (dialogs). The tool returns to the previous one afterwards.
    void requestPoint(const QString& prompt, std::function<void(Vec3)> done, bool includeStored = false);
    /// Point request that also reports the point-cloud stored coordinates and dataset (registration).
    void requestCloudPoint(const QString& prompt, std::function<void(Uuid dataset, Vec3 stored, Vec3 project)> done);
    /// Repeated point requests until finish/cancel.
    void requestPoints(const QString& prompt, std::function<void(std::vector<Vec3>)> done);

    // ---- settings --------------------------------------------------------------------------------
    cad::SnapSettings& snapSettings() { return snap_; }
    bool snapEnabled() const { return snapOn_; }
    void setSnapEnabled(bool on) { snapOn_ = on; emit settingsChanged(); }
    bool surfaceSnap() const { return surfaceSnap_; }
    void setSurfaceSnap(bool on) { surfaceSnap_ = on; emit settingsChanged(); }
    bool ortho() const { return ortho_; }
    void setOrtho(bool on) { ortho_ = on; emit settingsChanged(); }
    double textHeight() const { return textHeight_; }
    void setTextHeight(double h) { textHeight_ = h; }
    double filletRadius() const { return filletRadius_; }
    void setFilletRadius(double r) { filletRadius_ = r; }
    std::pair<double, double> chamferDistances() const { return {chamferA_, chamferB_}; }
    void setChamferDistances(double a, double b) {
        chamferA_ = a;
        chamferB_ = b;
    }
    double offsetDistance() const { return offsetDistance_; }
    void setOffsetDistance(double d) { offsetDistance_ = d; }

    // ---- grips ---------------------------------------------------------------------------------
    struct ObjectGrip {
        Uuid object;
        std::size_t index = 0; ///< index into cad::grips(object)
        cad::Grip grip;
    };
    /// Grips of the selected objects on unlocked layers (at most kMaxGrips; none while a tool runs).
    std::vector<ObjectGrip> selectionGrips() const;
    static constexpr std::size_t kMaxGrips = 5000;
    /// The grip within the snap aperture of `q` (nearest), if any.
    std::optional<ObjectGrip> gripAt(Viewport3D* view, QPoint q) const;
    /// A click while selecting: on a grip it starts moving that grip (returns true).
    bool gripClick(Viewport3D* view, QPoint q);
    /// The grip being moved (Grip tool).
    std::optional<ObjectGrip> hotGrip() const { return grip_; }

    // ---- typed input (command console) -------------------------------------------------------
    /// Parses typed coordinates or values for the running tool. Returns false when the text is
    /// not tool input (the console then treats it as a command).
    bool acceptInput(const QString& text);
    bool acceptPoint(Vec3 world);
    bool acceptValue(double v);
    QString prompt() const { return prompt_; }
    const std::vector<Vec3>& points() const { return pts_; }

    // ---- viewport events (Qt logical coordinates of `view`) ------------------------------------
    void press(Viewport3D* view, QPoint q, bool ctrl, bool shift);
    void move(Viewport3D* view, QPoint q, bool ctrl, bool shift);
    bool click(Viewport3D* view, QPoint q, bool ctrl, bool shift);
    bool rightClick(Viewport3D* view, QPoint q);
    bool key(Viewport3D* view, const std::string& key, bool ctrl, bool shift);
    void leave(Viewport3D* view);

    /// The snapped point for a cursor position (exposed for the smoke test and status bar).
    struct Resolved {
        Vec3 point;
        QString label;          ///< snap mode or "Surface" / "Plane"
        Uuid objectId;
        bool onCloud = false;
        Uuid cloudId;
        Vec3 stored;
    };
    std::optional<Resolved> resolve(Viewport3D* view, QPoint q) const;

signals:
    void toolChanged(openmine::app::Tool tool);
    void promptChanged(QString prompt);
    void settingsChanged();
    void snapped(QString label, openmine::Vec3 point);
    /// A section line was drawn (point on the plane, horizontal normal, length of the line).
    void sectionDefined(openmine::Vec3 point, openmine::Vec3 normal, double length);
    /// A crop polygon was closed (world vertices, the view's plane normal).
    void polygonDefined(std::vector<openmine::Vec3> polygon, openmine::Vec3 viewNormal);
    void measured(QString text);
    /// The displayed grips changed (selection, model, tool).
    void gripsChanged();

private:
    void addPoint(Viewport3D* view, const Resolved& r);
    void addPoint(Vec3 p);
    void updatePreview(Viewport3D* view, std::optional<Vec3> cursor);
    void clearPreview();
    void setPrompt(const QString& p);
    void complete();                 ///< the tool has all its input: apply
    void restart();                  ///< ready for the next object of the same tool
    void createObject(GeometryObject g, const QString& what);
    bool needsSelection() const;
    std::vector<const GeometryObject*> selectedObjects() const;
    std::optional<Uuid> objectAt(Viewport3D* view, QPoint q) const;
    void applyTransform(const Transform3& t, bool copy, const QString& what);

    AppContext& ctx_;
    Tool tool_ = Tool::Select;
    Tool returnTo_ = Tool::Select;
    std::vector<Vec3> pts_;
    std::vector<Uuid> picked_;        ///< objects picked by trim/extend/fillet/offset/split
    std::vector<Vec3> pickedAt_;
    QPointer<Viewport3D> lastView_;
    std::optional<Vec3> lastCursor_;
    QString prompt_;
    cad::SnapSettings snap_;
    bool snapOn_ = true;
    bool surfaceSnap_ = true;
    bool ortho_ = false;
    double textHeight_ = 2.0;
    double filletRadius_ = 5.0;
    double chamferA_ = 1.0, chamferB_ = 1.0;
    double offsetDistance_ = 2.5;
    std::optional<double> typedValue_;
    std::function<void(Vec3)> onPoint_;
    std::function<void(Uuid, Vec3, Vec3)> onCloudPoint_;
    std::function<void(std::vector<Vec3>)> onPoints_;
    std::optional<Resolved> lastResolved_;
    std::optional<ObjectGrip> grip_;
};

} // namespace openmine::app
