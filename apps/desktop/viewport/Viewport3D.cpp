// SPDX-License-Identifier: Apache-2.0
#include "Viewport3D.h"

#include "PointCloudLod.h"
#include "Tools.h"
#include "../theme/Theme.h"
#include "../vtk/QtVtkWidget.h"

#include "openmine/geometry/Polyline.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/mine_design/Development.h"
#include "openmine/planning/FourD.h"

#include <QMouseEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <vtkActor.h>
#include <vtkActor2D.h>
#include <vtkAxesActor.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkCamera.h>
#include <vtkCaptionActor2D.h>
#include <vtkCellArray.h>
#include <vtkCellPicker.h>
#include <vtkCornerAnnotation.h>
#include <vtkCutter.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkGenericRenderWindowInteractor.h>
#include <vtkInteractorStyle.h>
#include <vtkLegendScaleActor.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPlane.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkTextProperty.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace openmine::app {

namespace {

void setRgb(vtkProperty* p, std::uint32_t c) {
    p->SetColor(((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
}
std::uint32_t toRgb(const QColor& c) { return (static_cast<std::uint32_t>(c.red()) << 16) | (static_cast<std::uint32_t>(c.green()) << 8) | c.blue(); }

double niceStep(double span, int lines = 20) {
    const double raw = std::max(span / lines, 1e-3);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double f : {1.0, 2.0, 5.0, 10.0})
        if (raw <= f * mag) return f * mag;
    return 10 * mag;
}

vtkSmartPointer<vtkLookupTable> makeLut(const ScalarDisplay& sd) {
    auto lut = vtkSmartPointer<vtkLookupTable>::New();
    const int n = sd.lut == Lut::Categorical ? 12 : 256;
    lut->SetNumberOfTableValues(n);
    lut->SetRange(sd.min, sd.max);
    for (int i = 0; i < n; ++i) {
        const auto c = lutColor(sd.lut, n == 1 ? 0.0 : static_cast<double>(i) / (n - 1));
        lut->SetTableValue(i, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, 1.0);
    }
    lut->SetNanColor(0.45, 0.45, 0.45, 1.0);
    lut->Build();
    return lut;
}

} // namespace

// ---- interactor style: forwards everything to the viewport -------------------------------------------

class ViewInteractorStyle : public vtkInteractorStyle {
public:
    static ViewInteractorStyle* New();
    vtkTypeMacro(ViewInteractorStyle, vtkInteractorStyle)
    Viewport3D* view = nullptr;

    void OnLeftButtonDown() override { press(1); }
    void OnLeftButtonUp() override { release(1); }
    void OnMiddleButtonDown() override { press(2); }
    void OnMiddleButtonUp() override { release(2); }
    void OnRightButtonDown() override { press(3); }
    void OnRightButtonUp() override { release(3); }
    void OnMouseMove() override {
        auto* i = GetInteractor();
        view->onMove(i->GetEventPosition()[0], i->GetEventPosition()[1], i->GetControlKey() != 0, i->GetShiftKey() != 0);
    }
    void OnMouseWheelForward() override { wheel(1); }
    void OnMouseWheelBackward() override { wheel(-1); }
    void OnKeyPress() override {
        auto* i = GetInteractor();
        view->onKey(i->GetKeySym() ? i->GetKeySym() : "", i->GetControlKey() != 0, i->GetShiftKey() != 0);
    }
    void OnChar() override {}       // no VTK single-key shortcuts (w/s/e/q/3/…)
    void OnKeyRelease() override {}
    void OnLeave() override { view->onLeave(); }
    void OnEnter() override {}

private:
    void press(int b) {
        auto* i = GetInteractor();
        view->onPress(b, i->GetEventPosition()[0], i->GetEventPosition()[1], i->GetControlKey() != 0, i->GetShiftKey() != 0);
    }
    void release(int b) {
        auto* i = GetInteractor();
        view->onRelease(b, i->GetEventPosition()[0], i->GetEventPosition()[1], i->GetControlKey() != 0, i->GetShiftKey() != 0);
    }
    void wheel(int d) {
        auto* i = GetInteractor();
        view->onWheel(d, i->GetEventPosition()[0], i->GetEventPosition()[1]);
    }
};
vtkStandardNewMacro(ViewInteractorStyle);

// ---- implementation state ------------------------------------------------------------------------

struct Viewport3D::Impl {
    vtkSmartPointer<vtkRenderer> renderer;
    vtkSmartPointer<ViewInteractorStyle> style;
    vtkSmartPointer<vtkOrientationMarkerWidget> axes;
    vtkSmartPointer<vtkScalarBarActor> legend;
    vtkSmartPointer<vtkLegendScaleActor> scale;
    vtkSmartPointer<vtkCornerAnnotation> annotation;
    vtkSmartPointer<vtkActor> grid;
    vtkSmartPointer<vtkActor2D> rubber;
    vtkSmartPointer<vtkCellPicker> picker;
    std::unique_ptr<PointCloudView> clouds;

    struct Rec {
        vtkSmartPointer<vtkProp> prop;
        std::size_t index = 0;
        std::uint64_t revision = 0;
    };
    std::unordered_map<Uuid, std::vector<Rec>> actors;
    std::unordered_map<vtkProp*, std::pair<Uuid, std::size_t>> propIndex;
    std::map<std::string, std::vector<vtkSmartPointer<vtkProp>>> overlays;
    std::vector<vtkSmartPointer<vtkProp>> fourD;
    std::map<Uuid, std::pair<long, vtkSmartPointer<vtkPolyData>>> excavated; ///< dev → (fraction·1e5, mesh)
    std::vector<vtkSmartPointer<vtkProp>> sectionOutlines;
    std::set<Uuid> fourDHidden;   ///< envelopes replaced by 4D overlays
    std::map<Uuid, std::pair<std::uint32_t, double>> fourDStyle; ///< owner → colour, opacity

    // interaction
    int pressButton = 0;
    int pressX = 0, pressY = 0, lastX = 0, lastY = 0;
    bool pressCtrl = false, pressShift = false;
    enum class Drag { None, Pending, Orbit, Pan, Zoom, Box } drag = Drag::None;
    Vec3 pivot;              ///< render-local orbit pivot
    QTimer cameraTimer;
    bool sceneDirty = false;
};

// ---- construction ----------------------------------------------------------------------------------

Viewport3D::Viewport3D(AppContext& ctx, SceneCache& scene, PointCloudStore& clouds, int id, QWidget* parent)
    : QWidget(parent), d_(std::make_unique<Impl>()), ctx_(ctx), scene_(scene), clouds_(clouds), id_(id) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    vtk_ = new QtVtkWidget(this);
    lay->addWidget(vtk_);

    auto* win = vtk_->renderWindow();
    d_->renderer = vtkSmartPointer<vtkRenderer>::New();
    d_->renderer->GradientBackgroundOn();
    d_->renderer->SetUseDepthPeeling(true);
    d_->renderer->SetMaximumNumberOfPeels(6);
    d_->renderer->SetOcclusionRatio(0.05);
    win->SetAlphaBitPlanes(1);
    win->AddRenderer(d_->renderer);

    d_->style = vtkSmartPointer<ViewInteractorStyle>::New();
    d_->style->view = this;
    vtk_->interactor()->SetInteractorStyle(d_->style);

    d_->picker = vtkSmartPointer<vtkCellPicker>::New();
    d_->picker->SetTolerance(0.004);

    vtkNew<vtkAxesActor> axesActor;
    axesActor->SetShaftTypeToCylinder();
    axesActor->SetXAxisLabelText("E");
    axesActor->SetYAxisLabelText("N");
    axesActor->SetZAxisLabelText("Z");
    axesActor->SetTotalLength(1.2, 1.2, 1.2);
    d_->axes = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
    d_->axes->SetOrientationMarker(axesActor);
    d_->axes->SetInteractor(vtk_->interactor());
    d_->axes->SetViewport(0.0, 0.0, 0.13, 0.2);
    d_->axes->SetEnabled(1);
    d_->axes->InteractiveOff();

    d_->legend = vtkSmartPointer<vtkScalarBarActor>::New();
    d_->legend->SetNumberOfLabels(5);
    d_->legend->SetMaximumWidthInPixels(90);
    d_->legend->SetMaximumHeightInPixels(320);
    d_->legend->SetPosition(0.9, 0.12);
    d_->legend->SetWidth(0.08);
    d_->legend->SetHeight(0.6);
    d_->legend->VisibilityOff();
    d_->legend->GetTitleTextProperty()->SetFontSize(12);
    // Long titles ("Block model 1: est") grow to the left instead of out of the view.
    d_->legend->GetTitleTextProperty()->SetJustificationToRight();
    d_->legend->SetTextPositionToPrecedeScalarBar();
    d_->legend->GetLabelTextProperty()->SetFontSize(11);
    for (auto* tp : {d_->legend->GetTitleTextProperty(), d_->legend->GetLabelTextProperty()}) {
        tp->ShadowOff();
        tp->ItalicOff();
    }
    d_->legend->SetUnconstrainedFontSize(true);
    d_->renderer->AddViewProp(d_->legend);

    d_->scale = vtkSmartPointer<vtkLegendScaleActor>::New();
    d_->scale->AllAxesOff();
    d_->scale->LegendVisibilityOn();
    d_->scale->VisibilityOff();
    d_->renderer->AddViewProp(d_->scale);

    d_->annotation = vtkSmartPointer<vtkCornerAnnotation>::New();
    d_->annotation->SetMaximumFontSize(14);
    d_->annotation->SetLinearFontScaleFactor(2);
    d_->annotation->SetNonlinearFontScaleFactor(1);
    d_->annotation->GetTextProperty()->ShadowOff();
    d_->annotation->GetTextProperty()->BoldOn();
    d_->renderer->AddViewProp(d_->annotation);

    d_->clouds = std::make_unique<PointCloudView>(clouds_, d_->renderer, id_);

    d_->cameraTimer.setSingleShot(true);
    d_->cameraTimer.setInterval(40);
    connect(&d_->cameraTimer, &QTimer::timeout, this, [this] {
        updatePointClouds();
        updateGrid();
        updateAnnotation();
        render();
    });

    connect(&scene_, &SceneCache::sceneChanged, this, &Viewport3D::syncScene);
    connect(&scene_, &SceneCache::originChanged, this, [this](Vec3 delta) {
        // Keep the view where it was in world coordinates.
        auto* cam = d_->renderer->GetActiveCamera();
        double p[3], f[3];
        cam->GetPosition(p);
        cam->GetFocalPoint(f);
        cam->SetPosition(p[0] - delta.x, p[1] - delta.y, p[2] - delta.z);
        cam->SetFocalPoint(f[0] - delta.x, f[1] - delta.y, f[2] - delta.z);
        clearOverlays();
        cameraChanged();
    });
    connect(&ctx_, &AppContext::selectionChanged, this, [this] {
        restyleAll();
        render();
    });
    connect(&ctx_, &AppContext::playbackChanged, this, [this] {
        update4D();
        updateAnnotation();
        render();
    });
    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
        firstFit_ = true;
        workPlaneSet_ = false;
        d_->excavated.clear();
        d_->clouds->clear();
        clearOverlays();
        section_ = {};
        applyClipping();
    });
    connect(&ctx_, &AppContext::entityChanged, this, [this](int kind, int, const QString&) {
        if (static_cast<EntityKind>(kind) == EntityKind::PointCloud || static_cast<EntityKind>(kind) == EntityKind::Asset) d_->cameraTimer.start();
    });
    connect(&clouds_, &PointCloudStore::nodesLoaded, this, [this] {
        if (d_->clouds->update(ctx_.project(), scene_.origin(), vtk_->width(), vtk_->height())) {
            updateLegend();
            render();
        }
        updateAnnotation();
    });
    connect(vtk_, &QtVtkWidget::initialised, this, [this] { render(); });

    refreshTheme();
    setNamedView("iso");
    syncScene({}, true);
}

Viewport3D::~Viewport3D() {
    d_->clouds.reset();
    if (d_->axes) d_->axes->SetEnabled(0);
}

vtkRenderer* Viewport3D::renderer() const { return d_->renderer; }

void Viewport3D::render() { vtk_->requestRender(); }

QImage Viewport3D::grab() {
    updatePointClouds();
    return vtk_->grabFramebuffer();
}

void Viewport3D::setActiveView(bool a) {
    active_ = a;
    updateAnnotation();
    render();
}

void Viewport3D::refreshTheme() {
    const auto& c = Theme::colors();
    d_->renderer->SetBackground(c.viewportBottom.redF(), c.viewportBottom.greenF(), c.viewportBottom.blueF());
    d_->renderer->SetBackground2(c.viewportTop.redF(), c.viewportTop.greenF(), c.viewportTop.blueF());
    const double tr = c.text.redF(), tg = c.text.greenF(), tb = c.text.blueF();
    d_->annotation->GetTextProperty()->SetColor(tr, tg, tb);
    d_->legend->GetTitleTextProperty()->SetColor(tr, tg, tb);
    d_->legend->GetLabelTextProperty()->SetColor(tr, tg, tb);
    d_->scale->GetLegendTitleProperty()->SetColor(tr, tg, tb);
    d_->scale->GetLegendLabelProperty()->SetColor(tr, tg, tb);
    // The corner annotation rebuilds its text actors only when its text changes.
    d_->annotation->ClearAllTexts();
    d_->annotation->Modified();
    updateAnnotation();
    updateGrid();
    restyleAll();
    render();
}

// ---- scene synchronisation -----------------------------------------------------------------------

void Viewport3D::syncScene(const QSet<QString>& changed, bool all) {
    if (all) {
        for (auto& [id, recs] : d_->actors)
            for (auto& r : recs) d_->renderer->RemoveViewProp(r.prop);
        d_->actors.clear();
        d_->propIndex.clear();
        for (const auto& id : scene_.entities()) addEntityActors(id);
    } else {
        for (const auto& s : changed) {
            const Uuid id = uuidFrom(s);
            removeEntityActors(id);
            addEntityActors(id);
        }
    }
    if (ctx_.hasProject() && !workPlaneSet_) {
        const auto b = scene_.sceneBounds();
        if (!b.isEmpty()) {
            workPlaneZ_ = std::floor(b.min.z);
            workPlaneSet_ = true;
        }
    }
    update4D();
    if (section_.enabled) updateSectionOutlines();
    updateLegend();
    if (firstFit_ && ctx_.hasProject() && !scene_.sceneBounds().isEmpty()) {
        firstFit_ = false;
        fitAll();
    }
    d_->cameraTimer.start();
    render();
}

void Viewport3D::addEntityActors(const Uuid& id) {
    const auto* items = scene_.items(id);
    if (!items) return;
    auto& recs = d_->actors[id];
    for (std::size_t i = 0; i < items->size(); ++i) {
        const auto& it = (*items)[i];
        if (it.prim == SceneItem::Prim::Label) {
            if (!options_.labels) continue;
            auto lb = vtkSmartPointer<vtkBillboardTextActor3D>::New();
            lb->SetInput(it.labelText.c_str());
            lb->SetPosition(it.labelAnchor.x, it.labelAnchor.y, it.labelAnchor.z);
            auto* tp = lb->GetTextProperty();
            tp->SetFontSize(it.labelSize);
            tp->SetColor(((it.color >> 16) & 0xFF) / 255.0, ((it.color >> 8) & 0xFF) / 255.0, (it.color & 0xFF) / 255.0);
            tp->SetBold(false);
            tp->SetShadow(true);
            tp->SetJustificationToLeft();
            tp->SetVerticalJustificationToBottom();
            lb->SetDisplayOffset(6, 4);
            lb->PickableOff();
            d_->renderer->AddViewProp(lb);
            recs.push_back({lb, i, it.revision});
            continue;
        }
        if (!it.data) continue;
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(it.data);
        if (it.scalars) {
            mapper->SetLookupTable(makeLut(*it.scalars));
            mapper->SetScalarRange(it.scalars->min, it.scalars->max);
            mapper->SetScalarModeToDefault();
            if (it.scalars->cellData) mapper->SetScalarModeToUseCellData();
            else mapper->SetScalarModeToUsePointData();
            mapper->ScalarVisibilityOn();
            mapper->SetColorModeToMapScalars();
        } else {
            mapper->ScalarVisibilityOff();
        }
        mapper->SetResolveCoincidentTopologyToPolygonOffset();
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->SetPickable(it.pickable);
        d_->renderer->AddActor(actor);
        d_->propIndex[actor.Get()] = {id, i};
        recs.push_back({actor, i, it.revision});
    }
    applyClipping();
    restyleEntity(id);
}

void Viewport3D::removeEntityActors(const Uuid& id) {
    auto it = d_->actors.find(id);
    if (it == d_->actors.end()) return;
    for (auto& r : it->second) {
        d_->propIndex.erase(r.prop.Get());
        d_->renderer->RemoveViewProp(r.prop);
    }
    d_->actors.erase(it);
}

void Viewport3D::restyleAll() {
    for (const auto& [id, recs] : d_->actors) restyleEntity(id);
}

void Viewport3D::restyleEntity(const Uuid& id) {
    auto ait = d_->actors.find(id);
    if (ait == d_->actors.end()) return;
    const auto* items = scene_.items(id);
    if (!items) return;
    const auto& colors = Theme::colors();
    const std::uint32_t selColor = toRgb(colors.selection);
    for (auto& r : ait->second) {
        if (r.index >= items->size()) continue;
        const auto& it = (*items)[r.index];
        auto* actor = vtkActor::SafeDownCast(r.prop);
        if (!actor) continue;
        const bool selected = ctx_.isSelected(it.owner.kind, it.owner.id) ||
                              (!it.geometryId.isNil() && ctx_.isSelected(EntityKind::Geometry, it.geometryId));
        auto* p = actor->GetProperty();
        std::uint32_t color = it.color;
        double opacity = it.opacity;
        bool visible = true;
        // 4D state colouring of design objects.
        if (ctx_.playbackActive()) {
            if (d_->fourDHidden.count(it.owner.id) && it.part == "envelope") visible = false;
            if (auto s = d_->fourDStyle.find(it.owner.id); s != d_->fourDStyle.end() && it.part != "envelope") {
                color = s->second.first;
                opacity = s->second.second;
            }
        }
        if (selected) {
            color = selColor;
            opacity = std::max(opacity, it.prim == SceneItem::Prim::Surface ? 0.55 : 1.0);
        }
        setRgb(p, color);
        p->SetOpacity(opacity);
        p->SetLineWidth(static_cast<float>(it.lineWidth + (selected ? 2.0 : 0.0)));
        p->SetPointSize(static_cast<float>(it.pointSize + (selected ? 3.0 : 0.0)));
        p->SetRenderPointsAsSpheres(it.spheres);
        p->SetRenderLinesAsTubes(it.tubes || (selected && it.prim == SceneItem::Prim::Lines));
        p->SetLighting(it.lighting);
        p->SetBackfaceCulling(it.backfaceCulling);
        if (it.prim == SceneItem::Prim::Surface && it.lighting) {
            p->SetAmbient(0.18);
            p->SetDiffuse(0.82);
            p->SetSpecular(0.12);
            p->SetSpecularPower(20);
        }
        if (selected && it.prim == SceneItem::Prim::Surface) {
            p->EdgeVisibilityOff();
        }
        actor->SetVisibility(visible);
        if (auto* m = actor->GetMapper(); m && it.scalars) m->SetScalarVisibility(!selected);
    }
}

// ---- 4D -----------------------------------------------------------------------------------------

void Viewport3D::update4D() {
    for (auto& p : d_->fourD) d_->renderer->RemoveViewProp(p);
    d_->fourD.clear();
    d_->fourDHidden.clear();
    d_->fourDStyle.clear();
    if (!ctx_.hasProject() || !ctx_.playbackActive() || !ctx_.planSnapshot()) {
        restyleAll();
        return;
    }
    const auto& snap = *ctx_.planSnapshot();
    const auto& origin = scene_.origin();
    for (const auto& o : snap.objects) {
        const bool behind = ctx_.playbackCompareBaseline() && o.behindPlan;
        const std::uint32_t col = behind ? 0xD32F2F : o.color;
        if (o.designType == "MineDevelopment") {
            d_->fourDHidden.insert(o.designId);
            const double frac = o.state == planning::ObjectState::Complete ? 1.0 : o.plannedFraction;
            if (frac > 0) {
                const long key = std::lround(frac * 1e5);
                auto& cached = d_->excavated[o.designId];
                if (!cached.second || cached.first != key) {
                    try {
                        const MeshData mesh = planning::excavatedEnvelope(ctx_.project(), o.designId, frac);
                        cached = {key, mesh.triangles.empty() ? nullptr : scene::meshPolyData(mesh, origin, true, 40.0)};
                    } catch (const std::exception&) {
                        cached = {key, nullptr};
                    }
                }
                if (cached.second) {
                    vtkNew<vtkPolyDataMapper> m;
                    m->SetInputData(cached.second);
                    m->ScalarVisibilityOff();
                    vtkNew<vtkActor> a;
                    a->SetMapper(m);
                    setRgb(a->GetProperty(), col);
                    a->GetProperty()->SetOpacity(0.92);
                    a->PickableOff();
                    d_->renderer->AddActor(a);
                    d_->fourD.emplace_back(a.Get());
                }
            }
            // Not yet excavated remainder as a ghost of the full envelope.
            if (options_.ghostFuture && frac < 1.0)
                if (auto it = d_->actors.find(o.designId); it != d_->actors.end()) {
                    const auto* items = scene_.items(o.designId);
                    for (const auto& r : it->second)
                        if (items && r.index < items->size() && (*items)[r.index].part == "envelope") {
                            auto* src = vtkActor::SafeDownCast(r.prop);
                            if (!src) continue;
                            vtkNew<vtkActor> g;
                            g->SetMapper(src->GetMapper());
                            setRgb(g->GetProperty(), 0x9AA5B1);
                            g->GetProperty()->SetOpacity(0.10);
                            g->PickableOff();
                            d_->renderer->AddActor(g);
                            d_->fourD.emplace_back(g.Get());
                        }
                }
        } else {
            // Stopes and mining blocks: colour their shapes by state.
            double opacity = 1.0;
            switch (o.state) {
            case planning::ObjectState::NotScheduled: opacity = 0.15; break;
            case planning::ObjectState::Future: opacity = options_.ghostFuture ? 0.15 : 0.0; break;
            case planning::ObjectState::InProgress: opacity = 0.85; break;
            case planning::ObjectState::Complete: opacity = 1.0; break;
            }
            d_->fourDStyle[o.designId] = {col, std::max(opacity, 0.01)};
        }
    }
    restyleAll();
}

// ---- grid, legend, annotation ---------------------------------------------------------------------

void Viewport3D::updateGrid() {
    if (d_->grid) d_->renderer->RemoveActor(d_->grid);
    d_->grid = nullptr;
    if (!options_.grid || !vtk_->isInitialized() || vtk_->width() < 2) return;
    const auto [pp, pn] = workPlane();
    // Plane axes: horizontal plane → x, y; vertical plane → horizontal in-plane direction and z.
    Vec3 u{1, 0, 0}, v{0, 1, 0};
    if (std::abs(pn.z) < 0.9) {
        u = Vec3{-pn.y, pn.x, 0}.normalized();
        v = {0, 0, 1};
    }
    // Visible extent: intersect the corner and centre rays with the plane.
    const auto& o = scene_.origin();
    const Vec3 lp = o.toLocal(pp);
    auto acc = BoundingBox::empty();
    const int w = vtk_->width(), h = vtk_->height();
    int hits = 0;
    for (const QPoint q : {QPoint(0, 0), QPoint(w - 1, 0), QPoint(0, h - 1), QPoint(w - 1, h - 1), QPoint(w / 2, h / 2)}) {
        int x, y;
        vtk_->toDisplay(q, x, y);
        if (auto p = rayPlane(x, y, lp, pn)) {
            acc.expand(Vec3{dot(*p - lp, u), dot(*p - lp, v), 0.0});
            ++hits;
        }
    }
    double u0, u1, v0, v1;
    if (hits >= 2) {
        const double span = std::max(acc.size().x, acc.size().y);
        const double limit = std::max(span, 1.0) * 1.2;
        u0 = std::max(acc.min.x, -limit * 4);
        u1 = std::min(acc.max.x, limit * 4);
        v0 = std::max(acc.min.y, -limit * 4);
        v1 = std::min(acc.max.y, limit * 4);
    } else {
        const double r = std::max(50.0, pixelSize() * w);
        u0 = v0 = -r;
        u1 = v1 = r;
    }
    const double step = niceStep(std::max(u1 - u0, v1 - v0), 16);
    // Grid lines on whole multiples in world coordinates (so labels and snapping agree).
    const Vec3 wpp = pp;
    const double ou = dot(wpp, u), ov = dot(wpp, v);
    const double a0 = std::floor((u0 + ou) / step) * step - ou, a1 = std::ceil((u1 + ou) / step) * step - ou;
    const double b0 = std::floor((v0 + ov) / step) * step - ov, b1 = std::ceil((v1 + ov) / step) * step - ov;
    if ((a1 - a0) / step > 400 || (b1 - b0) / step > 400) return;
    std::vector<std::pair<Vec3, Vec3>> segs;
    for (double a = a0; a <= a1 + 1e-9; a += step) segs.emplace_back(pp + u * a + v * b0, pp + u * a + v * b1);
    for (double b = b0; b <= b1 + 1e-9; b += step) segs.emplace_back(pp + u * a0 + v * b, pp + u * a1 + v * b);
    vtkNew<vtkPolyDataMapper> m;
    m->SetInputData(scene::segmentsData(segs, o));
    d_->grid = vtkSmartPointer<vtkActor>::New();
    d_->grid->SetMapper(m);
    const auto& c = Theme::colors();
    d_->grid->GetProperty()->SetColor(c.grid.redF(), c.grid.greenF(), c.grid.blueF());
    d_->grid->GetProperty()->SetOpacity(0.35);
    d_->grid->GetProperty()->LightingOff();
    d_->grid->PickableOff();
    d_->grid->UseBoundsOff();
    d_->renderer->AddActor(d_->grid);
}

void Viewport3D::updateLegend() {
    std::optional<ScalarDisplay> sd;
    if (options_.legend) {
        // Prefer a selected item's scalars, then block models, then anything else displayed.
        int bestRank = 99;
        for (const auto& [id, recs] : d_->actors) {
            const auto* items = scene_.items(id);
            if (!items) continue;
            for (const auto& it : *items) {
                if (!it.scalars) continue;
                int rank = 3;
                if (ctx_.isSelected(it.owner.kind, it.owner.id)) rank = 0;
                else if (it.owner.kind == EntityKind::BlockModel) rank = 1;
                else if (it.owner.kind == EntityKind::Drillhole) rank = 2;
                if (rank < bestRank) {
                    bestRank = rank;
                    sd = it.scalars;
                }
            }
        }
        if (!sd && options_.pointClouds) sd = d_->clouds->legend();
    }
    if (!sd) {
        d_->legend->VisibilityOff();
        return;
    }
    d_->legend->SetLookupTable(makeLut(*sd));
    d_->legend->SetTitle(sd->title.c_str());
    d_->legend->VisibilityOn();
}

void Viewport3D::updateAnnotation() {
    QString tl = viewName();
    if (section_.enabled) {
        tl += QStringLiteral(" — %1 (±%2 m)").arg(section_.label.empty() ? tr("section") : qstr(section_.label)).arg(section_.halfWidth, 0, 'f', 1);
    }
    if (active_) tl = QStringLiteral("● ") + tl;
    d_->annotation->SetText(vtkCornerAnnotation::UpperLeft, tl.toUtf8().constData());
    QString bl;
    if (ctx_.hasProject() && ctx_.playbackActive() && ctx_.planSnapshot()) {
        const auto& s = *ctx_.planSnapshot();
        bl = tr("4D %1   planned %2 m   actual %3 m").arg(formatDateTimeMinutes(s.time)).arg(s.plannedMetres, 0, 'f', 1).arg(s.actualMetres, 0, 'f', 1);
    }
    const auto pts = d_->clouds->displayedPoints();
    if (pts > 0) {
        if (!bl.isEmpty()) bl += "\n";
        bl += tr("%L1 points").arg(static_cast<qulonglong>(pts));
        if (d_->clouds->pendingNodes() > 0) bl += tr(" (loading %1 nodes)").arg(d_->clouds->pendingNodes());
    }
    // Upper right: clear of the scale bar (bottom centre) and the orientation axes (bottom left).
    d_->annotation->SetText(vtkCornerAnnotation::UpperRight, bl.toUtf8().constData());
}

void Viewport3D::updatePointClouds() {
    if (!ctx_.hasProject() || !vtk_->isInitialized()) return;
    auto& o = d_->clouds->options();
    o.enabled = options_.pointClouds;
    o.pointBudget = options_.pointBudget;
    o.screenSpacing = options_.pointScreenSpacing;
    if (d_->clouds->update(ctx_.project(), scene_.origin(), vtk_->width(), vtk_->height())) updateLegend();
}

std::size_t Viewport3D::displayedPoints() const { return d_->clouds->displayedPoints(); }
std::size_t Viewport3D::pendingPointNodes() const { return d_->clouds->pendingNodes(); }

void Viewport3D::applyOptions() {
    d_->axes->SetEnabled(options_.axes ? 1 : 0);
    d_->scale->SetVisibility(options_.scaleBar && !perspective());
    syncScene({}, true); // labels on/off rebuild the label actors
    updateLegend();
    update4D();
}

// ---- section and clipping -------------------------------------------------------------------------

void Viewport3D::applyClipping() {
    std::vector<std::pair<Vec3, Vec3>> planes;
    if (section_.enabled && mode_ == ViewMode::Section) {
        const Vec3 n = section_.normal.normalized();
        const Vec3 p = scene_.origin().toLocal(section_.point);
        planes.emplace_back(p - n * section_.halfWidth, n);
        planes.emplace_back(p + n * section_.halfWidth, -n);
    }
    auto apply = [&](vtkActor* a) {
        auto* m = a->GetMapper();
        if (!m) return;
        m->RemoveAllClippingPlanes();
        for (const auto& [p, n] : planes) {
            vtkNew<vtkPlane> pl;
            pl->SetOrigin(p.x, p.y, p.z);
            pl->SetNormal(n.x, n.y, n.z);
            m->AddClippingPlane(pl);
        }
    };
    for (auto& [id, recs] : d_->actors)
        for (auto& r : recs)
            if (auto* a = vtkActor::SafeDownCast(r.prop)) apply(a);
    for (auto& p : d_->fourD)
        if (auto* a = vtkActor::SafeDownCast(p)) apply(a);
    d_->clouds->setClipPlanes(planes);
}

void Viewport3D::updateSectionOutlines() {
    for (auto& p : d_->sectionOutlines) d_->renderer->RemoveViewProp(p);
    d_->sectionOutlines.clear();
    if (!section_.enabled || mode_ != ViewMode::Section || !options_.sectionOutlines) return;
    const Vec3 n = section_.normal.normalized();
    const Vec3 p = scene_.origin().toLocal(section_.point);
    vtkNew<vtkPlane> plane;
    plane->SetOrigin(p.x, p.y, p.z);
    plane->SetNormal(n.x, n.y, n.z);
    for (const auto& [id, recs] : d_->actors) {
        const auto* items = scene_.items(id);
        if (!items) continue;
        for (const auto& r : recs) {
            if (r.index >= items->size()) continue;
            const auto& it = (*items)[r.index];
            if (it.prim != SceneItem::Prim::Surface || !it.data || it.data->GetNumberOfPolys() == 0) continue;
            vtkNew<vtkCutter> cut;
            cut->SetCutFunction(plane);
            cut->SetInputData(it.data);
            cut->Update();
            if (cut->GetOutput()->GetNumberOfLines() == 0) continue;
            auto pd = vtkSmartPointer<vtkPolyData>::New();
            pd->ShallowCopy(cut->GetOutput());
            vtkNew<vtkPolyDataMapper> m;
            m->SetInputData(pd);
            m->ScalarVisibilityOff();
            vtkNew<vtkActor> a;
            a->SetMapper(m);
            setRgb(a->GetProperty(), it.owner.kind == EntityKind::Development ? 0xFFD54F : (it.color == 0xD0D0D0 ? 0xFFFFFF : it.color));
            a->GetProperty()->SetLineWidth(3.0f);
            a->GetProperty()->LightingOff();
            a->PickableOff();
            d_->renderer->AddActor(a);
            d_->sectionOutlines.emplace_back(a.Get());
        }
    }
}

void Viewport3D::setSection(const SectionDef& s) {
    section_ = s;
    if (section_.normal.length() < 1e-9) section_.normal = {0, 1, 0};
    section_.normal = section_.normal.normalized();
    if (section_.enabled) {
        mode_ = ViewMode::Section;
        auto* cam = d_->renderer->GetActiveCamera();
        const Vec3 p = scene_.origin().toLocal(section_.point);
        const Vec3 n = section_.normal;
        const double dist = std::max(100.0, section_.halfWidth * 40);
        cam->SetFocalPoint(p.x, p.y, p.z);
        // Look along −normal (the viewer stands on the +normal side).
        cam->SetPosition(p.x + n.x * dist, p.y + n.y * dist, p.z + n.z * dist);
        cam->SetViewUp(std::abs(n.z) > 0.9 ? 0 : 0, std::abs(n.z) > 0.9 ? 1 : 0, std::abs(n.z) > 0.9 ? 0 : 1);
        cam->ParallelProjectionOn();
        d_->renderer->ResetCameraClippingRange();
    }
    applyClipping();
    updateSectionOutlines();
    d_->scale->SetVisibility(options_.scaleBar && !perspective());
    cameraChanged();
}

void Viewport3D::stepSection(int steps) {
    if (!section_.enabled) return;
    section_.point = section_.point + section_.normal * (section_.step * steps);
    auto* cam = d_->renderer->GetActiveCamera();
    double p[3], f[3];
    cam->GetPosition(p);
    cam->GetFocalPoint(f);
    const Vec3 dv = section_.normal * (section_.step * steps);
    cam->SetPosition(p[0] + dv.x, p[1] + dv.y, p[2] + dv.z);
    cam->SetFocalPoint(f[0] + dv.x, f[1] + dv.y, f[2] + dv.z);
    applyClipping();
    updateSectionOutlines();
    cameraChanged();
}

bool Viewport3D::sectionAtChainage(const Uuid& developmentId, double chainage, double halfWidth) {
    if (!ctx_.hasProject()) return false;
    const auto* d = ctx_.project().model().developments.find(developmentId);
    if (!d) return false;
    const auto cl = mine_design::centrelinePoints(ctx_.project(), *d);
    if (cl.size() < 2) return false;
    const double along = chainage - d->chainageFrom;
    const auto f = geometry::sectionFrameAt(cl, std::clamp(along, 0.0, geometry::length3d(cl)));
    SectionDef s;
    s.enabled = true;
    s.point = f.origin;
    Vec3 n = f.tangent;
    n.z = 0; // vertical section plane perpendicular to the heading in plan
    if (n.length() < 1e-9) n = f.tangent;
    s.normal = n.normalized();
    s.halfWidth = halfWidth;
    s.step = 1.0;
    s.label = d->headingId + " " + mine_design::formatChainage(chainage);
    setSection(s);
    const double r = std::max({d->designWidth, d->designHeight, 4.0}) * 1.6;
    BoundingBox b = BoundingBox::empty();
    b.expand(f.origin - Vec3{r, r, r * 0.5});
    b.expand(f.origin + Vec3{r, r, r * 1.2});
    fitBounds(b, 1.0);
    return true;
}

// ---- camera --------------------------------------------------------------------------------------

void Viewport3D::setMode(ViewMode m) {
    mode_ = m;
    switch (m) {
    case ViewMode::Perspective: section_.enabled = false; setNamedView("iso"); setPerspective(true); break;
    case ViewMode::Plan: section_.enabled = false; setNamedView("top"); break;
    case ViewMode::Elevation: section_.enabled = false; setNamedView("north"); break;
    case ViewMode::Section: {
        if (!section_.enabled) {
            // Default: east–west section through the view centre.
            double f[3];
            d_->renderer->GetActiveCamera()->GetFocalPoint(f);
            SectionDef s = section_;
            s.enabled = true;
            s.point = scene_.origin().toWorld({f[0], f[1], f[2]});
            s.normal = {0, 1, 0};
            setSection(s);
        } else {
            setSection(section_);
        }
        break;
    }
    }
    applyClipping();
    updateSectionOutlines();
    cameraChanged();
}

QString Viewport3D::viewName() const {
    switch (mode_) {
    case ViewMode::Perspective: return perspective() ? tr("3D") : tr("3D (parallel)");
    case ViewMode::Plan: return tr("Plan");
    case ViewMode::Section: return tr("Section");
    case ViewMode::Elevation: return tr("Elevation");
    }
    return {};
}

void Viewport3D::setNamedView(const QString& name) {
    auto* cam = d_->renderer->GetActiveCamera();
    double fp[3];
    cam->GetFocalPoint(fp);
    const double dist = std::max(10.0, cam->GetDistance());
    Vec3 dir{1, -1, 0.8};
    Vec3 up{0, 0, 1};
    if (name == "top") {
        dir = {0, 0, 1};
        up = {0, 1, 0};
        mode_ = ViewMode::Plan;
    } else if (name == "bottom") {
        dir = {0, 0, -1};
        up = {0, 1, 0};
        mode_ = ViewMode::Plan;
    } else if (name == "north") {
        dir = {0, 1, 0}; // looking south from the north
        mode_ = ViewMode::Elevation;
    } else if (name == "south") {
        dir = {0, -1, 0};
        mode_ = ViewMode::Elevation;
    } else if (name == "east") {
        dir = {1, 0, 0};
        mode_ = ViewMode::Elevation;
    } else if (name == "west") {
        dir = {-1, 0, 0};
        mode_ = ViewMode::Elevation;
    } else if (name == "iso-ne") {
        dir = {1, 1, 0.8};
        mode_ = ViewMode::Perspective;
    } else if (name == "iso-nw") {
        dir = {-1, 1, 0.8};
        mode_ = ViewMode::Perspective;
    } else if (name == "iso-sw") {
        dir = {-1, -1, 0.8};
        mode_ = ViewMode::Perspective;
    } else {
        mode_ = ViewMode::Perspective; // "iso" = south-east
    }
    if (mode_ != ViewMode::Section) section_.enabled = false;
    dir = dir.normalized();
    cam->SetPosition(fp[0] + dir.x * dist, fp[1] + dir.y * dist, fp[2] + dir.z * dist);
    cam->SetViewUp(up.x, up.y, up.z);
    cam->SetParallelProjection(mode_ != ViewMode::Perspective);
    applyClipping();
    updateSectionOutlines();
    fitAll();
}

void Viewport3D::fitBounds(const BoundingBox& world, double margin) {
    if (world.isEmpty()) return;
    const auto& o = scene_.origin();
    const Vec3 a = o.toLocal(world.min), b = o.toLocal(world.max);
    double bounds[6] = {a.x, b.x, a.y, b.y, a.z, b.z};
    // Pad degenerate extents (a single point, a horizontal line).
    const double pad = std::max({b.x - a.x, b.y - a.y, b.z - a.z, 1.0}) * (margin - 1.0) + 0.5;
    for (int i = 0; i < 3; ++i) {
        bounds[2 * i] -= pad;
        bounds[2 * i + 1] += pad;
    }
    d_->renderer->ResetCamera(bounds);
    if (section_.enabled && mode_ == ViewMode::Section) {
        // Keep the section plane: move along the normal back onto it.
        auto* cam = d_->renderer->GetActiveCamera();
        double f[3], p[3];
        cam->GetFocalPoint(f);
        cam->GetPosition(p);
        const Vec3 sp = o.toLocal(section_.point);
        const Vec3 n = section_.normal;
        const double off = dot(Vec3{f[0], f[1], f[2]} - sp, n);
        cam->SetFocalPoint(f[0] - n.x * off, f[1] - n.y * off, f[2] - n.z * off);
        cam->SetPosition(p[0] - n.x * off, p[1] - n.y * off, p[2] - n.z * off);
    }
    d_->renderer->ResetCameraClippingRange();
    cameraChanged();
}

void Viewport3D::fitAll() {
    auto b = scene_.sceneBounds();
    if (b.isEmpty()) {
        b = BoundingBox::empty();
        b.expand(scene_.origin().toWorld({-50, -50, -10}));
        b.expand(scene_.origin().toWorld({50, 50, 10}));
    }
    fitBounds(b, 1.05);
}

void Viewport3D::fitSelection() {
    auto b = BoundingBox::empty();
    for (const auto& s : ctx_.selection()) {
        if (const auto* items = scene_.items(s.id))
            for (const auto& it : *items)
                if (!it.bounds.isEmpty()) b.expand(it.bounds);
        if (ctx_.hasProject())
            if (const auto* dev = ctx_.project().model().developments.find(s.id))
                if (const auto* items = scene_.items(dev->geometryId))
                    for (const auto& it : *items) b.expand(it.bounds);
    }
    if (b.isEmpty()) fitAll();
    else fitBounds(b, 1.3);
}

void Viewport3D::zoomBy(double factor) {
    auto* cam = d_->renderer->GetActiveCamera();
    if (cam->GetParallelProjection()) cam->SetParallelScale(cam->GetParallelScale() / factor);
    else cam->Dolly(factor);
    d_->renderer->ResetCameraClippingRange();
    cameraChanged();
}

void Viewport3D::setPerspective(bool on) {
    d_->renderer->GetActiveCamera()->SetParallelProjection(!on);
    d_->scale->SetVisibility(options_.scaleBar && !on);
    d_->renderer->ResetCameraClippingRange();
    cameraChanged();
}

bool Viewport3D::perspective() const { return d_->renderer->GetActiveCamera()->GetParallelProjection() == 0; }

QString Viewport3D::cameraState() const {
    auto* cam = d_->renderer->GetActiveCamera();
    double p[3], f[3], u[3];
    cam->GetPosition(p);
    cam->GetFocalPoint(f);
    cam->GetViewUp(u);
    const auto& o = scene_.origin().origin();
    std::ostringstream s;
    s.precision(17);
    s << static_cast<int>(mode_) << ' ' << p[0] + o.x << ' ' << p[1] + o.y << ' ' << p[2] + o.z << ' ' << f[0] + o.x << ' ' << f[1] + o.y << ' '
      << f[2] + o.z << ' ' << u[0] << ' ' << u[1] << ' ' << u[2] << ' ' << cam->GetParallelProjection() << ' ' << cam->GetParallelScale() << ' '
      << cam->GetViewAngle();
    return qstr(s.str());
}

bool Viewport3D::restoreCamera(const QString& state) {
    std::istringstream s(state.toStdString());
    int mode, par;
    double p[3], f[3], u[3], scale, angle;
    if (!(s >> mode >> p[0] >> p[1] >> p[2] >> f[0] >> f[1] >> f[2] >> u[0] >> u[1] >> u[2] >> par >> scale >> angle)) return false;
    const auto& o = scene_.origin().origin();
    auto* cam = d_->renderer->GetActiveCamera();
    mode_ = static_cast<ViewMode>(std::clamp(mode, 0, 3));
    cam->SetPosition(p[0] - o.x, p[1] - o.y, p[2] - o.z);
    cam->SetFocalPoint(f[0] - o.x, f[1] - o.y, f[2] - o.z);
    cam->SetViewUp(u);
    cam->SetParallelProjection(par);
    cam->SetParallelScale(scale);
    cam->SetViewAngle(angle);
    d_->renderer->ResetCameraClippingRange();
    cameraChanged();
    return true;
}

void Viewport3D::cameraChanged() {
    d_->renderer->ResetCameraClippingRange();
    d_->cameraTimer.start();
    emit viewChanged();
    render();
}

void Viewport3D::orbit(double dx, double dy) {
    auto* cam = d_->renderer->GetActiveCamera();
    double p[3], f[3], u[3];
    cam->GetPosition(p);
    cam->GetFocalPoint(f);
    cam->GetViewUp(u);
    const Vec3 pivot = d_->pivot;
    Vec3 pos{p[0], p[1], p[2]}, foc{f[0], f[1], f[2]}, up{u[0], u[1], u[2]};
    // Turntable: azimuth about world Z through the pivot, elevation about the camera's right axis.
    const double az = -dx * 0.35 * std::numbers::pi / 180.0;
    const double el = dy * 0.35 * std::numbers::pi / 180.0;
    const auto rz = Transform3::rotation({0, 0, 1}, az, pivot);
    pos = rz.apply(pos);
    foc = rz.apply(foc);
    up = rz.applyVector(up);
    const Vec3 viewDir = (foc - pos).normalized();
    Vec3 right = cross(viewDir, up).normalized();
    if (right.length() < 1e-9) right = {1, 0, 0};
    // Limit elevation so the view never flips over the pole.
    const double currentEl = std::asin(std::clamp(-viewDir.z, -1.0, 1.0));
    const double maxEl = 89.5 * std::numbers::pi / 180.0;
    const double newEl = std::clamp(currentEl + el, -maxEl, maxEl);
    const auto rx = Transform3::rotation(right, -(newEl - currentEl), pivot);
    pos = rx.apply(pos);
    foc = rx.apply(foc);
    up = rx.applyVector(up);
    cam->SetPosition(pos.x, pos.y, pos.z);
    cam->SetFocalPoint(foc.x, foc.y, foc.z);
    cam->SetViewUp(up.x, up.y, up.z);
    cam->OrthogonalizeViewUp();
    if (mode_ != ViewMode::Perspective) mode_ = ViewMode::Perspective;
    cameraChanged();
}

void Viewport3D::pan(double dx, double dy) {
    auto* cam = d_->renderer->GetActiveCamera();
    double f[3], p[3];
    cam->GetFocalPoint(f);
    cam->GetPosition(p);
    // Display-space motion at the focal depth → world motion.
    d_->renderer->SetWorldPoint(f[0], f[1], f[2], 1.0);
    d_->renderer->WorldToDisplay();
    double fd[3];
    d_->renderer->GetDisplayPoint(fd);
    d_->renderer->SetDisplayPoint(fd[0] - dx, fd[1] - dy, fd[2]);
    d_->renderer->DisplayToWorld();
    double nw[4];
    d_->renderer->GetWorldPoint(nw);
    if (nw[3] == 0) return;
    const double mx = nw[0] / nw[3] - f[0], my = nw[1] / nw[3] - f[1], mz = nw[2] / nw[3] - f[2];
    cam->SetFocalPoint(f[0] + mx, f[1] + my, f[2] + mz);
    cam->SetPosition(p[0] + mx, p[1] + my, p[2] + mz);
    cameraChanged();
}

void Viewport3D::zoomAt(double factor, int x, int y) {
    auto* cam = d_->renderer->GetActiveCamera();
    // Point to keep fixed under the cursor: the picked surface, else the focal plane.
    std::optional<Vec3> target = pickLocal(x, y);
    if (!target) {
        double f[3], p[3];
        cam->GetFocalPoint(f);
        cam->GetPosition(p);
        const Vec3 n = (Vec3{p[0], p[1], p[2]} - Vec3{f[0], f[1], f[2]}).normalized();
        target = rayPlane(x, y, {f[0], f[1], f[2]}, n);
    }
    double f[3], p[3];
    cam->GetFocalPoint(f);
    cam->GetPosition(p);
    if (cam->GetParallelProjection()) {
        const double s = cam->GetParallelScale() / factor;
        if (target) {
            // Shift so the target stays under the cursor.
            const Vec3 fv{f[0], f[1], f[2]};
            const Vec3 t = *target;
            const Vec3 viewDir = (fv - Vec3{p[0], p[1], p[2]}).normalized();
            Vec3 d = (t - fv) * (1.0 - 1.0 / factor);
            d = d - viewDir * dot(d, viewDir);
            cam->SetFocalPoint(f[0] + d.x, f[1] + d.y, f[2] + d.z);
            cam->SetPosition(p[0] + d.x, p[1] + d.y, p[2] + d.z);
        }
        cam->SetParallelScale(s);
    } else if (target) {
        const Vec3 pos{p[0], p[1], p[2]}, foc{f[0], f[1], f[2]};
        const Vec3 move = (*target - pos) * (1.0 - 1.0 / factor);
        cam->SetPosition(pos.x + move.x, pos.y + move.y, pos.z + move.z);
        cam->SetFocalPoint(foc.x + move.x, foc.y + move.y, foc.z + move.z);
    } else {
        cam->Dolly(factor);
    }
    cameraChanged();
}

// ---- geometry queries ------------------------------------------------------------------------------

std::optional<Vec3> Viewport3D::rayPlane(int x, int y, Vec3 pp, Vec3 pn) const {
    auto* r = d_->renderer.Get();
    double nearP[4], farP[4];
    r->SetDisplayPoint(x, y, 0.0);
    r->DisplayToWorld();
    r->GetWorldPoint(nearP);
    r->SetDisplayPoint(x, y, 1.0);
    r->DisplayToWorld();
    r->GetWorldPoint(farP);
    if (nearP[3] == 0 || farP[3] == 0) return std::nullopt;
    const Vec3 a{nearP[0] / nearP[3], nearP[1] / nearP[3], nearP[2] / nearP[3]};
    const Vec3 b{farP[0] / farP[3], farP[1] / farP[3], farP[2] / farP[3]};
    const Vec3 d = b - a;
    const double den = dot(d, pn);
    if (std::abs(den) < 1e-12) return std::nullopt;
    const double t = dot(pp - a, pn) / den;
    if (t < 0) return std::nullopt;
    return a + d * t;
}

std::optional<Vec3> Viewport3D::pickLocal(int x, int y) const {
    if (d_->picker->Pick(x, y, 0, d_->renderer)) {
        double p[3];
        d_->picker->GetPickPosition(p);
        return Vec3{p[0], p[1], p[2]};
    }
    if (auto c = d_->clouds->pick(x, y, 6.0)) return scene_.origin().toLocal(c->project);
    return std::nullopt;
}

std::pair<Vec3, Vec3> Viewport3D::workPlane() const {
    if (mode_ == ViewMode::Section && section_.enabled) return {section_.point, section_.normal};
    if (mode_ == ViewMode::Elevation) {
        auto* cam = d_->renderer->GetActiveCamera();
        double f[3], p[3];
        cam->GetFocalPoint(f);
        cam->GetPosition(p);
        Vec3 n{p[0] - f[0], p[1] - f[1], 0};
        if (n.length() < 1e-9) n = {0, 1, 0};
        return {scene_.origin().toWorld({f[0], f[1], f[2]}), n.normalized()};
    }
    return {Vec3{scene_.origin().origin().x, scene_.origin().origin().y, workPlaneZ_}, {0, 0, 1}};
}

void Viewport3D::setWorkPlaneZ(double z) {
    workPlaneZ_ = z;
    workPlaneSet_ = true;
    updateGrid();
    render();
}

std::optional<Vec3> Viewport3D::workPlanePoint(QPoint q) const {
    int x, y;
    vtk_->toDisplay(q, x, y);
    const auto [pp, pn] = workPlane();
    if (auto p = rayPlane(x, y, scene_.origin().toLocal(pp), pn)) return scene_.origin().toWorld(*p);
    return std::nullopt;
}

std::optional<QPointF> Viewport3D::worldToWidget(Vec3 world) const {
    const Vec3 l = scene_.origin().toLocal(world);
    auto* r = d_->renderer.Get();
    r->SetWorldPoint(l.x, l.y, l.z, 1.0);
    r->WorldToDisplay();
    double dp[3];
    r->GetDisplayPoint(dp);
    if (dp[2] < 0 || dp[2] > 1) return std::nullopt;
    const double dpr = vtk_->pixelRatio();
    return QPointF(dp[0] / dpr, vtk_->height() - 1 - dp[1] / dpr);
}

double Viewport3D::pixelSize() const {
    auto* cam = d_->renderer->GetActiveCamera();
    const double h = std::max(1, vtk_->height());
    if (cam->GetParallelProjection()) return 2.0 * cam->GetParallelScale() / h;
    return 2.0 * cam->GetDistance() * std::tan(cam->GetViewAngle() * std::numbers::pi / 360.0) / h;
}

QPoint Viewport3D::toQt(int x, int y) const {
    const double dpr = vtk_->pixelRatio();
    return QPoint(static_cast<int>(std::lround(x / dpr)), static_cast<int>(std::lround(vtk_->height() - 1 - y / dpr)));
}

std::optional<PickHit> Viewport3D::pickAt(QPoint q, bool includePointClouds) const {
    int x, y;
    vtk_->toDisplay(q, x, y);
    std::optional<PickHit> hit;
    double hitDepth = 1e300;
    double camPos[3];
    d_->renderer->GetActiveCamera()->GetPosition(camPos);
    const Vec3 eye{camPos[0], camPos[1], camPos[2]};
    if (d_->picker->Pick(x, y, 0, d_->renderer)) {
        auto it = d_->propIndex.find(d_->picker->GetActor());
        if (it != d_->propIndex.end()) {
            if (const auto* items = scene_.items(it->second.first); items && it->second.second < items->size()) {
                const auto& item = (*items)[it->second.second];
                double p[3];
                d_->picker->GetPickPosition(p);
                PickHit h;
                h.owner = item.owner;
                h.geometryId = item.geometryId;
                h.world = scene_.origin().toWorld({p[0], p[1], p[2]});
                h.part = item.part;
                hitDepth = distance(Vec3{p[0], p[1], p[2]}, eye);
                hit = h;
            }
        }
    }
    if (includePointClouds && options_.pointClouds)
        if (auto c = d_->clouds->pick(x, y, 5.0)) {
            const double depth = distance(scene_.origin().toLocal(c->project), eye);
            if (!hit || depth < hitDepth - 0.05) {
                PickHit h;
                h.owner = {EntityKind::PointCloud, c->datasetId};
                h.world = c->project;
                h.stored = c->stored;
                h.onPointCloud = true;
                h.part = "point";
                hit = h;
            }
        }
    return hit;
}

std::vector<SelectionItem> Viewport3D::itemsInRect(const QRect& rect, bool crossing) const {
    std::vector<SelectionItem> out;
    std::set<SelectionItem> seen;
    const QRectF r = QRectF(rect).normalized();
    for (const auto& [id, recs] : d_->actors) {
        const auto* items = scene_.items(id);
        if (!items) continue;
        for (const auto& rec : recs) {
            if (rec.index >= items->size()) continue;
            const auto& it = (*items)[rec.index];
            if (!it.pickable || !it.data || it.ghost) continue;
            if (auto* a = vtkActor::SafeDownCast(rec.prop); a && !a->GetVisibility()) continue;
            auto* pts = it.data->GetPoints();
            if (!pts || pts->GetNumberOfPoints() == 0) continue;
            const vtkIdType n = pts->GetNumberOfPoints();
            const vtkIdType stride = std::max<vtkIdType>(1, n / 4000);
            bool anyIn = false, allIn = true;
            for (vtkIdType i = 0; i < n; i += stride) {
                double p[3];
                pts->GetPoint(i, p);
                auto q = worldToWidget(scene_.origin().toWorld({p[0], p[1], p[2]}));
                const bool in = q && r.contains(*q);
                anyIn = anyIn || in;
                allIn = allIn && in;
                if (crossing && anyIn) break;
                if (!crossing && !allIn) break;
            }
            if ((crossing && anyIn) || (!crossing && allIn)) {
                if (seen.insert(it.owner).second) out.push_back(it.owner);
            }
        }
    }
    return out;
}

// ---- overlays ------------------------------------------------------------------------------------

void Viewport3D::setOverlay(const std::string& key, const Overlay& o) {
    clearOverlay(key);
    auto& props = d_->overlays[key];
    const auto& origin = scene_.origin();
    if (!o.lines.empty()) {
        auto lines = o.dashed ? scene::applyLinetype(o.lines, "Dashed", pixelSize() * 8) : o.lines;
        vtkNew<vtkPolyDataMapper> m;
        m->SetInputData(scene::polylinesData(lines, origin));
        m->ScalarVisibilityOff();
        m->SetResolveCoincidentTopologyToPolygonOffset();
        vtkNew<vtkActor> a;
        a->SetMapper(m);
        setRgb(a->GetProperty(), o.color);
        a->GetProperty()->SetLineWidth(static_cast<float>(o.lineWidth));
        a->GetProperty()->LightingOff();
        a->PickableOff();
        d_->renderer->AddActor(a);
        props.emplace_back(a.Get());
    }
    if (!o.points.empty()) {
        vtkNew<vtkPolyDataMapper> m;
        m->SetInputData(scene::pointsData(o.points, origin));
        m->ScalarVisibilityOff();
        vtkNew<vtkActor> a;
        a->SetMapper(m);
        setRgb(a->GetProperty(), o.color);
        a->GetProperty()->SetPointSize(static_cast<float>(o.pointSize));
        a->GetProperty()->SetRenderPointsAsSpheres(true);
        a->GetProperty()->LightingOff();
        a->PickableOff();
        d_->renderer->AddActor(a);
        props.emplace_back(a.Get());
    }
    for (const auto& [p, text] : o.labels) {
        auto lb = vtkSmartPointer<vtkBillboardTextActor3D>::New();
        const Vec3 l = origin.toLocal(p);
        lb->SetInput(text.c_str());
        lb->SetPosition(l.x, l.y, l.z);
        lb->GetTextProperty()->SetFontSize(13);
        lb->GetTextProperty()->SetColor(((o.color >> 16) & 0xFF) / 255.0, ((o.color >> 8) & 0xFF) / 255.0, (o.color & 0xFF) / 255.0);
        lb->GetTextProperty()->SetBackgroundColor(0, 0, 0);
        lb->GetTextProperty()->SetBackgroundOpacity(0.55);
        lb->SetDisplayOffset(10, 8);
        lb->PickableOff();
        d_->renderer->AddViewProp(lb);
        props.emplace_back(lb.Get());
    }
    render();
}

void Viewport3D::clearOverlay(const std::string& key) {
    auto it = d_->overlays.find(key);
    if (it == d_->overlays.end()) return;
    for (auto& p : it->second) d_->renderer->RemoveViewProp(p);
    d_->overlays.erase(it);
    render();
}

void Viewport3D::clearOverlays() {
    for (auto& [k, props] : d_->overlays)
        for (auto& p : props) d_->renderer->RemoveViewProp(p);
    d_->overlays.clear();
}

void Viewport3D::setRubberBand(const std::optional<QRect>& r, bool crossing) {
    if (d_->rubber) d_->renderer->RemoveViewProp(d_->rubber);
    d_->rubber = nullptr;
    if (!r) {
        render();
        return;
    }
    const double dpr = vtk_->pixelRatio();
    const QRect n = r->normalized();
    const double x0 = n.left() * dpr, x1 = n.right() * dpr;
    const double y0 = (vtk_->height() - 1 - n.bottom()) * dpr, y1 = (vtk_->height() - 1 - n.top()) * dpr;
    vtkNew<vtkPoints> pts;
    pts->InsertNextPoint(x0, y0, 0);
    pts->InsertNextPoint(x1, y0, 0);
    pts->InsertNextPoint(x1, y1, 0);
    pts->InsertNextPoint(x0, y1, 0);
    vtkNew<vtkCellArray> lines;
    const vtkIdType ids[5] = {0, 1, 2, 3, 0};
    lines->InsertNextCell(5, ids);
    vtkNew<vtkPolyData> pd;
    pd->SetPoints(pts);
    pd->SetLines(lines);
    vtkNew<vtkPolyDataMapper2D> m;
    m->SetInputData(pd);
    d_->rubber = vtkSmartPointer<vtkActor2D>::New();
    d_->rubber->SetMapper(m);
    d_->rubber->GetProperty()->SetColor(crossing ? 0.3 : 0.2, crossing ? 0.85 : 0.6, crossing ? 0.4 : 1.0);
    d_->rubber->GetProperty()->SetLineWidth(1.5f);
    d_->renderer->AddViewProp(d_->rubber);
    render();
}

// ---- events -----------------------------------------------------------------------------------------

void Viewport3D::onPress(int button, int x, int y, bool ctrl, bool shift) {
    emit activated(this);
    d_->pressButton = button;
    d_->pressX = d_->lastX = x;
    d_->pressY = d_->lastY = y;
    d_->pressCtrl = ctrl;
    d_->pressShift = shift;
    d_->drag = Impl::Drag::Pending;
    if (tools_ && tools_->isActive() && button == 1) {
        tools_->press(this, toQt(x, y), ctrl, shift);
    }
    if (button == 1 || button == 2) {
        // Orbit about the point under the cursor (or keep the current pivot).
        if (auto p = pickLocal(x, y)) d_->pivot = *p;
        else {
            double f[3];
            d_->renderer->GetActiveCamera()->GetFocalPoint(f);
            d_->pivot = {f[0], f[1], f[2]};
        }
    }
}

void Viewport3D::onMove(int x, int y, bool ctrl, bool shift) {
    const QPoint q = toQt(x, y);
    const int dx = x - d_->lastX, dy = y - d_->lastY;
    if (d_->drag == Impl::Drag::Pending && d_->pressButton != 0) {
        if (std::abs(x - d_->pressX) > 3 || std::abs(y - d_->pressY) > 3) {
            const bool toolBusy = tools_ && tools_->isActive() && tools_->capturesDrag();
            if (d_->pressButton == 2 || (d_->pressButton == 1 && d_->pressShift && !toolBusy)) d_->drag = Impl::Drag::Pan;
            else if (d_->pressButton == 3) d_->drag = Impl::Drag::Zoom;
            else if (d_->pressButton == 1 && !toolBusy) {
                const bool box = d_->pressCtrl || mode_ != ViewMode::Perspective;
                d_->drag = box ? Impl::Drag::Box : Impl::Drag::Orbit;
            }
        }
    }
    switch (d_->drag) {
    case Impl::Drag::Orbit: orbit(dx, dy); break;
    case Impl::Drag::Pan: pan(dx, dy); break;
    case Impl::Drag::Zoom: zoomAt(std::pow(1.01, dy), d_->pressX, d_->pressY); break;
    case Impl::Drag::Box: {
        const QPoint a = toQt(d_->pressX, d_->pressY);
        setRubberBand(QRect(a, q), q.x() < a.x());
        break;
    }
    default: break;
    }
    d_->lastX = x;
    d_->lastY = y;
    if (tools_ && tools_->isActive()) tools_->move(this, q, ctrl, shift);
    if (auto p = workPlanePoint(q)) emit cursorMoved(*p, true);
    else emit cursorMoved({}, false);
}

void Viewport3D::onRelease(int button, int x, int y, bool ctrl, bool shift) {
    const QPoint q = toQt(x, y);
    const auto drag = d_->drag;
    d_->drag = Impl::Drag::None;
    d_->pressButton = 0;
    if (drag == Impl::Drag::Box) {
        const QPoint a = toQt(d_->pressX, d_->pressY);
        setRubberBand(std::nullopt, false);
        const bool crossing = q.x() < a.x();
        const auto items = itemsInRect(QRect(a, q), crossing);
        ctx_.selectMany(items, ctrl ? SelectMode::Toggle : shift ? SelectMode::Add : SelectMode::Replace);
        return;
    }
    if (drag != Impl::Drag::Pending) return; // end of orbit/pan/zoom
    // A click.
    if (tools_ && tools_->isActive()) {
        if (button == 1 && tools_->click(this, q, ctrl, shift)) return;
        if (button == 3 && tools_->rightClick(this, q)) return;
    }
    if (button == 1) {
        const auto hit = pickAt(q);
        if (hit) ctx_.select(hit->owner.kind, hit->owner.id, ctrl ? SelectMode::Toggle : shift ? SelectMode::Add : SelectMode::Replace);
        else if (!ctrl && !shift) ctx_.clearSelection();
    } else if (button == 3) {
        emit contextMenuRequested(vtk_->mapToGlobal(q), pickAt(q));
    }
}

void Viewport3D::onWheel(int delta, int x, int y) {
    emit activated(this);
    zoomAt(delta > 0 ? 1.18 : 1.0 / 1.18, x, y);
}

void Viewport3D::onKey(const std::string& key, bool ctrl, bool shift) {
    if (tools_ && tools_->isActive() && tools_->key(this, key, ctrl, shift)) return;
    if (key == "Escape") {
        ctx_.clearSelection();
    } else if (key == "f" || key == "F") {
        fitSelection();
    } else if (key == "Prior" || key == "plus") {
        if (section_.enabled) stepSection(shift ? 5 : 1);
        else zoomBy(1.25);
    } else if (key == "Next" || key == "minus") {
        if (section_.enabled) stepSection(shift ? -5 : -1);
        else zoomBy(0.8);
    } else if (key == "Left" || key == "Right" || key == "Up" || key == "Down") {
        const double s = std::max(20, vtk_->width() / 10);
        pan(key == "Left" ? -s : key == "Right" ? s : 0, key == "Up" ? s : key == "Down" ? -s : 0);
    }
}

void Viewport3D::onLeave() {
    if (tools_ && tools_->isActive()) tools_->leave(this);
}

void Viewport3D::simulateClick(QPoint p, Qt::KeyboardModifiers mods, Qt::MouseButton button) {
    int x, y;
    vtk_->toDisplay(p, x, y);
    const int b = button == Qt::LeftButton ? 1 : button == Qt::MiddleButton ? 2 : 3;
    const bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier;
    onMove(x, y, ctrl, shift);
    onPress(b, x, y, ctrl, shift);
    onRelease(b, x, y, ctrl, shift);
}

void Viewport3D::simulateMove(QPoint p) {
    int x, y;
    vtk_->toDisplay(p, x, y);
    onMove(x, y, false, false);
}

void Viewport3D::simulateDrag(QPoint from, QPoint to, Qt::MouseButton button, Qt::KeyboardModifiers mods) {
    int x0, y0, x1, y1;
    vtk_->toDisplay(from, x0, y0);
    vtk_->toDisplay(to, x1, y1);
    const int b = button == Qt::LeftButton ? 1 : button == Qt::MiddleButton ? 2 : 3;
    const bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier;
    onPress(b, x0, y0, ctrl, shift);
    const int steps = 8;
    for (int i = 1; i <= steps; ++i) onMove(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, ctrl, shift);
    onRelease(b, x1, y1, ctrl, shift);
}

} // namespace openmine::app
