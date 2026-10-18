// SPDX-License-Identifier: Apache-2.0
#include "SchedulerPanel.h"

#include "GanttChart.h"
#include "ScheduleCharts.h"
#include "ScheduleDialogs.h"
#include "../app/EntityInfo.h"
#include "../theme/Theme.h"

#include "openmine/mine_design/Development.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/reporting/ScheduleExport.h"
#include "openmine/scheduling/ScheduleTable.h"

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <fstream>

namespace openmine::app {

namespace {
enum Col { CName, CWbs, CKind, CDuration, CStart, CFinish, CFloat, CPercent, CQuantity, CRate, CResources, CDesign, CCalendar, CBasis, CImpact, NCols };
constexpr int kIdRole = GanttChart::kIdRole;

double standardDayHours(const ProjectModel& m, const ScheduleActivity& a) {
    if (const auto* c = m.calendarFor(a)) return c->standardHoursPerDay > 0 ? c->standardHoursPerDay : 24.0;
    return 24.0;
}
double ratePerDay(const ScheduleActivity& a, double dayHours) {
    switch (a.rateUnit) {
    case RateUnit::PerDay: return a.rate;
    case RateUnit::PerHour: return a.rate * dayHours;
    case RateUnit::PerShift: return a.rate; // shown as entered (per shift)
    }
    return a.rate;
}
} // namespace

SchedulerPanel::SchedulerPanel(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* tb = new QToolBar(this);
    tb->setIconSize(QSize(18, 18));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->addWidget(new QLabel(tr(" Scenario "), tb));
    scenario_ = new QComboBox(tb);
    scenario_->setMinimumWidth(180);
    tb->addWidget(scenario_);
    tb->addAction(toolIcon("scenario"), tr("New scenario…"), this, [this] { editScenario(true); });
    tb->addAction(toolIcon("properties"), tr("Edit scenario…"), this, [this] { editScenario(false); });
    auto* opt = tb->addAction(toolIcon("optimise"), tr("Optimise sequence (OR-Tools CP-SAT)…"), this, &SchedulerPanel::optimise);
    opt->setEnabled(OptimiserDialog::available());
    if (!OptimiserDialog::available()) opt->setToolTip(tr("Not available: this build has no OR-Tools"));
    tb->addSeparator();
    tb->addAction(toolIcon("task"), tr("Add task"), this, [this] { addActivity(ActivityKind::Task); });
    tb->addAction(toolIcon("milestone"), tr("Add milestone"), this, [this] { addActivity(ActivityKind::Milestone); });
    tb->addAction(toolIcon("summary"), tr("Add summary"), this, [this] { addActivity(ActivityKind::Summary); });
    tb->addAction(toolIcon("dependency"), tr("Add dependency (first selected → last selected)"), this, &SchedulerPanel::addDependency);
    tb->addAction(toolIcon("delete"), tr("Delete selected"), this, &SchedulerPanel::deleteSelected);
    tb->addAction(toolIcon("redo"), tr("Indent (make child of the summary above)"), this, &SchedulerPanel::indent);
    tb->addAction(toolIcon("undo"), tr("Outdent"), this, &SchedulerPanel::outdent);
    tb->addAction(tr("▲"), this, [this] { moveSelected(-1); })->setToolTip(tr("Move up"));
    tb->addAction(tr("▼"), this, [this] { moveSelected(+1); })->setToolTip(tr("Move down"));
    tb->addSeparator();
    tb->addAction(toolIcon("actual"), tr("Record actual…"), this, &SchedulerPanel::recordActual);
    tb->addAction(toolIcon("baseline"), tr("Capture baseline…"), this, [this] { captureBaseline(); });
    tb->addAction(toolIcon("impacts"), tr("Apply design quantities to flagged activities"), this, &SchedulerPanel::applyDesignQuantities);
    tb->addSeparator();
    tb->addAction(toolIcon("zoomin"), tr("Zoom in"), this, [this] { gantt_->zoom(1.5); });
    tb->addAction(toolIcon("zoomout"), tr("Zoom out"), this, [this] { gantt_->zoom(1 / 1.5); });
    tb->addAction(toolIcon("fit"), tr("Fit schedule"), this, [this] { gantt_->fitSchedule(); });
    status_ = new QLabel(tb);
    status_->setContentsMargins(8, 0, 4, 0);
    tb->addWidget(status_);
    lay->addWidget(tb);

    // Filtering and grouping.
    auto* fb = new QToolBar(this);
    fb->setIconSize(QSize(16, 16));
    fb->addWidget(new QLabel(tr(" Filter "), fb));
    filter_ = new QLineEdit(fb);
    filter_->setObjectName("schedulerFilter");
    filter_->setPlaceholderText(tr("name, WBS, heading, resource, zone…"));
    filter_->setClearButtonEnabled(true);
    filter_->setMaximumWidth(260);
    fb->addWidget(filter_);
    fb->addWidget(new QLabel(tr("  Show "), fb));
    show_ = new QComboBox(fb);
    show_->addItems({tr("All activities"), tr("Critical"), tr("Flagged by design changes"), tr("In progress"), tr("Not started"), tr("Complete"),
                     tr("Behind the baseline")});
    fb->addWidget(show_);
    fb->addWidget(new QLabel(tr("  Group by "), fb));
    group_ = new QComboBox(fb);
    group_->addItems({tr("WBS (hierarchy)"), tr("Level"), tr("Zone"), tr("Work area"), tr("Resource"), tr("Calendar"), tr("Kind")});
    fb->addWidget(group_);
    lay->addWidget(fb);
    connect(filter_, &QLineEdit::textChanged, this, &SchedulerPanel::setFilter);
    connect(show_, &QComboBox::currentIndexChanged, this, [this](int i) { setShow(static_cast<Show>(i)); });
    connect(group_, &QComboBox::currentIndexChanged, this, [this](int i) { setGroupBy(static_cast<GroupBy>(i)); });

    auto* vsplit = new QSplitter(Qt::Vertical, this);
    auto* hsplit = new QSplitter(Qt::Horizontal, vsplit);
    grid_ = new QTreeWidget(hsplit);
    grid_->setColumnCount(NCols);
    grid_->setHeaderLabels({tr("Activity"), tr("WBS"), tr("Kind"), tr("Duration (d)"), tr("Start"), tr("Finish"), tr("Total float (d)"), tr("%"), tr("Quantity"),
                            tr("Rate (/d)"), tr("Resources"), tr("Design"), tr("Calendar"), tr("Duration basis"), tr("Design impact")});
    grid_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    grid_->setUniformRowHeights(true);
    grid_->setAlternatingRowColors(true);
    grid_->header()->setSectionResizeMode(QHeaderView::Interactive);
    grid_->setColumnWidth(CName, 230);
    grid_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    gantt_ = new GanttChart(ctx_, grid_, hsplit);
    hsplit->addWidget(grid_);
    hsplit->addWidget(gantt_);
    hsplit->setStretchFactor(0, 2);
    hsplit->setStretchFactor(1, 3);
    tabs_ = new QTabWidget(vsplit);
    histogram_ = new ResourceHistogram(ctx_, tabs_);
    production_ = new ProductionChart(ctx_, tabs_);
    variance_ = new VarianceTable(ctx_, tabs_);
    log_ = new QPlainTextEdit(tabs_);
    log_->setReadOnly(true);
    tabs_->addTab(histogram_, toolIcon("histogram"), tr("Resources"));
    tabs_->addTab(production_, toolIcon("chart"), tr("Production"));
    tabs_->addTab(variance_, toolIcon("baseline"), tr("Baseline variance"));
    tabs_->addTab(log_, toolIcon("info"), tr("Calculation"));
    vsplit->addWidget(hsplit);
    vsplit->addWidget(tabs_);
    vsplit->setStretchFactor(0, 3);
    vsplit->setStretchFactor(1, 1);
    // The activity grid and Gantt keep most of the height; the charts can be enlarged by the user.
    for (auto* w : std::initializer_list<QWidget*>{histogram_, production_, variance_, log_}) w->setMinimumHeight(40);
    tabs_->setMinimumHeight(70);
    vsplit->setSizes({440, 120});
    lay->addWidget(vsplit, 1);

    connect(scenario_, &QComboBox::activated, this, [this](int) { ctx_.setActiveScenario(uuidFrom(scenario_->currentData().toString())); });
    connect(grid_, &QTreeWidget::itemChanged, this, &SchedulerPanel::onItemChanged);
    connect(grid_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (syncing_ || building_) return;
        syncing_ = true;
        std::vector<SelectionItem> sel;
        for (auto* it : grid_->selectedItems()) sel.push_back({EntityKind::Activity, uuidFrom(it->data(0, kIdRole).toString())});
        if (auto* cur = grid_->currentItem(); cur && cur->isSelected()) {
            SelectionItem c{EntityKind::Activity, uuidFrom(cur->data(0, kIdRole).toString())};
            std::erase(sel, c);
            sel.push_back(c);
        }
        ctx_.selectMany(sel);
        syncing_ = false;
    });
    connect(gantt_, &GanttChart::activityClicked, this, [this](Uuid id, Qt::KeyboardModifiers mods) {
        ctx_.select(EntityKind::Activity, id, mods & Qt::ControlModifier ? SelectMode::Toggle : mods & Qt::ShiftModifier ? SelectMode::Add : SelectMode::Replace);
    });
    connect(grid_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu m(this);
        m.addAction(toolIcon("task"), tr("Add task"), this, [this] { addActivity(ActivityKind::Task); });
        m.addAction(toolIcon("milestone"), tr("Add milestone"), this, [this] { addActivity(ActivityKind::Milestone); });
        m.addAction(toolIcon("summary"), tr("Add summary"), this, [this] { addActivity(ActivityKind::Summary); });
        m.addAction(toolIcon("dependency"), tr("Add dependency…"), this, &SchedulerPanel::addDependency);
        m.addAction(toolIcon("actual"), tr("Record actual…"), this, &SchedulerPanel::recordActual);
        m.addSeparator();
        m.addAction(toolIcon("delete"), tr("Delete"), this, &SchedulerPanel::deleteSelected);
        m.exec(grid_->viewport()->mapToGlobal(pos));
    });
    connect(&ctx_, &AppContext::scheduleRecalculated, this, &SchedulerPanel::scheduleRebuild);
    connect(&ctx_, &AppContext::modelChanged, this, &SchedulerPanel::scheduleRebuild);
    connect(&ctx_, &AppContext::projectReplaced, this, &SchedulerPanel::scheduleRebuild);
    connect(&ctx_, &AppContext::scenarioChanged, this, &SchedulerPanel::rebuildScenarios);
    connect(&ctx_, &AppContext::selectionChanged, this, &SchedulerPanel::syncSelectionFromContext);
    rebuild();
}

void SchedulerPanel::scheduleRebuild() {
    if (pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        rebuild();
    });
}

QTreeWidgetItem* SchedulerPanel::itemFor(const Uuid& id) const {
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : it->second;
}

void SchedulerPanel::rebuildScenarios() {
    const QSignalBlocker b(scenario_);
    scenario_->clear();
    scenario_->addItem(tr("Base schedule"), QString());
    if (!ctx_.hasProject()) return;
    for (const auto& s : ctx_.project().model().scenarios.all()) {
        QString label = qstr(s.name);
        if (s.levelling == LevellingMode::Heuristic) label += tr(" (levelled)");
        if (s.levelling == LevellingMode::Optimised) label += tr(" (optimised)");
        scenario_->addItem(label, uuidText(s.id));
    }
    const auto* a = ctx_.activeScenario();
    scenario_->setCurrentIndex(a ? std::max(0, scenario_->findData(uuidText(a->id))) : 0);
}

void SchedulerPanel::rebuild() {
    building_ = true;
    rebuildScenarios();
    std::set<QString> collapsed;
    for (auto& [id, it] : items_)
        if (it->childCount() && !it->isExpanded()) collapsed.insert(uuidText(id));
    for (int i = 0; i < grid_->topLevelItemCount(); ++i)
        if (auto* g = grid_->topLevelItem(i); g->data(0, Qt::UserRole + 11).isValid() && !g->isExpanded())
            collapsed.insert(g->data(0, Qt::UserRole + 11).toString());
    const int scroll = grid_->verticalScrollBar()->value();
    grid_->clear();
    items_.clear();
    if (!ctx_.hasProject()) {
        building_ = false;
        updateStatus();
        return;
    }
    const auto& m = ctx_.project().model();
    const auto& sch = ctx_.schedule();
    std::vector<const ScheduleActivity*> ordered;
    for (const auto& a : m.activities.all()) ordered.push_back(&a);
    std::stable_sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->sortOrder < b->sortOrder; });
    std::map<Uuid, std::vector<std::string>> resources;
    for (const auto& as : m.assignments.all())
        if (const auto* r = m.resources.find(as.resourceId))
            resources[as.activityId].push_back(r->name + (as.units != 1 ? " ×" + QString::number(as.units).toStdString() : std::string()));
    // Grouped views list tasks and milestones under group headers (summaries are WBS nodes).
    std::map<QString, QTreeWidgetItem*> groups;
    auto groupItem = [&](const QString& key) {
        auto& g = groups[key];
        if (!g) {
            g = new QTreeWidgetItem(grid_);
            g->setText(CName, key.isEmpty() ? tr("(none)") : key);
            g->setIcon(CName, toolIcon("summary"));
            QFont f = g->font(CName);
            f.setBold(true);
            g->setFont(CName, f);
            g->setFlags(Qt::ItemIsEnabled);
            g->setFirstColumnSpanned(false);
            g->setExpanded(!collapsed.count("group:" + key));
            g->setData(0, Qt::UserRole + 11, "group:" + key);
        }
        return g;
    };
    const bool grouped = groupBy_ != GroupBy::Wbs;
    auto pendingA = ordered;
    if (grouped) std::erase_if(pendingA, [](const ScheduleActivity* a) { return a->kind == ActivityKind::Summary; });
    for (int pass = 0; pass < 64 && !pendingA.empty(); ++pass) {
        std::vector<const ScheduleActivity*> next;
        for (const auto* a : pendingA) {
            QTreeWidgetItem* parent = nullptr;
            if (grouped) {
                parent = groupItem(groupKey(*a));
            } else if (!a->parentId.isNil() && m.activities.contains(a->parentId)) {
                auto it = items_.find(a->parentId);
                if (it == items_.end()) {
                    next.push_back(a);
                    continue;
                }
                parent = it->second;
            }
            auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(grid_);
            items_[a->id] = it;
            it->setData(0, kIdRole, uuidText(a->id));
            const double dayH = standardDayHours(m, *a);
            it->setText(CName, qstr(a->name));
            it->setIcon(CName, toolIcon(a->kind == ActivityKind::Milestone ? "milestone" : a->kind == ActivityKind::Summary ? "summary" : "task"));
            it->setText(CWbs, qstr(a->wbsCode));
            it->setText(CKind, qstr(toString(a->kind)));
            auto t = sch.result.timings.find(a->id);
            if (t != sch.result.timings.end()) {
                const auto& tt = t->second;
                it->setText(CDuration, QString::number(tt.totalHours / dayH, 'f', 2));
                it->setText(CStart, formatDateTimeMinutes(tt.es));
                it->setText(CFinish, formatDateTimeMinutes(tt.ef));
                it->setText(CFloat, QString::number(tt.totalFloatHours / dayH, 'f', 2));
                it->setText(CPercent, QString::number(tt.percentComplete, 'f', 1));
                it->setText(CBasis, qstr(tt.durationBasis));
                if (tt.critical) it->setForeground(CName, Theme::colors().ganttCritical);
            } else {
                it->setText(CDuration, QString::number(a->durationHours / dayH, 'f', 2));
            }
            it->setText(CQuantity, a->quantity != 0 ? QStringLiteral("%1 %2").arg(a->quantity, 0, 'f', 2).arg(qstr(a->quantityUnit)) : QString());
            it->setText(CRate, a->rate != 0 ? QString::number(ratePerDay(*a, dayH), 'f', 3) + (a->rateUnit == RateUnit::PerShift ? tr(" /shift") : QString()) : QString());
            QStringList rl;
            for (const auto& r : resources[a->id]) rl << qstr(r);
            it->setText(CResources, rl.join(", "));
            QStringList dl;
            for (const auto& d : m.designsForActivity(a->id)) {
                if (const auto* dev = m.developments.find(d)) dl << qstr(dev->headingId);
                else if (const auto* st = m.stopes.find(d)) dl << qstr(st->name);
                else if (const auto* b = m.miningBlocks.find(d)) dl << qstr(b->name);
            }
            it->setText(CDesign, dl.join(", "));
            if (const auto* c = m.calendarFor(*a)) it->setText(CCalendar, qstr(c->name));
            if (a->designImpactFlag) {
                it->setText(CImpact, qstr(a->designImpactNote.empty() ? std::string("Design changed") : a->designImpactNote));
                it->setForeground(CImpact, Theme::colors().warning);
                it->setIcon(CImpact, toolIcon("impacts"));
            }
            Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
            it->setFlags(f);
            it->setExpanded(!collapsed.count(uuidText(a->id)));
        }
        pendingA = std::move(next);
    }
    // Group summaries: count, quantity and date range of the group.
    for (auto& [key, g] : groups) {
        double qty = 0, es = 1e300, ef = -1e300;
        for (int i = 0; i < g->childCount(); ++i) {
            const Uuid id = uuidFrom(g->child(i)->data(0, kIdRole).toString());
            if (const auto* a = m.activities.find(id)) qty += a->quantity;
            if (auto t = sch.result.timings.find(id); t != sch.result.timings.end()) {
                es = std::min(es, t->second.es);
                ef = std::max(ef, t->second.ef);
            }
        }
        g->setText(CKind, tr("%1 activities").arg(g->childCount()));
        if (qty > 0) g->setText(CQuantity, QString::number(qty, 'f', 1));
        if (ef > es) {
            g->setText(CStart, formatDateTimeMinutes(es));
            g->setText(CFinish, formatDateTimeMinutes(ef));
        }
    }
    if (grouped) grid_->sortItems(CName, Qt::AscendingOrder);
    applyFilter();
    building_ = false;
    grid_->verticalScrollBar()->setValue(scroll);
    syncSelectionFromContext();
    updateStatus();
    gantt_->update();
}

QString SchedulerPanel::groupKey(const ScheduleActivity& a) const {
    const auto& m = ctx_.project().model();
    auto firstDev = [&]() -> const MineDevelopment* {
        for (const auto& d : m.developmentsForActivity(a.id))
            if (const auto* dev = m.developments.find(d)) return dev;
        return nullptr;
    };
    switch (groupBy_) {
    case GroupBy::Wbs: return {};
    case GroupBy::Level: {
        if (const auto* d = firstDev()) {
            if (const auto* l = m.levels.find(d->levelId)) return qstr(l->name);
            return qstr(d->level);
        }
        for (const auto& id : m.designsForActivity(a.id))
            if (const auto* st = m.stopes.find(id))
                if (const auto* l = m.levels.find(st->levelId)) return qstr(l->name);
        return {};
    }
    case GroupBy::Zone: {
        if (const auto* d = firstDev()) return qstr(d->zone);
        return qstr(a.workArea);
    }
    case GroupBy::WorkArea: return qstr(a.workArea);
    case GroupBy::Resource: {
        QStringList names;
        for (const auto* as : m.assignmentsForActivity(a.id))
            if (const auto* r = m.resources.find(as->resourceId)) names << qstr(r->name);
        names.sort();
        return names.join(", ");
    }
    case GroupBy::Calendar: {
        const auto* c = m.calendarFor(a);
        return c ? qstr(c->name) : tr("Continuous (24/7)");
    }
    case GroupBy::Kind: return qstr(toString(a.kind));
    }
    return {};
}

bool SchedulerPanel::matches(const ScheduleActivity& a, QTreeWidgetItem* item) const {
    const auto& sch = ctx_.schedule().result;
    const auto t = sch.timings.find(a.id);
    switch (showMode_) {
    case Show::All: break;
    case Show::Critical:
        if (t == sch.timings.end() || !t->second.critical) return false;
        break;
    case Show::Flagged:
        if (!a.designImpactFlag) return false;
        break;
    case Show::InProgress:
        if (t == sch.timings.end() || t->second.percentComplete <= 0 || t->second.percentComplete >= 100) return false;
        break;
    case Show::NotStarted:
        if (t != sch.timings.end() && t->second.percentComplete > 0) return false;
        break;
    case Show::Complete:
        if (t == sch.timings.end() || t->second.percentComplete < 100) return false;
        break;
    case Show::BehindBaseline: {
        const auto* bl = ctx_.comparisonBaseline();
        if (!bl || t == sch.timings.end()) return false;
        bool behind = false;
        for (const auto& b : bl->activities)
            if (b.activityId == a.id) behind = t->second.ef > static_cast<double>(b.finish.minutes()) + 1e-6;
        if (!behind) return false;
        break;
    }
    }
    if (filterText_.isEmpty()) return true;
    for (int c = 0; c < NCols; ++c)
        if (item->text(c).contains(filterText_, Qt::CaseInsensitive)) return true;
    for (const auto& d : ctx_.project().model().developmentsForActivity(a.id))
        if (const auto* dev = ctx_.project().model().developments.find(d))
            if (qstr(dev->zone).contains(filterText_, Qt::CaseInsensitive) || qstr(dev->level).contains(filterText_, Qt::CaseInsensitive)) return true;
    return qstr(a.workArea).contains(filterText_, Qt::CaseInsensitive);
}

void SchedulerPanel::applyFilter() {
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    // Children first: a WBS node stays visible when one of its descendants matches.
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* it) {
        bool anyChild = false;
        for (int i = 0; i < it->childCount(); ++i) anyChild = visit(it->child(i)) || anyChild;
        bool self = false;
        if (const auto* a = m.activities.find(uuidFrom(it->data(0, kIdRole).toString()))) self = matches(*a, it);
        const bool show = self || anyChild;
        it->setHidden(!show);
        return show;
    };
    for (int i = 0; i < grid_->topLevelItemCount(); ++i) visit(grid_->topLevelItem(i));
    gantt_->update();
}

void SchedulerPanel::setFilter(const QString& text) {
    filterText_ = text.trimmed();
    if (filter_ && filter_->text() != text) {
        const QSignalBlocker b(filter_);
        filter_->setText(text);
    }
    applyFilter();
    updateStatus();
}

void SchedulerPanel::setShow(Show s) {
    showMode_ = s;
    if (show_ && show_->currentIndex() != static_cast<int>(s)) {
        const QSignalBlocker b(show_);
        show_->setCurrentIndex(static_cast<int>(s));
    }
    applyFilter();
    updateStatus();
}

void SchedulerPanel::setGroupBy(GroupBy g) {
    groupBy_ = g;
    if (group_ && group_->currentIndex() != static_cast<int>(g)) {
        const QSignalBlocker b(group_);
        group_->setCurrentIndex(static_cast<int>(g));
    }
    grid_->setSortingEnabled(false);
    rebuild();
}

int SchedulerPanel::visibleActivityCount() const {
    int n = 0;
    for (const auto& [id, it] : items_) {
        bool hidden = false;
        for (auto* p = it; p; p = p->parent()) hidden = hidden || p->isHidden();
        n += hidden ? 0 : 1;
    }
    return n;
}

void SchedulerPanel::updateStatus() {
    const auto& s = ctx_.schedule();
    log_->clear();
    if (!ctx_.hasProject()) {
        status_->clear();
        return;
    }
    if (!s.result.ok) {
        status_->setText(tr("<span style='color:#d32f2f'>Not calculated</span>"));
        for (const auto& e : s.result.errors) log_->appendPlainText(tr("Error: %1").arg(qstr(e)));
    } else {
        int crit = 0;
        for (const auto& [id, t] : s.result.timings) crit += t.critical ? 1 : 0;
        status_->setText(tr("Finish <b>%1</b> · %2 days · %3 critical · %4 ms")
                             .arg(qstr(s.result.projectFinishDate.toString()))
                             .arg(s.result.makespanHours() / 24.0, 0, 'f', 1)
                             .arg(crit)
                             .arg(s.milliseconds, 0, 'f', 0));
        log_->appendPlainText(tr("Calculated %1 activities in %2 ms (%3).").arg(s.result.timings.size()).arg(s.milliseconds, 0, 'f', 1)
                                  .arg(ctx_.activeScenario() ? tr("scenario %1").arg(qstr(ctx_.activeScenario()->name)) : tr("base schedule")));
        if (s.levelling)
            log_->appendPlainText(tr("Resource levelling: %1 activities delayed, total delay %2 h, makespan +%3 h.")
                                      .arg(s.levelling->delayedActivities)
                                      .arg(s.levelling->totalDelayHours, 0, 'f', 1)
                                      .arg(s.levelling->makespanIncreaseHours, 0, 'f', 1));
        if (!s.result.levellingSummary.empty()) log_->appendPlainText(qstr(s.result.levellingSummary));
    }
    for (const auto& w : s.result.warnings) log_->appendPlainText(tr("Warning: %1").arg(qstr(w)));
    for (const auto& w : s.input.warnings) log_->appendPlainText(tr("Input: %1").arg(qstr(w)));
}

void SchedulerPanel::syncSelectionFromContext() {
    if (syncing_) return;
    syncing_ = true;
    grid_->clearSelection();
    QTreeWidgetItem* last = nullptr;
    for (const auto& s : ctx_.selection()) {
        Uuid act;
        if (s.kind == EntityKind::Activity) act = s.id;
        // A selected design object selects its linked activities.
        else if (ctx_.hasProject() && (s.kind == EntityKind::Development || s.kind == EntityKind::Stope || s.kind == EntityKind::MiningBlock)) {
            for (const auto& a : ctx_.project().model().activitiesForDesign(s.id))
                if (auto* it = itemFor(a)) {
                    it->setSelected(true);
                    last = it;
                }
            continue;
        }
        if (auto* it = itemFor(act)) {
            it->setSelected(true);
            last = it;
        }
    }
    if (last) grid_->scrollToItem(last);
    syncing_ = false;
    gantt_->update();
}

Uuid SchedulerPanel::selectedActivity() const {
    const auto p = ctx_.primary();
    return p.kind == EntityKind::Activity ? p.id : Uuid{};
}

Uuid SchedulerPanel::parentForNew() const {
    const Uuid sel = selectedActivity();
    if (sel.isNil() || !ctx_.hasProject()) return {};
    const auto* a = ctx_.project().model().activities.find(sel);
    if (!a) return {};
    return a->kind == ActivityKind::Summary ? a->id : a->parentId;
}

void SchedulerPanel::onItemChanged(QTreeWidgetItem* item, int col) {
    if (building_ || !ctx_.hasProject()) return;
    const Uuid id = uuidFrom(item->data(0, kIdRole).toString());
    const auto* a = ctx_.project().model().activities.find(id);
    if (!a) return;
    ScheduleActivity n = *a;
    const QString text = item->text(col).trimmed();
    const double dayH = standardDayHours(ctx_.project().model(), *a);
    auto number = [&](double& out) {
        bool ok = false;
        // Accept a trailing unit ("120 m").
        const double v = QLocale::c().toDouble(text.section(' ', 0, 0), &ok);
        if (!ok) {
            ctx_.report(tr("'%1' is not a number").arg(text), MessageLevel::Error);
            return false;
        }
        out = v;
        return true;
    };
    QString what;
    switch (col) {
    case CName:
        n.name = sstr(text);
        what = tr("Rename activity");
        break;
    case CWbs:
        n.wbsCode = sstr(text);
        what = tr("Edit WBS code");
        break;
    case CDuration: {
        double d;
        if (!number(d)) break;
        if (n.durationFromRate) {
            ctx_.report(tr("The duration of '%1' follows from its quantity and rate; edit those instead").arg(qstr(n.name)), MessageLevel::Warning);
            break;
        }
        n.durationHours = d * dayH;
        what = tr("Edit duration");
        break;
    }
    case CPercent: {
        double d;
        if (!number(d)) break;
        n.manualPercentComplete = d;
        what = tr("Edit percent complete");
        break;
    }
    case CQuantity: {
        double d;
        if (!number(d)) break;
        n.quantity = d;
        what = tr("Edit quantity");
        break;
    }
    case CRate: {
        double d;
        if (!number(d)) break;
        n.rate = n.rateUnit == RateUnit::PerHour ? d / dayH : d;
        what = tr("Edit rate");
        break;
    }
    default:
        ctx_.report(tr("This column is calculated; edit the activity's inputs in Properties"), MessageLevel::Warning);
        break;
    }
    if (what.isEmpty() || !ctx_.tryEdit(what, [&] { ProjectEditor(ctx_.project()).updateActivity(n, sstr(what)); })) scheduleRebuild();
}

Uuid SchedulerPanel::addActivity(ActivityKind kind, const QString& name) {
    if (!ctx_.hasProject()) return {};
    const auto& m = ctx_.project().model();
    ScheduleActivity a;
    a.kind = kind;
    a.parentId = parentForNew();
    a.name = sstr(name.isEmpty() ? (kind == ActivityKind::Milestone ? tr("Milestone %1") : kind == ActivityKind::Summary ? tr("Summary %1") : tr("Task %1")).arg(m.activities.size() + 1) : name);
    a.durationHours = kind == ActivityKind::Milestone || kind == ActivityKind::Summary ? 0 : 24;
    int maxOrder = 0;
    for (const auto& x : m.activities.all()) maxOrder = std::max(maxOrder, x.sortOrder);
    a.sortOrder = maxOrder + 10;
    if (const auto* sel = m.activities.find(selectedActivity()); sel && sel->kind != ActivityKind::Summary) a.sortOrder = sel->sortOrder + 1;
    Uuid id;
    if (ctx_.tryEdit(tr("Add activity"), [&] { id = ProjectEditor(ctx_.project()).addActivity(a); })) ctx_.select(EntityKind::Activity, id);
    return id;
}

void SchedulerPanel::addDependency() {
    const auto sel = ctx_.selected(EntityKind::Activity);
    Uuid pred = sel.size() >= 2 ? sel[sel.size() - 2] : (sel.empty() ? Uuid{} : sel.back());
    Uuid succ = sel.size() >= 2 ? sel.back() : Uuid{};
    DependencyDialog dlg(ctx_, pred, succ, this);
    dlg.exec();
}

void SchedulerPanel::deleteSelected() {
    const auto sel = ctx_.selected(EntityKind::Activity);
    if (sel.empty()) return;
    ctx_.tryEdit(tr("Delete activities"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Delete activities");
        ProjectEditor ed(p);
        for (const auto& id : sel)
            if (p.model().activities.contains(id)) ed.removeActivity(id);
        macro.commit();
    });
}

void SchedulerPanel::indent() {
    if (groupBy_ != GroupBy::Wbs) {
        ctx_.report(tr("Show the WBS hierarchy (Group by ▸ WBS) to change it"), MessageLevel::Warning);
        return;
    }
    const Uuid id = selectedActivity();
    auto* it = itemFor(id);
    if (!it) return;
    QTreeWidgetItem* above = it->parent() ? it->parent()->child(it->parent()->indexOfChild(it) - 1) : grid_->topLevelItem(grid_->indexOfTopLevelItem(it) - 1);
    if (!above) {
        ctx_.report(tr("There is no activity above to indent under"), MessageLevel::Warning);
        return;
    }
    const auto& m = ctx_.project().model();
    const auto* parent = m.activities.find(uuidFrom(above->data(0, kIdRole).toString()));
    const auto* a = m.activities.find(id);
    if (!parent || !a) return;
    if (parent->kind != ActivityKind::Summary) {
        ctx_.report(tr("'%1' is not a summary activity; only summaries can have children").arg(qstr(parent->name)), MessageLevel::Warning);
        return;
    }
    ScheduleActivity n = *a;
    n.parentId = parent->id;
    ctx_.tryEdit(tr("Indent"), [&] { ProjectEditor(ctx_.project()).updateActivity(n, "Indent " + n.name); });
}

void SchedulerPanel::outdent() {
    if (groupBy_ != GroupBy::Wbs) {
        ctx_.report(tr("Show the WBS hierarchy (Group by ▸ WBS) to change it"), MessageLevel::Warning);
        return;
    }
    const auto& m = ctx_.project().model();
    const auto* a = m.activities.find(selectedActivity());
    if (!a || a->parentId.isNil()) return;
    const auto* parent = m.activities.find(a->parentId);
    ScheduleActivity n = *a;
    n.parentId = parent ? parent->parentId : Uuid{};
    ctx_.tryEdit(tr("Outdent"), [&] { ProjectEditor(ctx_.project()).updateActivity(n, "Outdent " + n.name); });
}

void SchedulerPanel::moveSelected(int delta) {
    if (groupBy_ != GroupBy::Wbs) {
        ctx_.report(tr("Show the WBS hierarchy (Group by ▸ WBS) to change it"), MessageLevel::Warning);
        return;
    }
    const auto& m = ctx_.project().model();
    const auto* a = m.activities.find(selectedActivity());
    if (!a) return;
    // Siblings in display order.
    std::vector<const ScheduleActivity*> sib;
    for (const auto& x : m.activities.all())
        if (x.parentId == a->parentId) sib.push_back(&x);
    std::stable_sort(sib.begin(), sib.end(), [](auto* x, auto* y) { return x->sortOrder < y->sortOrder; });
    const auto pos = std::find(sib.begin(), sib.end(), a) - sib.begin();
    const long other = pos + delta;
    if (other < 0 || other >= static_cast<long>(sib.size())) return;
    ScheduleActivity x = *a, y = *sib[static_cast<std::size_t>(other)];
    std::swap(x.sortOrder, y.sortOrder);
    if (x.sortOrder == y.sortOrder) x.sortOrder += delta;
    ctx_.tryEdit(tr("Move activity"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Move activity");
        ProjectEditor ed(p);
        ed.updateActivity(x);
        ed.updateActivity(y);
        macro.commit();
    });
}

void SchedulerPanel::captureBaseline(const QString& nameIn) {
    if (!ctx_.hasProject()) return;
    const auto& r = ctx_.schedule().result;
    if (!r.ok) {
        ctx_.report(tr("The schedule must calculate before a baseline can be captured"), MessageLevel::Error);
        return;
    }
    QString name = nameIn;
    if (name.isEmpty()) {
        bool ok = false;
        name = QInputDialog::getText(this, tr("Capture baseline"), tr("Baseline name (baselines are immutable once captured):"), QLineEdit::Normal,
                                     tr("Baseline %1").arg(ctx_.project().model().baselines.size() + 1), &ok);
        if (!ok || name.isEmpty()) return;
    }
    ctx_.tryEdit(tr("Capture baseline"), [&] {
        ProjectEditor(ctx_.project()).addBaseline(scheduling::makeBaseline(ctx_.project(), r, sstr(name)));
    });
}

void SchedulerPanel::applyDesignQuantities() {
    if (!ctx_.hasProject()) return;
    std::vector<Uuid> ids = ctx_.selected(EntityKind::Activity);
    if (ids.empty())
        for (const auto& a : ctx_.project().model().activities.all())
            if (a.designImpactFlag) ids.push_back(a.id);
    if (ids.empty()) {
        ctx_.report(tr("No activities are flagged by design changes"));
        return;
    }
    ctx_.tryEdit(tr("Apply design quantities"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Apply design quantities");
        for (const auto& id : ids)
            if (!p.model().developmentsForActivity(id).empty()) mine_design::applyDesignQuantities(p, id);
        macro.commit();
    });
}

void SchedulerPanel::recordActual() {
    const Uuid id = selectedActivity();
    if (id.isNil()) {
        ctx_.report(tr("Select an activity first"), MessageLevel::Warning);
        return;
    }
    ActualDialog dlg(ctx_, id, this);
    dlg.exec();
}

void SchedulerPanel::editScenario(bool create) {
    if (!ctx_.hasProject()) return;
    const auto* active = ctx_.activeScenario();
    if (!create && !active) {
        ctx_.report(tr("The base schedule has no scenario settings; create a scenario"), MessageLevel::Warning);
        return;
    }
    ScenarioDialog dlg(ctx_, create ? Uuid{} : active->id, this);
    if (dlg.exec() == QDialog::Accepted && create) ctx_.setActiveScenario(dlg.scenarioId());
}

void SchedulerPanel::optimise() {
    auto* dlg = new OptimiserDialog(ctx_, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

bool SchedulerPanel::exportFile(const QString& path) {
    const auto& s = ctx_.schedule();
    if (!s.result.ok) {
        ctx_.report(tr("The schedule is not calculated"), MessageLevel::Error);
        return false;
    }
    const QString ext = QFileInfo(path).suffix().toLower();
    return ctx_.tryEdit(tr("Export schedule"), [&] {
        const auto file = std::filesystem::path(path.toStdU16String());
        const auto rows = scheduling::scheduleTable(ctx_.project(), s.input, s.result, ctx_.comparisonBaseline());
        if (ext == "csv") {
            std::ofstream out(file, std::ios::binary);
            if (!out) throw std::runtime_error("Cannot write " + file.string());
            scheduling::writeCsv(out, rows);
        } else if (ext == "xlsx") {
            reporting::writeScheduleXlsx(ctx_.project(), rows, file);
        } else if (ext == "pdf") {
            reporting::writeGanttPdf(ctx_.project(), s.input, s.result, ctx_.comparisonBaseline(), file);
        } else {
            throw ValidationError("Export format must be .csv, .xlsx or .pdf");
        }
        ctx_.report(tr("Schedule exported to %1").arg(path));
    });
}

} // namespace openmine::app
