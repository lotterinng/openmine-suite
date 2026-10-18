// SPDX-License-Identifier: Apache-2.0
#include "LibraryDialogs.h"

#include "../dialogs/DialogUtil.h"
#include "../theme/Theme.h"

#include "openmine/objects/EnumNames.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/CalendarTime.h"
#include "openmine/scheduling/Production.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

#include <cmath>

namespace openmine::app {

namespace {

QPushButton* button(const QString& icon, const QString& text, QWidget* parent) { return new QPushButton(toolIcon(icon), text, parent); }

QTableWidgetItem* cell(const QString& text, bool editable = true) {
    auto* it = new QTableWidgetItem(text);
    if (!editable) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    return it;
}

double cellNumber(const QTableWidget* t, int row, int col, bool* ok) {
    const auto* it = t->item(row, col);
    return it ? QLocale::c().toDouble(it->text().trimmed(), ok) : (*ok = false, 0.0);
}

QString hhmm(int minuteOfDay) { return QStringLiteral("%1:%2").arg(minuteOfDay / 60, 2, 10, QChar('0')).arg(minuteOfDay % 60, 2, 10, QChar('0')); }

std::optional<int> parseHhmm(const QString& s) {
    static const QRegularExpression re("^\\s*(\\d{1,2}):(\\d{2})\\s*$");
    const auto m = re.match(s);
    if (!m.hasMatch()) return std::nullopt;
    const int h = m.captured(1).toInt(), mi = m.captured(2).toInt();
    if (h > 24 || mi > 59 || (h == 24 && mi != 0)) return std::nullopt;
    return h * 60 + mi;
}

const char* kWeekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

} // namespace

// ---- resources ---------------------------------------------------------------------------------------

namespace {
enum RCol { RName, RCode, RKind, RCapacity, RCalendar, RProductivity, RCost, RDescription, RNCols };
enum ACol { AActivity, AResource, AUnits, ADrives, ANCols };
} // namespace

ResourceLibraryDialog::ResourceLibraryDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Resources and assignments"));
    resize(980, 640);
    auto* lay = new QVBoxLayout(this);
    auto* split = new QSplitter(Qt::Vertical, this);
    lay->addWidget(split, 1);

    auto* top = new QGroupBox(tr("Resource library"), split);
    auto* tl = new QVBoxLayout(top);
    resources_ = new QTableWidget(0, RNCols, top);
    resources_->setHorizontalHeaderLabels({tr("Name"), tr("Code"), tr("Kind"), tr("Capacity"), tr("Calendar"), tr("Productivity ×"), tr("Cost / h"), tr("Description")});
    resources_->horizontalHeader()->setStretchLastSection(true);
    resources_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tl->addWidget(resources_, 1);
    auto* rrow = new QHBoxLayout;
    auto* add = button("add", tr("Add resource"), top);
    auto* del = button("delete", tr("Delete"), top);
    rrow->addWidget(add);
    rrow->addWidget(del);
    rrow->addStretch(1);
    tl->addLayout(rrow);

    auto* bottom = new QGroupBox(tr("Assignments"), split);
    auto* bl = new QVBoxLayout(bottom);
    scope_ = new QLabel(bottom);
    bl->addWidget(scope_);
    assignments_ = new QTableWidget(0, ANCols, bottom);
    assignments_->setHorizontalHeaderLabels({tr("Activity"), tr("Resource"), tr("Units"), tr("Drives the rate")});
    assignments_->horizontalHeader()->setStretchLastSection(true);
    assignments_->setSelectionBehavior(QAbstractItemView::SelectRows);
    bl->addWidget(assignments_, 1);
    auto* arow = new QHBoxLayout;
    auto* assignBtn = button("link", tr("Assign selected resource to the selected activities"), bottom);
    auto* unassign = button("delete", tr("Remove assignment"), bottom);
    arow->addWidget(assignBtn);
    arow->addWidget(unassign);
    arow->addStretch(1);
    bl->addLayout(arow);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(add, &QPushButton::clicked, this, [this] {
        bool ok = true;
        QString name = tr("Resource %1").arg(ctx_.project().model().resources.size() + 1);
        if (isVisible() && !property("nonInteractive").toBool()) name = QInputDialog::getText(this, tr("Add resource"), tr("Name:"), QLineEdit::Normal, name, &ok);
        if (ok && !name.trimmed().isEmpty()) addResource(name.trimmed(), ResourceKind::Equipment, 1.0);
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const int r = resources_->currentRow();
        if (r < 0) return;
        const Uuid id = uuidFrom(resources_->item(r, RName)->data(Qt::UserRole).toString());
        ctx_.tryEdit(tr("Delete resource"), [&] { ProjectEditor(ctx_.project()).remove<Resource>(id); });
    });
    connect(assignBtn, &QPushButton::clicked, this, [this] {
        const int r = resources_->currentRow();
        const auto acts = ctx_.selected(EntityKind::Activity);
        if (r < 0 || acts.empty()) {
            ctx_.report(tr("Select a resource here and one or more activities in the scheduler"), MessageLevel::Warning);
            return;
        }
        const Uuid res = uuidFrom(resources_->item(r, RName)->data(Qt::UserRole).toString());
        for (const auto& a : acts) assign(a, res, 1.0, false);
    });
    connect(unassign, &QPushButton::clicked, this, [this] {
        const int r = assignments_->currentRow();
        if (r < 0) return;
        const Uuid id = uuidFrom(assignments_->item(r, AActivity)->data(Qt::UserRole).toString());
        ctx_.tryEdit(tr("Remove assignment"), [&] { ProjectEditor(ctx_.project()).remove<ActivityResourceAssignment>(id); });
    });
    connect(resources_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (!building_) commitResource(it->row());
    });
    connect(assignments_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (!building_) commitAssignment(it->row());
    });
    connect(&ctx_, &AppContext::modelChanged, this, &ResourceLibraryDialog::refresh);
    connect(&ctx_, &AppContext::selectionChanged, this, &ResourceLibraryDialog::rebuildAssignments);
    refresh();
}

void ResourceLibraryDialog::refresh() {
    if (!ctx_.hasProject()) return;
    rebuildResources();
    rebuildAssignments();
}

void ResourceLibraryDialog::rebuildResources() {
    const auto& m = ctx_.project().model();
    building_ = true;
    const int keep = resources_->currentRow();
    resources_->setRowCount(0);
    for (const auto& r : m.resources.all()) {
        const int row = resources_->rowCount();
        resources_->insertRow(row);
        auto* name = cell(qstr(r.name));
        name->setData(Qt::UserRole, uuidText(r.id));
        resources_->setItem(row, RName, name);
        resources_->setItem(row, RCode, cell(qstr(r.code)));
        auto* kind = new QComboBox;
        for (const auto& n : enumNames<ResourceKind>()) kind->addItem(qstr(n));
        kind->setCurrentText(toString(r.kind));
        resources_->setCellWidget(row, RKind, kind);
        resources_->setItem(row, RCapacity, cell(QString::number(r.capacity, 'g', 10)));
        auto* cal = new QComboBox;
        fillEntityCombo(cal, m, EntityKind::Calendar, r.calendarId, true);
        resources_->setCellWidget(row, RCalendar, cal);
        resources_->setItem(row, RProductivity, cell(QString::number(r.productivityFactor, 'g', 10)));
        resources_->setItem(row, RCost, cell(QString::number(r.costPerHour, 'g', 10)));
        resources_->setItem(row, RDescription, cell(qstr(r.description)));
        connect(kind, &QComboBox::currentIndexChanged, this, [this, row] { commitResource(row); });
        connect(cal, &QComboBox::currentIndexChanged, this, [this, row] { commitResource(row); });
    }
    resources_->resizeColumnsToContents();
    if (keep >= 0 && keep < resources_->rowCount()) resources_->selectRow(keep);
    building_ = false;
}

void ResourceLibraryDialog::rebuildAssignments() {
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    building_ = true;
    assignments_->setRowCount(0);
    const auto acts = ctx_.selected(EntityKind::Activity);
    scope_->setText(acts.empty() ? tr("All assignments (select activities in the scheduler to show theirs)")
                                 : tr("Assignments of %1 selected activit%2").arg(acts.size()).arg(acts.size() == 1 ? "y" : "ies"));
    for (const auto& a : m.assignments.all()) {
        if (!acts.empty() && std::find(acts.begin(), acts.end(), a.activityId) == acts.end()) continue;
        const int row = assignments_->rowCount();
        assignments_->insertRow(row);
        const auto* act = m.activities.find(a.activityId);
        auto* name = cell(act ? qstr(act->name) : tr("(missing)"), false);
        name->setData(Qt::UserRole, uuidText(a.id));
        assignments_->setItem(row, AActivity, name);
        auto* res = new QComboBox;
        fillEntityCombo(res, m, EntityKind::Resource, a.resourceId);
        assignments_->setCellWidget(row, AResource, res);
        assignments_->setItem(row, AUnits, cell(QString::number(a.units, 'g', 10)));
        auto* drives = cell(QString(), false);
        drives->setFlags(drives->flags() | Qt::ItemIsUserCheckable);
        drives->setCheckState(a.drivesRate ? Qt::Checked : Qt::Unchecked);
        assignments_->setItem(row, ADrives, drives);
        connect(res, &QComboBox::currentIndexChanged, this, [this, row] { commitAssignment(row); });
    }
    assignments_->resizeColumnsToContents();
    building_ = false;
}

void ResourceLibraryDialog::commitResource(int row) {
    if (building_ || row < 0 || row >= resources_->rowCount()) return;
    const Uuid id = uuidFrom(resources_->item(row, RName)->data(Qt::UserRole).toString());
    const auto* cur = ctx_.project().model().resources.find(id);
    if (!cur) return;
    Resource r = *cur;
    bool ok1 = false, ok2 = false, ok3 = false;
    r.name = sstr(resources_->item(row, RName)->text().trimmed());
    r.code = sstr(resources_->item(row, RCode)->text().trimmed());
    r.kind = enumFromString<ResourceKind>(sstr(static_cast<QComboBox*>(resources_->cellWidget(row, RKind))->currentText())).value_or(r.kind);
    r.capacity = cellNumber(resources_, row, RCapacity, &ok1);
    r.calendarId = comboUuid(static_cast<QComboBox*>(resources_->cellWidget(row, RCalendar)));
    r.productivityFactor = cellNumber(resources_, row, RProductivity, &ok2);
    r.costPerHour = cellNumber(resources_, row, RCost, &ok3);
    r.description = sstr(resources_->item(row, RDescription)->text());
    if (!ok1 || !ok2 || !ok3) {
        ctx_.report(tr("Capacity, productivity and cost must be numbers"), MessageLevel::Error);
        QMetaObject::invokeMethod(this, &ResourceLibraryDialog::rebuildResources, Qt::QueuedConnection);
        return;
    }
    if (!ctx_.tryEdit(tr("Edit resource"), [&] { ProjectEditor(ctx_.project()).update(r, "Edit resource " + r.name); }))
        QMetaObject::invokeMethod(this, &ResourceLibraryDialog::rebuildResources, Qt::QueuedConnection);
}

void ResourceLibraryDialog::commitAssignment(int row) {
    if (building_ || row < 0 || row >= assignments_->rowCount()) return;
    const Uuid id = uuidFrom(assignments_->item(row, AActivity)->data(Qt::UserRole).toString());
    const auto* cur = ctx_.project().model().assignments.find(id);
    if (!cur) return;
    ActivityResourceAssignment a = *cur;
    bool ok = false;
    a.resourceId = comboUuid(static_cast<QComboBox*>(assignments_->cellWidget(row, AResource)));
    a.units = cellNumber(assignments_, row, AUnits, &ok);
    a.drivesRate = assignments_->item(row, ADrives)->checkState() == Qt::Checked;
    if (!ok) {
        ctx_.report(tr("Units must be a number"), MessageLevel::Error);
        QMetaObject::invokeMethod(this, &ResourceLibraryDialog::rebuildAssignments, Qt::QueuedConnection);
        return;
    }
    if (!ctx_.tryEdit(tr("Edit assignment"), [&] { ProjectEditor(ctx_.project()).update(a, "Edit resource assignment"); }))
        QMetaObject::invokeMethod(this, &ResourceLibraryDialog::rebuildAssignments, Qt::QueuedConnection);
}

Uuid ResourceLibraryDialog::addResource(const QString& name, ResourceKind kind, double capacity) {
    Resource r;
    r.name = sstr(name);
    r.kind = kind;
    r.capacity = capacity;
    Uuid id;
    ctx_.tryEdit(tr("Add resource"), [&] { id = ProjectEditor(ctx_.project()).add(r, "Add resource " + r.name); });
    return id;
}

Uuid ResourceLibraryDialog::assign(const Uuid& activityId, const Uuid& resourceId, double units, bool drivesRate) {
    ActivityResourceAssignment a;
    a.activityId = activityId;
    a.resourceId = resourceId;
    a.units = units;
    a.drivesRate = drivesRate;
    Uuid id;
    ctx_.tryEdit(tr("Assign resource"), [&] { id = ProjectEditor(ctx_.project()).add(a, "Assign resource"); });
    return id;
}

// ---- calendars ---------------------------------------------------------------------------------------

namespace {
enum SCol { SName, SStart, SHours, SDay0, SNCols = SDay0 + 7 };
enum ECol { EDate, EPeriods, EReason, ENCols };
} // namespace

CalendarDialog::CalendarDialog(AppContext& ctx, const Uuid& calendarId, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Working calendars"));
    resize(1020, 660);
    auto* lay = new QHBoxLayout(this);
    auto* left = new QVBoxLayout;
    list_ = new QListWidget(this);
    list_->setMaximumWidth(240);
    left->addWidget(list_, 1);
    auto* add = button("add", tr("New"), this);
    auto* dup = button("copy", tr("Duplicate"), this);
    auto* del = button("delete", tr("Delete"), this);
    left->addWidget(add);
    left->addWidget(dup);
    left->addWidget(del);
    lay->addLayout(left);

    auto* right = new QVBoxLayout;
    auto* form = new QFormLayout;
    name_ = new QLineEdit(this);
    kind_ = new QComboBox(this);
    kind_->addItem(tr("Project calendar"), static_cast<int>(CalendarKind::Project));
    kind_->addItem(tr("Shift calendar"), static_cast<int>(CalendarKind::Shift));
    kind_->addItem(tr("Resource calendar"), static_cast<int>(CalendarKind::Resource));
    hours_ = spin(0.25, 24, 24, 2, " h", this);
    hours_->setToolTip(tr("Working hours that make one 'day' for rates and durations shown in days"));
    description_ = new QLineEdit(this);
    projectDefault_ = new QCheckBox(tr("Project default calendar (activities without a calendar use it)"), this);
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Kind"), kind_);
    form->addRow(tr("Hours per day"), hours_);
    form->addRow(tr("Description"), description_);
    form->addRow(QString(), projectDefault_);
    right->addLayout(form);

    auto* sg = new QGroupBox(tr("Weekly shift pattern (a shift belongs to the day it starts; it may run past midnight)"), this);
    auto* sl = new QVBoxLayout(sg);
    shifts_ = new QTableWidget(0, SNCols, sg);
    QStringList sh{tr("Shift"), tr("Starts"), tr("Hours")};
    for (const char* d : kWeekdays) sh << tr(d);
    shifts_->setHorizontalHeaderLabels(sh);
    sl->addWidget(shifts_);
    auto* srow = new QHBoxLayout;
    auto* addShiftBtn = button("add", tr("Add shift"), sg);
    auto* delShift = button("delete", tr("Remove shift"), sg);
    auto* preset = new QComboBox(sg);
    preset->addItems({tr("Presets…"), tr("24/7 continuous"), tr("Two 12 h shifts, 7 days"), tr("Two 10 h shifts, Mon–Fri"), tr("Day shift 07:00–17:00, Mon–Fri"),
                      tr("Three 8 h shifts, Mon–Sat")});
    srow->addWidget(addShiftBtn);
    srow->addWidget(delShift);
    srow->addWidget(preset);
    srow->addStretch(1);
    sl->addLayout(srow);
    right->addWidget(sg, 2);

    auto* eg = new QGroupBox(tr("Exceptions (the periods worked on that date; empty = no work, e.g. a holiday)"), this);
    auto* el = new QVBoxLayout(eg);
    exceptions_ = new QTableWidget(0, ENCols, eg);
    exceptions_->setHorizontalHeaderLabels({tr("Date"), tr("Working periods (HH:MM-HH:MM, …)"), tr("Reason")});
    exceptions_->horizontalHeader()->setStretchLastSection(true);
    el->addWidget(exceptions_);
    auto* erow = new QHBoxLayout;
    auto* addExc = button("add", tr("Add exception"), eg);
    auto* delExc = button("delete", tr("Remove exception"), eg);
    erow->addWidget(addExc);
    erow->addWidget(delExc);
    erow->addStretch(1);
    el->addLayout(erow);
    right->addWidget(eg, 2);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    right->addWidget(summary_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    right->addWidget(bb);
    lay->addLayout(right, 1);

    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int r) {
        if (r >= 0) load(uuidFrom(list_->item(r)->data(Qt::UserRole).toString()));
    });
    connect(add, &QPushButton::clicked, this, [this] { select(createCalendar(tr("Calendar %1").arg(ctx_.project().model().calendars.size() + 1), CalendarKind::Shift)); });
    connect(dup, &QPushButton::clicked, this, [this] {
        const auto* c = ctx_.project().model().calendars.find(current_);
        if (!c) return;
        WorkCalendar copy = *c;
        copy.id = {};
        copy.name += " (copy)";
        Uuid id;
        if (ctx_.tryEdit(tr("Duplicate calendar"), [&] { id = ProjectEditor(ctx_.project()).addCalendar(copy); })) select(id);
    });
    connect(del, &QPushButton::clicked, this, [this] {
        if (current_.isNil()) return;
        const Uuid id = current_;
        if (ctx_.tryEdit(tr("Delete calendar"), [&] { ProjectEditor(ctx_.project()).remove<WorkCalendar>(id); })) current_ = {};
        rebuildList();
    });
    connect(addShiftBtn, &QPushButton::clicked, this, [this] { addShift(tr("Shift %1").arg(shifts_->rowCount() + 1), 7 * 60, 10 * 60, 0b0011111); });
    connect(delShift, &QPushButton::clicked, this, [this] {
        if (shifts_->currentRow() >= 0) shifts_->removeRow(shifts_->currentRow());
        updateSummary();
    });
    connect(preset, &QComboBox::activated, this, [this, preset](int i) {
        if (i == 0) return;
        shifts_->setRowCount(0);
        switch (i) {
        case 1: addShift(tr("Continuous"), 0, 1440, 0x7F); break;
        case 2:
            addShift(tr("Day"), 6 * 60, 12 * 60, 0x7F);
            addShift(tr("Night"), 18 * 60, 12 * 60, 0x7F);
            break;
        case 3:
            addShift(tr("Day"), 6 * 60, 10 * 60, 0b0011111);
            addShift(tr("Night"), 18 * 60, 10 * 60, 0b0011111);
            break;
        case 4: addShift(tr("Day"), 7 * 60, 10 * 60, 0b0011111); break;
        case 5:
            addShift(tr("Morning"), 6 * 60, 8 * 60, 0b0111111);
            addShift(tr("Afternoon"), 14 * 60, 8 * 60, 0b0111111);
            addShift(tr("Night"), 22 * 60, 8 * 60, 0b0111111);
            break;
        }
        preset->setCurrentIndex(0);
        updateSummary();
    });
    connect(addExc, &QPushButton::clicked, this, [this] { addException(QDate::currentDate(), QString(), tr("Public holiday")); });
    connect(delExc, &QPushButton::clicked, this, [this] {
        if (exceptions_->currentRow() >= 0) exceptions_->removeRow(exceptions_->currentRow());
        updateSummary();
    });
    connect(shifts_, &QTableWidget::itemChanged, this, &CalendarDialog::updateSummary);
    connect(exceptions_, &QTableWidget::itemChanged, this, &CalendarDialog::updateSummary);
    connect(hours_, &QDoubleSpinBox::valueChanged, this, &CalendarDialog::updateSummary);

    rebuildList();
    const auto& m = ctx_.project().model();
    if (!calendarId.isNil()) select(calendarId);
    else if (!m.info.defaultCalendarId.isNil()) select(m.info.defaultCalendarId);
    else if (!m.calendars.empty()) select(m.calendars.all().front().id);
}

void CalendarDialog::rebuildList() {
    const auto& m = ctx_.project().model();
    list_->blockSignals(true);
    list_->clear();
    for (const auto& c : m.calendars.all()) {
        auto* it = new QListWidgetItem(toolIcon("calendar"), qstr(c.name) + (c.id == m.info.defaultCalendarId ? tr("  (default)") : QString()), list_);
        it->setData(Qt::UserRole, uuidText(c.id));
        if (c.id == current_) list_->setCurrentItem(it);
    }
    list_->blockSignals(false);
}

void CalendarDialog::select(const Uuid& id) {
    rebuildList();
    for (int i = 0; i < list_->count(); ++i)
        if (uuidFrom(list_->item(i)->data(Qt::UserRole).toString()) == id) {
            list_->blockSignals(true);
            list_->setCurrentRow(i);
            list_->blockSignals(false);
        }
    load(id);
}

void CalendarDialog::load(const Uuid& id) {
    const auto& m = ctx_.project().model();
    const auto* c = m.calendars.find(id);
    current_ = c ? id : Uuid{};
    shifts_->blockSignals(true);
    exceptions_->blockSignals(true);
    shifts_->setRowCount(0);
    exceptions_->setRowCount(0);
    if (c) {
        name_->setText(qstr(c->name));
        kind_->setCurrentIndex(kind_->findData(static_cast<int>(c->kind)));
        hours_->setValue(c->standardHoursPerDay);
        description_->setText(qstr(c->description));
        projectDefault_->setChecked(m.info.defaultCalendarId == c->id);
        for (const auto& s : c->shifts) addShift(qstr(s.name), s.startMinute, s.durationMinutes, s.weekdays);
        for (const auto& e : c->exceptions) {
            QStringList periods;
            for (const auto& p : e.periods) periods << hhmm(p.startMinute) + "-" + hhmm(p.endMinute);
            addException(toQDate(e.date), periods.join(", "), qstr(e.reason));
        }
    }
    shifts_->blockSignals(false);
    exceptions_->blockSignals(false);
    updateSummary();
}

void CalendarDialog::addShift(const QString& name, int startMinute, int durationMinutes, std::uint8_t weekdays) {
    const bool blocked = shifts_->signalsBlocked();
    shifts_->blockSignals(true);
    const int r = shifts_->rowCount();
    shifts_->insertRow(r);
    shifts_->setItem(r, SName, cell(name));
    shifts_->setItem(r, SStart, cell(hhmm(startMinute)));
    shifts_->setItem(r, SHours, cell(QString::number(durationMinutes / 60.0, 'g', 6)));
    for (int d = 0; d < 7; ++d) {
        auto* it = cell(QString(), false);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState((weekdays >> d) & 1 ? Qt::Checked : Qt::Unchecked);
        shifts_->setItem(r, SDay0 + d, it);
    }
    shifts_->resizeColumnsToContents();
    shifts_->blockSignals(blocked);
    updateSummary();
}

void CalendarDialog::addException(const QDate& date, const QString& periods, const QString& reason) {
    const bool blocked = exceptions_->signalsBlocked();
    exceptions_->blockSignals(true);
    const int r = exceptions_->rowCount();
    exceptions_->insertRow(r);
    exceptions_->setItem(r, EDate, cell(date.toString(Qt::ISODate)));
    exceptions_->setItem(r, EPeriods, cell(periods));
    exceptions_->setItem(r, EReason, cell(reason));
    exceptions_->blockSignals(blocked);
    updateSummary();
}

std::optional<WorkCalendar> CalendarDialog::edited(QString* error) const {
    const auto* cur = ctx_.project().model().calendars.find(current_);
    if (!cur) {
        *error = tr("No calendar selected");
        return std::nullopt;
    }
    WorkCalendar c = *cur;
    c.name = sstr(name_->text().trimmed());
    c.kind = static_cast<CalendarKind>(kind_->currentData().toInt());
    c.standardHoursPerDay = hours_->value();
    c.description = sstr(description_->text());
    c.shifts.clear();
    for (int r = 0; r < shifts_->rowCount(); ++r) {
        ShiftDefinition s;
        s.name = sstr(shifts_->item(r, SName)->text().trimmed());
        const auto start = parseHhmm(shifts_->item(r, SStart)->text());
        bool ok = false;
        const double hours = QLocale::c().toDouble(shifts_->item(r, SHours)->text().trimmed(), &ok);
        if (!start || *start >= 1440) {
            *error = tr("Shift %1: the start must be HH:MM (00:00–23:59)").arg(r + 1);
            return std::nullopt;
        }
        if (!ok || hours <= 0 || hours > 24) {
            *error = tr("Shift %1: the duration must be more than 0 and at most 24 hours").arg(r + 1);
            return std::nullopt;
        }
        s.startMinute = *start;
        s.durationMinutes = static_cast<int>(std::lround(hours * 60));
        s.weekdays = 0;
        for (int d = 0; d < 7; ++d)
            if (shifts_->item(r, SDay0 + d)->checkState() == Qt::Checked) s.weekdays |= static_cast<std::uint8_t>(1u << d);
        c.shifts.push_back(s);
    }
    c.exceptions.clear();
    for (int r = 0; r < exceptions_->rowCount(); ++r) {
        CalendarException e;
        const QDate d = QDate::fromString(exceptions_->item(r, EDate)->text().trimmed(), Qt::ISODate);
        if (!d.isValid()) {
            *error = tr("Exception %1: the date must be YYYY-MM-DD").arg(r + 1);
            return std::nullopt;
        }
        e.date = fromQDate(d);
        for (const auto& part : exceptions_->item(r, EPeriods)->text().split(',', Qt::SkipEmptyParts)) {
            const auto ends = part.split('-');
            const auto a = ends.size() == 2 ? parseHhmm(ends[0]) : std::nullopt;
            const auto b = ends.size() == 2 ? parseHhmm(ends[1]) : std::nullopt;
            if (!a || !b || *b <= *a) {
                *error = tr("Exception %1: periods must be HH:MM-HH:MM within the day ('%2')").arg(r + 1).arg(part.trimmed());
                return std::nullopt;
            }
            e.periods.push_back({*a, *b});
        }
        e.reason = sstr(exceptions_->item(r, EReason)->text());
        c.exceptions.push_back(e);
    }
    return c;
}

void CalendarDialog::updateSummary() {
    QString error;
    const auto c = edited(&error);
    if (!c) {
        summary_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::colors().ganttCritical.name(), error.toHtmlEscaped()));
        return;
    }
    const scheduling::CalendarTime ct({&*c});
    const double weekHours = c->shifts.empty() ? 0.0 : ct.weeklyMinutes() / 60.0;
    // The next 14 days from today, as worked hours.
    QStringList days;
    const QDate today = QDate::currentDate();
    for (int i = 0; i < 14; ++i) {
        const QDate d = today.addDays(i);
        const double start = static_cast<double>(DateTime::fromDate(fromQDate(d)).minutes());
        const double h = c->shifts.empty() ? 0.0 : ct.workBetween(start, start + 1440) / 60.0;
        days << QStringLiteral("%1 %2h").arg(d.toString("ddd d")).arg(h, 0, 'g', 3);
    }
    summary_->setText(tr("<b>%1 working hours per week</b> (%2 shift(s), %3 exception(s)). Next 14 days: %4")
                          .arg(weekHours, 0, 'f', 1)
                          .arg(c->shifts.size())
                          .arg(c->exceptions.size())
                          .arg(days.join(" · ")));
}

QString CalendarDialog::summary() const { return summary_->text(); }

bool CalendarDialog::apply() {
    QString error;
    auto c = edited(&error);
    if (!c) {
        ctx_.report(error, MessageLevel::Error);
        return false;
    }
    const bool makeDefault = projectDefault_->isChecked();
    const bool ok = ctx_.tryEdit(tr("Edit calendar"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Edit calendar " + c->name);
        ProjectEditor ed(p);
        ed.updateCalendar(*c);
        if (makeDefault != (p.model().info.defaultCalendarId == c->id)) {
            ProjectInfo info = p.model().info;
            info.defaultCalendarId = makeDefault ? c->id : Uuid{};
            ed.setProjectInfo(info, makeDefault ? "Set the project calendar" : "Clear the project calendar");
        }
        macro.commit();
    });
    if (ok) {
        ctx_.report(tr("Calendar '%1' saved").arg(qstr(c->name)));
        select(c->id);
    }
    return ok;
}

Uuid CalendarDialog::createCalendar(const QString& name, CalendarKind kind) {
    WorkCalendar c;
    c.name = sstr(name);
    c.kind = kind;
    c.shifts = {ShiftDefinition{"Day", 6 * 60, 12 * 60, 0x7F}, ShiftDefinition{"Night", 18 * 60, 12 * 60, 0x7F}};
    c.standardHoursPerDay = 24;
    Uuid id;
    ctx_.tryEdit(tr("Add calendar"), [&] { id = ProjectEditor(ctx_.project()).addCalendar(c); });
    return id;
}

// ---- development cycles --------------------------------------------------------------------------------

namespace {
struct CycleField {
    const char* key;
    const char* label;
    double min, max;
    int decimals;
    const char* suffix;
};
const CycleField kCycleFields[] = {
    {"roundLength", "Round length (drilled)", 0.5, 10, 2, " m"},
    {"advanceEfficiency", "Advance efficiency (pulled / drilled)", 0.3, 1.0, 2, ""},
    {"holesPerRound", "Holes per round", 1, 500, 0, ""},
    {"drillPenetrationRate", "Penetration rate per boom", 0.1, 10, 2, " m/min"},
    {"drillBooms", "Drill booms", 1, 4, 0, ""},
    {"drillSetupMinutes", "Drill set-up", 0, 600, 0, " min"},
    {"collarMinutesPerHole", "Collaring per hole", 0, 30, 2, " min"},
    {"chargeMinutesPerHole", "Charging per hole", 0, 30, 2, " min"},
    {"blastAndReentryMinutes", "Blast and re-entry", 0, 600, 0, " min"},
    {"swellFactor", "Swell factor", 1, 2.5, 2, ""},
    {"bucketCapacity", "Bucket capacity (loose)", 0.5, 20, 2, " m³"},
    {"bucketFillFactor", "Bucket fill factor", 0.3, 1.2, 2, ""},
    {"muckCycleMinutes", "Load–haul–dump cycle per bucket", 0.5, 60, 2, " min"},
    {"boltsPerRound", "Bolts per round (without a support class)", 0, 200, 0, ""},
    {"minutesPerBolt", "Minutes per bolt", 0, 60, 2, " min"},
    {"meshMinutes", "Mesh", 0, 600, 0, " min"},
    {"shotcreteMinutes", "Shotcrete", 0, 600, 0, " min"},
    {"scalingMinutes", "Scaling", 0, 600, 0, " min"},
    {"surveyMarkupMinutes", "Survey mark-up", 0, 600, 0, " min"},
    {"serviceMinutes", "Services", 0, 600, 0, " min"},
    {"utilisation", "Utilisation of shift time", 0.1, 1, 2, ""},
    {"shiftHours", "Shift length", 1, 24, 2, " h"},
    {"shiftsPerDay", "Shifts per day", 1, 4, 0, ""},
};

double& cycleValue(DevelopmentCycle& c, const std::string& key, double& scratch) {
#define F(name) \
    if (key == #name) return c.name;
    F(roundLength) F(advanceEfficiency) F(drillPenetrationRate) F(drillSetupMinutes) F(collarMinutesPerHole) F(chargeMinutesPerHole)
    F(blastAndReentryMinutes) F(swellFactor) F(bucketCapacity) F(bucketFillFactor) F(muckCycleMinutes) F(minutesPerBolt) F(meshMinutes)
    F(shotcreteMinutes) F(scalingMinutes) F(surveyMarkupMinutes) F(serviceMinutes) F(utilisation) F(shiftHours)
#undef F
    return scratch;
}
} // namespace

CycleDialog::CycleDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Development cycles"));
    resize(1020, 720);
    auto* lay = new QHBoxLayout(this);
    auto* left = new QVBoxLayout;
    list_ = new QListWidget(this);
    list_->setMaximumWidth(220);
    left->addWidget(list_, 1);
    auto* add = button("add", tr("New cycle"), this);
    auto* del = button("delete", tr("Delete"), this);
    auto* use = button("link", tr("Use for selected activities"), this);
    left->addWidget(add);
    left->addWidget(del);
    left->addWidget(use);
    lay->addLayout(left);

    auto* mid = new QWidget(this);
    auto* form = new QFormLayout(mid);
    name_ = new QLineEdit(mid);
    form->addRow(tr("Name"), name_);
    for (const auto& f : kCycleFields) {
        auto* s = spin(f.min, f.max, f.min, f.decimals, f.suffix, mid);
        fields_.emplace_back(f.key, s);
        form->addRow(tr(f.label), s);
        connect(s, &QDoubleSpinBox::valueChanged, this, &CycleDialog::evaluate);
    }
    lay->addWidget(mid, 1);

    auto* right = new QVBoxLayout;
    auto* pg = new QGroupBox(tr("Evaluate for a heading"), this);
    auto* pf = new QFormLayout(pg);
    width_ = spin(1, 20, 5, 2, " m", pg);
    height_ = spin(1, 20, 5, 2, " m", pg);
    support_ = new QComboBox(pg);
    fillEntityCombo(support_, ctx_.project().model(), EntityKind::SupportClass, {}, true);
    pf->addRow(tr("Width"), width_);
    pf->addRow(tr("Height"), height_);
    pf->addRow(tr("Ground support"), support_);
    right->addWidget(pg);
    breakdown_ = new QTableWidget(0, 2, this);
    breakdown_->setHorizontalHeaderLabels({tr("Cycle step"), tr("Minutes")});
    breakdown_->horizontalHeader()->setStretchLastSection(true);
    breakdown_->verticalHeader()->hide();
    right->addWidget(breakdown_, 1);
    result_ = new QLabel(this);
    result_->setWordWrap(true);
    right->addWidget(result_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    right->addWidget(bb);
    lay->addLayout(right, 1);

    connect(width_, &QDoubleSpinBox::valueChanged, this, &CycleDialog::evaluate);
    connect(height_, &QDoubleSpinBox::valueChanged, this, &CycleDialog::evaluate);
    connect(support_, &QComboBox::currentIndexChanged, this, &CycleDialog::evaluate);
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int r) {
        if (r >= 0) load(uuidFrom(list_->item(r)->data(Qt::UserRole).toString()));
    });
    connect(add, &QPushButton::clicked, this, [this] { select(createCycle(tr("Cycle %1").arg(ctx_.project().model().cycles.size() + 1))); });
    connect(del, &QPushButton::clicked, this, [this] {
        if (current_.isNil()) return;
        const Uuid id = current_;
        if (ctx_.tryEdit(tr("Delete cycle"), [&] { ProjectEditor(ctx_.project()).remove<DevelopmentCycle>(id); })) current_ = {};
        rebuildList();
    });
    connect(use, &QPushButton::clicked, this, [this] { useForActivities(ctx_.selected(EntityKind::Activity)); });

    // Defaults from the selected heading.
    const auto devs = ctx_.selected(EntityKind::Development);
    if (!devs.empty())
        if (const auto* d = ctx_.project().model().developments.find(devs.back())) {
            width_->setValue(d->designWidth);
            height_->setValue(d->designHeight);
            const int i = support_->findData(uuidText(d->supportClassId));
            if (i >= 0) support_->setCurrentIndex(i);
        }
    rebuildList();
    if (!ctx_.project().model().cycles.empty()) select(ctx_.project().model().cycles.all().front().id);
}

void CycleDialog::rebuildList() {
    list_->blockSignals(true);
    list_->clear();
    for (const auto& c : ctx_.project().model().cycles.all()) {
        auto* it = new QListWidgetItem(toolIcon("calc"), qstr(c.name), list_);
        it->setData(Qt::UserRole, uuidText(c.id));
        if (c.id == current_) list_->setCurrentItem(it);
    }
    list_->blockSignals(false);
}

void CycleDialog::select(const Uuid& id) {
    current_ = id;
    rebuildList();
    load(id);
}

void CycleDialog::load(const Uuid& id) {
    const auto* c = ctx_.project().model().cycles.find(id);
    current_ = c ? id : Uuid{};
    if (!c) return;
    DevelopmentCycle copy = *c;
    name_->setText(qstr(c->name));
    for (auto& [key, s] : fields_) {
        s->blockSignals(true);
        double scratch = 0;
        if (key == "holesPerRound") s->setValue(c->holesPerRound);
        else if (key == "drillBooms") s->setValue(c->drillBooms);
        else if (key == "boltsPerRound") s->setValue(c->boltsPerRound);
        else if (key == "shiftsPerDay") s->setValue(c->shiftsPerDay);
        else s->setValue(cycleValue(copy, key, scratch));
        s->blockSignals(false);
    }
    evaluate();
}

DevelopmentCycle CycleDialog::edited() const {
    DevelopmentCycle c;
    if (const auto* cur = ctx_.project().model().cycles.find(current_)) c = *cur;
    c.name = sstr(name_->text().trimmed());
    for (const auto& [key, s] : fields_) {
        double scratch = 0;
        if (key == "holesPerRound") c.holesPerRound = static_cast<int>(std::lround(s->value()));
        else if (key == "drillBooms") c.drillBooms = static_cast<int>(std::lround(s->value()));
        else if (key == "boltsPerRound") c.boltsPerRound = static_cast<int>(std::lround(s->value()));
        else if (key == "shiftsPerDay") c.shiftsPerDay = static_cast<int>(std::lround(s->value()));
        else cycleValue(c, key, scratch) = s->value();
    }
    return c;
}

void CycleDialog::evaluate() {
    breakdown_->setRowCount(0);
    if (current_.isNil()) {
        result_->setText(tr("Create or select a cycle."));
        return;
    }
    try {
        const auto c = edited();
        const auto* sc = ctx_.project().model().supportClasses.find(comboUuid(support_));
        const auto b = scheduling::evaluateCycle(c, width_->value(), height_->value(), sc);
        for (const auto& [label, minutes] : b.items()) {
            const int r = breakdown_->rowCount();
            breakdown_->insertRow(r);
            breakdown_->setItem(r, 0, cell(qstr(label), false));
            breakdown_->setItem(r, 1, cell(QString::number(minutes, 'f', 1), false));
        }
        result_->setText(tr("<b>Cycle %1 min</b> · advance %2 m per cycle · %3 bucket loads · %4 bolts<br>"
                            "%5 cycles per shift · <b>%6 m per shift · %7 m per day</b>")
                             .arg(b.totalMinutes, 0, 'f', 0)
                             .arg(b.advancePerCycle, 0, 'f', 2)
                             .arg(b.bucketLoads)
                             .arg(b.bolts)
                             .arg(b.cyclesPerShift, 0, 'f', 2)
                             .arg(b.metresPerShift, 0, 'f', 2)
                             .arg(b.metresPerDay, 0, 'f', 2));
    } catch (const std::exception& e) {
        result_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::colors().ganttCritical.name(), qstr(e.what()).toHtmlEscaped()));
    }
}

QString CycleDialog::breakdownText() const { return result_->text(); }

bool CycleDialog::apply() {
    if (current_.isNil()) return false;
    const auto c = edited();
    const bool ok = ctx_.tryEdit(tr("Edit cycle"), [&] { ProjectEditor(ctx_.project()).update(c, "Edit development cycle " + c.name); });
    if (ok) rebuildList();
    return ok;
}

Uuid CycleDialog::createCycle(const QString& name) {
    DevelopmentCycle c;
    c.name = sstr(name);
    Uuid id;
    ctx_.tryEdit(tr("Add cycle"), [&] { id = ProjectEditor(ctx_.project()).add(c, "Add development cycle " + c.name); });
    return id;
}

int CycleDialog::useForActivities(const std::vector<Uuid>& ids) {
    if (current_.isNil() || ids.empty()) {
        ctx_.report(tr("Select a cycle here and the activities in the scheduler"), MessageLevel::Warning);
        return 0;
    }
    int n = 0;
    ctx_.tryEdit(tr("Use cycle"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Use development cycle");
        ProjectEditor ed(p);
        for (const auto& id : ids)
            if (const auto* a = p.model().activities.find(id); a && a->kind == ActivityKind::Task) {
                ScheduleActivity u = *a;
                u.productionModelId = current_;
                u.useCycleRate = true;
                u.durationFromRate = true;
                ed.updateActivity(u, "Rate from development cycle");
                ++n;
            }
        macro.commit();
    });
    ctx_.report(tr("%1 activit%2 now take their rate from the cycle").arg(n).arg(n == 1 ? "y" : "ies"));
    return n;
}

// ---- production targets -------------------------------------------------------------------------------

namespace {
enum TCol { TName, TFrom, TTo, TQuantity, TUnit, TZone, TLevel, TNCols };
} // namespace

TargetsDialog::TargetsDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Production targets"));
    resize(860, 520);
    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(tr("Targets are compared with scheduled and actual quantities per period (Scheduler ▸ Production)."), this));
    table_ = new QTableWidget(0, TNCols, this);
    table_->setHorizontalHeaderLabels({tr("Name"), tr("From"), tr("To (inclusive)"), tr("Quantity"), tr("Unit"), tr("Zone (empty = all)"), tr("Level (empty = all)")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    lay->addWidget(table_, 1);
    auto* row = new QHBoxLayout;
    auto* add = button("add", tr("Add target"), this);
    auto* monthly = button("calendar", tr("Monthly targets…"), this);
    auto* del = button("delete", tr("Delete"), this);
    row->addWidget(add);
    row->addWidget(monthly);
    row->addWidget(del);
    row->addStretch(1);
    lay->addLayout(row);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(add, &QPushButton::clicked, this, [this] {
        const QDate s = QDate::currentDate();
        addTarget(tr("Target %1").arg(ctx_.project().model().targets.size() + 1), QDate(s.year(), s.month(), 1),
                  QDate(s.year(), s.month(), 1).addMonths(1).addDays(-1), 100);
    });
    connect(monthly, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const double q = QInputDialog::getDouble(this, tr("Monthly targets"), tr("Metres per month for the next 12 months:"), 120, 0, 1e7, 1, &ok);
        if (!ok) return;
        const QDate s = QDate::currentDate();
        const QDate from(s.year(), s.month(), 1);
        addMonthly(from, from.addMonths(12).addDays(-1), q);
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const int r = table_->currentRow();
        if (r < 0) return;
        const Uuid id = uuidFrom(table_->item(r, TName)->data(Qt::UserRole).toString());
        ctx_.tryEdit(tr("Delete target"), [&] { ProjectEditor(ctx_.project()).remove<ProductionTarget>(id); });
    });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (!building_) commit(it->row());
    });
    connect(&ctx_, &AppContext::modelChanged, this, &TargetsDialog::rebuild);
    rebuild();
}

void TargetsDialog::rebuild() {
    if (!ctx_.hasProject()) return;
    building_ = true;
    table_->setRowCount(0);
    for (const auto& t : ctx_.project().model().targets.all()) {
        const int r = table_->rowCount();
        table_->insertRow(r);
        auto* name = cell(qstr(t.name));
        name->setData(Qt::UserRole, uuidText(t.id));
        table_->setItem(r, TName, name);
        table_->setItem(r, TFrom, cell(qstr(t.periodStart.toString())));
        table_->setItem(r, TTo, cell(qstr(t.periodEnd.toString())));
        table_->setItem(r, TQuantity, cell(QString::number(t.quantity, 'g', 10)));
        table_->setItem(r, TUnit, cell(qstr(t.unit)));
        table_->setItem(r, TZone, cell(qstr(t.scopeZone)));
        table_->setItem(r, TLevel, cell(qstr(t.scopeLevel)));
    }
    table_->resizeColumnsToContents();
    building_ = false;
}

void TargetsDialog::commit(int r) {
    const Uuid id = uuidFrom(table_->item(r, TName)->data(Qt::UserRole).toString());
    const auto* cur = ctx_.project().model().targets.find(id);
    if (!cur) return;
    ProductionTarget t = *cur;
    t.name = sstr(table_->item(r, TName)->text().trimmed());
    const auto from = Date::parse(sstr(table_->item(r, TFrom)->text().trimmed()));
    const auto to = Date::parse(sstr(table_->item(r, TTo)->text().trimmed()));
    bool ok = false;
    const double q = QLocale::c().toDouble(table_->item(r, TQuantity)->text().trimmed(), &ok);
    if (!from || !to || !ok) {
        ctx_.report(tr("Dates must be YYYY-MM-DD and the quantity a number"), MessageLevel::Error);
        QMetaObject::invokeMethod(this, &TargetsDialog::rebuild, Qt::QueuedConnection);
        return;
    }
    t.periodStart = *from;
    t.periodEnd = *to;
    t.quantity = q;
    t.unit = sstr(table_->item(r, TUnit)->text().trimmed());
    t.scopeZone = sstr(table_->item(r, TZone)->text().trimmed());
    t.scopeLevel = sstr(table_->item(r, TLevel)->text().trimmed());
    if (!ctx_.tryEdit(tr("Edit target"), [&] { ProjectEditor(ctx_.project()).update(t, "Edit production target " + t.name); }))
        QMetaObject::invokeMethod(this, &TargetsDialog::rebuild, Qt::QueuedConnection);
}

Uuid TargetsDialog::addTarget(const QString& name, const QDate& from, const QDate& to, double quantity, const QString& unit) {
    ProductionTarget t;
    t.name = sstr(name);
    t.periodStart = fromQDate(from);
    t.periodEnd = fromQDate(to);
    t.quantity = quantity;
    t.unit = sstr(unit);
    Uuid id;
    ctx_.tryEdit(tr("Add target"), [&] { id = ProjectEditor(ctx_.project()).add(t, "Add production target " + t.name); });
    return id;
}

int TargetsDialog::addMonthly(const QDate& from, const QDate& to, double quantity, const QString& zone) {
    int n = 0;
    ctx_.tryEdit(tr("Monthly targets"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Add monthly production targets");
        ProjectEditor ed(p);
        for (QDate m(from.year(), from.month(), 1); m <= to; m = m.addMonths(1)) {
            ProductionTarget t;
            t.name = sstr(m.toString("yyyy-MM")) + (zone.isEmpty() ? std::string() : " " + sstr(zone));
            t.periodStart = fromQDate(std::max(m, from));
            t.periodEnd = fromQDate(std::min(m.addMonths(1).addDays(-1), to));
            t.quantity = quantity;
            t.scopeZone = sstr(zone);
            ed.add(t);
            ++n;
        }
        macro.commit();
    });
    return n;
}

} // namespace openmine::app
