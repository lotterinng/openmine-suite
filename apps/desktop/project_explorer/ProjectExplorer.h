#pragma once
// SPDX-License-Identifier: Apache-2.0

#include "../AppContext.h"

#include <QTreeWidget>

namespace openmine::app {

/// Hierarchical view of the unified project model. Selecting an entity selects it
/// everywhere (viewport, properties, scheduler). Layer check boxes toggle visibility
/// through undoable commands.
class ProjectExplorer : public QTreeWidget {
    Q_OBJECT
public:
    explicit ProjectExplorer(AppContext& ctx, QWidget* parent = nullptr);
    QTreeWidgetItem* findItem(const Uuid& id) const;

private:
    void rebuild();
    void syncSelection();
    void scheduleRebuild();
    AppContext& ctx_;
    bool rebuilding_ = false;
    bool pending_ = false;
};

} // namespace openmine::app
