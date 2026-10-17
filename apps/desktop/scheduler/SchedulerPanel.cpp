// SPDX-License-Identifier: Apache-2.0
#include "SchedulerPanel.h"

#include "../theme/Theme.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/WorkingTime.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSplitter>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace openmine::app {

namespace {
constexpr int RoleId = Qt::UserRole + 1;
enum Col { ColName, ColWbs, ColDur, ColStart, ColFinish, ColQty, ColRate, ColActual, ColTf, ColDesign, ColCount };

const ScheduleBaseline* latestBaseline(const ProjectModel& m) { return m.baselines.empty() ? nullptr : &m.baselines.back(); }
} // namespace

// ===========================================================================
SchedulerPanel::SchedulerPanel(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* tb = new QToolBar(this);
    tb->setIconSize(QSize(18, 18));
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->addAction(toolIcon("activity"), "Task", this, &SchedulerPanel::addTask);
    tb->addAction(toolIcon("baseline"), "Milestone", this, &SchedulerPanel::addMilestone);
    tb->addAction(toolIcon("gantt"), "Summary", this, &SchedulerPanel::addSummary);
    tb->addAction(toolIcon("link"), "Dependency…", this, &SchedulerPanel::addDependencyDialog);
    tb->addAction(toolIcon("delete"), "Delete", this, &SchedulerPanel::deleteSelected);
    tb->addSeparator();
    tb->addAction(toolIcon("calc"), "Calculate", this, &SchedulerPanel::calculate);
    tb->addAction(toolIcon("baseline"), "Baseline", this, &SchedulerPanel::captureBaseline);
    tb->addSeparator();
    tb->addAction("Zoom +", this, [this] { zoom(1.4); });
    tb->addAction("Zoom −", this, [this] { zoom(1 / 1.4); });
    tb->addAction("Export CSV…", this, [this] { exportCsv(); });
    auto* pdf = tb->addAction("Export PDF");
    pdf->setEnabled(false);
    pdf->setToolTip("Not available in this build (planned: Phase 4)");
    lay->addWidget(tb);

    auto* split = new QSplitter(Qt::Horizontal, this);
    tree_ = new QTreeWidget(split);
    tree_->setColumnCount(ColCount);
    tree_->setHeaderLabels({"Activity", "WBS", "Dur (d)", "Start", "Finish", "Qty", "Rate/d", "Actual", "TF (d)", "Design"});
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->setRootIsDecorated(true);
    tree_->header()->setStretchLastSection(false);
    tree_->setColumnWidth(ColName, 220);
    for (int c = ColWbs; c < ColCount; ++c) tree_->setColumnWidth(c, c == ColStart || c == ColFinish ? 86 : 62);
    tree_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    chart_ = new GanttChart(ctx_, tree_, split);
    split->addWidget(tree_);
    split->addWidget(chart_);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    split->setSizes({560, 900});
    lay->addWidget(split, 1);

    connect(tree_->verticalScrollBar(), &QScrollBar::valueChanged, chart_, qOverload<>(&QWidget::update));
    connect(tree_, &QTreeWidget::itemExpanded, chart_, qOverload<>(&QWidget::update));
    connect(tree_, &QTreeWidget::itemCollapsed, chart_, qOverload<>(&QWidget::update));
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (building_) return;
        const Uuid id = selectedActivity();
        if (id) ctx_.select(EntityKind::Activity, id);
        chart_->update();
    });
    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
        rebuild();
        chart_->resetRange();
    });
    connect(&ctx_, &AppContext::scheduleRecalculated, this, &SchedulerPanel::scheduleRebuild);
    connect(&ctx_, &AppContext::selectionChanged, this, &SchedulerPanel::syncSelection);
    rebuild();
}

QTreeWidgetItem* SchedulerPanel::itemFor(const Uuid& id) const {
    const QString key = qstr(id.toString());
    QTreeWidgetItemIterator it(tree_);
    while (*it) {
        if ((*it)->data(0, RoleId).toString() == key) return *it;
        ++it;
    }
    return nullptr;
}

void SchedulerPanel::scheduleRebuild() {
    if (pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        rebuild();
    });
}

void SchedulerPanel::rebuild() {
    building_ = true;
    const int scroll = tree_->verticalScrollBar()->value();
    tree_->clear();
    if (!ctx_.hasProject()) {
        building_ = false;
        chart_->update();
        return;
    }
    const auto& m = ctx_.project().model();
    const auto& sched = ctx_.schedule();
    const auto& c = Theme::colors();

    std::function<void(QTreeWidgetItem*, const Uuid&)> add = [&](QTreeWidgetItem* parent, const Uuid& parentId) {
        std::vector<const ScheduleActivity*> kids;
        for (const auto& a : m.activities.all())
            if (a.parentId == parentId) kids.push_back(&a);
        std::stable_sort(kids.begin(), kids.end(), [](auto* x, auto* y) { return x->sortOrder < y->sortOrder; });
        for (const auto* a : kids) {
            auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
            it->setData(0, RoleId, qstr(a->id.toString()));
            it->setText(ColName, qstr(a->name));
            it->setText(ColWbs, qstr(a->wbsCode));
            if (a->kind == ActivityKind::Summary) {
                QFont f = it->font(0);
                f.setBold(true);
                it->setFont(ColName, f);
            }
            if (a->kind == ActivityKind::Task && a->quantity > 0) {
                it->setText(ColQty, formatNumber(a->quantity, 1) + " " + qstr(a->quantityUnit));
                it->setText(ColActual, formatNumber(a->actualQuantity, 1));
            }
            if (a->durationFromRate) it->setText(ColRate, formatNumber(a->ratePerDay, 2));
            if (auto t = sched.timings.find(a->id); sched.ok && t != sched.timings.end()) {
                it->setText(ColDur, formatNumber(t->second.duration, 1));
                it->setText(ColStart, qstr(t->second.startDate.toString()));
                it->setText(ColFinish, qstr(t->second.finishDate.toString()));
                it->setText(ColTf, formatNumber(t->second.totalFloat, 1));
                if (t->second.critical && a->kind != ActivityKind::Summary) it->setForeground(ColTf, c.ganttCritical);
            }
            const auto devs = m.developmentsForActivity(a->id);
            if (!devs.empty()) {
                QStringList names;
                for (const auto& d : devs)
                    if (const auto* dev = m.developments.find(d)) names << qstr(dev->headingId);
                it->setText(ColDesign, (a->designImpactFlag ? "⚠ " : "🔗 ") + names.join(", "));
                it->setToolTip(ColDesign, a->designImpactFlag ? qstr(a->designImpactNote) : "Linked to " + names.join(", "));
                if (a->designImpactFlag) it->setForeground(ColDesign, c.warning);
            }
            add(it, a->id);
        }
    };
    add(nullptr, Uuid{});
    tree_->expandAll();
    tree_->verticalScrollBar()->setValue(scroll);
    building_ = false;
    syncSelection();
    chart_->update();
}

void SchedulerPanel::syncSelection() {
    building_ = true;
    tree_->clearSelection();
    if (ctx_.hasProject() && ctx_.selection().kind == EntityKind::Activity)
        if (auto* it = itemFor(ctx_.selection().id)) {
            it->setSelected(true);
            tree_->scrollToItem(it);
        }
    building_ = false;
    chart_->update();
}

Uuid SchedulerPanel::selectedActivity() const {
    auto items = tree_->selectedItems();
    if (items.isEmpty()) return {};
    auto id = Uuid::parse(items.front()->data(0, RoleId).toString().toStdString());
    return id ? *id : Uuid{};
}

Uuid SchedulerPanel::newActivityParent() const {
    const Uuid sel = selectedActivity();
    if (!sel) return {};
    const auto* a = ctx_.project().model().activities.find(sel);
    if (!a) return {};
    return a->kind == ActivityKind::Summary ? a->id : a->parentId;
}

void SchedulerPanel::addTask() {
    if (!ctx_.hasProject()) return;
    try {
        ScheduleActivity a;
        a.name = "New task";
        a.durationDays = 5;
        a.parentId = newActivityParent();
        ctx_.select(EntityKind::Activity, ProjectEditor(ctx_.project()).addActivity(a));
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
    }
}

void SchedulerPanel::addMilestone() {
    if (!ctx_.hasProject()) return;
    try {
        ScheduleActivity a;
        a.name = "New milestone";
        a.kind = ActivityKind::Milestone;
        a.parentId = newActivityParent();
        ctx_.select(EntityKind::Activity, ProjectEditor(ctx_.project()).addActivity(a));
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
    }
}

void SchedulerPanel::addSummary() {
    if (!ctx_.hasProject()) return;
    try {
        ScheduleActivity a;
        a.name = "New WBS group";
        a.kind = ActivityKind::Summary;
        a.parentId = newActivityParent();
        ctx_.select(EntityKind::Activity, ProjectEditor(ctx_.project()).addActivity(a));
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
    }
}

void SchedulerPanel::addDependencyDialog() {
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    QDialog dlg(this);
    dlg.setWindowTitle("Add dependency");
    auto* form = new QFormLayout(&dlg);
    auto* pred = new QComboBox(&dlg);
    auto* succ = new QComboBox(&dlg);
    for (const auto& a : m.activities.all()) {
        if (a.kind == ActivityKind::Summary) continue;
        pred->addItem(qstr(a.name), qstr(a.id.toString()));
        succ->addItem(qstr(a.name), qstr(a.id.toString()));
    }
    if (pred->count() < 2) {
        ctx_.report("At least two non-summary activities are needed for a dependency", true);
        return;
    }
    const int selIdx = pred->findData(qstr(selectedActivity().toString()));
    pred->setCurrentIndex(selIdx >= 0 ? selIdx : 0);
    succ->setCurrentIndex(pred->currentIndex() + 1 < succ->count() ? pred->currentIndex() + 1 : 0);
    auto* type = new QComboBox(&dlg);
    type->addItems({"FS", "SS", "FF", "SF"});
    auto* lag = new QDoubleSpinBox(&dlg);
    lag->setRange(-1000, 1000);
    lag->setSuffix(" working days");
    form->addRow("Predecessor", pred);
    form->addRow("Successor", succ);
    form->addRow("Type", type);
    form->addRow("Lag", lag);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    try {
        ActivityDependency d;
        d.predecessorId = *Uuid::parse(pred->currentData().toString().toStdString());
        d.successorId = *Uuid::parse(succ->currentData().toString().toStdString());
        d.type = *dependencyTypeFromString(sstr(type->currentText()));
        d.lagDays = lag->value();
        ProjectEditor(ctx_.project()).addDependency(d);
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
    }
}

void SchedulerPanel::deleteSelected() {
    const Uuid id = selectedActivity();
    if (!id) return;
    try {
        ProjectEditor(ctx_.project()).removeActivity(id);
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
    }
}

void SchedulerPanel::calculate() {
    if (!ctx_.hasProject()) return;
    ctx_.recalculateSchedule();
    const auto& r = ctx_.schedule();
    if (!r.ok) {
        ctx_.report("Schedule NOT calculated: " + qstr(r.errors.empty() ? "unknown error" : r.errors.front()), true);
        return;
    }
    int critical = 0;
    for (const auto& [id, t] : r.timings)
        if (t.critical) ++critical;
    ctx_.report(QStringLiteral("CPM calculated: %1 activities, project %2 → %3 (%4 working days), %5 critical.")
                    .arg(r.timings.size())
                    .arg(qstr(r.projectStartDate.toString()), qstr(r.projectFinishDate.toString()))
                    .arg(formatNumber(r.projectFinish, 1))
                    .arg(critical));
    for (const auto& w : r.warnings) ctx_.report("Warning: " + qstr(w), false);
}

void SchedulerPanel::captureBaseline() {
    if (!ctx_.hasProject()) return;
    ctx_.recalculateSchedule();
    if (!ctx_.schedule().ok) {
        ctx_.report("Cannot baseline: the schedule does not calculate", true);
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Capture baseline", "Baseline name:", QLineEdit::Normal,
                                               "BL-" + qstr(Date::todayUtc().toString()), &ok);
    if (!ok || name.isEmpty()) return;
    try {
        ProjectEditor(ctx_.project()).addBaseline(scheduling::makeBaseline(ctx_.project(), ctx_.schedule(), sstr(name)));
        ctx_.report("Baseline '" + name + "' captured (approved; it will not change when the schedule changes).");
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
    }
}

void SchedulerPanel::applyDesignQuantities() {
    const Uuid id = ctx_.selection().kind == EntityKind::Activity ? ctx_.selection().id : selectedActivity();
    if (!id) {
        ctx_.report("Select a linked activity first", true);
        return;
    }
    try {
        mine_design::applyDesignQuantities(ctx_.project(), id);
        ctx_.report("Activity quantity recalculated from current design (audited; baselines unchanged).");
    } catch (const std::exception& e) {
        ctx_.report(e.what(), true);
    }
}

void SchedulerPanel::exportCsv(const QString& givenPath) {
    if (!ctx_.hasProject()) return;
    QString path = givenPath;
    if (path.isEmpty()) path = QFileDialog::getSaveFileName(this, "Export schedule", "schedule.csv", "CSV (*.csv)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        ctx_.report("Cannot write " + path, true);
        return;
    }
    QTextStream out(&f);
    auto csv = [](QString s) {
        if (s.contains(',') || s.contains('"') || s.contains('\n')) s = '"' + s.replace('"', "\"\"") + '"';
        return s;
    };
    out << "activity_id,wbs,name,type,duration_d,start,finish,total_float_d,free_float_d,critical,quantity,unit,rate_per_d,actual,linked_design,design_impact\n";
    const auto& m = ctx_.project().model();
    const auto& r = ctx_.schedule();
    for (const auto& a : m.activities.all()) {
        auto t = r.timings.find(a.id);
        const bool has = r.ok && t != r.timings.end();
        QStringList devs;
        for (const auto& d : m.developmentsForActivity(a.id))
            if (const auto* dev = m.developments.find(d)) devs << qstr(dev->headingId);
        out << qstr(a.id.toString()) << ',' << csv(qstr(a.wbsCode)) << ',' << csv(qstr(a.name)) << ',' << toString(a.kind) << ','
            << (has ? formatNumber(t->second.duration, 3) : "") << ',' << (has ? qstr(t->second.startDate.toString()) : "") << ','
            << (has ? qstr(t->second.finishDate.toString()) : "") << ',' << (has ? formatNumber(t->second.totalFloat, 3) : "") << ','
            << (has ? formatNumber(t->second.freeFloat, 3) : "") << ',' << (has && t->second.critical ? "yes" : "no") << ','
            << formatNumber(a.quantity, 3) << ',' << csv(qstr(a.quantityUnit)) << ',' << formatNumber(a.ratePerDay, 3) << ','
            << formatNumber(a.actualQuantity, 3) << ',' << csv(devs.join(';')) << ',' << csv(qstr(a.designImpactNote)) << '\n';
    }
    ctx_.report("Schedule exported to " + path);
}

void SchedulerPanel::zoom(double factor) { chart_->setPixelsPerDay(chart_->pixelsPerDay() * factor); }

// ===========================================================================
GanttChart::GanttChart(AppContext& ctx, QTreeWidget* tree, QWidget* parent) : QWidget(parent), ctx_(ctx), tree_(tree) {
    hbar_ = new QScrollBar(Qt::Horizontal, this);
    connect(hbar_, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    setMinimumWidth(200);
    setMouseTracking(true);
    resetRange();
}

void GanttChart::resetRange() {
    origin_ = ctx_.hasProject() ? ctx_.project().model().info.scheduleStart.addDays(-7) : Date::todayUtc();
    updateScrollRange();
    hbar_->setValue(0);
    update();
}

void GanttChart::setPixelsPerDay(double ppd) {
    ppd_ = std::clamp(ppd, 1.5, 80.0);
    updateScrollRange();
    update();
}

void GanttChart::updateScrollRange() {
    long span = 120;
    if (ctx_.hasProject() && ctx_.schedule().ok) span = std::max<long>(span, ctx_.schedule().projectFinishDate.days() - origin_.days() + 30);
    spanDays_ = span;
    hbar_->setRange(0, std::max(0, static_cast<int>(span * ppd_) - width()));
    hbar_->setPageStep(width());
}

void GanttChart::resizeEvent(QResizeEvent*) {
    hbar_->setGeometry(0, height() - hbar_->sizeHint().height(), width(), hbar_->sizeHint().height());
    updateScrollRange();
}

int GanttChart::headerHeight() const { return tree_->header()->height(); }
double GanttChart::xOf(Date d) const { return (d.days() - origin_.days()) * ppd_ - hbar_->value(); }

void GanttChart::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::ControlModifier) {
        setPixelsPerDay(ppd_ * (e->angleDelta().y() > 0 ? 1.2 : 1 / 1.2));
        e->accept();
        return;
    }
    QApplication::sendEvent(tree_->verticalScrollBar(), e);
}

void GanttChart::mousePressEvent(QMouseEvent* e) {
    const QPoint inTree(5, e->position().toPoint().y() - headerHeight());
    if (auto* it = tree_->itemAt(inTree)) {
        tree_->setCurrentItem(it);
        it->setSelected(true);
    }
}

void GanttChart::paintEvent(QPaintEvent*) {
    updateScrollRange();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& c = Theme::colors();
    const int hh = headerHeight();
    const int bottom = height() - hbar_->height();
    p.fillRect(rect(), c.panel);
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    const auto& sched = ctx_.schedule();
    const WorkCalendar* cal = m.calendars.find(m.info.defaultCalendarId);

    // Non-working days shading + grid.
    const long firstDay = static_cast<long>(hbar_->value() / ppd_);
    const long lastDay = firstDay + static_cast<long>(width() / ppd_) + 2;
    p.setClipRect(0, hh, width(), bottom - hh);
    if (cal) {
        scheduling::WorkingTime wt(*cal, origin_);
        for (long dday = firstDay; dday <= lastDay; ++dday) {
            const Date d = origin_.addDays(dday);
            if (!wt.isWorkingDay(d)) p.fillRect(QRectF(xOf(d), hh, ppd_, bottom - hh), c.ganttWeekend);
        }
    }
    p.setPen(c.ganttGrid);
    for (long dday = firstDay; dday <= lastDay; ++dday) {
        const Date d = origin_.addDays(dday);
        if (d.isoWeekday() == 1) p.drawLine(QPointF(xOf(d), hh), QPointF(xOf(d), bottom));
    }
    if (m.info.statusDate) {
        p.setPen(QPen(c.accent, 2, Qt::DashLine));
        p.drawLine(QPointF(xOf(*m.info.statusDate), hh), QPointF(xOf(*m.info.statusDate), bottom));
    }
    p.setClipping(false);

    // Timescale header: months and week starts.
    p.fillRect(QRect(0, 0, width(), hh), c.panelAlt);
    p.setPen(c.border);
    p.drawLine(0, hh - 1, width(), hh - 1);
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.85);
    p.setFont(small);
    for (long dday = firstDay - 31; dday <= lastDay; ++dday) {
        const Date d = origin_.addDays(dday);
        int y;
        unsigned mo, da;
        d.toYmd(y, mo, da);
        if (da == 1) {
            p.setPen(c.text);
            static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            p.drawText(QPointF(std::max(2.0, xOf(d) + 3), hh * 0.42), QStringLiteral("%1 %2").arg(months[mo - 1]).arg(y));
            p.setPen(c.border);
            p.drawLine(QPointF(xOf(d), 0), QPointF(xOf(d), hh));
        }
        if (d.isoWeekday() == 1 && ppd_ * 7 > 28) {
            p.setPen(c.textMuted);
            p.drawText(QPointF(xOf(d) + 2, hh - 5), QString::number(da));
        }
    }

    if (!sched.ok) {
        p.setPen(c.ganttCritical);
        p.drawText(QRect(0, hh, width(), 40), Qt::AlignCenter,
                   "Schedule not calculated: " + qstr(sched.errors.empty() ? "" : sched.errors.front()));
        return;
    }

    // Rows (only items visible in the grid, at the grid's y positions).
    std::map<Uuid, QRectF> barRects;
    const auto* bl = latestBaseline(m);
    p.setClipRect(0, hh, width(), bottom - hh);
    QTreeWidgetItemIterator it(tree_);
    for (; *it; ++it) {
        QTreeWidgetItem* item = *it;
        const QRect r = tree_->visualItemRect(item);
        if (r.height() == 0) continue; // collapsed
        const double y = hh + r.y();
        if (y > bottom || y + r.height() < hh) continue;
        auto id = Uuid::parse(item->data(0, RoleId).toString().toStdString());
        if (!id) continue;
        const auto* a = m.activities.find(*id);
        auto t = sched.timings.find(*id);
        if (!a || t == sched.timings.end()) continue;
        if (item->isSelected()) p.fillRect(QRectF(0, y, width(), r.height()), QColor(c.accent.red(), c.accent.green(), c.accent.blue(), 40));

        const double x0 = xOf(t->second.startDate);
        const double x1 = xOf(t->second.finishDate.addDays(1));
        const double h = r.height();
        if (bl) {
            for (const auto& b : bl->activities)
                if (b.activityId == *id && a->kind != ActivityKind::Summary) {
                    p.fillRect(QRectF(xOf(b.start), y + h * 0.72, xOf(b.finish.addDays(1)) - xOf(b.start), h * 0.14), c.ganttBaseline);
                }
        }
        if (a->kind == ActivityKind::Milestone || t->second.duration <= 0) {
            const double cx = x0, cy = y + h * 0.42, s = h * 0.3;
            QPainterPath dia;
            dia.moveTo(cx, cy - s);
            dia.lineTo(cx + s, cy);
            dia.lineTo(cx, cy + s);
            dia.lineTo(cx - s, cy);
            dia.closeSubpath();
            p.fillPath(dia, t->second.critical ? c.ganttCritical : c.ganttSummary);
            barRects[*id] = QRectF(cx - s, cy - s, 2 * s, 2 * s);
        } else if (a->kind == ActivityKind::Summary) {
            const QRectF br(x0, y + h * 0.3, x1 - x0, h * 0.2);
            p.fillRect(br, c.ganttSummary);
            QPainterPath l, rr;
            p.fillRect(QRectF(x0, br.top(), 3, h * 0.38), c.ganttSummary);
            p.fillRect(QRectF(x1 - 3, br.top(), 3, h * 0.38), c.ganttSummary);
            barRects[*id] = br;
        } else {
            const QRectF br(x0, y + h * 0.2, std::max(2.0, x1 - x0), h * 0.46);
            p.setPen(Qt::NoPen);
            p.setBrush(t->second.critical ? c.ganttCritical : c.ganttTask);
            p.drawRoundedRect(br, 2, 2);
            if (t->second.percentComplete > 0) {
                p.setBrush(c.ganttProgress);
                p.drawRect(QRectF(br.left(), br.top() + br.height() * 0.6, br.width() * t->second.percentComplete / 100.0, br.height() * 0.4));
            }
            barRects[*id] = br;
            if (ppd_ >= 6) {
                p.setPen(c.textMuted);
                p.drawText(QPointF(br.right() + 6, br.bottom() - 2), qstr(a->name));
            }
        }
        if (a->designImpactFlag) {
            const QRectF br = barRects[*id];
            QPainterPath tri;
            const double tx = br.right() + 2, ty = y + 2;
            tri.moveTo(tx + 6, ty);
            tri.lineTo(tx + 12, ty + 11);
            tri.lineTo(tx, ty + 11);
            tri.closeSubpath();
            p.fillPath(tri, c.warning);
        }
    }

    // Dependency arrows.
    p.setRenderHint(QPainter::Antialiasing);
    for (const auto& d : m.dependencies.all()) {
        auto pa = barRects.find(d.predecessorId), sa = barRects.find(d.successorId);
        if (pa == barRects.end() || sa == barRects.end()) continue;
        const QRectF P = pa->second, S = sa->second;
        const bool fromFinish = d.type == DependencyType::FinishToStart || d.type == DependencyType::FinishToFinish;
        const bool toStart = d.type == DependencyType::FinishToStart || d.type == DependencyType::StartToStart;
        const auto pt = sched.timings.find(d.predecessorId), st = sched.timings.find(d.successorId);
        const bool crit = pt->second.critical && st->second.critical;
        p.setPen(QPen(crit ? c.ganttCritical : c.textMuted, 1.3));
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
        QPolygonF head{b0, {b0.x() - dir * 5, b0.y() - 3.5}, {b0.x() - dir * 5, b0.y() + 3.5}};
        p.setBrush(crit ? c.ganttCritical : c.textMuted);
        p.drawPolygon(head);
    }
}

} // namespace openmine::app
