// SPDX-License-Identifier: Apache-2.0
#include "StereonetPanel.h"

#include "../theme/Theme.h"
#include "../viewport/SceneTypes.h"

#include "openmine/project/ProjectEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>
#include <QScrollArea>
#include <QSplitter>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <numbers>

namespace openmine::app {

using namespace geotechnical;

namespace {
QColor rgb(std::uint32_t c) { return QColor((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }
}

// ---- view --------------------------------------------------------------------------------------------

StereonetView::StereonetView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(260, 260);
}

void StereonetView::setData(Data d) {
    d_ = std::move(d);
    update();
}

QPointF StereonetView::toWidget(NetPoint p) const {
    const double r = std::min(width(), height()) / 2.0 - 24;
    return {width() / 2.0 + p.x * r, height() / 2.0 - p.y * r};
}

void StereonetView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& c = Theme::colors();
    p.fillRect(rect(), c.panel);
    const double r = std::min(width(), height()) / 2.0 - 24;
    const QPointF centre(width() / 2.0, height() / 2.0);
    p.setBrush(c.window);
    p.setPen(QPen(c.text, 1.5));
    p.drawEllipse(centre, r, r);

    // Density contours (filled bands by level, then lines).
    if (d_.density && !d_.levels.empty()) {
        for (std::size_t li = 0; li < d_.levels.size(); ++li) {
            const double t = d_.levels.size() == 1 ? 1.0 : static_cast<double>(li) / (d_.levels.size() - 1);
            QColor col = rgb(lutColor(Lut::Viridis, 0.25 + 0.75 * t));
            for (const auto& line : contour(*d_.density, d_.levels[li])) {
                QPainterPath path;
                for (std::size_t i = 0; i < line.size(); ++i) {
                    const QPointF q = toWidget(line[i]);
                    if (i == 0) path.moveTo(q);
                    else path.lineTo(q);
                }
                p.setPen(QPen(col, 1.6));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            }
        }
    }
    // 10° net.
    p.setPen(QPen(c.grid, 0.6));
    for (int a = 10; a < 90; a += 10) {
        for (double dd : {90.0, 270.0}) {
            const auto gc = greatCircle(Plane{static_cast<double>(a), dd}, 90, d_.projection);
            QPolygonF poly;
            for (const auto& q : gc) poly << toWidget(q);
            p.drawPolyline(poly);
        }
        for (const auto& part : smallCircle(Line{0, 0}, a, 180, d_.projection)) {
            QPolygonF poly;
            for (const auto& q : part) poly << toWidget(q);
            p.drawPolyline(poly);
        }
    }
    p.setPen(QPen(c.textMuted, 1));
    p.drawLine(toWidget({-1, 0}), toWidget({1, 0}));
    p.drawLine(toWidget({0, -1}), toWidget({0, 1}));
    p.setPen(c.text);
    p.drawText(QRectF(centre.x() - 10, centre.y() - r - 20, 20, 16), Qt::AlignCenter, "N");
    p.drawText(QRectF(centre.x() + r + 4, centre.y() - 8, 16, 16), Qt::AlignCenter, "E");
    p.drawText(QRectF(centre.x() - 10, centre.y() + r + 4, 20, 16), Qt::AlignCenter, "S");
    p.drawText(QRectF(centre.x() - r - 20, centre.y() - 8, 16, 16), Qt::AlignCenter, "W");

    // Friction circle (pole plot: poles inside are stable against planar sliding on a flat face).
    if (d_.frictionDeg) {
        p.setPen(QPen(c.warning, 1.4, Qt::DashLine));
        // Poles of planes dipping less than φ plot within φ of the centre.
        for (const auto& part : smallCircle(Line{0, 90}, *d_.frictionDeg, 180, d_.projection)) {
            QPolygonF poly;
            for (const auto& q : part) poly << toWidget(q);
            p.drawPolyline(poly);
        }
    }
    // Face plane.
    if (d_.face) {
        QPolygonF poly;
        for (const auto& q : greatCircle(*d_.face, 120, d_.projection)) poly << toWidget(q);
        p.setPen(QPen(c.accent, 2.5));
        p.drawPolyline(poly);
    }
    // Set mean planes and poles.
    for (std::size_t s = 0; s < d_.sets.size(); ++s) {
        const QColor col = rgb(categoricalColor(s));
        QPolygonF poly;
        for (const auto& q : greatCircle(d_.sets[s].meanPlane, 120, d_.projection)) poly << toWidget(q);
        p.setPen(QPen(col, 2.2));
        p.drawPolyline(poly);
    }
    // Poles.
    std::vector<char> crit(d_.poles.size(), 0);
    for (auto i : d_.critical)
        if (i < crit.size()) crit[i] = 1;
    for (std::size_t i = 0; i < d_.poles.size(); ++i) {
        const auto q = toWidget(project(lineOf(d_.poles[i]), d_.projection));
        const int set = i < d_.assignment.size() ? d_.assignment[i] : -1;
        const QColor col = set >= 0 ? rgb(categoricalColor(static_cast<std::size_t>(set))) : c.textMuted;
        p.setPen(crit[i] ? QPen(QColor(0xD3, 0x2F, 0x2F), 2) : QPen(c.panel, 0.8));
        p.setBrush(col);
        p.drawEllipse(q, 3.6, 3.6);
    }
    // Set mean poles with their 95 % cones.
    for (std::size_t s = 0; s < d_.sets.size(); ++s) {
        const QColor col = rgb(categoricalColor(s));
        const auto& st = d_.sets[s];
        if (std::isfinite(st.stats.alpha95) && st.stats.alpha95 > 0)
            for (const auto& part : smallCircle(st.meanPole, st.stats.alpha95, 90, d_.projection)) {
                QPolygonF poly;
                for (const auto& q : part) poly << toWidget(q);
                p.setPen(QPen(col, 1.2, Qt::DotLine));
                p.setBrush(Qt::NoBrush);
                p.drawPolyline(poly);
            }
        const auto q = toWidget(project(st.meanPole, d_.projection));
        p.setPen(QPen(Qt::black, 1.5));
        p.setBrush(col);
        p.drawRect(QRectF(q.x() - 5, q.y() - 5, 10, 10));
        p.setPen(c.text);
        p.drawText(q + QPointF(8, -6), qstr(st.name));
    }
    p.setPen(c.textMuted);
    p.drawText(QRectF(6, height() - 20, width() - 12, 16), Qt::AlignLeft,
               tr("%1, lower hemisphere — %2 poles").arg(d_.projection == Projection::EqualArea ? tr("Equal area") : tr("Equal angle")).arg(d_.poles.size()));
}

void StereonetView::mouseMoveEvent(QMouseEvent* e) {
    const double r = std::min(width(), height()) / 2.0 - 24;
    const NetPoint q{(e->position().x() - width() / 2.0) / r, (height() / 2.0 - e->position().y()) / r};
    if (q.x * q.x + q.y * q.y > 1.0) {
        emit hovered({});
        return;
    }
    const Line l = unproject(q, d_.projection);
    const Plane pl = planeOfPole(l);
    emit hovered(tr("Line %1/%2 — as a pole: plane %3/%4")
                     .arg(l.plunge, 2, 'f', 0, QChar('0'))
                     .arg(l.trend, 3, 'f', 0, QChar('0'))
                     .arg(pl.dip, 2, 'f', 0, QChar('0'))
                     .arg(pl.dipDirection, 3, 'f', 0, QChar('0')));
}

// ---- panel ---------------------------------------------------------------------------------------------

StereonetPanel::StereonetPanel(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    // Net above, settings below (scrollable): the panel lives in a narrow side dock.
    auto* split = new QSplitter(Qt::Vertical, this);
    lay->addWidget(split);
    auto* left = new QWidget;
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    view_ = new StereonetView(left);
    ll->addWidget(view_, 1);
    hover_ = new QLabel(left);
    ll->addWidget(hover_);
    split->addWidget(left);

    auto* right = new QWidget;
    auto* form = new QFormLayout(right);
    projection_ = new QComboBox;
    projection_->addItem(tr("Equal area (Schmidt)"), static_cast<int>(Projection::EqualArea));
    projection_->addItem(tr("Equal angle (Wulff)"), static_cast<int>(Projection::EqualAngle));
    form->addRow(tr("Net"), projection_);
    density_ = new QComboBox;
    density_->addItem(tr("Schmidt 1 % counting"), static_cast<int>(DensityMethod::Schmidt1Percent));
    density_->addItem(tr("Kamb (σ)"), static_cast<int>(DensityMethod::Kamb));
    density_->addItem(tr("No contours"), -1);
    form->addRow(tr("Density"), density_);
    terzaghi_ = new QCheckBox(tr("Terzaghi weighting (sampling along the face normal)"));
    form->addRow(QString(), terzaghi_);
    sets_ = new QSpinBox;
    sets_->setRange(0, 8);
    sets_->setValue(3);
    sets_->setToolTip(tr("Number of joint sets to identify (0 = none)"));
    form->addRow(tr("Joint sets"), sets_);
    cone_ = new QDoubleSpinBox;
    cone_->setRange(0, 60);
    cone_->setValue(25);
    cone_->setSuffix("°");
    cone_->setToolTip(tr("Poles farther than this from every set mean are outliers (0 = keep all)"));
    form->addRow(tr("Set cone"), cone_);
    friction_ = new QDoubleSpinBox;
    friction_->setRange(0, 60);
    friction_->setValue(30);
    friction_->setSuffix("°");
    form->addRow(tr("Friction angle"), friction_);
    faceMode_ = new QComboBox;
    faceMode_->addItem(tr("Face (dip / dip direction)"), 0);
    faceMode_->addItem(tr("Drive walls (trend = dip direction)"), 1);
    form->addRow(tr("Kinematics"), faceMode_);
    faceDip_ = new QDoubleSpinBox;
    faceDip_->setRange(0, 90);
    faceDip_->setValue(80);
    faceDip_->setSuffix("°");
    form->addRow(tr("Face dip"), faceDip_);
    faceDir_ = new QDoubleSpinBox;
    faceDir_->setRange(0, 360);
    faceDir_->setValue(180);
    faceDir_->setSuffix("°");
    form->addRow(tr("Face dip direction / drive trend"), faceDir_);
    auto* save = new QPushButton(tr("Write sets to measurements"));
    auto* svg = new QPushButton(tr("Export SVG…"));
    form->addRow(save);
    form->addRow(svg);
    results_ = new QTextBrowser;
    results_->setMinimumWidth(260);
    form->addRow(results_);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(right);
    split->addWidget(scroll);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    for (auto* w : std::initializer_list<QWidget*>{projection_, density_, faceMode_})
        connect(static_cast<QComboBox*>(w), &QComboBox::activated, this, [this](int) { recompute(); });
    for (auto* s : {cone_, friction_, faceDip_, faceDir_}) connect(s, &QDoubleSpinBox::editingFinished, this, &StereonetPanel::recompute);
    connect(sets_, &QSpinBox::editingFinished, this, &StereonetPanel::recompute);
    connect(terzaghi_, &QCheckBox::toggled, this, &StereonetPanel::recompute);
    connect(view_, &StereonetView::hovered, hover_, &QLabel::setText);
    connect(save, &QPushButton::clicked, this, &StereonetPanel::writeSets);
    connect(svg, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, tr("Export stereonet"), "stereonet.svg", tr("SVG (*.svg)"));
        if (f.isEmpty()) return;
        QFile out(f);
        if (!out.open(QIODevice::WriteOnly)) return;
        out.write(exportSvg().toUtf8());
        ctx_.report(tr("Stereonet written to %1").arg(f));
    });
    auto later = [this] { QTimer::singleShot(0, this, &StereonetPanel::recompute); };
    connect(&ctx_, &AppContext::modelChanged, this, later);
    connect(&ctx_, &AppContext::selectionChanged, this, later);
    connect(&ctx_, &AppContext::projectReplaced, this, later);
    recompute();
}

std::vector<const StructuralMeasurement*> StereonetPanel::measurements() const {
    std::vector<const StructuralMeasurement*> out;
    if (!ctx_.hasProject()) return out;
    const auto& m = ctx_.project().model();
    for (const auto& id : ctx_.selected(EntityKind::Structure))
        if (const auto* s = m.structures.find(id)) out.push_back(s);
    if (!out.empty()) return out;
    const auto devs = ctx_.selected(EntityKind::Development);
    if (!devs.empty()) {
        for (const auto& s : m.structures.all())
            if (std::find(devs.begin(), devs.end(), s.developmentId) != devs.end()) out.push_back(&s);
        if (!out.empty()) return out;
    }
    for (const auto& s : m.structures.all()) out.push_back(&s);
    return out;
}

void StereonetPanel::recompute() {
    const auto ms = measurements();
    StereonetView::Data d;
    d.projection = static_cast<Projection>(projection_->currentData().toInt());
    d.frictionDeg = friction_->value();
    lastIds_.clear();
    std::vector<StructuralMeasurement> copies;
    for (const auto* m : ms) {
        copies.push_back(*m);
        lastIds_.push_back(m->id);
    }
    d.poles = measurementPoles(copies);
    QString html;
    if (d.poles.empty()) {
        view_->setData(d);
        results_->setHtml(tr("<p>No structural measurements.</p><p>Import mapping data (Geotechnical ▸ Import structures) or add measurements.</p>"));
        return;
    }
    const bool walls = faceMode_->currentData().toInt() == 1;
    const Plane face{faceDip_->value(), faceDir_->value()};
    std::vector<double> weights;
    if (terzaghi_->isChecked()) {
        const Vec3 sampling = poleVector(walls ? driveWalls(faceDir_->value())[0] : face);
        for (const auto& pv : d.poles) weights.push_back(terzaghiWeight(pv, sampling));
    }
    const int dm = density_->currentData().toInt();
    if (dm >= 0 && d.poles.size() >= 3) {
        d.density = poleDensity(d.poles, weights, static_cast<DensityMethod>(dm), 61);
        const double mx = d.density->maxValue;
        if (mx > 0) {
            if (static_cast<DensityMethod>(dm) == DensityMethod::Kamb)
                for (double l = 2; l < mx; l += 2) d.levels.push_back(l);
            else
                for (int i = 1; i <= 6; ++i) d.levels.push_back(mx * i / 7.0);
        }
    }
    html += tr("<h3>%1 measurements</h3>").arg(d.poles.size());
    const FisherStats all = fisher(d.poles, weights);
    html += tr("<p>Mean pole %1/%2, K = %3, α95 = %4°</p>")
                .arg(all.mean.plunge, 2, 'f', 0, QChar('0'))
                .arg(all.mean.trend, 3, 'f', 0, QChar('0'))
                .arg(all.k, 0, 'g', 3)
                .arg(all.alpha95, 0, 'f', 1);
    if (d.density)
        html += tr("<p>Density maximum %1 %2 at %3</p>")
                    .arg(d.density->maxValue, 0, 'f', 1)
                    .arg(static_cast<DensityMethod>(dm) == DensityMethod::Kamb ? tr("σ") : tr("% per 1 % area"))
                    .arg([&] {
                        const Line l = unproject(d.density->maxAt, d.projection);
                        return QStringLiteral("%1/%2").arg(l.plunge, 2, 'f', 0, QChar('0')).arg(l.trend, 3, 'f', 0, QChar('0'));
                    }());
    std::vector<Plane> setPlanes;
    if (sets_->value() > 0 && d.poles.size() >= static_cast<std::size_t>(sets_->value())) {
        lastSets_ = identifyJointSets(d.poles, sets_->value(), cone_->value(), 1, weights);
        d.sets = lastSets_.sets;
        d.assignment = lastSets_.assignment;
        html += tr("<h3>Joint sets</h3><table cellspacing=4><tr><th>Set</th><th>n</th><th>Plane dip/dd</th><th>K</th><th>α95</th></tr>");
        for (const auto& s : lastSets_.sets) {
            setPlanes.push_back(s.meanPlane);
            html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3/%4</td><td>%5</td><td>%6°</td></tr>")
                        .arg(qstr(s.name))
                        .arg(s.members.size())
                        .arg(s.meanPlane.dip, 2, 'f', 0, QChar('0'))
                        .arg(s.meanPlane.dipDirection, 3, 'f', 0, QChar('0'))
                        .arg(s.stats.k, 0, 'g', 3)
                        .arg(s.stats.alpha95, 0, 'f', 1);
        }
        html += "</table>";
        const auto outliers = std::count(lastSets_.assignment.begin(), lastSets_.assignment.end(), -1);
        if (outliers) html += tr("<p>%1 poles outside every set cone</p>").arg(outliers);
    } else {
        lastSets_ = {};
    }
    // Kinematics.
    std::vector<Plane> faces = walls ? std::vector<Plane>{driveWalls(faceDir_->value())[0], driveWalls(faceDir_->value())[1]} : std::vector<Plane>{face};
    std::set<std::size_t> critical;
    html += tr("<h3>Kinematic analysis (φ = %1°)</h3>").arg(friction_->value());
    for (const auto& f : faces) {
        KinematicOptions o;
        o.face = f;
        o.frictionDeg = friction_->value();
        const auto k = kinematicAnalysis(d.poles, setPlanes, o);
        for (auto i : k.planar.critical) critical.insert(i);
        for (auto i : k.toppling.critical) critical.insert(i);
        html += tr("<p><b>Face %1/%2</b>: planar sliding %3 %, flexural toppling %4 %")
                    .arg(f.dip, 2, 'f', 0, QChar('0'))
                    .arg(f.dipDirection, 3, 'f', 0, QChar('0'))
                    .arg(k.planar.percent, 0, 'f', 1)
                    .arg(k.toppling.percent, 0, 'f', 1);
        if (d.poles.size() <= 1500) html += tr(", wedge intersections %1 %").arg(wedgeIntersectionPercent(d.poles, o), 0, 'f', 1);
        html += "</p>";
        for (const auto& w : k.wedges)
            if (w.critical)
                html += tr("<p>&nbsp;&nbsp;Wedge %1 × %2 plunging %3/%4%5</p>")
                            .arg(w.setA < lastSets_.sets.size() ? qstr(lastSets_.sets[w.setA].name) : QString::number(w.setA))
                            .arg(w.setB < lastSets_.sets.size() ? qstr(lastSets_.sets[w.setB].name) : QString::number(w.setB))
                            .arg(w.intersection.plunge, 2, 'f', 0, QChar('0'))
                            .arg(w.intersection.trend, 3, 'f', 0, QChar('0'))
                            .arg(w.singlePlaneSliding ? tr(" (slides on one plane)") : QString());
    }
    d.face = faces.front();
    d.critical.assign(critical.begin(), critical.end());
    results_->setHtml(html);
    view_->setData(std::move(d));
}

bool StereonetPanel::writeSets() {
    if (lastSets_.sets.empty() || lastIds_.empty()) {
        ctx_.report(tr("Identify joint sets first"), MessageLevel::Warning);
        return false;
    }
    return ctx_.tryEdit(tr("Assign joint sets"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Assign joint sets");
        ProjectEditor ed(p);
        for (std::size_t i = 0; i < lastIds_.size() && i < lastSets_.assignment.size(); ++i) {
            const int s = lastSets_.assignment[i];
            const auto* m = p.model().structures.find(lastIds_[i]);
            if (!m || s < 0) continue;
            const std::string name = lastSets_.sets[static_cast<std::size_t>(s)].name;
            if (m->setName == name) continue;
            StructuralMeasurement n = *m;
            n.setName = name;
            ed.update(n, "Assign joint set");
        }
        macro.commit();
    });
}

QString StereonetPanel::exportSvg() const {
    const auto& d = view_->data();
    StereonetSvgOptions o;
    o.projection = d.projection;
    o.frictionDeg = d.frictionDeg;
    o.contourLevels = d.levels;
    o.title = ctx_.hasProject() ? ctx_.project().model().info.name + " — structural measurements" : std::string("Stereonet");
    return qstr(stereonetSvg(d.poles, d.sets, d.density ? &*d.density : nullptr, o));
}

} // namespace openmine::app
