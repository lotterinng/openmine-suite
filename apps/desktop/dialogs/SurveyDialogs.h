#pragma once
// SPDX-License-Identifier: Apache-2.0
// Survey workflows: point-cloud import and processing (subsampling, outliers, normals, ground,
// cropping, surfaces, distances), registration by point pairs / control and by ICP, as-built
// models from scans, design conformance, face advance reconciliation, control-point import and
// surface volumes. Heavy work runs as background jobs on a snapshot; results are committed as
// derived datasets, registrations, meshes and analysis results — original survey data is never
// modified.

#include "../app/AppContext.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace openmine::app {

class ToolController;

class PointCloudImportDialog : public QDialog {
    Q_OBJECT
public:
    PointCloudImportDialog(AppContext& ctx, QStringList files, QWidget* parent = nullptr);
    /// Starts one import job per file. Returns the job ids.
    std::vector<int> start();

private:
    AppContext& ctx_;
    QListWidget* files_;
    QLineEdit* name_;
    QLineEdit* crs_;
    QCheckBox* hasDate_;
    QDateEdit* date_;
    QLineEdit* scanner_;
    QComboBox* development_;
    QSpinBox* nodePoints_;
    QSpinBox* grid_;
};

class CloudProcessDialog : public QDialog {
    Q_OBJECT
public:
    enum class Operation { Subsample, Outliers, Normals, Ground, Delaunay, Poisson, Distance, Statistics, Planes };
    CloudProcessDialog(AppContext& ctx, Operation op, const Uuid& datasetId, QWidget* parent = nullptr);
    int start();
    static QString title(Operation op);

private:
    AppContext& ctx_;
    Operation op_;
    Uuid dataset_;
    QComboBox* mode_ = nullptr;
    QDoubleSpinBox* a_ = nullptr;
    QDoubleSpinBox* b_ = nullptr;
    QSpinBox* k_ = nullptr;
    QComboBox* reference_ = nullptr;
    QCheckBox* flag_ = nullptr;
    QDoubleSpinBox* c_ = nullptr;
    QDoubleSpinBox* d_ = nullptr;
    QLineEdit* name_ = nullptr;
    QComboBox* engine_ = nullptr; ///< "openmine" | "open3d" | "pdal"
};

/// Starts the crop job for a polygon drawn in a view (points inside the prism through the
/// polygon along the view direction are kept as a derived dataset).
int startCropJob(AppContext& ctx, const Uuid& datasetId, const std::vector<Vec3>& polygon, Vec3 viewNormal, bool keepInside = true);

class RegistrationDialog : public QDialog {
    Q_OBJECT
public:
    RegistrationDialog(AppContext& ctx, ToolController& tools, const Uuid& datasetId, QWidget* parent = nullptr);
    /// Adds a pair programmatically (stored-coordinate source, target or control point).
    void addPair(const QString& name, Vec3 source, std::optional<Vec3> target, const Uuid& controlId = {}, bool check = false);
    bool compute();
    bool commit(bool apply);
    const Registration* result() const { return result_ ? &*result_ : nullptr; }

private:
    void refreshResiduals();
    AppContext& ctx_;
    ToolController& tools_;
    QComboBox* dataset_;
    QTableWidget* pairs_;
    QCheckBox* scale_;
    QLabel* summary_;
    std::optional<Registration> result_;
};

class IcpDialog : public QDialog {
    Q_OBJECT
public:
    IcpDialog(AppContext& ctx, const Uuid& movingId, QWidget* parent = nullptr);
    int start(bool apply);

private:
    AppContext& ctx_;
    QComboBox* moving_;
    QComboBox* target_;
    QComboBox* mode_;
    QDoubleSpinBox* maxCorr_;
    QSpinBox* iterations_;
    QDoubleSpinBox* trim_;
    QSpinBox* sample_;
    QComboBox* engine_;
    QPlainTextEdit* log_;
};

/// Creates an as-built mesh of a development from a registered scan (corridor selection, normals
/// oriented towards the centreline, Poisson reconstruction) and links it to the heading.
class AsBuiltDialog : public QDialog {
    Q_OBJECT
public:
    AsBuiltDialog(AppContext& ctx, const Uuid& datasetId, QWidget* parent = nullptr);
    int start();

private:
    AppContext& ctx_;
    QComboBox* dataset_;
    QComboBox* development_;
    QDoubleSpinBox* corridor_;
    QSpinBox* depth_;
    QDoubleSpinBox* voxel_;
    QComboBox* engine_;
};

class ConformanceDialog : public QDialog {
    Q_OBJECT
public:
    ConformanceDialog(AppContext& ctx, const Uuid& asBuiltId, QWidget* parent = nullptr);
    int start();

private:
    AppContext& ctx_;
    QComboBox* asBuilt_;
    QComboBox* development_;
    QDoubleSpinBox* interval_;
    QCheckBox* range_;
    QDoubleSpinBox* from_;
    QDoubleSpinBox* to_;
    QComboBox* orientation_;
    QCheckBox* envelope_;
    QDoubleSpinBox* envW_;
    QDoubleSpinBox* envH_;
    QCheckBox* report_;
};

class FaceAdvanceDialog : public QDialog {
    Q_OBJECT
public:
    FaceAdvanceDialog(AppContext& ctx, const Uuid& developmentId, QWidget* parent = nullptr);
    int start();

private:
    AppContext& ctx_;
    QComboBox* development_;
    QComboBox* dataset_;
    QComboBox* activity_;
    QDateEdit* date_;
    QDoubleSpinBox* corridor_;
    QLabel* result_;
};

class ControlImportDialog : public QDialog {
    Q_OBJECT
public:
    ControlImportDialog(AppContext& ctx, const QString& file, QWidget* parent = nullptr);
    bool apply();

private:
    void preview();
    AppContext& ctx_;
    QString file_;
    QComboBox* cols_[5];
    QComboBox* role_;
    QDoubleSpinBox* hSigma_;
    QDoubleSpinBox* vSigma_;
    QLineEdit* source_;
    QLabel* info_;
};

class VolumeDialog : public QDialog {
    Q_OBJECT
public:
    VolumeDialog(AppContext& ctx, const Uuid& surfaceId, QWidget* parent = nullptr);
    int start();

private:
    AppContext& ctx_;
    QComboBox* mode_;
    QComboBox* surface_;
    QComboBox* other_;
    QDoubleSpinBox* elevation_;
    QDoubleSpinBox* cell_;
    QLabel* result_;
};

} // namespace openmine::app
