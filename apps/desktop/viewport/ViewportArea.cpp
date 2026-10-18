// SPDX-License-Identifier: Apache-2.0
#include "ViewportArea.h"

#include <QSplitter>
#include <QVBoxLayout>

namespace openmine::app {

ViewportArea::ViewportArea(AppContext& ctx, QWidget* parent)
    : QWidget(parent), ctx_(ctx), scene_(std::make_unique<SceneCache>(ctx)), clouds_(std::make_unique<PointCloudStore>()),
      tools_(std::make_unique<ToolController>(ctx)) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    connect(&ctx_, &AppContext::projectReplaced, this, [this] { clouds_->clear(); });
    connect(clouds_.get(), &PointCloudStore::loadFailed, this, [this](const QString& m) {
        ctx_.report(tr("Point cloud: %1").arg(m), MessageLevel::Warning);
    });
    connect(tools_.get(), &ToolController::sectionDefined, this, [this](Vec3 p, Vec3 n, double) { showSection(p, n, 2.5); });
    ensure(0);
    rebuildLayout();
    setActive(views_[0]);
}

ViewportArea::~ViewportArea() {
    // Views reference the scene, clouds and tools: delete them first.
    for (auto*& v : views_) {
        delete v;
        v = nullptr;
    }
}

Viewport3D* ViewportArea::ensure(int i) {
    if (views_[static_cast<std::size_t>(i)]) return views_[static_cast<std::size_t>(i)];
    auto* v = new Viewport3D(ctx_, *scene_, *clouds_, i + 1, nullptr);
    v->setToolController(tools_.get());
    connect(v, &Viewport3D::activated, this, &ViewportArea::setActive);
    connect(v, &Viewport3D::cursorMoved, this, &ViewportArea::cursorMoved);
    connect(v, &Viewport3D::contextMenuRequested, this, &ViewportArea::contextMenuRequested);
    views_[static_cast<std::size_t>(i)] = v;
    // Sensible defaults for the views of the quad layout.
    switch (i) {
    case 1: v->setMode(ViewMode::Plan); break;
    case 2: v->setMode(ViewMode::Section); break;
    case 3: v->setMode(ViewMode::Elevation); break;
    default: break;
    }
    return v;
}

void ViewportArea::rebuildLayout() {
    // Detach views from the old splitters before deleting them.
    for (auto* v : views_)
        if (v) v->setParent(nullptr);
    delete root_;
    root_ = nullptr;
    auto* lay = static_cast<QVBoxLayout*>(layout());
    switch (layout_) {
    case Layout::Single: {
        root_ = new QSplitter(Qt::Horizontal, this);
        root_->addWidget(ensure(0));
        break;
    }
    case Layout::TwoColumns:
    case Layout::TwoRows: {
        root_ = new QSplitter(layout_ == Layout::TwoColumns ? Qt::Horizontal : Qt::Vertical, this);
        root_->addWidget(ensure(0));
        root_->addWidget(ensure(1));
        root_->setSizes({1000, 1000});
        break;
    }
    case Layout::Quad: {
        root_ = new QSplitter(Qt::Vertical, this);
        auto* top = new QSplitter(Qt::Horizontal);
        auto* bottom = new QSplitter(Qt::Horizontal);
        top->addWidget(ensure(0));
        top->addWidget(ensure(1));
        bottom->addWidget(ensure(2));
        bottom->addWidget(ensure(3));
        top->setSizes({1000, 1000});
        bottom->setSizes({1000, 1000});
        root_->addWidget(top);
        root_->addWidget(bottom);
        root_->setSizes({1000, 1000});
        break;
    }
    }
    root_->setChildrenCollapsible(false);
    root_->setHandleWidth(3);
    lay->addWidget(root_);
    for (auto* v : views())
        v->show();
    if (!active_ || !active_->isVisibleTo(this)) setActive(views_[0]);
}

void ViewportArea::setLayoutKind(Layout l) {
    if (l == layout_) return;
    layout_ = l;
    rebuildLayout();
}

std::vector<Viewport3D*> ViewportArea::views() const {
    std::vector<Viewport3D*> out;
    const int n = layout_ == Layout::Single ? 1 : (layout_ == Layout::Quad ? 4 : 2);
    for (int i = 0; i < n; ++i)
        if (views_[static_cast<std::size_t>(i)]) out.push_back(views_[static_cast<std::size_t>(i)]);
    return out;
}

Viewport3D* ViewportArea::view(int i) const { return i >= 0 && i < 4 ? views_[static_cast<std::size_t>(i)] : nullptr; }

void ViewportArea::setActive(Viewport3D* v) {
    if (!v || v == active_) return;
    if (active_) active_->setActiveView(false);
    active_ = v;
    active_->setActiveView(views().size() > 1);
    emit activeChanged(v);
}

void ViewportArea::forEach(const std::function<void(Viewport3D&)>& f) {
    for (auto* v : views_)
        if (v) f(*v);
}

void ViewportArea::showSection(Vec3 point, Vec3 normal, double halfWidth, const std::string& label) {
    // Prefer an existing section view other than the active one, else open one beside it.
    Viewport3D* target = nullptr;
    for (auto* v : views())
        if (v != active_ && v->mode() == ViewMode::Section) target = v;
    if (!target) {
        for (auto* v : views())
            if (v != active_) target = v;
    }
    if (!target) {
        setLayoutKind(Layout::TwoColumns);
        target = views_[1] == active_ ? views_[0] : views_[1];
    }
    SectionDef s = target->section();
    s.enabled = true;
    s.point = point;
    s.normal = normal;
    s.halfWidth = halfWidth;
    s.label = label;
    target->setSection(s);
    target->fitAll();
    ctx_.report(tr("Section through %1, %2, %3 (±%4 m) in view %5")
                    .arg(formatNumber(point.x), formatNumber(point.y), formatNumber(point.z))
                    .arg(halfWidth)
                    .arg(target->id()));
}

} // namespace openmine::app
