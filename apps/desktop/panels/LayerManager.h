#pragma once
// SPDX-License-Identifier: Apache-2.0
// Layer manager: current layer, visibility, lock, colour, transparency, line width and line type
// of every CAD layer, object counts, creation, deletion (refused while objects use the layer) and
// drawing order — every change one validated undo step.

#include "../app/AppContext.h"

#include <QWidget>

class QTableWidget;

namespace openmine::app {

class LayerManager : public QWidget {
    Q_OBJECT
public:
    explicit LayerManager(AppContext& ctx, QWidget* parent = nullptr);
    QTableWidget* table() const { return table_; }

    bool addLayer(const QString& name, std::uint32_t color);
    bool moveLayer(const Uuid& id, int delta);

private:
    void scheduleRebuild();
    void rebuild();
    void edit(const Uuid& id, const std::function<void(CadLayer&)>& change, const QString& what);
    Uuid layerAt(int row) const;

    AppContext& ctx_;
    QTableWidget* table_ = nullptr;
    bool pending_ = false;
    bool building_ = false;
};

} // namespace openmine::app
