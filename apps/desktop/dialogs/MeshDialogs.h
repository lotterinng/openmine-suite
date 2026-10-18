#pragma once
// SPDX-License-Identifier: Apache-2.0
// Surface (mesh) tools: quadric decimation, Taubin smoothing, repair (merge, degenerate and
// duplicate removal, consistent and outward orientation, hole filling) and surface-to-surface
// comparison (signed distance of every vertex to a reference surface, with statistics). Results
// are new mesh objects (the input is never altered), computed as background jobs.

#include "../app/AppContext.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace openmine::app {

class MeshToolDialog : public QDialog {
    Q_OBJECT
public:
    enum class Operation { Decimate, Smooth, Repair, Compare };
    MeshToolDialog(AppContext& ctx, Operation op, const Uuid& meshId, QWidget* parent = nullptr);
    static QString title(Operation op);
    /// Starts the job; returns its id (0 when the input is invalid).
    int start();
    void setDecimation(std::size_t targetTriangles, double maxError);
    void setSmoothing(int iterations);
    void setRepair(std::size_t fillHolesUpTo, bool outward);
    void setReference(const Uuid& referenceMeshId, double maxDistance);

private:
    AppContext& ctx_;
    Operation op_;
    QComboBox* mesh_;
    QComboBox* reference_ = nullptr;
    QSpinBox* count_ = nullptr;
    QDoubleSpinBox* value_ = nullptr;
    QDoubleSpinBox* value2_ = nullptr;
    QCheckBox* flag_ = nullptr;
    QLineEdit* name_;
    QLabel* info_;
};

} // namespace openmine::app
