#pragma once
// SPDX-License-Identifier: Apache-2.0
// Project explorer: every entity of the project grouped by workspace (survey, CAD, mine design,
// geology, geotechnical, schedule, reports), with hierarchy where the model has one (objects under
// their layer, developments under their level, derived point clouds under their parent, mining
// blocks under their stope, activities in WBS order, assignments under resources), visibility
// check boxes, a filter, multi-selection synchronised with the viewports, and context menus.

#include "../app/AppContext.h"

#include <QWidget>

#include <set>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QMenu;

namespace openmine::app {

class SceneCache;

class ProjectExplorer : public QWidget {
    Q_OBJECT
public:
    ProjectExplorer(AppContext& ctx, SceneCache& scene, QWidget* parent = nullptr);

    QTreeWidget* tree() const { return tree_; }
    /// Item of an entity (nullptr when filtered out or absent).
    QTreeWidgetItem* itemFor(const Uuid& id) const;
    void setFilter(const QString& text);
    /// Number of entity items currently shown.
    int entityItemCount() const;

signals:
    /// A context-menu action handled by the main window ("zoom", "report", "export", …).
    void actionRequested(QString action, std::vector<openmine::app::SelectionItem> items);
    void propertiesRequested();

private:
    void scheduleRebuild();
    void rebuild();
    void syncSelectionFromContext();
    void syncSelectionToContext();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void contextMenu(const QPoint& pos);
    QTreeWidgetItem* group(QTreeWidgetItem* parent, const QString& key, const QString& title, const QString& icon);
    QTreeWidgetItem* entity(QTreeWidgetItem* parent, EntityKind kind, const Uuid& id, const QString& text = {}, const QString& icon = {});
    void finishGroups();
    void applyFilter();
    void deleteSelected();
    void renameItem(const SelectionItem& s);

    AppContext& ctx_;
    SceneCache& scene_;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    std::unordered_map<Uuid, QTreeWidgetItem*> items_;
    std::vector<QTreeWidgetItem*> groups_;
    std::set<QString> expanded_;
    bool building_ = false;
    bool syncing_ = false;
    bool pending_ = false;
};

} // namespace openmine::app
