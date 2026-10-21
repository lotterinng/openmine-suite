// SPDX-License-Identifier: Apache-2.0
#include "MeshDialogs.h"

#include "DialogUtil.h"

#include "openmine/geometry/MeshOps.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/Meshing.h"
#include "openmine/survey/SurfaceTools.h"
#include "openmine/survey/Processing.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

namespace openmine::app {

namespace {
bool isSurface(const GeometryObject& g) { return g.mesh && !g.mesh->triangles.empty() && (g.type == GeometryType::Mesh || g.type == GeometryType::Solid); }
} // namespace

QString MeshToolDialog::title(Operation op) {
    switch (op) {
    case Operation::Decimate: return tr("Decimate surface");
    case Operation::Smooth: return tr("Smooth surface");
    case Operation::Repair: return tr("Repair surface");
    case Operation::Compare: return tr("Compare with a reference surface");
    }
    return {};
}

MeshToolDialog::MeshToolDialog(AppContext& ctx, Operation op, const Uuid& meshId, QWidget* parent) : QDialog(parent), ctx_(ctx), op_(op) {
    setWindowTitle(title(op));
    const auto& m = ctx_.project().model();
    auto* form = new QFormLayout(this);
    mesh_ = new QComboBox(this);
    fillEntityCombo(mesh_, m, EntityKind::Geometry, meshId, false, [&m](const Uuid& id) {
        const auto* g = m.geometry.find(id);
        return g && isSurface(*g);
    });
    form->addRow(tr("Surface"), mesh_);
    info_ = new QLabel(this);
    info_->setWordWrap(true);
    auto describe = [this] {
        const auto* g = ctx_.project().model().geometry.find(comboUuid(mesh_));
        if (!g || !g->mesh) {
            info_->setText(tr("Select a surface."));
            return;
        }
        const auto topo = geometry::topology(*g->mesh);
        info_->setText(tr("%L1 vertices, %L2 triangles, %3, %4 non-manifold edges, area %5 m²")
                           .arg(static_cast<qulonglong>(g->mesh->vertices.size()))
                           .arg(static_cast<qulonglong>(g->mesh->triangles.size()))
                           .arg(topo.closed() ? tr("closed") : tr("%1 boundary edges").arg(topo.boundaryEdges))
                           .arg(topo.nonManifoldEdges)
                           .arg(formatNumber(geometry::meshArea(*g->mesh), 2)));
    };
    connect(mesh_, &QComboBox::currentIndexChanged, this, describe);
    form->addRow(info_);
    const auto* g = m.geometry.find(comboUuid(mesh_));
    const std::size_t tris = g && g->mesh ? g->mesh->triangles.size() : 0;
    switch (op) {
    case Operation::Decimate:
        count_ = new QSpinBox(this);
        count_->setRange(0, 2000000000);
        count_->setValue(static_cast<int>(tris / 4));
        value_ = spin(0, 1000, 0.05, 3, " m", this);
        value_->setToolTip(tr("Stop before a collapse would move the surface more than this (0 = no limit)"));
        flag_ = new QCheckBox(tr("Preserve the boundary"), this);
        flag_->setChecked(true);
        form->addRow(tr("Target triangles (0 = error limit only)"), count_);
        form->addRow(tr("Maximum error"), value_);
        form->addRow(QString(), flag_);
        break;
    case Operation::Smooth:
        count_ = new QSpinBox(this);
        count_->setRange(1, 500);
        count_->setValue(10);
        value_ = spin(0.01, 0.99, 0.5, 2, {}, this);
        value2_ = spin(-0.99, -0.01, -0.53, 2, {}, this);
        flag_ = new QCheckBox(tr("Keep boundary vertices fixed"), this);
        flag_->setChecked(true);
        form->addRow(tr("Iterations"), count_);
        form->addRow(tr("λ (shrink)"), value_);
        form->addRow(tr("μ (inflate)"), value2_);
        form->addRow(QString(), flag_);
        break;
    case Operation::Repair:
        value_ = spin(0, 1, 1e-6, 6, " m", this);
        count_ = new QSpinBox(this);
        count_->setRange(0, 100000);
        count_->setValue(0);
        count_->setToolTip(tr("Fill boundary loops with at most this many edges (0 = do not fill)"));
        flag_ = new QCheckBox(tr("Orient a closed surface outward"), this);
        flag_->setChecked(true);
        form->addRow(tr("Merge vertices closer than"), value_);
        form->addRow(tr("Fill holes up to (edges)"), count_);
        form->addRow(QString(), flag_);
        break;
    case Operation::Compare:
        reference_ = new QComboBox(this);
        fillEntityCombo(reference_, m, EntityKind::Geometry, {}, false, [&m, meshId](const Uuid& id) {
            const auto* r = m.geometry.find(id);
            return id != meshId && r && isSurface(*r);
        });
        value_ = spin(0, 1e5, 0, 3, " m", this);
        value_->setToolTip(tr("Vertices farther than this get no value (0 = unlimited)"));
        flag_ = new QCheckBox(tr("Signed (by the reference surface's orientation)"), this);
        flag_->setChecked(true);
        form->addRow(tr("Reference surface"), reference_);
        form->addRow(tr("Max distance"), value_);
        form->addRow(QString(), flag_);
        break;
    }
    const auto* src = m.geometry.find(comboUuid(mesh_));
    name_ = new QLineEdit((src ? qstr(src->name) : QString()) +
                              (op == Operation::Decimate ? tr(" (decimated)") : op == Operation::Smooth ? tr(" (smoothed)") : op == Operation::Repair ? tr(" (repaired)") : tr(" (distance)")),
                          this);
    form->addRow(tr("New surface"), name_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Run"));
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (start() > 0) accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(bb);
    describe();
}

void MeshToolDialog::setDecimation(std::size_t target, double maxError) {
    count_->setValue(static_cast<int>(target));
    value_->setValue(maxError);
}
void MeshToolDialog::setSmoothing(int iterations) { count_->setValue(iterations); }
void MeshToolDialog::setRepair(std::size_t fill, bool outward) {
    count_->setValue(static_cast<int>(fill));
    flag_->setChecked(outward);
}
void MeshToolDialog::setReference(const Uuid& ref, double maxDistance) {
    reference_->setCurrentIndex(std::max(0, reference_->findData(uuidText(ref))));
    value_->setValue(maxDistance);
}

int MeshToolDialog::start() {
    const auto& m = ctx_.project().model();
    const auto* g = m.geometry.find(comboUuid(mesh_));
    if (!g || !isSurface(*g)) {
        ctx_.report(tr("Select a surface"), MessageLevel::Error);
        return 0;
    }
    const auto mesh = g->mesh;
    const Uuid sourceId = g->id;
    const std::string name = sstr(name_->text().trimmed());
    MeshPtr reference;
    if (op_ == Operation::Compare) {
        const auto* r = m.geometry.find(comboUuid(reference_));
        if (!r || !isSurface(*r)) {
            ctx_.report(tr("Choose the reference surface"), MessageLevel::Error);
            return 0;
        }
        reference = r->mesh;
    }
    survey::SurfaceToolOptions o;
    const int count = count_ ? count_->value() : 0;
    const double v1 = value_ ? value_->value() : 0, v2 = value2_ ? value2_->value() : 0;
    const bool flag = flag_ && flag_->isChecked();
    survey::SurfaceOperation sop = survey::SurfaceOperation::Decimate;
    switch (op_) {
    case Operation::Decimate:
        o.targetTriangles = static_cast<std::size_t>(count);
        o.maxError = v1;
        o.preserveBoundary = flag;
        break;
    case Operation::Smooth:
        sop = survey::SurfaceOperation::Smooth;
        o.iterations = count;
        o.lambda = v1;
        o.mu = v2;
        o.fixBoundary = flag;
        break;
    case Operation::Repair:
        sop = survey::SurfaceOperation::Repair;
        o.mergeTolerance = v1;
        o.fillHolesUpTo = static_cast<std::size_t>(count);
        o.orientOutward = flag;
        break;
    case Operation::Compare:
        sop = survey::SurfaceOperation::Compare;
        o.signedDistance = flag;
        o.maxDistance = v1;
        break;
    }
    AppContext* c = &ctx_;
    return ctx_.jobs().start(title(op_), tr("Surface"), [=](jobs::Progress& pr) -> UiApply {
        auto result = std::make_shared<survey::SurfaceToolResult>(survey::runSurfaceTool(*mesh, sop, o, reference.get(), pr));
        return [c, result, name, sourceId] {
            Uuid id;
            if (c->tryEdit(QObject::tr("Add surface"), [&] { id = survey::addDerivedSurface(c->project(), sourceId, *result, name); })) {
                c->report(QObject::tr("'%1': %2").arg(qstr(c->project().model().geometry.find(id)->name), qstr(result->summary)));
                c->select(EntityKind::Geometry, id);
            }
        };
    });
}

} // namespace openmine::app
