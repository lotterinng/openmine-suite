#pragma once
// SPDX-License-Identifier: Apache-2.0
// The scheduler workspace: scenario and baseline selection, the activity grid (WBS tree with
// editable name, duration, quantity, rate and progress) beside the Gantt chart, and resource
// histograms, production charts, baseline variances and levelling results below. Every edit is
// a validated undo step; the schedule of the active scenario recalculates automatically.

#include "../app/AppContext.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;

namespace openmine::app {

class GanttChart;
class ResourceHistogram;
class ProductionChart;
class VarianceTable;

class SchedulerPanel : public QWidget {
    Q_OBJECT
public:
    explicit SchedulerPanel(AppContext& ctx, QWidget* parent = nullptr);

    QTreeWidget* grid() const { return grid_; }
    GanttChart* gantt() const { return gantt_; }
    QTreeWidgetItem* itemFor(const Uuid& id) const;

public slots:
    Uuid addActivity(ActivityKind kind, const QString& name = {});
    void addDependency();
    void deleteSelected();
    void indent();
    void outdent();
    void moveSelected(int delta);
    void captureBaseline(const QString& name = {});
    void applyDesignQuantities();
    void recordActual();
    void editScenario(bool create);
    void optimise();
    bool exportFile(const QString& path);

private:
    void scheduleRebuild();
    void rebuild();
    void rebuildScenarios();
    void updateStatus();
    void syncSelectionFromContext();
    void onItemChanged(QTreeWidgetItem* item, int column);
    Uuid selectedActivity() const;
    Uuid parentForNew() const;

    AppContext& ctx_;
    QComboBox* scenario_ = nullptr;
    QLabel* status_ = nullptr;
    QTreeWidget* grid_ = nullptr;
    GanttChart* gantt_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    ResourceHistogram* histogram_ = nullptr;
    ProductionChart* production_ = nullptr;
    VarianceTable* variance_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    std::unordered_map<Uuid, QTreeWidgetItem*> items_;
    bool building_ = false;
    bool pending_ = false;
    bool syncing_ = false;
};

} // namespace openmine::app
