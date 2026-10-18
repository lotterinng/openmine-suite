#pragma once
// SPDX-License-Identifier: Apache-2.0
// Gantt chart of the calculated schedule, row-aligned with the activity grid: timescale (years,
// months, weeks, days or shifts by zoom), non-working time of the project calendar, bars for
// tasks (critical in red, progress, levelling delay), summaries and milestones, the comparison
// baseline, dependency arrows by type, the status date and design-impact flags. Dragging a task
// bar sets a "start no earlier than" constraint (one undo step); the schedule recalculates.

#include "../app/AppContext.h"

#include <QWidget>

#include <map>
#include <optional>

class QScrollBar;
class QTreeWidget;

namespace openmine::app {

class GanttChart : public QWidget {
    Q_OBJECT
public:
    GanttChart(AppContext& ctx, QTreeWidget* grid, QWidget* parent = nullptr);

    void setPixelsPerDay(double ppd);
    double pixelsPerDay() const { return ppd_; }
    void zoom(double factor) { setPixelsPerDay(ppd_ * factor); }
    void fitSchedule();
    void scrollTo(double minutes);
    /// x (widget px) of an instant and back.
    double xOf(double minutes) const;
    double timeAt(double x) const;
    /// Bar rectangle of an activity as last painted.
    std::optional<QRectF> barOf(const Uuid& id) const;
    static constexpr int kIdRole = Qt::UserRole + 10;

signals:
    void activityClicked(openmine::Uuid id, Qt::KeyboardModifiers mods);
    void activityDoubleClicked(openmine::Uuid id);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    int headerHeight() const;
    void updateScrollRange();
    std::optional<Uuid> activityAt(QPoint p) const;
    void drawTimescale(QPainter& p, int hh);

    AppContext& ctx_;
    QTreeWidget* grid_;
    QScrollBar* hbar_;
    double origin_ = 0;    ///< minutes at x = 0 (before scrolling)
    double span_ = 0;      ///< minutes covered
    double ppd_ = 14.0;    ///< pixels per day
    std::map<Uuid, QRectF> bars_;
    // Bar drag.
    std::optional<Uuid> dragId_;
    double dragStartX_ = 0, dragOffset_ = 0;
};

} // namespace openmine::app
