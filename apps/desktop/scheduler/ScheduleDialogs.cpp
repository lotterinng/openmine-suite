// SPDX-License-Identifier: Apache-2.0
#include "ScheduleDialogs.h"

#include "../app/EntityInfo.h"
#include "../theme/Theme.h"

#include "openmine/objects/EnumNames.h"
#include "openmine/project/ProjectEditor.h"

#ifdef OPENMINE_APP_HAVE_OPTIMISATION
#include "openmine/optimisation/Optimiser.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QPointer>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace openmine::app {

namespace {
void fillActivities(QComboBox* c, const ProjectModel& m, const Uuid& select) {
    for (const auto& a : m.activities.all()) {
        if (a.kind == ActivityKind::Summary) continue;
        c->addItem(displayName(m, {EntityKind::Activity, a.id}), uuidText(a.id));
        if (a.id == select) c->setCurrentIndex(c->count() - 1);
    }
}
QDate toQDate(Date d) {
    int y;
    unsigned m, dd;
    d.toYmd(y, m, dd);
    return QDate(y, static_cast<int>(m), static_cast<int>(dd));
}
} // namespace

// ---- dependency --------------------------------------------------------------------------------------

DependencyDialog::DependencyDialog(AppContext& ctx, Uuid predecessor, Uuid successor, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Add dependency"));
    auto* form = new QFormLayout(this);
    const auto& m = ctx_.project().model();
    pred_ = new QComboBox(this);
    succ_ = new QComboBox(this);
    fillActivities(pred_, m, predecessor);
    fillActivities(succ_, m, successor);
    type_ = new QComboBox(this);
    type_->addItem(tr("Finish → start (FS)"), static_cast<int>(DependencyType::FinishToStart));
    type_->addItem(tr("Start → start (SS)"), static_cast<int>(DependencyType::StartToStart));
    type_->addItem(tr("Finish → finish (FF)"), static_cast<int>(DependencyType::FinishToFinish));
    type_->addItem(tr("Start → finish (SF)"), static_cast<int>(DependencyType::StartToFinish));
    lag_ = new QDoubleSpinBox(this);
    lag_->setRange(-10000, 10000);
    lag_->setSuffix(tr(" h"));
    lag_->setToolTip(tr("Working hours on the successor's calendar; negative = lead"));
    form->addRow(tr("Predecessor"), pred_);
    form->addRow(tr("Successor"), succ_);
    form->addRow(tr("Type"), type_);
    form->addRow(tr("Lag"), lag_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (apply()) accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

bool DependencyDialog::apply() {
    ActivityDependency d;
    d.predecessorId = uuidFrom(pred_->currentData().toString());
    d.successorId = uuidFrom(succ_->currentData().toString());
    d.type = static_cast<DependencyType>(type_->currentData().toInt());
    d.lagHours = lag_->value();
    return ctx_.tryEdit(tr("Add dependency"), [&] { ProjectEditor(ctx_.project()).addDependency(d); });
}

// ---- actual ------------------------------------------------------------------------------------------

ActualDialog::ActualDialog(AppContext& ctx, Uuid activity, QWidget* parent) : QDialog(parent), ctx_(ctx), activity_(activity) {
    const auto* a = ctx_.project().model().activities.find(activity);
    setWindowTitle(tr("Record actual — %1").arg(a ? qstr(a->name) : QString()));
    auto* form = new QFormLayout(this);
    date_ = new QDateEdit(this);
    date_->setCalendarPopup(true);
    date_->setDisplayFormat("yyyy-MM-dd");
    date_->setDate(toQDate(ctx_.project().model().info.statusDate ? ctx_.project().model().info.statusDate->date() : Date::todayUtc()));
    qty_ = new QDoubleSpinBox(this);
    qty_->setRange(-1e9, 1e9);
    qty_->setDecimals(3);
    qty_->setToolTip(tr("Incremental quantity on this date (negative corrects an earlier entry)"));
    unit_ = new QLineEdit(a ? qstr(a->quantityUnit) : QStringLiteral("m"), this);
    source_ = new QComboBox(this);
    for (const auto& n : enumNames<ActualSource>()) source_->addItem(qstr(n));
    hasFace_ = new QCheckBox(tr("Surveyed face chainage"), this);
    face_ = new QDoubleSpinBox(this);
    face_->setRange(-1e6, 1e6);
    face_->setDecimals(3);
    face_->setEnabled(false);
    connect(hasFace_, &QCheckBox::toggled, face_, &QWidget::setEnabled);
    note_ = new QLineEdit(this);
    form->addRow(tr("Date"), date_);
    form->addRow(tr("Quantity"), qty_);
    form->addRow(tr("Unit"), unit_);
    form->addRow(tr("Source"), source_);
    form->addRow(hasFace_, face_);
    form->addRow(tr("Note"), note_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (apply()) accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

bool ActualDialog::apply() {
    ProductionActual x;
    x.activityId = activity_;
    const QDate d = date_->date();
    x.date = Date::fromYmd(d.year(), static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
    x.quantity = qty_->value();
    x.unit = sstr(unit_->text());
    x.source = enumFromString<ActualSource>(sstr(source_->currentText())).value_or(ActualSource::Manual);
    if (hasFace_->isChecked()) x.faceChainage = face_->value();
    x.note = sstr(note_->text());
    x.enteredBy = ctx_.project().userName();
    const auto devs = ctx_.project().model().developmentsForActivity(activity_);
    if (devs.size() == 1) x.developmentId = devs.front();
    return ctx_.tryEdit(tr("Record actual"), [&] { ProjectEditor(ctx_.project()).add(x, "Record actual"); });
}

// ---- scenario ----------------------------------------------------------------------------------------

ScenarioDialog::ScenarioDialog(AppContext& ctx, Uuid scenario, QWidget* parent) : QDialog(parent), ctx_(ctx), id_(scenario) {
    const auto& m = ctx_.project().model();
    const auto* s = m.scenarios.find(scenario);
    setWindowTitle(s ? tr("Scenario — %1").arg(qstr(s->name)) : tr("New scenario"));
    resize(640, 520);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(s ? qstr(s->name) : tr("Scenario %1").arg(m.scenarios.size() + 1), this);
    description_ = new QPlainTextEdit(s ? qstr(s->description) : QString(), this);
    description_->setMaximumHeight(60);
    levelling_ = new QComboBox(this);
    levelling_->addItem(tr("None (CPM, unlimited resources)"), static_cast<int>(LevellingMode::None));
    levelling_->addItem(tr("Heuristic resource levelling"), static_cast<int>(LevellingMode::Heuristic));
    levelling_->addItem(tr("Optimised (sequence stored by the optimiser)"), static_cast<int>(LevellingMode::Optimised));
    if (s) levelling_->setCurrentIndex(levelling_->findData(static_cast<int>(s->levelling)));
    objective_ = new QComboBox(this);
    objective_->addItems({"Makespan", "WeightedCompletion", "Lateness"});
    if (s) objective_->setCurrentText(qstr(s->objective));
    timeLimit_ = new QDoubleSpinBox(this);
    timeLimit_->setRange(1, 3600);
    timeLimit_->setSuffix(" s");
    timeLimit_->setValue(s ? s->optimiserTimeLimitSeconds : 30);
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Description"), description_);
    form->addRow(tr("Levelling"), levelling_);
    form->addRow(tr("Optimiser objective"), objective_);
    form->addRow(tr("Optimiser time limit"), timeLimit_);
    lay->addLayout(form);
    lay->addWidget(new QLabel(tr("Overrides (applied to copies of the base schedule's inputs):"), this));
    overrides_ = new QTableWidget(this);
    overrides_->setColumnCount(3);
    overrides_->setHorizontalHeaderLabels({tr("Activity / resource"), tr("Field"), tr("Value")});
    overrides_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    lay->addWidget(overrides_, 1);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add override"), this);
    auto* del = new QPushButton(tr("Remove"), this);
    row->addWidget(add);
    row->addWidget(del);
    row->addStretch(1);
    lay->addLayout(row);
    if (s)
        for (const auto& o : s->overrides) addOverrideRow(o);
    connect(add, &QPushButton::clicked, this, [this] {
        ScenarioOverride o;
        if (!ctx_.project().model().activities.empty()) o.entityId = ctx_.project().model().activities.all().front().id;
        o.field = "rate";
        addOverrideRow(o);
    });
    connect(del, &QPushButton::clicked, this, [this] {
        if (overrides_->currentRow() >= 0) overrides_->removeRow(overrides_->currentRow());
    });
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (apply()) accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ScenarioDialog::addOverrideRow(const ScenarioOverride& o) {
    const auto& m = ctx_.project().model();
    const int r = overrides_->rowCount();
    overrides_->insertRow(r);
    auto* ent = new QComboBox;
    for (const auto& a : m.activities.all()) ent->addItem(tr("Activity: %1").arg(displayName(m, {EntityKind::Activity, a.id})), uuidText(a.id));
    for (const auto& x : m.resources.all()) ent->addItem(tr("Resource: %1").arg(qstr(x.name)), uuidText(x.id));
    ent->setCurrentIndex(std::max(0, ent->findData(uuidText(o.entityId))));
    overrides_->setCellWidget(r, 0, ent);
    auto* field = new QComboBox;
    field->addItems({"rate", "durationHours", "priority", "capacity", "excluded", "calendarId", "productivityFactor", "startNoEarlierThan"});
    field->setCurrentText(qstr(o.field));
    overrides_->setCellWidget(r, 1, field);
    overrides_->setItem(r, 2, new QTableWidgetItem(qstr(o.value)));
}

bool ScenarioDialog::apply() {
    Scenario s;
    if (const auto* e = ctx_.project().model().scenarios.find(id_)) s = *e;
    s.name = sstr(name_->text().trimmed());
    s.description = sstr(description_->toPlainText());
    s.levelling = static_cast<LevellingMode>(levelling_->currentData().toInt());
    s.objective = sstr(objective_->currentText());
    s.optimiserTimeLimitSeconds = timeLimit_->value();
    s.overrides.clear();
    for (int r = 0; r < overrides_->rowCount(); ++r) {
        ScenarioOverride o;
        o.entityId = uuidFrom(static_cast<QComboBox*>(overrides_->cellWidget(r, 0))->currentData().toString());
        o.field = sstr(static_cast<QComboBox*>(overrides_->cellWidget(r, 1))->currentText());
        o.value = overrides_->item(r, 2) ? sstr(overrides_->item(r, 2)->text().trimmed()) : std::string();
        s.overrides.push_back(o);
    }
    return ctx_.tryEdit(tr("Save scenario"), [&] {
        ProjectEditor ed(ctx_.project());
        if (ctx_.project().model().scenarios.contains(s.id)) ed.update(s, "Edit scenario " + s.name);
        else id_ = ed.add(s, "Create scenario " + s.name);
    });
}

// ---- optimiser -------------------------------------------------------------------------------------

bool OptimiserDialog::available() {
#ifdef OPENMINE_APP_HAVE_OPTIMISATION
    return true;
#else
    return false;
#endif
}

OptimiserDialog::OptimiserDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Optimise schedule (CP-SAT)"));
    resize(620, 520);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    objective_ = new QComboBox(this);
    objective_->addItem(tr("Finish everything as early as possible (makespan)"), 0);
    objective_->addItem(tr("Weighted completion (priorities)"), 1);
    objective_->addItem(tr("Minimise lateness against deadlines"), 2);
    slot_ = new QDoubleSpinBox(this);
    slot_->setRange(0.25, 48);
    slot_->setValue(1.0);
    slot_->setSuffix(tr(" h"));
    slot_->setToolTip(tr("Time resolution of the model; calendars are respected exactly at this resolution"));
    limit_ = new QDoubleSpinBox(this);
    limit_->setRange(1, 3600);
    limit_->setValue(ctx_.activeScenario() ? ctx_.activeScenario()->optimiserTimeLimitSeconds : 30);
    limit_->setSuffix(" s");
    workers_ = new QSpinBox(this);
    workers_->setRange(1, 64);
    workers_->setValue(8);
    hard_ = new QCheckBox(tr("Deadlines (finish no later than) are hard constraints"), this);
    hard_->setChecked(true);
    form->addRow(tr("Objective"), objective_);
    form->addRow(tr("Time slot"), slot_);
    form->addRow(tr("Time limit"), limit_);
    form->addRow(tr("Search workers"), workers_);
    form->addRow(QString(), hard_);
    lay->addLayout(form);
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    lay->addWidget(log_, 1);
    auto* row = new QHBoxLayout;
    name_ = new QLineEdit(tr("Optimised %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm")), this);
    store_ = new QPushButton(tr("Store as scenario and activate"), this);
    store_->setEnabled(false);
    auto* run = new QPushButton(toolIcon("optimise"), tr("Optimise"), this);
    auto* close = new QPushButton(tr("Close"), this);
    row->addWidget(name_, 1);
    row->addWidget(store_);
    row->addWidget(run);
    row->addWidget(close);
    lay->addLayout(row);
    connect(run, &QPushButton::clicked, this, &OptimiserDialog::start);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    const auto* sc = ctx_.activeScenario();
    log_->setPlainText(tr("Optimises the remaining work of %1 against resource capacities, work-area limits, calendars, "
                          "dependencies and constraints. The base schedule is not changed: a sequence is stored as a scenario "
                          "whose dates CPM then recalculates.\n")
                           .arg(sc ? tr("scenario '%1'").arg(qstr(sc->name)) : tr("the base schedule")));
    if (!available()) {
        run->setEnabled(false);
        log_->appendPlainText(tr("\nOR-Tools is not available in this build: optimisation is disabled."));
    }
#ifdef OPENMINE_APP_HAVE_OPTIMISATION
    connect(store_, &QPushButton::clicked, this, [this] {
        auto r = std::static_pointer_cast<optimisation::OptimisationResult>(result_);
        if (!r) return;
        Uuid id;
        if (ctx_.tryEdit(tr("Store optimised sequence"), [&] {
                id = optimisation::storeAsScenario(ctx_.project(), *r, sstr(name_->text()), ctx_.activeScenario());
            })) {
            ctx_.setActiveScenario(id);
            log_->appendPlainText(tr("\nStored as scenario '%1' and activated.").arg(name_->text()));
            store_->setEnabled(false);
        }
    });
#endif
}

void OptimiserDialog::start() {
#ifdef OPENMINE_APP_HAVE_OPTIMISATION
    optimisation::OptimiserOptions o;
    o.objective = static_cast<optimisation::Objective>(objective_->currentData().toInt());
    o.slotHours = slot_->value();
    o.timeLimitSeconds = limit_->value();
    o.workers = workers_->value();
    o.hardDeadlines = hard_->isChecked();
    const auto snap = ctx_.snapshot();
    const Uuid scenarioId = ctx_.schedule().scenarioId;
    store_->setEnabled(false);
    log_->appendPlainText(tr("\nSolving (time limit %1 s)…").arg(o.timeLimitSeconds));
    QPointer<OptimiserDialog> self(this);
    ctx_.jobs().start(tr("Optimise schedule"), tr("Schedule"), [snap, scenarioId, o, self](jobs::Progress& p) -> UiApply {
        const Scenario* sc = snap->model().scenarios.find(scenarioId);
        auto r = std::make_shared<optimisation::OptimisationResult>(optimisation::optimise(*snap, sc, o, p));
        return [self, r] {
            if (!self) return;
            self->result_ = r;
            QString text = tr("Status: %1\nMakespan %2 h (CPM without resource limits: %3 h)\nObjective %4, bound %5, %6 s, %7 variables, %8 constraints")
                               .arg(qstr(r->statusText))
                               .arg(r->makespanHours, 0, 'f', 1)
                               .arg(r->cpmMakespanHours, 0, 'f', 1)
                               .arg(r->objective, 0, 'g', 6)
                               .arg(r->bestBound, 0, 'g', 6)
                               .arg(r->solveSeconds, 0, 'f', 1)
                               .arg(r->variables)
                               .arg(r->constraints);
            for (const auto& c : r->conflicts) text += tr("\nConflict (%1): %2").arg(qstr(c.kind), qstr(c.description));
            for (const auto& n : r->notes) text += "\n" + qstr(n);
            self->log_->appendPlainText(text);
            const bool feasible = r->status == optimisation::OptimisationResult::Status::Optimal || r->status == optimisation::OptimisationResult::Status::Feasible;
            self->store_->setEnabled(feasible);
        };
    }, false, [self](const QString& err) {
        if (self) self->log_->appendPlainText(tr("Failed: %1").arg(err));
    });
#endif
}

} // namespace openmine::app
