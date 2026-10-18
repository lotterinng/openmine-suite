#pragma once
// SPDX-License-Identifier: Apache-2.0
// Dialogs of the scheduler: dependencies, production actuals, scenarios (overrides, levelling,
// optimiser objective) and schedule optimisation (OR-Tools CP-SAT) with its result review.

#include "../app/AppContext.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;
class QCheckBox;
class QDateEdit;

namespace openmine::app {

/// Creates a dependency between two activities (validated: no cycles, no duplicates).
class DependencyDialog : public QDialog {
    Q_OBJECT
public:
    DependencyDialog(AppContext& ctx, Uuid predecessor, Uuid successor, QWidget* parent = nullptr);
    /// Applies the dialog's values (also used without showing the dialog).
    bool apply();

private:
    AppContext& ctx_;
    QComboBox* pred_;
    QComboBox* succ_;
    QComboBox* type_;
    QDoubleSpinBox* lag_;
};

/// Records a dated production actual on an activity (optionally with the surveyed face).
class ActualDialog : public QDialog {
    Q_OBJECT
public:
    ActualDialog(AppContext& ctx, Uuid activity, QWidget* parent = nullptr);
    bool apply();

private:
    AppContext& ctx_;
    Uuid activity_;
    QDateEdit* date_;
    QDoubleSpinBox* qty_;
    QLineEdit* unit_;
    QComboBox* source_;
    QDoubleSpinBox* face_;
    QCheckBox* hasFace_;
    QLineEdit* note_;
};

/// Creates or edits a scenario: name, description, levelling, optimiser objective and limits,
/// and the overrides it applies to activities and resources.
class ScenarioDialog : public QDialog {
    Q_OBJECT
public:
    ScenarioDialog(AppContext& ctx, Uuid scenario, QWidget* parent = nullptr);
    bool apply();
    Uuid scenarioId() const { return id_; }

private:
    void addOverrideRow(const ScenarioOverride& o);
    AppContext& ctx_;
    Uuid id_;
    QLineEdit* name_;
    QPlainTextEdit* description_;
    QComboBox* levelling_;
    QComboBox* objective_;
    QDoubleSpinBox* timeLimit_;
    QTableWidget* overrides_;
};

/// Runs the CP-SAT optimiser on the active scenario in the background and offers to store the
/// optimised sequence as a new scenario (CPM then recomputes exact dates and floats).
class OptimiserDialog : public QDialog {
    Q_OBJECT
public:
    OptimiserDialog(AppContext& ctx, QWidget* parent = nullptr);
    static bool available();
    /// Starts the optimisation job; the dialog shows the result when it finishes.
    void start();

private:
    AppContext& ctx_;
    QComboBox* objective_;
    QDoubleSpinBox* slot_;
    QDoubleSpinBox* limit_;
    QSpinBox* workers_;
    QCheckBox* hard_;
    QPlainTextEdit* log_;
    QLineEdit* name_;
    std::shared_ptr<void> result_; ///< optimisation::OptimisationResult when available
    class QPushButton* store_ = nullptr;
};

} // namespace openmine::app
