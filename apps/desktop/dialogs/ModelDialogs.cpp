// SPDX-License-Identifier: Apache-2.0
#include "ModelDialogs.h"

#include "DialogUtil.h"
#include "../app/EntityInfo.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polygon2D.h"
#include "openmine/geometry/Profile.h"
#include "openmine/project/ProjectEditor.h"

#ifdef OPENMINE_APP_HAVE_OCCT
#include "openmine/occt/Solid.h"
#include "openmine/occt/SolidObjects.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <cmath>
#include <numbers>

namespace openmine::app {

namespace {

bool isClosedCurve(const GeometryObject& g) {
    return g.type == GeometryType::Circle || ((g.type == GeometryType::Polyline || g.type == GeometryType::Spline) && g.closed);
}
bool isOpenCurve(const GeometryObject& g) { return geometry::isCurve(g.type) && !isClosedCurve(g) && g.type != GeometryType::Point; }

/// Newell normal and in-plane basis of a closed polyline.
struct PlaneBasis {
    Vec3 origin, n, u, v;
};
PlaneBasis basisOf(const std::vector<Vec3>& ring) {
    Vec3 n;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Vec3 a = ring[i] - ring.front(), b = ring[(i + 1) % ring.size()] - ring.front();
        n += Vec3{(a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)};
    }
    PlaneBasis pb;
    pb.origin = ring.front();
    pb.n = n.length() > 0 ? n.normalized() : Vec3{0, 0, 1};
    pb.u = std::abs(pb.n.z) < 0.9 ? cross(Vec3{0, 0, 1}, pb.n).normalized() : cross(pb.n, Vec3{1, 0, 0}).normalized();
    pb.v = cross(pb.n, pb.u).normalized();
    return pb;
}
geometry::Ring toRing(const std::vector<Vec3>& pts, const PlaneBasis& pb) {
    geometry::Ring r;
    for (const auto& p : pts) r.push_back({dot(p - pb.origin, pb.u), dot(p - pb.origin, pb.v)});
    return r;
}

} // namespace

bool SolidModelDialog::available() {
#ifdef OPENMINE_APP_HAVE_OCCT
    return true;
#else
    return false;
#endif
}

QString SolidModelDialog::title(Operation op) {
    switch (op) {
    case Operation::Extrude: return tr("Extrude");
    case Operation::Revolve: return tr("Revolve");
    case Operation::Sweep: return tr("Sweep profile along path");
    case Operation::Loft: return tr("Loft through sections");
    }
    return {};
}

SolidModelDialog::SolidModelDialog(AppContext& ctx, Operation op, QWidget* parent) : QDialog(parent), ctx_(ctx), op_(op) {
    setWindowTitle(title(op));
    inputs_ = ctx_.selectedGeometry();
    const auto& m = ctx_.project().model();
    auto* form = new QFormLayout(this);
    info_ = new QLabel(this);
    info_->setWordWrap(true);
    form->addRow(info_);
    int closed = 0, open = 0;
    for (const auto& id : inputs_)
        if (const auto* g = m.geometry.find(id)) {
            closed += isClosedCurve(*g) ? 1 : 0;
            open += isOpenCurve(*g) ? 1 : 0;
        }
    switch (op) {
    case Operation::Extrude:
        info_->setText(tr("%1 closed curve(s) selected. Each outer curve becomes a solid; closed curves inside it can become holes.").arg(closed));
        mode_ = new QComboBox(this);
        mode_->addItem(tr("Vertical, by height"), 0);
        mode_->addItem(tr("Along the curve's plane normal, by distance"), 1);
        mode_->addItem(tr("Along a vector"), 2);
        a_ = spin(-1e5, 1e5, 5.0, 3, " m", this);
        b_ = spin(-1e5, 1e5, 0, 3, {}, this);
        c_ = spin(-1e5, 1e5, 0, 3, {}, this);
        d_ = spin(-1e5, 1e5, 5.0, 3, {}, this);
        flag1_ = new QCheckBox(tr("Symmetric (half each side of the curve)"), this);
        flag2_ = new QCheckBox(tr("Closed curves inside an outer curve are holes"), this);
        flag2_->setChecked(true);
        form->addRow(tr("Direction"), mode_);
        form->addRow(tr("Height / distance"), a_);
        form->addRow(tr("Vector dx"), b_);
        form->addRow(tr("Vector dy"), c_);
        form->addRow(tr("Vector dz"), d_);
        form->addRow(QString(), flag1_);
        form->addRow(QString(), flag2_);
        break;
    case Operation::Revolve: {
        info_->setText(tr("%1 closed curve(s) selected. A selected line is used as the axis; otherwise the axis below.").arg(closed));
        Vec3 start;
        for (const auto& id : inputs_)
            if (const auto* g = m.geometry.find(id); g && isClosedCurve(*g))
                if (auto s = geometry::curveStart(*g)) start = *s;
        a_ = spin(-1e8, 1e8, start.x, 3, {}, this);
        b_ = spin(-1e8, 1e8, start.y, 3, {}, this);
        c_ = spin(-1e8, 1e8, start.z, 3, {}, this);
        d_ = spin(-1, 1, 0, 4, {}, this);
        e_ = spin(-1, 1, 0, 4, {}, this);
        f_ = spin(-1, 1, 1, 4, {}, this);
        g_ = spin(0.1, 360, 360, 2, " °", this);
        form->addRow(tr("Axis point X"), a_);
        form->addRow(tr("Axis point Y"), b_);
        form->addRow(tr("Axis point Z"), c_);
        form->addRow(tr("Axis direction X"), d_);
        form->addRow(tr("Axis direction Y"), e_);
        form->addRow(tr("Axis direction Z"), f_);
        form->addRow(tr("Angle"), g_);
        break;
    }
    case Operation::Sweep:
        info_->setText(tr("%1 path curve(s) selected. The profile stays upright along the path (mine-development convention).").arg(open + closed));
        profile_ = new QComboBox(this);
        profile_->addItem(tr("Rectangle (width × height below)"), QString());
        for (const auto& p : m.profiles.all()) profile_->addItem(tr("Profile: %1").arg(qstr(p.name)), uuidText(p.id));
        a_ = spin(0.1, 100, 5.0, 3, " m", this);
        b_ = spin(0.1, 100, 5.0, 3, " m", this);
        mode_ = new QComboBox(this);
        mode_->addItem(tr("Right corners (mitred)"), static_cast<int>(1));
        mode_->addItem(tr("Round corners"), static_cast<int>(2));
        mode_->addItem(tr("Transformed"), static_cast<int>(0));
        form->addRow(tr("Profile"), profile_);
        form->addRow(tr("Width"), a_);
        form->addRow(tr("Height"), b_);
        form->addRow(tr("Corners"), mode_);
        break;
    case Operation::Loft:
        info_->setText(tr("%1 closed section(s) selected — lofted in selection order.").arg(closed));
        flag1_ = new QCheckBox(tr("Solid (otherwise a shell)"), this);
        flag1_->setChecked(true);
        flag2_ = new QCheckBox(tr("Ruled (straight between sections)"), this);
        form->addRow(QString(), flag1_);
        form->addRow(QString(), flag2_);
        break;
    }
    name_ = new QLineEdit(title(op), this);
    layer_ = new QComboBox(this);
    fillEntityCombo(layer_, m, EntityKind::Layer, ctx_.currentLayer());
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Layer"), layer_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    if (!available()) {
        bb->button(QDialogButtonBox::Ok)->setEnabled(false);
        form->addRow(new QLabel(tr("<i>Not available: solid modelling needs Open CASCADE, which this build does not include.</i>"), this));
    }
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (!create().empty()) accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(bb);
}

void SolidModelDialog::setExtrusion(Vec3 v, bool symmetric, bool holes) {
    mode_->setCurrentIndex(2);
    b_->setValue(v.x);
    c_->setValue(v.y);
    d_->setValue(v.z);
    flag1_->setChecked(symmetric);
    flag2_->setChecked(holes);
}
void SolidModelDialog::setRevolution(Vec3 p, Vec3 dir, double angleDeg) {
    a_->setValue(p.x);
    b_->setValue(p.y);
    c_->setValue(p.z);
    d_->setValue(dir.x);
    e_->setValue(dir.y);
    f_->setValue(dir.z);
    g_->setValue(angleDeg);
}
void SolidModelDialog::setRevolutionAngle(double angleDeg) { g_->setValue(angleDeg); }
void SolidModelDialog::setSweepRectangle(double w, double h) {
    profile_->setCurrentIndex(0);
    a_->setValue(w);
    b_->setValue(h);
}
bool SolidModelDialog::setSweepProfile(const Uuid& id) {
    const int i = profile_ ? profile_->findData(uuidText(id)) : -1;
    if (i < 0) return false;
    profile_->setCurrentIndex(i);
    return true;
}
void SolidModelDialog::setLoft(bool solid, bool ruled) {
    flag1_->setChecked(solid);
    flag2_->setChecked(ruled);
}

std::vector<Uuid> SolidModelDialog::create() {
    std::vector<Uuid> out;
#ifdef OPENMINE_APP_HAVE_OCCT
    auto& p = ctx_.project();
    const auto& m = p.model();
    std::vector<const GeometryObject*> closed, open, lines;
    for (const auto& id : inputs_)
        if (const auto* g = m.geometry.find(id)) {
            if (isClosedCurve(*g)) closed.push_back(g);
            else if (isOpenCurve(*g)) open.push_back(g);
            if (g->type == GeometryType::Line) lines.push_back(g);
        }
    const Uuid layer = comboUuid(layer_);
    const std::string name = sstr(name_->text().trimmed());
    QString what = title(op_);
    ctx_.tryEdit(what, [&] {
        CommandStack::MacroScope macro(p.commands(), sstr(what));
        switch (op_) {
        case Operation::Extrude: {
            if (closed.empty()) throw ValidationError("Select closed curves to extrude (closed polylines, circles or closed splines)");
            // Outer curves by decreasing area; a curve inside an outer one is its hole.
            struct Loop {
                const GeometryObject* g;
                PlaneBasis pb;
                geometry::Ring ring;
                double area;
            };
            std::vector<Loop> loops;
            for (const auto* g : closed) {
                auto pts = geometry::curvePolyline(*g, 0.001);
                if (pts.size() > 1 && distance(pts.front(), pts.back()) < 1e-9) pts.pop_back();
                const auto pb = basisOf(pts);
                auto ring = toRing(pts, pb);
                loops.push_back({g, pb, ring, geometry::area(ring)});
            }
            std::sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return a.area > b.area; });
            std::vector<char> used(loops.size(), 0);
            for (std::size_t i = 0; i < loops.size(); ++i) {
                if (used[i]) continue;
                used[i] = 1;
                std::vector<const GeometryObject*> holes;
                if (flag2_->isChecked())
                    for (std::size_t j = i + 1; j < loops.size(); ++j) {
                        if (used[j]) continue;
                        const auto other = toRing(geometry::curvePolyline(*loops[j].g, 0.001), loops[i].pb);
                        bool inside = std::abs(dot(loops[j].pb.n, loops[i].pb.n)) > 0.999;
                        for (const auto& q : other) inside = inside && geometry::contains(loops[i].ring, q);
                        if (inside) {
                            holes.push_back(loops[j].g);
                            used[j] = 1;
                        }
                    }
                Vec3 v;
                switch (mode_->currentIndex()) {
                case 0: v = {0, 0, a_->value()}; break;
                case 1: v = loops[i].pb.n * a_->value(); break;
                default: v = {b_->value(), c_->value(), d_->value()}; break;
                }
                if (v.length() < 1e-9) throw ValidationError("The extrusion distance must not be zero");
                auto face = occt::planarFace(*loops[i].g, holes);
                if (flag1_->isChecked()) face = occt::transformed(face, Transform3::translation(v * -0.5));
                const auto solid = occt::extrude(face, v);
                out.push_back(occt::addSolidObject(p, solid, name.empty() ? loops[i].g->name + " extrusion" : name, layer, {}, "Extrude"));
            }
            break;
        }
        case Operation::Revolve: {
            if (closed.empty()) throw ValidationError("Select a closed curve (the profile) to revolve");
            Vec3 axisP{a_->value(), b_->value(), c_->value()}, axisD{d_->value(), e_->value(), f_->value()};
            if (!lines.empty()) {
                axisP = lines.front()->vertices[0];
                axisD = lines.front()->vertices[1] - lines.front()->vertices[0];
            }
            if (axisD.length() < 1e-12) throw ValidationError("The axis direction must not be zero");
            for (const auto* g : closed) {
                const auto solid = occt::revolve(occt::planarFace(*g), axisP, axisD.normalized(), g_->value() * std::numbers::pi / 180.0);
                out.push_back(occt::addSolidObject(p, solid, name.empty() ? g->name + " revolution" : name, layer, {}, "Revolve"));
            }
            break;
        }
        case Operation::Sweep: {
            std::vector<const GeometryObject*> paths = open;
            if (paths.empty()) paths = closed;
            if (paths.empty()) throw ValidationError("Select the path curve(s) to sweep along");
            geometry::ProfileCurve pc;
            if (const auto* prof = m.profiles.find(uuidFrom(profile_->currentData().toString()))) pc = geometry::profileCurve(*prof);
            else {
                pc.points = geometry::rectangleOutline(a_->value(), b_->value());
                pc.bulges.assign(pc.points.size(), 0.0);
            }
            const auto transition = static_cast<occt::SweepTransition>(mode_->currentData().toInt());
            for (const auto* path : paths) {
                const auto solid = occt::sweepProfile(pc.points, pc.bulges, *path, transition);
                out.push_back(occt::addSolidObject(p, solid, name.empty() ? path->name + " sweep" : name, layer, {}, "Sweep"));
            }
            break;
        }
        case Operation::Loft: {
            if (closed.size() < 2) throw ValidationError("Select two or more closed sections, in order");
            std::vector<std::vector<Vec3>> sections;
            for (const auto* g : closed) {
                auto pts = geometry::curvePolyline(*g, 0.001);
                if (pts.size() > 1 && distance(pts.front(), pts.back()) < 1e-9) pts.pop_back();
                sections.push_back(std::move(pts));
            }
            const auto solid = occt::loft(sections, flag1_->isChecked(), flag2_->isChecked());
            out.push_back(occt::addSolidObject(p, solid, name.empty() ? std::string("Loft") : name, layer, {}, "Loft"));
            break;
        }
        }
        macro.commit();
    });
    if (!out.empty()) {
        double volume = 0;
        for (const auto& id : out)
            if (const auto* g = m.geometry.find(id); g && g->mesh) volume += std::abs(geometry::meshSignedVolume(*g->mesh));
        ctx_.report(tr("%1: %2 solid(s), %3 m³").arg(title(op_)).arg(out.size()).arg(formatNumber(volume, 2)));
        std::vector<SelectionItem> sel;
        for (const auto& id : out) sel.push_back({EntityKind::Geometry, id});
        ctx_.selectMany(sel);
    }
#else
    ctx_.report(tr("Solid modelling needs Open CASCADE, which this build does not include"), MessageLevel::Error);
#endif
    return out;
}

QString solidReport(const Project& p, const Uuid& id) {
    const auto* g = p.model().geometry.find(id);
    if (!g) return QObject::tr("The object no longer exists");
#ifdef OPENMINE_APP_HAVE_OCCT
    if (g->type == GeometryType::Solid) {
        const auto info = occt::analyse(occt::loadSolid(p, *g));
        QStringList l;
        l << QObject::tr("%1: %2, %3").arg(qstr(g->name), info.valid ? QObject::tr("valid") : QObject::tr("NOT VALID"), info.closed ? QObject::tr("closed") : QObject::tr("open"));
        l << QObject::tr("Volume %1 m³ · area %2 m²").arg(formatNumber(info.volume, 3), formatNumber(info.area, 3));
        l << QObject::tr("%1 solid(s), %2 shell(s), %3 faces, %4 edges, %5 vertices").arg(info.solids).arg(info.shells).arg(info.faces).arg(info.edges).arg(info.vertices);
        l << QObject::tr("Centre of mass %1, %2, %3").arg(info.centreOfMass.x, 0, 'f', 3).arg(info.centreOfMass.y, 0, 'f', 3).arg(info.centreOfMass.z, 0, 'f', 3);
        for (const auto& pr : info.problems) l << "  " + qstr(pr);
        return l.join('\n');
    }
#endif
    if (g->mesh) {
        const auto topo = geometry::topology(*g->mesh);
        return QObject::tr("%1: mesh, %2 triangles, %3 · area %4 m²%5")
            .arg(qstr(g->name))
            .arg(g->mesh->triangles.size())
            .arg(topo.closed() ? QObject::tr("closed") : QObject::tr("open"))
            .arg(formatNumber(geometry::meshArea(*g->mesh), 3))
            .arg(topo.closed() ? QObject::tr(" · volume %1 m³").arg(formatNumber(std::abs(geometry::meshSignedVolume(*g->mesh)), 3)) : QString());
    }
    return QObject::tr("%1 is not a solid or mesh").arg(qstr(g->name));
}

bool healSolid(AppContext& ctx, const Uuid& id) {
#ifdef OPENMINE_APP_HAVE_OCCT
    std::vector<std::string> fixes;
    return ctx.tryEdit(QObject::tr("Heal solid"), [&] {
        auto& p = ctx.project();
        const auto* g = p.model().geometry.find(id);
        if (!g) throw ValidationError("The object no longer exists");
        const auto before = occt::analyse(occt::loadSolid(p, *g));
        const auto healed = occt::unify(occt::fix(occt::loadSolid(p, *g), &fixes));
        const auto after = occt::replaceSolidShape(p, id, healed, "Heal solid " + g->name);
        ctx.report(QObject::tr("%1: %2 → %3; %4 fix(es)%5")
                       .arg(qstr(p.model().geometry.find(id)->name), before.valid ? QObject::tr("valid") : QObject::tr("invalid"),
                            after.valid ? QObject::tr("valid") : QObject::tr("still invalid"))
                       .arg(fixes.size())
                       .arg(fixes.empty() ? QString() : ": " + qstr(fixes.front())));
    });
#else
    (void)id;
    ctx.report(QObject::tr("Solid healing needs Open CASCADE, which this build does not include"), MessageLevel::Error);
    return false;
#endif
}

std::vector<Uuid> sectionObjects(AppContext& ctx, const std::vector<Uuid>& ids, Vec3 point, Vec3 normal) {
    std::vector<Uuid> out;
    if (normal.length() < 1e-12) {
        ctx.report(QObject::tr("The section plane has no normal"), MessageLevel::Error);
        return out;
    }
    ctx.tryEdit(QObject::tr("Section"), [&] {
        auto& p = ctx.project();
        CommandStack::MacroScope macro(p.commands(), "Section objects");
        const Uuid layer = workflowLayer(ctx, "Sections", 0xFF7043);
        ProjectEditor ed(p);
        for (const auto& id : ids) {
            const auto* g = p.model().geometry.find(id);
            if (!g) continue;
#ifdef OPENMINE_APP_HAVE_OCCT
            if (g->type == GeometryType::Solid) {
                for (const auto& s : occt::sectionObject(p, id, point, normal.normalized(), layer)) out.push_back(s);
                continue;
            }
#endif
            if (g->mesh && (g->type == GeometryType::Mesh || g->type == GeometryType::Solid)) {
                const std::string base = g->name;
                for (auto& loop : geometry::planeSection(*g->mesh, point, normal.normalized())) {
                    if (loop.size() < 2) continue;
                    GeometryObject c;
                    c.type = GeometryType::Polyline;
                    c.closed = loop.size() > 3 && distance(loop.front(), loop.back()) < 1e-9;
                    if (c.closed) loop.pop_back();
                    c.vertices = std::move(loop);
                    c.name = base + " section";
                    c.layerId = layer;
                    out.push_back(ed.addGeometry(std::move(c)));
                }
            }
        }
        if (out.empty()) throw ValidationError("The plane does not cut the selected solids or meshes");
        macro.commit();
    });
    if (!out.empty()) ctx.report(QObject::tr("%1 section curve(s) on layer 'Sections'").arg(out.size()));
    return out;
}

} // namespace openmine::app
