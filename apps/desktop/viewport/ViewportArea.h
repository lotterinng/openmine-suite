#pragma once
// SPDX-License-Identifier: Apache-2.0
// The central viewport area: one, two or four views of the shared scene (3D, plan, section,
// elevation), the active view, and the services the views share (scene items, the point-cloud
// node cache and the CAD tools).

#include "PointCloudLod.h"
#include "SceneCache.h"
#include "Tools.h"
#include "Viewport3D.h"

#include <QWidget>

#include <array>

class QSplitter;

namespace openmine::app {

class ViewportArea : public QWidget {
    Q_OBJECT
public:
    enum class Layout { Single, TwoColumns, TwoRows, Quad };

    explicit ViewportArea(AppContext& ctx, QWidget* parent = nullptr);
    ~ViewportArea() override;

    Layout layoutKind() const { return layout_; }
    void setLayoutKind(Layout l);

    Viewport3D* active() const { return active_; }
    void setActive(Viewport3D* v);
    /// The views currently shown, in layout order.
    std::vector<Viewport3D*> views() const;
    Viewport3D* view(int index) const;

    SceneCache& scene() { return *scene_; }
    PointCloudStore& clouds() { return *clouds_; }
    ToolController& tools() { return *tools_; }

    /// Shows a vertical section in a section view (a second view is opened when needed).
    void showSection(Vec3 point, Vec3 normal, double halfWidth, const std::string& label = {});
    /// Applies `f` to every view (display options, theme).
    void forEach(const std::function<void(Viewport3D&)>& f);

signals:
    void activeChanged(openmine::app::Viewport3D* view);
    void cursorMoved(openmine::Vec3 world, bool valid);
    void contextMenuRequested(QPoint globalPos, std::optional<openmine::app::PickHit> hit);

private:
    Viewport3D* ensure(int index);
    void rebuildLayout();

    AppContext& ctx_;
    std::unique_ptr<SceneCache> scene_;
    std::unique_ptr<PointCloudStore> clouds_;
    std::unique_ptr<ToolController> tools_;
    std::array<Viewport3D*, 4> views_{};
    Viewport3D* active_ = nullptr;
    Layout layout_ = Layout::Single;
    QSplitter* root_ = nullptr;
};

} // namespace openmine::app
