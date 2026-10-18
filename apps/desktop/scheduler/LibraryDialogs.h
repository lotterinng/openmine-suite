#pragma once
// SPDX-License-Identifier: Apache-2.0
// Scheduling libraries: resources and their assignment to activities, working calendars (shift
// patterns and exceptions; project, shift and resource calendars), development cycles (drill,
// charge, blast, muck, support — the production model that drives activity rates) and
// production targets. Every edit is a validated, undoable model edit; the schedule recalculates
// from the model.

#include "../app/AppContext.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;

namespace openmine::app {

class ResourceLibraryDialog : public QDialog {
    Q_OBJECT
public:
    explicit ResourceLibraryDialog(AppContext& ctx, QWidget* parent = nullptr);
    Uuid addResource(const QString& name, ResourceKind kind, double capacity);
    /// Assigns a resource to an activity (one undo step). Returns the assignment id.
    Uuid assign(const Uuid& activityId, const Uuid& resourceId, double units, bool drivesRate);
    void refresh();

private:
    void rebuildResources();
    void rebuildAssignments();
    void commitResource(int row);
    void commitAssignment(int row);
    AppContext& ctx_;
    QTableWidget* resources_;
    QTableWidget* assignments_;
    QLabel* scope_;
    bool building_ = false;
};

class CalendarDialog : public QDialog {
    Q_OBJECT
public:
    explicit CalendarDialog(AppContext& ctx, const Uuid& calendarId = {}, QWidget* parent = nullptr);
    Uuid createCalendar(const QString& name, CalendarKind kind);
    void select(const Uuid& calendarId);
    /// Writes the edited calendar (one undo step). Returns false when the input is invalid.
    bool apply();
    /// Adds a shift row to the editor (not applied until apply()).
    void addShift(const QString& name, int startMinute, int durationMinutes, std::uint8_t weekdays);
    /// Adds an exception row: `periods` like "06:00-14:00, 18:00-22:00"; empty = no work that day.
    void addException(const QDate& date, const QString& periods, const QString& reason);
    QString summary() const;

private:
    void rebuildList();
    void load(const Uuid& id);
    void updateSummary();
    std::optional<WorkCalendar> edited(QString* error) const;
    AppContext& ctx_;
    Uuid current_;
    QListWidget* list_;
    QLineEdit* name_;
    QComboBox* kind_;
    QDoubleSpinBox* hours_;
    QLineEdit* description_;
    QCheckBox* projectDefault_;
    QTableWidget* shifts_;
    QTableWidget* exceptions_;
    QLabel* summary_;
};

class CycleDialog : public QDialog {
    Q_OBJECT
public:
    explicit CycleDialog(AppContext& ctx, QWidget* parent = nullptr);
    Uuid createCycle(const QString& name);
    void select(const Uuid& cycleId);
    bool apply();
    /// Makes the selected cycle the production model of the activities (rates from the cycle).
    int useForActivities(const std::vector<Uuid>& activityIds);
    QString breakdownText() const;

private:
    void rebuildList();
    void load(const Uuid& id);
    void evaluate();
    DevelopmentCycle edited() const;
    AppContext& ctx_;
    Uuid current_;
    QListWidget* list_;
    QLineEdit* name_;
    std::vector<std::pair<std::string, QDoubleSpinBox*>> fields_;
    QDoubleSpinBox* width_;
    QDoubleSpinBox* height_;
    QComboBox* support_;
    QTableWidget* breakdown_;
    QLabel* result_;
};

class TargetsDialog : public QDialog {
    Q_OBJECT
public:
    explicit TargetsDialog(AppContext& ctx, QWidget* parent = nullptr);
    Uuid addTarget(const QString& name, const QDate& from, const QDate& to, double quantity, const QString& unit = "m");
    /// Adds one target per calendar month between the dates (quantity each).
    int addMonthly(const QDate& from, const QDate& to, double quantity, const QString& zone = {});

private:
    void rebuild();
    void commit(int row);
    AppContext& ctx_;
    QTableWidget* table_;
    bool building_ = false;
};

} // namespace openmine::app
