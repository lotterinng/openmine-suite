// SPDX-License-Identifier: Apache-2.0
#include "GanttChart.h"

#include "../theme/Theme.h"

#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/CalendarTime.h"

#include <QApplication>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <cmath>

namespace openmine::app {

namespace {
constexpr double kDay = 1440.0;
const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
}

GanttChart::GanttChart(AppContext& ctx, QTreeWidget* grid, QWidget* parent) : QWidget(parent), ctx_(ctx), grid_(grid) {
    hbar_ = new QScrollBar(Qt::Horizontal, this);
    connect(hbar_, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    connect(grid_, &QTreeWidget::itemExpanded, this, qOverload<>(&QWidget::update));
    connect(grid_, &QTreeWidget::itemCollapsed, this, qOverload<>(&QWidget::update));
    connect(grid_, &QTreeWidget::itemSelectionChanged, this, qOverload<>(&QWidget::update));
    connect(&ctx_, &AppContext::scheduleRecalculated, this, [this] {
        updateScrollRange();
        update();
    });
    connect(&ctx_, &AppContext::projectReplaced, this, &GanttChart::fitSchedule);
    setMouseTracking(true);
    setMinimumWidth(200);
    fitSchedule();
}

int GanttChart::headerHeight() const { return grid_->header()->height(); }

double GanttChart::xOf(double minutes) const { return (minutes - origin_) / kDay * ppd_ - hbar_->value(); }
double GanttChart::timeAt(double x) const { return origin_ + (x + hbar_->value()) / ppd_ * kDay; }

std::optional<QRectF> GanttChart::barOf(const Uuid& id) const {
    auto it = bars_.find(id);
    if (it == bars_.end()) return std::nullopt;
    return it->second;
}

void GanttChart::updateScrollRange() {
    const auto& r = ctx_.schedule().result;
    double start = ctx_.hasProject() ? static_cast<double>(ctx_.project().model().info.scheduleStart.minutes()) : 0.0;
    double finish = start + 120 * kDay;
    if (r.ok) {
        start = std::min(start, r.projectStart);
        finish = std::max(finish, r.projectFinish);
    }
    origin_ = std::floor(start / kDay) * kDay - 7 * kDay;
    span_ = finish - origin_ + 30 * kDay;
    const int total = static_cast<int>(span_ / kDay * ppd_);
    hbar_->setRange(0, std::max(0, total - width()));
    hbar_->setPageStep(std::max(1, width()));
    hbar_->setSingleStep(std::max(1, static_cast<int>(ppd_)));
}

void GanttChart::setPixelsPerDay(double ppd) {
    // Keep the centre of the view in place.
    const double centre = timeAt(width() / 2.0);
    ppd_ = std::clamp(ppd, 0.3, 600.0);
    updateScrollRange();
    hbar_->setValue(static_cast<int>((centre - origin_) / kDay * ppd_ - width() / 2.0));
    update();
}

void GanttChart::fitSchedule() {
    updateScrollRange();
    const auto& r = ctx_.schedule().result;
    if (r.ok && r.projectFinish > r.projectStart) {
        const double days = (r.projectFinish - r.projectStart) / kDay + 14;
        ppd_ = std::clamp((width() - 20) / days, 0.3, 600.0);
        updateScrollRange();
        hbar_->setValue(static_cast<int>((r.projectStart - 3 * kDay - origin_) / kDay * ppd_));
    } else {
        hbar_->setValue(0);
    }
    update();
}

void GanttChart::scrollTo(double minutes) { hbar_->setValue(static_cast<int>((minutes - origin_) / kDay * ppd_ - width() / 3.0)); }

void GanttChart::resizeEvent(QResizeEvent*) {
    hbar_->setGeometry(0, height() - hbar_->sizeHint().height(), width(), hbar_->sizeHint().height());
    updateScrollRange();
}

void GanttChart::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::ControlModifier) {
        zoom(e->angleDelta().y() > 0 ? 1.25 : 1 / 1.25);
        e->accept();
        return;
    }
    if (e->modifiers() & Qt::ShiftModifier) {
        hbar_->setValue(hbar_->value() - e->angleDelta().y());
        e->accept();
        return;
    }
    QApplication::sendEvent(grid_->verticalScrollBar(), e);
}

std::optional<Uuid> GanttChart::activityAt(QPoint p) const {
    for (const auto& [id, r] : bars_)
        if (r.adjusted(-3, -3, 3, 3).contains(p)) return id;
    const QPoint inTree(5, p.y() - headerHeight());
    if (auto* it = grid_->itemAt(inTree))
        if (auto id = Uuid::parse(it->data(0, kIdRole).toString().toStdString())) return *id;
    return std::nullopt;
}

void GanttChart::mousePressEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    auto id = activityAt(pos);
    if (!id) return;
    emit activityClicked(*id, e->modifiers());
    // Dragging a task bar (not started, not a summary) moves it by a SNET constraint.
    if (e->button() == Qt::LeftButton && ctx_.hasProject()) {
        auto it = bars_.find(*id);
        const auto* a = ctx_.project().model().activities.find(*id);
        if (it != bars_.end() && it->second.contains(pos) && a && a->kind != ActivityKind::Summary && !a->actualStart) {
            dragId_ = *id;
            dragStartX_ = pos.x();
            dragOffset_ = 0;
        }
    }
}

void GanttChart::mouseMoveEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    if (dragId_) {
        dragOffset_ = pos.x() - dragStartX_;
        const auto t = ctx_.schedule().result.timings.find(*dragId_);
        if (t != ctx_.schedule().result.timings.end()) {
            const double ns = t->second.es + dragOffset_ / ppd_ * kDay;
            QToolTip::showText(e->globalPosition().toPoint(), tr("Start no earlier than %1").arg(formatDateTimeMinutes(std::round(ns / 60) * 60)), this);
        }
        update();
        return;
    }
    if (auto id = activityAt(pos); id && ctx_.hasProject()) {
        const auto* a = ctx_.project().model().activities.find(*id);
        const auto t = ctx_.schedule().result.timings.find(*id);
        if (a && t != ctx_.schedule().result.timings.end()) {
            const auto& tt = t->second;
            setToolTip(tr("<b>%1</b><br>%2 → %3<br>%4 working hours, total float %5 h%6<br>%7")
                           .arg(qstr(a->name).toHtmlEscaped(), formatDateTimeMinutes(tt.es), formatDateTimeMinutes(tt.ef))
                           .arg(tt.totalHours, 0, 'f', 1)
                           .arg(tt.totalFloatHours, 0, 'f', 1)
                           .arg(tt.critical ? tr(" — <b>critical</b>") : QString())
                           .arg(qstr(tt.durationBasis)));
        }
    } else {
        setToolTip({});
    }
}

void GanttChart::mouseReleaseEvent(QMouseEvent*) {
    if (!dragId_) return;
    const Uuid id = *dragId_;
    dragId_.reset();
    const double dx = dragOffset_;
    dragOffset_ = 0;
    update();
    if (std::abs(dx) < 3 || !ctx_.hasProject()) return;
    const auto t = ctx_.schedule().result.timings.find(id);
    const auto* a = ctx_.project().model().activities.find(id);
    if (!a || t == ctx_.schedule().result.timings.end()) return;
    const double ns = std::round((t->second.es + dx / ppd_ * kDay) / 60.0) * 60.0;
    ScheduleActivity n = *a;
    n.constraint = ConstraintType::StartNoEarlierThan;
    n.constraintDate = DateTime(static_cast<std::int64_t>(ns));
    ctx_.tryEdit(tr("Move activity"), [&] { ProjectEditor(ctx_.project()).updateActivity(n, "Move " + n.name + " (start no earlier than)"); });
}

void GanttChart::mouseDoubleClickEvent(QMouseEvent* e) {
    if (auto id = activityAt(e->position().toPoint())) emit activityDoubleClicked(*id);
}

void GanttChart::drawTimescale(QPainter& p, int hh) {
    const auto& c = Theme::colors();
    p.fillRect(QRect(0, 0, width(), hh), c.panelAlt);
    p.setPen(c.border);
    p.drawLine(0, hh - 1, width(), hh - 1);
    p.drawLine(0, hh / 2, width(), hh / 2);
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.82);
    p.setFont(small);
    const double t0 = timeAt(0), t1 = timeAt(width());
    const Date d0 = DateTime(static_cast<std::int64_t>(t0)).date().addDays(-40), d1 = DateTime(static_cast<std::int64_t>(t1)).date().addDays(1);
    // Upper band: years (coarse) or months; lower band: months, weeks, days or hours by zoom.
    const bool showDays = ppd_ >= 18, showHours = ppd_ >= 160, showWeeks = ppd_ * 7 >= 30, monthsLower = !showWeeks;
    for (Date d = d0; d <= d1; d = d.addDays(1)) {
        int y;
        unsigned mo, da;
        d.toYmd(y, mo, da);
        const double x = xOf(static_cast<double>(d.days()) * kDay);
        if (da == 1) {
            const bool yearMark = mo == 1;
            if (!monthsLower || yearMark) {
                p.setPen(c.border);
                p.drawLine(QPointF(x, 0), QPointF(x, monthsLower ? hh / 2.0 : hh / 2.0));
                p.setPen(c.text);
                const QString label = monthsLower ? QString::number(y) : QStringLiteral("%1 %2").arg(kMonths[mo - 1]).arg(y);
                p.drawText(QPointF(std::max(2.0, x + 3), hh * 0.38), label);
            }
            if (monthsLower) {
                p.setPen(c.border);
                p.drawLine(QPointF(x, hh / 2.0), QPointF(x, hh));
                if (ppd_ * 30 > 24) {
                    p.setPen(c.textMuted);
                    p.drawText(QPointF(x + 2, hh - 5), kMonths[mo - 1]);
                }
            }
        }
        if (!monthsLower) {
            if (showHours) {
                p.setPen(c.border);
                p.drawLine(QPointF(x, hh / 2.0), QPointF(x, hh));
                p.setPen(c.textMuted);
                p.drawText(QPointF(x + 2, hh - 5), QStringLiteral("%1 %2").arg(da).arg(kMonths[mo - 1]));
                for (int h = 6; h < 24; h += 6) {
                    const double xh = x + ppd_ * h / 24.0;
                    p.setPen(c.ganttGrid);
                    p.drawLine(QPointF(xh, hh * 0.75), QPointF(xh, hh));
                    p.setPen(c.textMuted);
                    p.drawText(QPointF(xh + 2, hh - 5), QStringLiteral("%1:00").arg(h));
                }
            } else if (showDays) {
                p.setPen(c.border);
                p.drawLine(QPointF(x, hh / 2.0), QPointF(x, hh));
                p.setPen(d.isoWeekday() >= 6 ? c.warning : c.textMuted);
                p.drawText(QPointF(x + 2, hh - 5), QString::number(da));
            } else if (d.isoWeekday() == 1) {
                p.setPen(c.border);
                p.drawLine(QPointF(x, hh / 2.0), QPointF(x, hh));
                p.setPen(c.textMuted);
                p.drawText(QPointF(x + 2, hh - 5), QString::number(da));
            }
        }
    }
}

void GanttChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& c = Theme::colors();
    const int hh = headerHeight();
    const int bottom = height() - hbar_->height();
    p.fillRect(rect(), c.panel);
    bars_.clear();
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    const auto& st = ctx_.schedule();
    const auto& sched = st.result;

    // Non-working time of the project calendar (day resolution when zoomed out, periods when in).
    p.setClipRect(0, hh, width(), bottom - hh);
    const auto* cal = m.calendars.find(m.info.defaultCalendarId);
    if (cal) {
        scheduling::CalendarTime ct({cal});
        const double t0 = timeAt(0), t1 = timeAt(width());
        const Date d0 = DateTime(static_cast<std::int64_t>(t0)).date().addDays(-1), d1 = DateTime(static_cast<std::int64_t>(t1)).date().addDays(1);
        if (d1.days() - d0.days() < 3000) {
            for (Date d = d0; d <= d1; d = d.addDays(1)) {
                const auto& periods = ct.periodsOn(d);
                const double day0 = static_cast<double>(d.days()) * kDay;
                if (periods.empty()) {
                    p.fillRect(QRectF(xOf(day0), hh, ppd_, bottom - hh), c.ganttWeekend);
                } else if (ppd_ >= 40) {
                    int prev = 0;
                    for (const auto& pr : periods) {
                        if (pr.startMinute > prev) p.fillRect(QRectF(xOf(day0 + prev), hh, (pr.startMinute - prev) / kDay * ppd_, bottom - hh), c.ganttWeekend);
                        prev = pr.endMinute;
                    }
                    if (prev < 1440) p.fillRect(QRectF(xOf(day0 + prev), hh, (1440 - prev) / kDay * ppd_, bottom - hh), c.ganttWeekend);
                }
            }
        }
    }
    // Month grid lines.
    p.setPen(c.ganttGrid);
    {
        const Date d0 = DateTime(static_cast<std::int64_t>(timeAt(0))).date(), d1 = DateTime(static_cast<std::int64_t>(timeAt(width()))).date();
        for (Date d = d0; d <= d1; d = d.addDays(1)) {
            int y;
            unsigned mo, da;
            d.toYmd(y, mo, da);
            if (da == 1 || (ppd_ * 7 >= 30 && d.isoWeekday() == 1)) p.drawLine(QPointF(xOf(d.days() * kDay), hh), QPointF(xOf(d.days() * kDay), bottom));
        }
    }
    if (m.info.statusDate) {
        const double x = xOf(static_cast<double>(m.info.statusDate->minutes()));
        p.setPen(QPen(c.accent, 2, Qt::DashLine));
        p.drawLine(QPointF(x, hh), QPointF(x, bottom));
    }
    if (ctx_.playbackTime()) {
        const double x = xOf(*ctx_.playbackTime());
        p.setPen(QPen(QColor(0x00, 0xB4, 0xFF), 2));
        p.drawLine(QPointF(x, hh), QPointF(x, bottom));
    }
    p.setClipping(false);
    drawTimescale(p, hh);

    if (!sched.ok) {
        p.setPen(c.ganttCritical);
        p.drawText(QRect(0, hh + 10, width(), 40), Qt::AlignCenter,
                   tr("Schedule not calculated: %1").arg(sched.errors.empty() ? QString() : qstr(sched.errors.front())));
        return;
    }
    const auto* bl = ctx_.comparisonBaseline();
    std::map<Uuid, const BaselineActivity*> blIndex;
    if (bl)
        for (const auto& b : bl->activities) blIndex[b.activityId] = &b;
    std::map<Uuid, double> levelDelay;
    if (st.levelling)
        for (const auto& [id, h] : st.levelling->delays) levelDelay[id] = h;

    p.setClipRect(0, hh, width(), bottom - hh);
    for (QTreeWidgetItemIterator it(grid_); *it; ++it) {
        QTreeWidgetItem* item = *it;
        const QRect r = grid_->visualItemRect(item);
        if (r.height() == 0) continue;
        const double y = hh + r.y();
        if (y > bottom || y + r.height() < hh) continue;
        auto id = Uuid::parse(item->data(0, kIdRole).toString().toStdString());
        if (!id) continue;
        const auto* a = m.activities.find(*id);
        auto t = sched.timings.find(*id);
        if (!a || t == sched.timings.end()) continue;
        const auto& tt = t->second;
        if (item->isSelected()) p.fillRect(QRectF(0, y, width(), r.height()), QColor(c.accent.red(), c.accent.green(), c.accent.blue(), 40));
        double x0 = xOf(tt.es), x1 = xOf(tt.ef);
        if (dragId_ && *dragId_ == *id) {
            x0 += dragOffset_;
            x1 += dragOffset_;
        }
        const double h = r.height();
        if (auto b = blIndex.find(*id); b != blIndex.end() && a->kind != ActivityKind::Summary) {
            const double bx0 = xOf(static_cast<double>(b->second->start.minutes())), bx1 = xOf(static_cast<double>(b->second->finish.minutes()));
            p.fillRect(QRectF(bx0, y + h * 0.74, std::max(1.5, bx1 - bx0), h * 0.13), c.ganttBaseline);
        }
        if (auto ld = levelDelay.find(*id); ld != levelDelay.end() && ld->second > 0) {
            // Where CPM alone would have started it (resource levelling delay).
            if (auto ct = st.cpm.timings.find(*id); ct != st.cpm.timings.end()) {
                const double cx = xOf(ct->second.es);
                p.setPen(QPen(c.warning, 1, Qt::DotLine));
                p.drawLine(QPointF(cx, y + h * 0.43), QPointF(x0, y + h * 0.43));
                p.drawLine(QPointF(cx, y + h * 0.3), QPointF(cx, y + h * 0.56));
            }
        }
        if (a->kind == ActivityKind::Milestone || tt.totalHours <= 0) {
            const double cx = x0, cy = y + h * 0.43, s = h * 0.3;
            QPainterPath dia;
            dia.moveTo(cx, cy - s);
            dia.lineTo(cx + s, cy);
            dia.lineTo(cx, cy + s);
            dia.lineTo(cx - s, cy);
            dia.closeSubpath();
            p.fillPath(dia, tt.critical ? c.ganttCritical : (tt.completed ? c.ganttProgress : c.ganttSummary));
            bars_[*id] = QRectF(cx - s, cy - s, 2 * s, 2 * s);
            p.setPen(c.textMuted);
            if (ppd_ >= 4) p.drawText(QPointF(cx + s + 4, cy + 4), qstr(a->name));
        } else if (a->kind == ActivityKind::Summary) {
            const QRectF br(x0, y + h * 0.3, std::max(2.0, x1 - x0), h * 0.2);
            p.fillRect(br, c.ganttSummary);
            QPainterPath l;
            l.moveTo(x0, br.bottom());
            l.lineTo(x0 + 5, br.bottom());
            l.lineTo(x0, br.bottom() + 6);
            l.closeSubpath();
            QPainterPath rr;
            rr.moveTo(x1, br.bottom());
            rr.lineTo(x1 - 5, br.bottom());
            rr.lineTo(x1, br.bottom() + 6);
            rr.closeSubpath();
            p.fillPath(l, c.ganttSummary);
            p.fillPath(rr, c.ganttSummary);
            bars_[*id] = br;
        } else {
            const QRectF br(x0, y + h * 0.2, std::max(2.0, x1 - x0), h * 0.46);
            p.setPen(Qt::NoPen);
            p.setBrush(tt.critical ? c.ganttCritical : c.ganttTask);
            p.drawRoundedRect(br, 2, 2);
            if (tt.percentComplete > 0) {
                p.setBrush(c.ganttProgress);
                p.drawRect(QRectF(br.left(), br.top() + br.height() * 0.62, br.width() * std::min(100.0, tt.percentComplete) / 100.0, br.height() * 0.38));
            }
            if (tt.started && !tt.completed && tt.remainingStart > tt.es) {
                // Actual part to the status date.
                p.setPen(QPen(c.ganttProgress, 1.5));
                p.drawLine(QPointF(br.left(), br.top() - 2), QPointF(xOf(tt.remainingStart), br.top() - 2));
            }
            bars_[*id] = br;
            if (ppd_ >= 3) {
                p.setPen(c.textMuted);
                QString label = qstr(a->name);
                if (!a->workArea.empty()) label += QStringLiteral("  [%1]").arg(qstr(a->workArea));
                p.drawText(QPointF(br.right() + 6, br.bottom() - 2), label);
            }
        }
        if (a->designImpactFlag) {
            const QRectF br = bars_[*id];
            QPainterPath tri;
            const double tx = br.right() + 2, ty = y + 2;
            tri.moveTo(tx + 6, ty);
            tri.lineTo(tx + 12, ty + 11);
            tri.lineTo(tx, ty + 11);
            tri.closeSubpath();
            p.fillPath(tri, c.warning);
        }
        if (a->constraint != ConstraintType::None && a->constraintDate) {
            const double cx = xOf(static_cast<double>(a->constraintDate->minutes()));
            p.setPen(QPen(c.accent, 1.5));
            p.drawLine(QPointF(cx, y + 2), QPointF(cx, y + h - 2));
            p.drawText(QPointF(cx + 2, y + 10), a->constraint == ConstraintType::StartNoEarlierThan ? "▸" : a->constraint == ConstraintType::FinishNoLaterThan ? "◂" : "◆");
        }
    }
    // Dependency arrows.
    for (const auto& d : m.dependencies.all()) {
        auto pa = bars_.find(d.predecessorId), sa = bars_.find(d.successorId);
        if (pa == bars_.end() || sa == bars_.end()) continue;
        const QRectF P = pa->second, S = sa->second;
        const bool fromFinish = d.type == DependencyType::FinishToStart || d.type == DependencyType::FinishToFinish;
        const bool toStart = d.type == DependencyType::FinishToStart || d.type == DependencyType::StartToStart;
        const auto pt = sched.timings.find(d.predecessorId), stt = sched.timings.find(d.successorId);
        const bool crit = pt != sched.timings.end() && stt != sched.timings.end() && pt->second.critical && stt->second.critical;
        const bool selected = ctx_.isSelected(EntityKind::Dependency, d.id);
        p.setPen(QPen(selected ? c.selection : crit ? c.ganttCritical : c.textMuted, selected ? 2.2 : 1.2));
        const QPointF a0(fromFinish ? P.right() : P.left(), P.center().y());
        const QPointF b0(toStart ? S.left() : S.right(), S.center().y());
        const double stub = toStart ? -6 : 6;
        QPainterPath path(a0);
        const double xm = fromFinish ? a0.x() + 6 : a0.x() - 6;
        path.lineTo(xm, a0.y());
        path.lineTo(xm, (a0.y() + b0.y()) / 2);
        path.lineTo(b0.x() + stub, (a0.y() + b0.y()) / 2);
        path.lineTo(b0.x() + stub, b0.y());
        path.lineTo(b0.x(), b0.y());
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        const double dir = toStart ? 1 : -1;
        const QPolygonF head{b0, {b0.x() - dir * 5, b0.y() - 3.5}, {b0.x() - dir * 5, b0.y() + 3.5}};
        p.setBrush(crit ? c.ganttCritical : c.textMuted);
        p.drawPolygon(head);
        if (d.lagHours != 0 && ppd_ >= 8) {
            p.setPen(c.textMuted);
            p.drawText(QPointF(xm + 3, (a0.y() + b0.y()) / 2 - 2), QStringLiteral("%1%2h").arg(d.lagHours > 0 ? "+" : "").arg(d.lagHours));
        }
    }
}

} // namespace openmine::app
