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
class QLineEdit;
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

    /// Grid filters: text (name, WBS, design, resources, zone), and a status filter.
    enum class Show { All, Critical, Flagged, InProgress, NotStarted, Complete, BehindBaseline };
    /// Grouping of the grid (WBS = the activity hierarchy).
    enum class GroupBy { Wbs, Level, Zone, WorkArea, Resource, Calendar, Kind };
    void setFilter(const QString& text);
    void setShow(Show s);
    void setGroupBy(GroupBy g);
    GroupBy groupBy() const { return groupBy_; }
    /// Activities currently shown in the grid (after filtering).
    int visibleActivityCount() const;

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
    void applyFilter();
    bool matches(const ScheduleActivity& a, QTreeWidgetItem* item) const;
    QString groupKey(const ScheduleActivity& a) const;
    Uuid selectedActivity() const;
    Uuid parentForNew() const;

    AppContext& ctx_;
    QComboBox* scenario_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QComboBox* show_ = nullptr;
    QComboBox* group_ = nullptr;
    Show showMode_ = Show::All;
    GroupBy groupBy_ = GroupBy::Wbs;
    QString filterText_;
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
