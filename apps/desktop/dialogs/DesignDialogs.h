#pragma once
// SPDX-License-Identifier: Apache-2.0
// Mine-design workflows: centreline layouts (switchback and spiral declines, ramps, horizontal
// alignments through PIs, ore drives along a contact, access drives, crosscuts) with a live
// preview, headings from existing centrelines, stopes from shapes or outlines and their
// subdivision into mining blocks, levels, linked schedule activities, design revisions
// (capture, compare, approve, restore) and gradient / design-rule checks.

#include "../app/AppContext.h"

#include "openmine/mine_design/Development.h"
#include "openmine/mine_design/Layout.h"

#include <QDialog>

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QPlainTextEdit;

namespace openmine::app {

class ToolController;

/// Fields of a new development (heading id or prefix, level, profile, material, support, …).
class HeadingTemplate : public QWidget {
    Q_OBJECT
public:
    HeadingTemplate(AppContext& ctx, const QString& activityType, bool prefix, QWidget* parent = nullptr);
    mine_design::NewDevelopment value() const;
    QString idOrPrefix() const;

private:
    AppContext& ctx_;
    QLineEdit* id_;
    QLineEdit* name_;
    QComboBox* level_;
    QComboBox* decline_;
    QComboBox* profile_;
    QComboBox* material_;
    QComboBox* support_;
    QLineEdit* zone_;
    QComboBox* type_;
    QDoubleSpinBox* width_;
    QDoubleSpinBox* height_;
    QDoubleSpinBox* chainage_;
};

/// Generates centreline layouts and creates headings from them.
class LayoutDialog : public QDialog {
    Q_OBJECT
public:
    enum class Kind { Decline, Ramp, Alignment, OreDrive, AccessDrive };
    using Preview = std::function<void(const std::vector<std::vector<Vec3>>& lines)>;
    LayoutDialog(AppContext& ctx, ToolController& tools, Kind kind, Preview preview, QWidget* parent = nullptr);
    /// The layout for the current parameters (throws ValidationError with the reason).
    std::vector<mine_design::LayoutResult> layouts() const;
    std::vector<Uuid> create();

private:
    void updatePreview();
    AppContext& ctx_;
    ToolController& tools_;
    Kind kind_;
    Preview preview_;
    QDoubleSpinBox *sx_, *sy_, *sz_;
    QDoubleSpinBox *tx_ = nullptr, *ty_ = nullptr, *tz_ = nullptr;
    QDoubleSpinBox* azimuth_ = nullptr;
    QDoubleSpinBox* grade_ = nullptr;
    QDoubleSpinBox* straight_ = nullptr;
    QDoubleSpinBox* radius_ = nullptr;
    QDoubleSpinBox* targetZ_ = nullptr;
    QDoubleSpinBox* offset_ = nullptr;
    QComboBox* turn_ = nullptr;
    QComboBox* style_ = nullptr;
    QComboBox* source_ = nullptr; ///< alignment PIs / ore contact polyline
    QCheckBox* useZ_ = nullptr;
    QLabel* info_ = nullptr;
    HeadingTemplate* templ_ = nullptr;
};

class CrosscutDialog : public QDialog {
    Q_OBJECT
public:
    CrosscutDialog(AppContext& ctx, const Uuid& developmentId, QWidget* parent = nullptr);
    std::vector<Uuid> create();

private:
    AppContext& ctx_;
    Uuid dev_;
    QLineEdit* chainages_;
    QDoubleSpinBox *interval_, *first_, *length_, *angle_, *grade_, *offset_;
    QComboBox* side_;
};

class HeadingDialog : public QDialog {
    Q_OBJECT
public:
    HeadingDialog(AppContext& ctx, const Uuid& geometryId, QWidget* parent = nullptr);
    Uuid create();

private:
    AppContext& ctx_;
    Uuid geometry_;
    HeadingTemplate* templ_;
    QCheckBox* activity_;
    QDoubleSpinBox* rate_;
};

class StopeDialog : public QDialog {
    Q_OBJECT
public:
    /// From a closed mesh/solid, or (closed polyline) from an outline between two elevations.
    StopeDialog(AppContext& ctx, const Uuid& geometryId, QWidget* parent = nullptr);
    Uuid create();

private:
    AppContext& ctx_;
    Uuid geometry_;
    bool outline_ = false;
    QLineEdit* name_;
    QLineEdit* code_;
    QComboBox* level_;
    QComboBox* material_;
    QComboBox* method_;
    QLineEdit* zone_;
    QDoubleSpinBox *grade_, *dilution_, *recovery_, *floor_, *back_;
    QLineEdit* unit_;
};

class SubdivideDialog : public QDialog {
    Q_OBJECT
public:
    SubdivideDialog(AppContext& ctx, const Uuid& stopeId, QWidget* parent = nullptr);
    std::vector<Uuid> create();

private:
    AppContext& ctx_;
    Uuid stope_;
    QComboBox* outline_;
    QDoubleSpinBox *floor_, *back_, *strike_, *panel_, *lift_;
};

class LevelDialog : public QDialog {
    Q_OBJECT
public:
    LevelDialog(AppContext& ctx, QWidget* parent = nullptr);
    Uuid create();

private:
    AppContext& ctx_;
    QLineEdit *name_, *code_, *zone_;
    QDoubleSpinBox* elevation_;
    QCheckBox* assign_;
    QDoubleSpinBox* tolerance_;
};

class RevisionDialog : public QDialog {
    Q_OBJECT
public:
    RevisionDialog(AppContext& ctx, const Uuid& entityId, QWidget* parent = nullptr);
    Uuid capture();

private:
    AppContext& ctx_;
    Uuid entity_;
    QLineEdit* code_;
    QPlainTextEdit* comment_;
};

/// Read-only analysis of a centreline: gradient profile and design-rule violations.
class GradientDialog : public QDialog {
    Q_OBJECT
public:
    GradientDialog(AppContext& ctx, const Uuid& geometryId, QWidget* parent = nullptr);

private:
    void recompute();
    AppContext& ctx_;
    Uuid geometry_;
    QDoubleSpinBox *maxGrade_, *maxCurveGrade_, *minRadius_, *maxChange_;
    QTableWidget* table_;
    QLabel* summary_;
};

/// Text summary of a revision comparison (for a message box).
QString revisionComparisonText(const ProjectModel& m, const Uuid& fromId, const std::optional<Uuid>& toId, const Project& p);

} // namespace openmine::app
