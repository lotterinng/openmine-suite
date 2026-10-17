// SPDX-License-Identifier: Apache-2.0
#include "Viewport3D.h"

#include "../theme/Theme.h"
#include "openmine/geometry/Polyline.h"

#include <QTimer>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkActor.h>
#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCellPicker.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace openmine::app {

namespace {
QColor rgbColor(std::uint32_t rgb) { return QColor((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }
void setColor(vtkActor* a, const QColor& c) { a->GetProperty()->SetColor(c.redF(), c.greenF(), c.blueF()); }

vtkSmartPointer<vtkActor> makeActor(vtkPolyData* pd) {
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(pd);
    auto a = vtkSmartPointer<vtkActor>::New();
    a->SetMapper(mapper);
    return a;
}

vtkSmartPointer<vtkPolyData> polylineData(const std::vector<Vec3>& local, bool closed) {
    auto pd = vtkSmartPointer<vtkPolyData>::New();
    vtkNew<vtkPoints> pts;
    pts->SetDataTypeToDouble();
    for (const auto& p : local) pts->InsertNextPoint(p.x, p.y, p.z);
    vtkNew<vtkCellArray> lines;
    const auto n = static_cast<vtkIdType>(local.size());
    lines->InsertNextCell(closed ? n + 1 : n);
    for (vtkIdType i = 0; i < n; ++i) lines->InsertCellPoint(i);
    if (closed) lines->InsertCellPoint(0);
    pd->SetPoints(pts);
    pd->SetLines(lines);
    return pd;
}

/// Rectangular excavation envelope with vertical walls, floor on the centreline.
vtkSmartPointer<vtkPolyData> tunnelData(const std::vector<Vec3>& c, double w, double h) {
    auto pd = vtkSmartPointer<vtkPolyData>::New();
    vtkNew<vtkPoints> pts;
    pts->SetDataTypeToDouble();
    vtkNew<vtkCellArray> polys;
    for (std::size_t i = 1; i < c.size(); ++i) {
        const Vec3 a = c[i - 1], b = c[i];
        const Vec3 d = b - a;
        const double hl = d.horizontalLength();
        if (hl < 1e-9) continue;
        const Vec3 side{-d.y / hl * w / 2, d.x / hl * w / 2, 0};
        const Vec3 up{0, 0, h};
        const Vec3 q[8] = {a - side, a + side, a + side + up, a - side + up,
                           b - side, b + side, b + side + up, b - side + up};
        const vtkIdType base = pts->GetNumberOfPoints();
        for (const auto& p : q) pts->InsertNextPoint(p.x, p.y, p.z);
        const int faces[6][4] = {{0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}, {0, 3, 2, 1}, {4, 5, 6, 7}};
        for (const auto& f : faces) {
            vtkIdType ids[4] = {base + f[0], base + f[1], base + f[2], base + f[3]};
            polys->InsertNextCell(4, ids);
        }
    }
    pd->SetPoints(pts);
    pd->SetPolys(polys);
    return pd;
}

double niceStep(double span) {
    const double raw = std::max(span / 20.0, 1e-3);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double f : {1.0, 2.0, 5.0, 10.0})
        if (raw <= f * mag) return f * mag;
    return 10 * mag;
}
} // namespace

// ---------------------------------------------------------------------------
class ViewportInteractorStyle : public vtkInteractorStyleTrackballCamera {
public:
    static ViewportInteractorStyle* New();
    vtkTypeMacro(ViewportInteractorStyle, vtkInteractorStyleTrackballCamera);
    Viewport3D* view = nullptr;

    void OnLeftButtonDown() override {
        auto* i = GetInteractor();
        downX_ = i->GetEventPosition()[0];
        downY_ = i->GetEventPosition()[1];
        if (view->tool() != Viewport3D::Tool::Select) return; // drawing: no rotation
        vtkInteractorStyleTrackballCamera::OnLeftButtonDown();
    }
    void OnLeftButtonUp() override {
        auto* i = GetInteractor();
        const int x = i->GetEventPosition()[0], y = i->GetEventPosition()[1];
        if (view->tool() == Viewport3D::Tool::Select) vtkInteractorStyleTrackballCamera::OnLeftButtonUp();
        if (std::abs(x - downX_) <= 3 && std::abs(y - downY_) <= 3) view->onClick(x, y);
    }
    void OnRightButtonDown() override {
        if (view->tool() != Viewport3D::Tool::Select) {
            view->finishDrawing();
            return;
        }
        vtkInteractorStyleTrackballCamera::OnRightButtonDown();
    }
    void OnMouseMove() override {
        auto* i = GetInteractor();
        view->onMove(i->GetEventPosition()[0], i->GetEventPosition()[1]);
        vtkInteractorStyleTrackballCamera::OnMouseMove();
    }
    void OnKeyPress() override {
        const std::string key = GetInteractor()->GetKeySym() ? GetInteractor()->GetKeySym() : "";
        if (key == "Escape") view->cancelDrawing();
        else if (key == "Return" || key == "KP_Enter") view->finishDrawing();
    }
    void OnChar() override {} // suppress VTK default single-key shortcuts (w/s/q/e…)

private:
    int downX_ = 0, downY_ = 0;
};
vtkStandardNewMacro(ViewportInteractorStyle);

struct Viewport3D::Impl {
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> window;
    vtkSmartPointer<vtkRenderer> renderer;
    vtkSmartPointer<vtkOrientationMarkerWidget> axes;
    vtkSmartPointer<vtkCellPicker> picker;
    vtkSmartPointer<ViewportInteractorStyle> style;
    std::vector<vtkSmartPointer<vtkActor>> sceneActors;
    std::map<vtkActor*, Selection> pickMap;
    std::map<Uuid, vtkSmartPointer<vtkActor>> geometryActors;
    std::map<Uuid, vtkSmartPointer<vtkActor>> tunnelActors;
    vtkSmartPointer<vtkActor> grid;
    vtkSmartPointer<vtkActor> rubber;
    vtkSmartPointer<vtkActor> selectionVertices;
};

Viewport3D::Viewport3D(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx), d_(std::make_unique<Impl>()) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    vtk_ = new QVTKOpenGLNativeWidget(this);
    lay->addWidget(vtk_);

    d_->window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    d_->window->SetMultiSamples(4);
    vtk_->setRenderWindow(d_->window);
    d_->renderer = vtkSmartPointer<vtkRenderer>::New();
    d_->renderer->GradientBackgroundOn();
    d_->window->AddRenderer(d_->renderer);

    d_->style = vtkSmartPointer<ViewportInteractorStyle>::New();
    d_->style->view = this;
    d_->window->GetInteractor()->SetInteractorStyle(d_->style);

    d_->picker = vtkSmartPointer<vtkCellPicker>::New();
    d_->picker->SetTolerance(0.008);

    vtkNew<vtkAxesActor> axesActor;
    axesActor->SetShaftTypeToCylinder();
    axesActor->SetXAxisLabelText("E");
    axesActor->SetYAxisLabelText("N");
    axesActor->SetZAxisLabelText("Z");
    d_->axes = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
    d_->axes->SetOrientationMarker(axesActor);
    d_->axes->SetInteractor(d_->window->GetInteractor());
    d_->axes->SetViewport(0.0, 0.0, 0.14, 0.2);
    d_->axes->SetEnabled(1);
    d_->axes->InteractiveOff();

    refreshTheme();
    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
        originSet_ = false;
        firstFit_ = true;
        drawPoints_.clear();
        rebuildScene();
    });
    connect(&ctx_, &AppContext::modelChanged, this, &Viewport3D::scheduleRebuild);
    connect(&ctx_, &AppContext::scheduleRecalculated, this, &Viewport3D::scheduleRebuild);
    connect(&ctx_, &AppContext::selectionChanged, this, [this] {
        updateSelectionStyle();
        render();
    });
    setView("iso");
}

Viewport3D::~Viewport3D() = default;

void Viewport3D::render() { d_->window->Render(); vtk_->update(); }

void Viewport3D::refreshTheme() {
    const auto& c = Theme::colors();
    d_->renderer->SetBackground(c.viewportBottom.redF(), c.viewportBottom.greenF(), c.viewportBottom.blueF());
    d_->renderer->SetBackground2(c.viewportTop.redF(), c.viewportTop.greenF(), c.viewportTop.blueF());
    rebuildScene();
}

void Viewport3D::scheduleRebuild() {
    if (rebuildPending_) return;
    rebuildPending_ = true;
    QTimer::singleShot(0, this, [this] {
        rebuildPending_ = false;
        rebuildScene();
    });
}

void Viewport3D::rebuildScene() {
    auto& R = *d_->renderer;
    for (auto& a : d_->sceneActors) R.RemoveActor(a);
    d_->sceneActors.clear();
    d_->pickMap.clear();
    d_->geometryActors.clear();
    d_->tunnelActors.clear();
    if (d_->grid) R.RemoveActor(d_->grid);
    d_->grid = nullptr;

    if (!ctx_.hasProject()) {
        render();
        return;
    }
    const auto& m = ctx_.project().model();
    const auto& colors = Theme::colors();

    // Render origin: chosen once from the data, re-chosen only if data is far away.
    std::vector<Vec3> all;
    for (const auto& g : m.geometry.all()) all.insert(all.end(), g.vertices.begin(), g.vertices.end());
    if (auto bb = geometry::bounds(all)) {
        const Vec3 c = bb->center();
        if (!originSet_ || (c - origin_.origin()).length() > 50000.0) {
            origin_ = coordinates::RenderOrigin::around(c);
            if (!originSet_) workPlaneZ_ = bb->min.z;
            originSet_ = true;
            firstFit_ = true;
        }
    }

    for (const auto& g : m.geometry.all()) {
        const auto* layer = m.layers.find(g.layerId);
        if (layer && !layer->visible) continue;
        std::vector<Vec3> local;
        local.reserve(g.vertices.size());
        for (const auto& v : g.vertices) local.push_back(origin_.toLocal(v));
        auto actor = makeActor(polylineData(local, g.closed));
        setColor(actor, rgbColor(layer ? layer->colorRgb : 0xD0D0D0));
        actor->GetProperty()->SetLineWidth(2.5f);
        actor->GetProperty()->SetRenderLinesAsTubes(true);
        actor->GetProperty()->SetPointSize(7.f);
        R.AddActor(actor);
        d_->sceneActors.push_back(actor);
        d_->geometryActors[g.id] = actor;
        const auto* dev = m.developmentForGeometry(g.id);
        d_->pickMap[actor] = dev ? Selection{EntityKind::Development, dev->id} : Selection{EntityKind::Geometry, g.id};

        if (!dev) continue;
        // Excavation envelope, coloured by schedule state.
        auto tunnel = makeActor(tunnelData(local, dev->designWidth, dev->designHeight));
        QColor tc = rgbColor(layer ? layer->colorRgb : 0x3FA9F5);
        double actual = 0, planned = 0;
        bool flagged = false;
        for (const auto& aid : m.activitiesForDevelopment(dev->id)) {
            if (const auto* a = m.activities.find(aid)) {
                actual += a->actualQuantity;
                planned += a->quantity;
                flagged = flagged || a->designImpactFlag;
            }
        }
        if (flagged) tc = colors.warning;
        setColor(tunnel, tc);
        tunnel->GetProperty()->SetOpacity(0.22);
        tunnel->GetProperty()->SetEdgeVisibility(false);
        R.AddActor(tunnel);
        d_->sceneActors.push_back(tunnel);
        d_->tunnelActors[dev->id] = tunnel;
        d_->pickMap[tunnel] = Selection{EntityKind::Development, dev->id};

        // Actual advance from the start of the centreline (completed vs planned).
        const double len = geometry::length3d(local, g.closed);
        if (actual > 0 && len > 0) {
            const double s = std::min(actual, len);
            std::vector<Vec3> done{local.front()};
            double acc = 0;
            for (std::size_t i = 1; i < local.size(); ++i) {
                const double seg = distance(local[i - 1], local[i]);
                if (acc + seg >= s) {
                    done.push_back(*geometry::pointAtChainage(local, s));
                    break;
                }
                done.push_back(local[i]);
                acc += seg;
            }
            for (auto& p : done) p.z += 0.05;
            auto prog = makeActor(polylineData(done, false));
            setColor(prog, colors.ganttProgress);
            prog->GetProperty()->SetLineWidth(7.f);
            prog->GetProperty()->SetRenderLinesAsTubes(true);
            prog->PickableOff();
            R.AddActor(prog);
            d_->sceneActors.push_back(prog);
        }
    }
    buildGrid();
    updateSelectionStyle();
    updateRubberBand(std::nullopt);
    if (firstFit_ && !all.empty()) {
        firstFit_ = false;
        fitAll();
    }
    render();
}

void Viewport3D::buildGrid() {
    const auto& m = ctx_.project().model();
    std::vector<Vec3> all;
    for (const auto& g : m.geometry.all())
        for (const auto& v : g.vertices) all.push_back(origin_.toLocal(v));
    for (const auto& p : drawPoints_) all.push_back(origin_.toLocal(p));
    double minX = -100, maxX = 100, minY = -100, maxY = 100;
    if (auto bb = geometry::bounds(all)) {
        const double pad = std::max({bb->size().x, bb->size().y, 50.0}) * 0.25;
        minX = bb->min.x - pad;
        maxX = bb->max.x + pad;
        minY = bb->min.y - pad;
        maxY = bb->max.y + pad;
    }
    const double step = niceStep(std::max(maxX - minX, maxY - minY));
    minX = std::floor(minX / step) * step;
    maxX = std::ceil(maxX / step) * step;
    minY = std::floor(minY / step) * step;
    maxY = std::ceil(maxY / step) * step;
    const double z = workPlaneZ_ - origin_.origin().z;

    auto pd = vtkSmartPointer<vtkPolyData>::New();
    vtkNew<vtkPoints> pts;
    vtkNew<vtkCellArray> lines;
    auto addLine = [&](double x0, double y0, double x1, double y1) {
        vtkIdType a = pts->InsertNextPoint(x0, y0, z), b = pts->InsertNextPoint(x1, y1, z);
        vtkIdType ids[2] = {a, b};
        lines->InsertNextCell(2, ids);
    };
    for (double x = minX; x <= maxX + 1e-9; x += step) addLine(x, minY, x, maxY);
    for (double y = minY; y <= maxY + 1e-9; y += step) addLine(minX, y, maxX, y);
    pd->SetPoints(pts);
    pd->SetLines(lines);
    d_->grid = makeActor(pd);
    setColor(d_->grid, Theme::colors().grid);
    d_->grid->GetProperty()->SetOpacity(0.45);
    d_->grid->PickableOff();
    d_->renderer->AddActor(d_->grid);
}

void Viewport3D::updateSelectionStyle() {
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    const auto& sel = ctx_.selection();
    const auto& colors = Theme::colors();

    // Which geometry / development is highlighted (activity selection highlights its design).
    std::vector<Uuid> hiGeom, hiDev;
    if (sel.kind == EntityKind::Geometry) hiGeom.push_back(sel.id);
    if (sel.kind == EntityKind::Development) hiDev.push_back(sel.id);
    if (sel.kind == EntityKind::Activity)
        for (const auto& d : m.developmentsForActivity(sel.id)) hiDev.push_back(d);
    for (const auto& d : hiDev)
        if (const auto* dev = m.developments.find(d)) hiGeom.push_back(dev->geometryId);

    for (auto& [id, actor] : d_->geometryActors) {
        const bool hi = std::find(hiGeom.begin(), hiGeom.end(), id) != hiGeom.end();
        const auto* g = m.geometry.find(id);
        const auto* layer = g ? m.layers.find(g->layerId) : nullptr;
        setColor(actor, hi ? colors.selection : rgbColor(layer ? layer->colorRgb : 0xD0D0D0));
        actor->GetProperty()->SetLineWidth(hi ? 5.f : 2.5f);
        actor->GetProperty()->SetVertexVisibility(hi);
        actor->GetProperty()->SetVertexColor(colors.accent.redF(), colors.accent.greenF(), colors.accent.blueF());
    }
    for (auto& [id, actor] : d_->tunnelActors) {
        const bool hi = std::find(hiDev.begin(), hiDev.end(), id) != hiDev.end();
        actor->GetProperty()->SetOpacity(hi ? 0.4 : 0.22);
    }
}

void Viewport3D::updateRubberBand(std::optional<Vec3> cursor) {
    if (d_->rubber) d_->renderer->RemoveActor(d_->rubber);
    d_->rubber = nullptr;
    if (drawPoints_.empty()) return;
    std::vector<Vec3> local;
    for (const auto& p : drawPoints_) local.push_back(origin_.toLocal(p));
    if (cursor) local.push_back(origin_.toLocal(*cursor));
    if (local.size() < 2) local.push_back(local.front());
    d_->rubber = makeActor(polylineData(local, false));
    setColor(d_->rubber, Theme::colors().accent);
    d_->rubber->GetProperty()->SetLineWidth(2.f);
    d_->rubber->GetProperty()->SetLineStipplePattern(0xF0F0);
    d_->rubber->GetProperty()->SetVertexVisibility(true);
    d_->rubber->GetProperty()->SetPointSize(8.f);
    d_->rubber->PickableOff();
    d_->renderer->AddActor(d_->rubber);
}

std::optional<Vec3> Viewport3D::planePointFromVtk(int x, int y) const {
    auto* r = d_->renderer.Get();
    double nearP[4], farP[4];
    r->SetDisplayPoint(x, y, 0.0);
    r->DisplayToWorld();
    r->GetWorldPoint(nearP);
    r->SetDisplayPoint(x, y, 1.0);
    r->DisplayToWorld();
    r->GetWorldPoint(farP);
    if (nearP[3] == 0 || farP[3] == 0) return std::nullopt;
    for (int i = 0; i < 3; ++i) {
        nearP[i] /= nearP[3];
        farP[i] /= farP[3];
    }
    const double z = workPlaneZ_ - origin_.origin().z;
    const double dz = farP[2] - nearP[2];
    if (std::abs(dz) < 1e-12) return std::nullopt; // ray parallel to the work plane
    const double t = (z - nearP[2]) / dz;
    if (t < 0 || t > 1) return std::nullopt;
    const Vec3 local{nearP[0] + t * (farP[0] - nearP[0]), nearP[1] + t * (farP[1] - nearP[1]), z};
    return origin_.toWorld(local);
}

std::optional<Vec3> Viewport3D::pickWorkPlane(QPoint q) const {
    const double dpr = vtk_->devicePixelRatioF();
    return planePointFromVtk(static_cast<int>(q.x() * dpr), static_cast<int>((vtk_->height() - q.y() - 1) * dpr));
}

Vec3 Viewport3D::applySnaps(Vec3 w, int vx, int vy) const {
    if (endpointSnap_ && ctx_.hasProject()) {
        // Object snap: nearest existing vertex within 12 px on screen.
        double best = 12.0 * vtk_->devicePixelRatioF();
        std::optional<Vec3> hit;
        auto* r = d_->renderer.Get();
        auto test = [&](const Vec3& v) {
            const Vec3 l = origin_.toLocal(v);
            r->SetWorldPoint(l.x, l.y, l.z, 1.0);
            r->WorldToDisplay();
            double dp[3];
            r->GetDisplayPoint(dp);
            const double dd = std::hypot(dp[0] - vx, dp[1] - vy);
            if (dd < best) {
                best = dd;
                hit = v;
            }
        };
        for (const auto& g : ctx_.project().model().geometry.all())
            for (const auto& v : g.vertices) test(v);
        for (const auto& v : drawPoints_) test(v);
        if (hit) return *hit;
    }
    if (gridSnap_ > 0) {
        w.x = std::round(w.x / gridSnap_) * gridSnap_;
        w.y = std::round(w.y / gridSnap_) * gridSnap_;
    }
    return w;
}

void Viewport3D::onMove(int x, int y) {
    auto p = planePointFromVtk(x, y);
    if (p) {
        const Vec3 snapped = tool_ == Tool::Select ? *p : applySnaps(*p, x, y);
        emit cursorMoved(snapped, true);
        if (tool_ != Tool::Select && !drawPoints_.empty()) {
            updateRubberBand(snapped);
            d_->window->Render();
        }
    } else {
        emit cursorMoved({}, false);
    }
}

void Viewport3D::onClick(int x, int y) {
    if (tool_ == Tool::Select) {
        if (d_->picker->Pick(x, y, 0, d_->renderer)) {
            auto it = d_->pickMap.find(d_->picker->GetActor());
            if (it != d_->pickMap.end()) {
                ctx_.select(it->second.kind, it->second.id);
                return;
            }
        }
        ctx_.clearSelection();
        return;
    }
    auto p = planePointFromVtk(x, y);
    if (!p) {
        ctx_.report("Click is not on the work plane (rotate the view so the plane is visible)", true);
        return;
    }
    addDrawPoint(applySnaps(*p, x, y));
}

void Viewport3D::addDrawPoint(Vec3 w) {
    if (!drawPoints_.empty() && distance(drawPoints_.back(), w) < 1e-9) return; // ignore duplicate clicks
    drawPoints_.push_back(w);
    ctx_.report(QStringLiteral("Point %1: %2, %3, %4")
                    .arg(drawPoints_.size())
                    .arg(formatNumber(w.x), formatNumber(w.y), formatNumber(w.z)));
    if (tool_ == Tool::DrawLine && drawPoints_.size() == 2) {
        finishDrawing();
        return;
    }
    updateRubberBand(std::nullopt);
    render();
}

void Viewport3D::simulateClick(QPoint q) {
    const double dpr = vtk_->devicePixelRatioF();
    onClick(static_cast<int>(q.x() * dpr), static_cast<int>((vtk_->height() - q.y() - 1) * dpr));
}

QPoint Viewport3D::worldToWidget(Vec3 world) const {
    const Vec3 l = origin_.toLocal(world);
    auto* r = d_->renderer.Get();
    r->SetWorldPoint(l.x, l.y, l.z, 1.0);
    r->WorldToDisplay();
    double dp[3];
    r->GetDisplayPoint(dp);
    const double dpr = vtk_->devicePixelRatioF();
    return QPoint(static_cast<int>(std::lround(dp[0] / dpr)), static_cast<int>(std::lround(vtk_->height() - 1 - dp[1] / dpr)));
}

void Viewport3D::setTool(Tool t) {
    if (t != tool_) cancelDrawing();
    tool_ = t;
    if (t == Tool::DrawLine) ctx_.report("LINE: click two points on the work plane (Esc cancels).");
    if (t == Tool::DrawPolyline) ctx_.report("POLYLINE: click points; right-click or Enter finishes, Esc cancels.");
    vtk_->setCursor(t == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    emit toolChanged();
}

void Viewport3D::finishDrawing() {
    if (tool_ == Tool::Select) return;
    const bool isLine = tool_ == Tool::DrawLine;
    auto pts = drawPoints_;
    drawPoints_.clear();
    updateRubberBand(std::nullopt);
    render();
    if (pts.size() >= 2) emit drawingFinished(pts, isLine && pts.size() == 2);
    else if (!pts.empty()) ctx_.report("At least two points are needed; nothing created.", true);
    if (isLine) setTool(Tool::Select);
}

void Viewport3D::cancelDrawing() {
    if (drawPoints_.empty()) return;
    drawPoints_.clear();
    updateRubberBand(std::nullopt);
    render();
    ctx_.report("Drawing cancelled.");
}

void Viewport3D::setWorkPlaneZ(double z) {
    workPlaneZ_ = z;
    rebuildScene();
}

void Viewport3D::setView(const QString& name) {
    auto* cam = d_->renderer->GetActiveCamera();
    double fp[3] = {0, 0, 0};
    cam->SetFocalPoint(fp);
    if (name == "top") {
        cam->SetPosition(0, 0, 1);
        cam->SetViewUp(0, 1, 0);
    } else if (name == "front") {
        cam->SetPosition(0, -1, 0);
        cam->SetViewUp(0, 0, 1);
    } else if (name == "side") {
        cam->SetPosition(1, 0, 0);
        cam->SetViewUp(0, 0, 1);
    } else {
        cam->SetPosition(1, -1, 0.8);
        cam->SetViewUp(0, 0, 1);
    }
    fitAll();
}

void Viewport3D::fitAll() {
    d_->renderer->ResetCamera();
    d_->renderer->ResetCameraClippingRange();
    render();
}

void Viewport3D::setPerspective(bool on) {
    perspective_ = on;
    d_->renderer->GetActiveCamera()->SetParallelProjection(!on);
    fitAll();
}

} // namespace openmine::app
