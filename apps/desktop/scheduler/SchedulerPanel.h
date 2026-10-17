#pragma once
// SPDX-License-Identifier: Apache-2.0
// Activity grid (expandable WBS) + Gantt chart with dependency arrows, critical path,
// baseline and progress, driven by the CPM engine.

#include "../AppContext.h"

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QScrollBar;

namespace openmine::app {

class GanttChart;

class SchedulerPanel : public QWidget {
    Q_OBJECT
public:
    explicit SchedulerPanel(AppContext& ctx, QWidget* parent = nullptr);

    QTreeWidget* grid() const { return tree_; }
    GanttChart* chart() const { return chart_; }
    QTreeWidgetItem* itemFor(const Uuid& id) const;

public slots:
    void addTask();
    void addMilestone();
    void addSummary();
    void addDependencyDialog();
    void deleteSelected();
    void calculate();
    void captureBaseline();
    void applyDesignQuantities();
    void exportCsv(const QString& path = {});
    void zoom(double factor);

private:
    void rebuild();
    void scheduleRebuild();
    void syncSelection();
    Uuid selectedActivity() const;
    Uuid newActivityParent() const;

    AppContext& ctx_;
    QTreeWidget* tree_ = nullptr;
    GanttChart* chart_ = nullptr;
    bool building_ = false;
    bool pending_ = false;
};

class GanttChart : public QWidget {
    Q_OBJECT
public:
    GanttChart(AppContext& ctx, QTreeWidget* tree, QWidget* parent = nullptr);
    void setPixelsPerDay(double ppd);
    double pixelsPerDay() const { return ppd_; }
    void resetRange();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    double xOf(Date d) const;
    int headerHeight() const;
    void updateScrollRange();

    AppContext& ctx_;
    QTreeWidget* tree_;
    QScrollBar* hbar_;
    Date origin_;
    long spanDays_ = 120;
    double ppd_ = 14.0;
};

} // namespace openmine::app
