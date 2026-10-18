#pragma once
// SPDX-License-Identifier: Apache-2.0
// Geology and geotechnical workflows: drillhole import (collar, survey and interval tables with
// validation), compositing and statistics, block-model creation, domain coding against solids,
// grade estimation (NN, IDW, ordinary and simple kriging) with experimental variogram
// calculation, model fitting and cross-validation, block-model display settings, structural
// mapping import and rock-mass classification (Q-system, RMR89).

#include "../app/AppContext.h"

#include "openmine/geology/Estimation.h"

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
class QTabWidget;
class QDateEdit;

namespace openmine::app {

class SceneCache;

class DrillholeImportDialog : public QDialog {
    Q_OBJECT
public:
    explicit DrillholeImportDialog(AppContext& ctx, QWidget* parent = nullptr);
    void setFiles(const QString& collars, const QString& surveys, const QStringList& intervals);
    /// Reads and validates the tables (no change to the project); returns the error count.
    int validate();
    bool importNow();

private:
    AppContext& ctx_;
    QLineEdit* collars_;
    QLineEdit* surveys_;
    QListWidget* intervals_;
    QComboBox* below_;
    QCheckBox* replace_;
    QCheckBox* traces_;
    QDoubleSpinBox* defaultDip_;
    QPlainTextEdit* report_;
};

class CompositeDialog : public QDialog {
    Q_OBJECT
public:
    CompositeDialog(AppContext& ctx, QWidget* parent = nullptr);
    int start();

private:
    AppContext& ctx_;
    QDoubleSpinBox* length_;
    QComboBox* residual_;
    QComboBox* breakField_;
    QDoubleSpinBox* coverage_;
    QLineEdit* fields_;
    QLineEdit* csv_;
};

class BlockModelDialog : public QDialog {
    Q_OBJECT
public:
    explicit BlockModelDialog(AppContext& ctx, QWidget* parent = nullptr);
    Uuid create();

private:
    void fit(bool drillholes);
    AppContext& ctx_;
    QLineEdit* name_;
    QDoubleSpinBox *ox_, *oy_, *oz_, *dx_, *dy_, *dz_, *rot_, *pad_;
    QSpinBox *nx_, *ny_, *nz_;
};

class BlockCodeDialog : public QDialog {
    Q_OBJECT
public:
    BlockCodeDialog(AppContext& ctx, const Uuid& blockModelId, QWidget* parent = nullptr);
    int start();

private:
    AppContext& ctx_;
    Uuid model_;
    QComboBox* solid_;
    QComboBox* mode_;
    QLineEdit* field_;
    QDoubleSpinBox* value_;
    QSpinBox* sub_;
};

class EstimationDialog : public QDialog {
    Q_OBJECT
public:
    EstimationDialog(AppContext& ctx, const Uuid& blockModelId, QWidget* parent = nullptr);
    geology::EstimationOptions options() const;
    int start();
    /// Experimental variogram of the current samples along the search major axis, fitted.
    bool computeVariogram();
    bool crossValidate();

private:
    geology::SampleSet samples() const;
    AppContext& ctx_;
    QComboBox* model_;
    QComboBox* field_;
    QDoubleSpinBox* compLength_;
    QComboBox* codeField_;
    QLineEdit* code_;
    QLineEdit* mask_;
    QComboBox* method_;
    QDoubleSpinBox* power_;
    QDoubleSpinBox *major_, *semi_, *minor_, *az_, *dip_, *tilt_;
    QSpinBox *minS_, *maxS_, *perHole_;
    QDoubleSpinBox* nugget_;
    QComboBox* type1_;
    QDoubleSpinBox *sill1_, *range1_;
    QCheckBox* second_;
    QComboBox* type2_;
    QDoubleSpinBox *sill2_, *range2_;
    QDoubleSpinBox *lag_;
    QSpinBox* lags_;
    QSpinBox* discret_;
    QLineEdit* output_;
    QPlainTextEdit* log_;
    QWidget* chart_;
    std::vector<geology::LagPoint> experimental_;
};

class BlockDisplayDialog : public QDialog {
    Q_OBJECT
public:
    BlockDisplayDialog(AppContext& ctx, SceneCache& scene, const Uuid& blockModelId, QWidget* parent = nullptr);
    void apply();

private:
    AppContext& ctx_;
    SceneCache& scene_;
    Uuid model_;
    QComboBox* attribute_;
    QCheckBox *hasMin_, *hasMax_, *outline_;
    QDoubleSpinBox *min_, *max_, *opacity_;
    QComboBox* lut_;
};

class StructureImportDialog : public QDialog {
    Q_OBJECT
public:
    StructureImportDialog(AppContext& ctx, const QString& file, QWidget* parent = nullptr);
    bool apply();

private:
    AppContext& ctx_;
    QString file_;
    QComboBox* cols_[8];
    QComboBox* development_;
    QLabel* info_;
};

class ClassificationDialog : public QDialog {
    Q_OBJECT
public:
    ClassificationDialog(AppContext& ctx, const Uuid& developmentId, QWidget* parent = nullptr);
    Uuid record();

private:
    void recompute();
    AppContext& ctx_;
    QTabWidget* tabs_;
    QComboBox* development_;
    QDoubleSpinBox* chainage_;
    QLineEdit* mappedBy_;
    QDateEdit* date_;
    QLineEdit* notes_;
    // Q
    QDoubleSpinBox* rqd_;
    QComboBox *jn_, *jr_, *ja_, *jw_, *srf_;
    QDoubleSpinBox *span_, *esr_;
    // RMR
    QDoubleSpinBox *ucs_, *rmrRqd_, *spacing_;
    QComboBox *condition_, *water_, *orientation_;
    QLabel* result_;
};

} // namespace openmine::app
