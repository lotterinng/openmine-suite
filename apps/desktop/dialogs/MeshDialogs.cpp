// SPDX-License-Identifier: Apache-2.0
#include "MeshDialogs.h"

#include "DialogUtil.h"

#include "openmine/geometry/MeshOps.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/Meshing.h"
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
    const Uuid sourceId = g->id, layer = g->layerId;
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
    const Operation op = op_;
    const int count = count_ ? count_->value() : 0;
    const double v1 = value_ ? value_->value() : 0, v2 = value2_ ? value2_->value() : 0;
    const bool flag = flag_ && flag_->isChecked();
    AppContext* c = &ctx_;
    return ctx_.jobs().start(title(op), tr("Surface"), [=](jobs::Progress& pr) -> UiApply {
        auto out = std::make_shared<MeshData>();
        QString summary;
        std::string method;
        switch (op) {
        case Operation::Decimate: {
            survey::DecimateOptions o;
            o.targetTriangles = static_cast<std::size_t>(count);
            o.maxError = v1 > 0 ? v1 : 1e300;
            o.preserveBoundary = flag;
            if (o.targetTriangles == 0 && v1 <= 0) throw ValidationError("Give a target triangle count or a maximum error");
            *out = survey::decimate(*mesh, o, pr);
            summary = QObject::tr("%L1 → %L2 triangles").arg(static_cast<qulonglong>(mesh->triangles.size())).arg(static_cast<qulonglong>(out->triangles.size()));
            method = "Quadric decimation target=" + std::to_string(count) + " maxError=" + std::to_string(v1);
            break;
        }
        case Operation::Smooth:
            *out = survey::smoothTaubin(*mesh, count, v1, v2, flag);
            summary = QObject::tr("%1 Taubin iterations (λ %2, μ %3)").arg(count).arg(v1).arg(v2);
            method = "Taubin smoothing n=" + std::to_string(count);
            break;
        case Operation::Repair: {
            survey::RepairOptions o;
            o.mergeTolerance = v1;
            o.fillHolesUpTo = static_cast<std::size_t>(count);
            o.orientOutward = flag;
            survey::RepairReport rep;
            *out = survey::repairMesh(*mesh, o, &rep);
            summary = QObject::tr("%1 vertices merged, %2 degenerate and %3 duplicate triangles removed, %4 flipped, %5 holes filled; "
                                  "%6 non-manifold edges and %7 boundary loops remain")
                          .arg(rep.mergedVertices)
                          .arg(rep.degenerateRemoved)
                          .arg(rep.duplicatesRemoved)
                          .arg(rep.flipped)
                          .arg(rep.holesFilled)
                          .arg(rep.nonManifoldEdges)
                          .arg(rep.boundaryLoops);
            method = "Mesh repair";
            break;
        }
        case Operation::Compare: {
            survey::PointBuffer pts;
            pts.xyz = mesh->vertices;
            const auto d = survey::cloudToMesh(pts, *reference, flag, v1 > 0 ? v1 : 1e300, pr);
            *out = *mesh;
            out->vertexScalars = d;
            out->scalarName = "distance";
            std::vector<float> valid;
            for (float x : d)
                if (std::isfinite(x)) valid.push_back(x);
            if (valid.empty()) throw ValidationError("No vertex lies within the maximum distance of the reference");
            std::sort(valid.begin(), valid.end());
            double sum = 0, sq = 0;
            for (float x : valid) {
                sum += x;
                sq += static_cast<double>(x) * x;
            }
            const double mean = sum / static_cast<double>(valid.size());
            summary = QObject::tr("distance to reference: mean %1 m, RMS %2 m, min %3 m, median %4 m, max %5 m (%L6 of %L7 vertices)")
                          .arg(mean, 0, 'f', 4)
                          .arg(std::sqrt(sq / static_cast<double>(valid.size())), 0, 'f', 4)
                          .arg(valid.front(), 0, 'f', 4)
                          .arg(valid[valid.size() / 2], 0, 'f', 4)
                          .arg(valid.back(), 0, 'f', 4)
                          .arg(static_cast<qulonglong>(valid.size()))
                          .arg(static_cast<qulonglong>(d.size()));
            method = "Distance to reference surface";
            break;
        }
        }
        if (out->triangles.empty()) throw ValidationError("The operation produced an empty surface");
        return [c, out, summary, method, name, layer, sourceId] {
            Uuid id;
            if (c->tryEdit(QObject::tr("Add surface"), [&] {
                    auto& p = c->project();
                    CommandStack::MacroScope macro(p.commands(), "Add surface " + name);
                    GeometryObject obj;
                    obj.type = GeometryType::Mesh;
                    obj.name = name;
                    obj.layerId = layer;
                    obj.mesh = out;
                    ensureAttribute(p, "GeometryObject", "method", "Method");
                    ensureAttribute(p, "GeometryObject", "source_object", "Source object");
                    obj.attributes["method"] = method;
                    obj.attributes["source_object"] = sourceId.toString();
                    id = ProjectEditor(p).addGeometry(std::move(obj));
                    macro.commit();
                })) {
                c->report(QObject::tr("'%1': %2").arg(qstr(name), summary));
                c->select(EntityKind::Geometry, id);
            }
        };
    });
}

} // namespace openmine::app
