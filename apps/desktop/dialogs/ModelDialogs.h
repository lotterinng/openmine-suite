#pragma once
// SPDX-License-Identifier: Apache-2.0
// 3D solid modelling on Open CASCADE: extrusion of closed curves (with holes), revolution, sweeps
// of tunnel profiles along any path, lofts through closed sections, solid checks and healing, and
// sections of solids and meshes by a plane. Results are exact B-rep Solid objects (immutable
// BREP assets), each operation one undo step.

#include "../app/AppContext.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

namespace openmine::app {

class SolidModelDialog : public QDialog {
    Q_OBJECT
public:
    enum class Operation { Extrude, Revolve, Sweep, Loft };
    SolidModelDialog(AppContext& ctx, Operation op, QWidget* parent = nullptr);
    static bool available();
    static QString title(Operation op);
    /// Creates the solid(s) from the current selection and settings. Returns the new object ids
    /// (empty with a reported reason when the input does not fit the operation).
    std::vector<Uuid> create();

    // Programmatic settings (the dialog's fields).
    void setExtrusion(Vec3 vector, bool symmetric, bool holes);
    void setRevolution(Vec3 axisPoint, Vec3 axisDirection, double angleDeg);
    /// Keeps the default axis (vertical through the profile's start, or a selected line).
    void setRevolutionAngle(double angleDeg);
    void setSweepRectangle(double width, double height);
    bool setSweepProfile(const Uuid& tunnelProfileId);
    void setLoft(bool solid, bool ruled);

private:
    AppContext& ctx_;
    Operation op_;
    std::vector<Uuid> inputs_; ///< selected geometry, in selection order
    QLabel* info_ = nullptr;
    QLineEdit* name_ = nullptr;
    QComboBox* layer_ = nullptr;
    QComboBox* mode_ = nullptr;
    QDoubleSpinBox *a_ = nullptr, *b_ = nullptr, *c_ = nullptr, *d_ = nullptr, *e_ = nullptr, *f_ = nullptr, *g_ = nullptr;
    QCheckBox* flag1_ = nullptr;
    QCheckBox* flag2_ = nullptr;
    QComboBox* profile_ = nullptr;
};

/// Validity, closure, volume, area and topology of a Solid object, as text (several lines).
QString solidReport(const Project& project, const Uuid& solidId);
/// Heals a solid (ShapeFix, orientation, unify) in place; reports what was fixed.
bool healSolid(AppContext& ctx, const Uuid& solidId);
/// Sections solids and meshes by a plane; the section curves are added on a "Sections" layer.
/// Returns the new curve ids.
std::vector<Uuid> sectionObjects(AppContext& ctx, const std::vector<Uuid>& objectIds, Vec3 planePoint, Vec3 planeNormal);

} // namespace openmine::app
