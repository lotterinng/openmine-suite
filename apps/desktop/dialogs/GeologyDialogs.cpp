// SPDX-License-Identifier: Apache-2.0
#include "GeologyDialogs.h"

#include "DialogUtil.h"
#include "../app/EntityInfo.h"
#include "../theme/Theme.h"
#include "../viewport/SceneCache.h"

#include "openmine/geology/BlockModel.h"
#include "openmine/geology/Compositing.h"
#include "openmine/geology/Drillholes.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/geotechnical/RockMass.h"
#include "openmine/mine_design/Development.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/util/Csv.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cmath>
#include <set>

namespace openmine::app {

using nlohmann::json;

namespace {

QDialogButtonBox* buttons(QDialog* d, const QString& okText, std::function<bool()> onOk) {
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
    bb->button(QDialogButtonBox::Ok)->setText(okText);
    QObject::connect(bb, &QDialogButtonBox::accepted, d, [d, onOk] {
        if (onOk()) d->accept();
    });
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    return bb;
}

QWidget* fileRow(QWidget* parent, QLineEdit*& edit, const QString& filter, bool optional) {
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    edit = new QLineEdit(w);
    if (optional) edit->setPlaceholderText(QObject::tr("(optional)"));
    auto* b = new QPushButton(QObject::tr("Browse…"), w);
    h->addWidget(edit, 1);
    h->addWidget(b);
    QLineEdit* e = edit;
    QObject::connect(b, &QPushButton::clicked, w, [w, e, filter] {
        const QString f = QFileDialog::getOpenFileName(w, QObject::tr("Choose file"), QFileInfo(e->text()).absolutePath(), filter);
        if (!f.isEmpty()) e->setText(f);
    });
    return w;
}

std::set<std::string> numericFields(const ProjectModel& m) {
    std::set<std::string> s;
    for (const auto& h : m.drillholes.all())
        for (const auto& iv : h.intervals)
            for (const auto& [k, v] : iv.values) s.insert(k);
    return s;
}

std::set<std::string> codeFields(const ProjectModel& m) {
    std::set<std::string> s;
    for (const auto& h : m.drillholes.all())
        for (const auto& iv : h.intervals)
            for (const auto& [k, v] : iv.codes) s.insert(k);
    return s;
}

/// Variogram chart (experimental points and model).
class VariogramChart : public QWidget {
public:
    explicit VariogramChart(QWidget* parent) : QWidget(parent) { setMinimumSize(320, 200); }
    std::vector<geology::LagPoint> points;
    std::optional<geology::VariogramModel> model;
    double range = 100;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& c = Theme::colors();
        p.fillRect(rect(), c.panel);
        const QRectF plot(46, 10, width() - 56, height() - 36);
        double xmax = range, ymax = 0;
        for (const auto& l : points) {
            xmax = std::max(xmax, l.distance);
            ymax = std::max(ymax, l.gamma);
        }
        if (model) ymax = std::max(ymax, model->totalSill());
        if (ymax <= 0) {
            p.setPen(c.textMuted);
            p.drawText(rect(), Qt::AlignCenter, tr("Compute the experimental variogram"));
            return;
        }
        ymax *= 1.15;
        xmax *= 1.05;
        auto X = [&](double d) { return plot.left() + d / xmax * plot.width(); };
        auto Y = [&](double g) { return plot.bottom() - g / ymax * plot.height(); };
        p.setPen(c.border);
        p.drawRect(plot);
        p.setPen(c.textMuted);
        for (int i = 0; i <= 4; ++i) {
            p.drawText(QRectF(0, Y(ymax * i / 4) - 8, 42, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(ymax * i / 4, 'g', 3));
            p.drawText(QRectF(X(xmax * i / 4) - 30, plot.bottom() + 2, 60, 16), Qt::AlignCenter, QString::number(xmax * i / 4, 'f', 0));
        }
        if (model) {
            QPolygonF line;
            for (int i = 0; i <= 200; ++i) {
                const double h = xmax * i / 200;
                // Along the major axis: evaluate with the anisotropy's major direction.
                Vec3 dir{0, 1, 0};
                if (!model->structures.empty()) dir = model->structures.front().ranges.axes()[0];
                line << QPointF(X(h), Y(model->gamma(dir * h)));
            }
            p.setPen(QPen(c.accent, 2));
            p.drawPolyline(line);
        }
        for (const auto& l : points) {
            p.setPen(Qt::NoPen);
            p.setBrush(c.text);
            p.drawEllipse(QPointF(X(l.distance), Y(l.gamma)), 3.5, 3.5);
            p.setPen(c.textMuted);
            p.drawText(QPointF(X(l.distance) + 4, Y(l.gamma) - 4), QString::number(l.pairs));
        }
    }
};

} // namespace

// ---- drillhole import ----------------------------------------------------------------------------

DrillholeImportDialog::DrillholeImportDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Import drillholes"));
    resize(680, 560);
    auto* form = new QFormLayout(this);
    const QString csv = tr("Delimited text (*.csv *.txt *.tsv);;All files (*)");
    form->addRow(tr("Collars"), fileRow(this, collars_, csv, false));
    form->addRow(tr("Surveys"), fileRow(this, surveys_, csv, true));
    intervals_ = new QListWidget(this);
    intervals_->setMaximumHeight(80);
    auto* iv = new QWidget(this);
    auto* ivl = new QHBoxLayout(iv);
    ivl->setContentsMargins(0, 0, 0, 0);
    ivl->addWidget(intervals_, 1);
    auto* addIv = new QPushButton(tr("Add…"), iv);
    auto* delIv = new QPushButton(tr("Remove"), iv);
    auto* ivButtons = new QVBoxLayout;
    ivButtons->addWidget(addIv);
    ivButtons->addWidget(delIv);
    ivl->addLayout(ivButtons);
    form->addRow(tr("Interval tables\n(assays, lithology…)"), iv);
    connect(addIv, &QPushButton::clicked, this, [this, csv] {
        for (const auto& f : QFileDialog::getOpenFileNames(this, tr("Interval tables"), {}, csv)) intervals_->addItem(f);
    });
    connect(delIv, &QPushButton::clicked, this, [this] { delete intervals_->currentItem(); });
    below_ = new QComboBox(this);
    below_->addItem(tr("Half the detection limit"), static_cast<int>(geology::BelowDetection::HalfLimit));
    below_->addItem(tr("The detection limit"), static_cast<int>(geology::BelowDetection::Limit));
    below_->addItem(tr("Zero"), static_cast<int>(geology::BelowDetection::Zero));
    below_->addItem(tr("Missing"), static_cast<int>(geology::BelowDetection::Missing));
    form->addRow(tr("Values below detection (<x)"), below_);
    defaultDip_ = spin(-90, 90, -90, 1, "°", this);
    form->addRow(tr("Dip of holes without surveys"), defaultDip_);
    replace_ = new QCheckBox(tr("Update holes whose ID already exists"), this);
    traces_ = new QCheckBox(tr("Also create desurveyed trace polylines on a 'Drillholes' layer"), this);
    form->addRow(replace_);
    form->addRow(traces_);
    report_ = new QPlainTextEdit(this);
    report_->setReadOnly(true);
    form->addRow(report_);
    auto* bb = buttons(this, tr("Import"), [this] { return importNow(); });
    auto* val = bb->addButton(tr("Validate"), QDialogButtonBox::ActionRole);
    connect(val, &QPushButton::clicked, this, [this] { validate(); });
    form->addRow(bb);
}

void DrillholeImportDialog::setFiles(const QString& collars, const QString& surveys, const QStringList& intervals) {
    collars_->setText(collars);
    surveys_->setText(surveys);
    intervals_->clear();
    intervals_->addItems(intervals);
}

namespace {
std::optional<geology::DrillholeReadResult> readTables(const QString& collars, const QString& surveys, const QStringList& intervals,
                                                       const geology::DrillholeImportOptions& o, QString& error) {
    try {
        geology::DrillholeTables t;
        t.collars = util::readCsvFile(sstr(collars));
        if (!surveys.isEmpty()) t.surveys = util::readCsvFile(sstr(surveys));
        for (const auto& f : intervals) t.intervals.emplace_back(sstr(QFileInfo(f).completeBaseName()), util::readCsvFile(sstr(f)));
        return geology::readDrillholes(t, o);
    } catch (const std::exception& e) {
        error = qstr(e.what());
        return std::nullopt;
    }
}
} // namespace

int DrillholeImportDialog::validate() {
    geology::DrillholeImportOptions o;
    o.belowDetection = static_cast<geology::BelowDetection>(below_->currentData().toInt());
    o.defaultDip = defaultDip_->value();
    o.replaceExisting = replace_->isChecked();
    QStringList ivs;
    for (int i = 0; i < intervals_->count(); ++i) ivs << intervals_->item(i)->text();
    QString err;
    const auto r = readTables(collars_->text(), surveys_->text().trimmed(), ivs, o, err);
    report_->clear();
    if (!r) {
        report_->appendPlainText(tr("Cannot read the tables: %1").arg(err));
        return 1;
    }
    QStringList nf, cf;
    for (const auto& f : r->numericFields) nf << qstr(f);
    for (const auto& f : r->codeFields) cf << qstr(f);
    report_->appendPlainText(tr("%1 holes; numeric fields: %2; code fields: %3").arg(r->holes.size()).arg(nf.join(", "), cf.join(", ")));
    report_->appendPlainText(tr("%1 below detection, %2 above detection, %3 missing values; %4 rows and %5 holes rejected")
                                 .arg(r->belowDetectionCount)
                                 .arg(r->aboveDetectionCount)
                                 .arg(r->missingCount)
                                 .arg(r->rejectedRows)
                                 .arg(r->rejectedHoles));
    int shown = 0;
    for (const auto& i : r->issues) {
        if (++shown > 300) {
            report_->appendPlainText(tr("… %1 more issues").arg(r->issues.size() - 300));
            break;
        }
        const char* sev = i.severity == geology::DrillholeIssue::Severity::Error ? "ERROR" : i.severity == geology::DrillholeIssue::Severity::Warning ? "warning" : "info";
        report_->appendPlainText(QStringLiteral("%1 %2 row %3 %4: %5").arg(sev, qstr(i.table)).arg(i.row).arg(qstr(i.holeId), qstr(i.message)));
    }
    return static_cast<int>(r->errors());
}

bool DrillholeImportDialog::importNow() {
    geology::DrillholeImportOptions o;
    o.belowDetection = static_cast<geology::BelowDetection>(below_->currentData().toInt());
    o.defaultDip = defaultDip_->value();
    o.replaceExisting = replace_->isChecked();
    QStringList ivs;
    for (int i = 0; i < intervals_->count(); ++i) ivs << intervals_->item(i)->text();
    QString err;
    const auto r = readTables(collars_->text(), surveys_->text().trimmed(), ivs, o, err);
    if (!r) {
        ctx_.report(tr("Cannot read the drillhole tables: %1").arg(err), MessageLevel::Error);
        return false;
    }
    geology::DrillholeImportSummary sum;
    const bool ok = ctx_.tryEdit(tr("Import drillholes"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Import drillholes");
        sum = geology::importDrillholes(p, *r, o.replaceExisting);
        if (traces_->isChecked()) geology::createDrillholeTraces(p, workflowLayer(ctx_, "Drillholes", 0xBA68C8));
        macro.commit();
    });
    if (ok)
        ctx_.report(tr("Drillholes: %1 created, %2 updated, %3 skipped (already present), %4 validation errors reported")
                        .arg(sum.created.size())
                        .arg(sum.updated.size())
                        .arg(sum.skipped.size())
                        .arg(r->errors()));
    return ok;
}

// ---- compositing ---------------------------------------------------------------------------------

CompositeDialog::CompositeDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Composite drillholes"));
    auto* form = new QFormLayout(this);
    const auto& m = ctx_.project().model();
    length_ = spin(0.1, 100, 2, 2, " m", this);
    residual_ = new QComboBox(this);
    residual_->addItem(tr("Equal lengths per run (distribute)"), static_cast<int>(geology::ResidualMode::Distribute));
    residual_->addItem(tr("Add the residual to the previous composite"), static_cast<int>(geology::ResidualMode::AddToPrevious));
    residual_->addItem(tr("Discard short residuals"), static_cast<int>(geology::ResidualMode::Discard));
    breakField_ = new QComboBox(this);
    breakField_->addItem(tr("(none)"), QString());
    for (const auto& f : codeFields(m)) breakField_->addItem(qstr(f), qstr(f));
    coverage_ = spin(0, 1, 0.5, 2, {}, this);
    fields_ = new QLineEdit(this);
    fields_->setPlaceholderText(tr("all numeric fields"));
    csv_ = new QLineEdit(this);
    csv_->setPlaceholderText(tr("CSV file to write (optional)"));
    form->addRow(tr("Composite length"), length_);
    form->addRow(tr("Residuals"), residual_);
    form->addRow(tr("Break at changes of"), breakField_);
    form->addRow(tr("Minimum sampled fraction"), coverage_);
    form->addRow(tr("Fields (comma separated)"), fields_);
    form->addRow(tr("Export"), csv_);
    form->addRow(new QLabel(tr("Holes: the selected drillholes, else all. The composites and their statistics are stored as an analysis result."), this));
    form->addRow(buttons(this, tr("Composite"), [this] { return start() > 0; }));
}

int CompositeDialog::start() {
    geology::CompositeOptions o;
    o.length = length_->value();
    o.residual = static_cast<geology::ResidualMode>(residual_->currentData().toInt());
    o.breakField = sstr(breakField_->currentData().toString());
    o.minCoverage = coverage_->value();
    for (const auto& f : fields_->text().split(',', Qt::SkipEmptyParts)) o.fields.push_back(sstr(f.trimmed()));
    std::vector<Uuid> holes = ctx_.selected(EntityKind::Drillhole);
    if (holes.empty())
        for (const auto& h : ctx_.project().model().drillholes.all()) holes.push_back(h.id);
    const QString csv = csv_->text().trimmed();
    auto snap = ctx_.snapshot();
    AppContext* c = &ctx_;
    return ctx_.jobs().start(tr("Composite drillholes"), tr("Geology"), [snap, holes, o, csv, c](jobs::Progress& pr) -> UiApply {
        auto all = std::make_shared<std::vector<geology::Composite>>();
        std::size_t k = 0;
        for (const auto& id : holes) {
            pr.update(static_cast<double>(k++) / holes.size(), "Compositing");
            const auto* h = snap->model().drillholes.find(id);
            if (!h) continue;
            auto cs = o.breakField.empty() ? geology::compositeByLength(*h, o) : geology::compositeByCode(*h, o.breakField, o);
            all->insert(all->end(), cs.begin(), cs.end());
        }
        if (!csv.isEmpty()) {
            QFile f(csv);
            if (!f.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write " + csv.toStdString());
            f.write(QByteArray::fromStdString(geology::compositesCsv(*all)));
        }
        json stats = json::object();
        std::set<std::string> fields;
        for (const auto& cp : *all)
            for (const auto& [f, v] : cp.values) fields.insert(f);
        for (const auto& f : fields) {
            std::vector<double> v, w;
            for (const auto& cp : *all)
                if (auto it = cp.values.find(f); it != cp.values.end() && std::isfinite(it->second)) {
                    v.push_back(it->second);
                    w.push_back(cp.length());
                }
            const auto s = geology::statistics(v, w);
            stats[f] = {{"count", s.count}, {"mean", s.mean}, {"stdDev", s.stdDev}, {"cv", s.cv}, {"min", s.min}, {"median", s.median}, {"p95", s.p95}, {"max", s.max}};
        }
        return [c, all, stats, o, csv] {
            c->report(QObject::tr("%1 composites of %2 m%3").arg(all->size()).arg(o.length).arg(csv.isEmpty() ? QString() : QObject::tr(" written to %1").arg(csv)));
            c->tryEdit(QObject::tr("Store composite statistics"), [&] {
                storeAnalysis(c->project(), AnalysisKind::Other, "Drillhole composites " + std::to_string(o.length) + " m", json{{"composites", all->size()}},
                              {{"length", o.length}, {"minCoverage", o.minCoverage}, {"breakField", o.breakField}}, stats);
            });
        };
    });
}

// ---- block model creation ---------------------------------------------------------------------------

BlockModelDialog::BlockModelDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("New block model"));
    auto* form = new QFormLayout(this);
    name_ = new QLineEdit(tr("Block model %1").arg(ctx_.project().model().blockModels.size() + 1), this);
    ox_ = spin(-1e8, 1e8, 0, 3, {}, this);
    oy_ = spin(-1e8, 1e8, 0, 3, {}, this);
    oz_ = spin(-1e5, 1e5, 0, 3, {}, this);
    dx_ = spin(0.01, 1000, 5, 3, " m", this);
    dy_ = spin(0.01, 1000, 5, 3, " m", this);
    dz_ = spin(0.01, 1000, 5, 3, " m", this);
    nx_ = ispin(1, 5000, 20, this);
    ny_ = ispin(1, 5000, 20, this);
    nz_ = ispin(1, 5000, 10, this);
    rot_ = spin(-360, 360, 0, 3, "°", this);
    pad_ = spin(0, 1000, 10, 2, " m", this);
    auto row3 = [&](QWidget* a, QWidget* b, QWidget* c) {
        auto* w = new QWidget(this);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(a);
        h->addWidget(b);
        h->addWidget(c);
        return w;
    };
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Origin (corner) X, Y, Z"), row3(ox_, oy_, oz_));
    form->addRow(tr("Block size"), row3(dx_, dy_, dz_));
    form->addRow(tr("Blocks nx, ny, nz"), row3(nx_, ny_, nz_));
    form->addRow(tr("Rotation about Z (CCW)"), rot_);
    auto* fitRow = new QHBoxLayout;
    auto* fitDh = new QPushButton(tr("Fit to drillholes"), this);
    auto* fitSel = new QPushButton(tr("Fit to selected objects"), this);
    fitRow->addWidget(new QLabel(tr("Padding"), this));
    fitRow->addWidget(pad_);
    fitRow->addWidget(fitDh);
    fitRow->addWidget(fitSel);
    form->addRow(fitRow);
    connect(fitDh, &QPushButton::clicked, this, [this] { fit(true); });
    connect(fitSel, &QPushButton::clicked, this, [this] { fit(false); });
    form->addRow(buttons(this, tr("Create"), [this] { return !create().isNil(); }));
    // Start from a grid that covers the data: the selection when there is one, else the drillholes.
    if (!ctx_.selectedGeometry().empty()) fit(false);
    else if (!ctx_.project().model().drillholes.empty()) fit(true);
}

void BlockModelDialog::fit(bool drillholes) {
    std::vector<Vec3> pts;
    const auto& m = ctx_.project().model();
    if (drillholes) {
        for (const auto& h : m.drillholes.all()) {
            try {
                const geology::DesurveyedHole dh(h);
                for (const auto& p : dh.trace(5.0)) pts.push_back(p);
            } catch (const std::exception&) {
            }
        }
    } else {
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* g = m.geometry.find(id))
                if (auto b = geometry::objectBounds(*g)) {
                    pts.push_back(b->min);
                    pts.push_back(b->max);
                }
    }
    if (pts.empty()) {
        ctx_.report(tr("Nothing to fit to"), MessageLevel::Warning);
        return;
    }
    const auto b = geology::gridCovering(pts, {dx_->value(), dy_->value(), dz_->value()}, rot_->value(), {}, pad_->value());
    ox_->setValue(b.origin.x);
    oy_->setValue(b.origin.y);
    oz_->setValue(b.origin.z);
    nx_->setValue(b.nx);
    ny_->setValue(b.ny);
    nz_->setValue(b.nz);
}

Uuid BlockModelDialog::create() {
    BlockModel b;
    b.name = sstr(name_->text().trimmed());
    b.origin = {ox_->value(), oy_->value(), oz_->value()};
    b.blockSize = {dx_->value(), dy_->value(), dz_->value()};
    b.nx = nx_->value();
    b.ny = ny_->value();
    b.nz = nz_->value();
    b.rotationDeg = rot_->value();
    Uuid id;
    ctx_.tryEdit(tr("Create block model"), [&] { id = geology::createBlockModel(ctx_.project(), b); });
    if (!id.isNil()) ctx_.select(EntityKind::BlockModel, id);
    return id;
}

// ---- domain coding ----------------------------------------------------------------------------------

BlockCodeDialog::BlockCodeDialog(AppContext& ctx, const Uuid& blockModelId, QWidget* parent) : QDialog(parent), ctx_(ctx), model_(blockModelId) {
    setWindowTitle(tr("Code blocks by a solid / surface"));
    auto* form = new QFormLayout(this);
    const auto& m = ctx_.project().model();
    solid_ = new QComboBox(this);
    fillEntityCombo(solid_, m, EntityKind::Geometry, {}, false, [&m](const Uuid& id) {
        const auto* g = m.geometry.find(id);
        return g && g->mesh && !g->mesh->triangles.empty();
    });
    mode_ = new QComboBox(this);
    mode_->addItem(tr("Proportion inside a closed solid (0…1)"), 0);
    mode_->addItem(tr("Proportion below a surface (0…1)"), 1);
    mode_->addItem(tr("Code value where the centroid is inside the solid"), 2);
    field_ = new QLineEdit("domain", this);
    value_ = spin(-1e9, 1e9, 1, 3, {}, this);
    sub_ = ispin(1, 10, 4, this);
    form->addRow(tr("Shape"), solid_);
    form->addRow(tr("Method"), mode_);
    form->addRow(tr("Field"), field_);
    form->addRow(tr("Code value"), value_);
    form->addRow(tr("Sub-samples per axis"), sub_);
    form->addRow(buttons(this, tr("Code"), [this] { return start() > 0; }));
}

int BlockCodeDialog::start() {
    const Uuid solid = comboUuid(solid_);
    const int mode = mode_->currentData().toInt();
    const std::string field = sstr(field_->text().trimmed());
    const double value = value_->value();
    const int sub = sub_->value();
    const Uuid id = model_;
    auto snap = ctx_.snapshot();
    AppContext* c = &ctx_;
    const auto revision = snap->model().blockModels.find(id) ? snap->model().blockModels.find(id)->meta.revision : 0;
    return ctx_.jobs().start(tr("Code blocks"), tr("Geology"), [snap, solid, mode, field, value, sub, id, c, revision](jobs::Progress& pr) -> UiApply {
        const auto* g = snap->model().geometry.find(solid);
        if (!g || !g->mesh) throw ValidationError("Choose a mesh or solid");
        auto bmd = std::make_shared<geology::BlockModelData>(geology::loadBlockModel(*snap, id));
        std::size_t n = 0;
        if (mode == 2) {
            n = geology::codeBlocksInside(*bmd, *g->mesh, field, value);
        } else {
            const geology::BlockGrid grid(bmd->header);
            auto f = mode == 0 ? geology::fractionInside(grid, *g->mesh, sub, pr) : geology::fractionBelow(grid, *g->mesh, sub, pr);
            auto& out = bmd->attribute(field);
            for (std::size_t i = 0; i < f.size(); ++i) {
                out[i] = f[i];
                n += f[i] > 0;
            }
        }
        return [c, bmd, id, n, field, revision] {
            const auto* cur = c->project().model().blockModels.find(id);
            if (!cur || cur->meta.revision != revision) throw ValidationError("The block model changed while coding; run it again");
            geology::saveBlockModel(c->project(), *bmd, "Code blocks: " + field);
            c->report(QObject::tr("%1 blocks coded in '%2'").arg(n).arg(qstr(field)));
        };
    });
}

// ---- estimation ---------------------------------------------------------------------------------------

EstimationDialog::EstimationDialog(AppContext& ctx, const Uuid& blockModelId, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Grade estimation"));
    resize(820, 700);
    const auto& m = ctx_.project().model();
    auto* lay = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    lay->addWidget(tabs, 1);

    auto* dataTab = new QWidget;
    auto* df = new QFormLayout(dataTab);
    model_ = new QComboBox(dataTab);
    fillEntityCombo(model_, m, EntityKind::BlockModel, blockModelId);
    field_ = new QComboBox(dataTab);
    for (const auto& f : numericFields(m)) field_->addItem(qstr(f));
    compLength_ = spin(0.1, 100, 2, 2, " m", dataTab);
    codeField_ = new QComboBox(dataTab);
    codeField_->addItem(tr("(all samples)"), QString());
    for (const auto& f : codeFields(m)) codeField_->addItem(qstr(f), qstr(f));
    code_ = new QLineEdit(dataTab);
    mask_ = new QLineEdit(dataTab);
    mask_->setPlaceholderText(tr("block field > 0 limits the blocks estimated (e.g. domain)"));
    output_ = new QLineEdit("est", dataTab);
    discret_ = ispin(1, 5, 1, dataTab);
    df->addRow(tr("Block model"), model_);
    df->addRow(tr("Grade field"), field_);
    df->addRow(tr("Composite length"), compLength_);
    df->addRow(tr("Domain code field"), codeField_);
    df->addRow(tr("Domain code"), code_);
    df->addRow(tr("Block mask field"), mask_);
    df->addRow(tr("Output field"), output_);
    df->addRow(tr("Discretisation (per axis)"), discret_);
    tabs->addTab(dataTab, tr("Data"));

    auto* srch = new QWidget;
    auto* sf = new QFormLayout(srch);
    method_ = new QComboBox(srch);
    method_->addItem(tr("Ordinary kriging"), static_cast<int>(geology::EstimationMethod::OrdinaryKriging));
    method_->addItem(tr("Simple kriging (mean of the samples)"), static_cast<int>(geology::EstimationMethod::SimpleKriging));
    method_->addItem(tr("Inverse distance"), static_cast<int>(geology::EstimationMethod::InverseDistance));
    method_->addItem(tr("Nearest neighbour"), static_cast<int>(geology::EstimationMethod::NearestNeighbour));
    power_ = spin(0.5, 5, 2, 2, {}, srch);
    major_ = spin(0.1, 1e5, 100, 1, " m", srch);
    semi_ = spin(0.1, 1e5, 80, 1, " m", srch);
    minor_ = spin(0.1, 1e5, 30, 1, " m", srch);
    az_ = spin(-360, 360, 0, 1, "°", srch);
    dip_ = spin(-90, 90, 0, 1, "°", srch);
    tilt_ = spin(-90, 90, 0, 1, "°", srch);
    minS_ = ispin(1, 200, 4, srch);
    maxS_ = ispin(1, 500, 24, srch);
    perHole_ = ispin(0, 100, 4, srch);
    sf->addRow(tr("Method"), method_);
    sf->addRow(tr("IDW power"), power_);
    sf->addRow(tr("Search major / semi / minor"), [&] {
        auto* w = new QWidget(srch);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(major_);
        h->addWidget(semi_);
        h->addWidget(minor_);
        return w;
    }());
    sf->addRow(tr("Azimuth / dip / tilt"), [&] {
        auto* w = new QWidget(srch);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(az_);
        h->addWidget(dip_);
        h->addWidget(tilt_);
        return w;
    }());
    sf->addRow(tr("Samples min / max"), [&] {
        auto* w = new QWidget(srch);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(minS_);
        h->addWidget(maxS_);
        return w;
    }());
    sf->addRow(tr("Max samples per hole (0 = no limit)"), perHole_);
    tabs->addTab(srch, tr("Search"));

    auto* vg = new QWidget;
    auto* vl = new QVBoxLayout(vg);
    auto* vf = new QFormLayout;
    nugget_ = spin(0, 1e9, 0.1, 4, {}, vg);
    type1_ = new QComboBox(vg);
    type2_ = new QComboBox(vg);
    for (auto* t : {type1_, type2_}) {
        t->addItem(tr("Spherical"), static_cast<int>(geology::VariogramType::Spherical));
        t->addItem(tr("Exponential"), static_cast<int>(geology::VariogramType::Exponential));
        t->addItem(tr("Gaussian"), static_cast<int>(geology::VariogramType::Gaussian));
    }
    sill1_ = spin(0, 1e9, 0.6, 4, {}, vg);
    range1_ = spin(0.1, 1e5, 50, 1, " m", vg);
    second_ = new QCheckBox(tr("Second structure"), vg);
    sill2_ = spin(0, 1e9, 0.3, 4, {}, vg);
    range2_ = spin(0.1, 1e5, 150, 1, " m", vg);
    lag_ = spin(0.1, 1e4, 10, 2, " m", vg);
    lags_ = ispin(2, 100, 12, vg);
    vf->addRow(tr("Nugget"), nugget_);
    vf->addRow(tr("Structure 1 type / sill / major range"), [&] {
        auto* w = new QWidget(vg);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(type1_);
        h->addWidget(sill1_);
        h->addWidget(range1_);
        return w;
    }());
    vf->addRow(second_, [&] {
        auto* w = new QWidget(vg);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(type2_);
        h->addWidget(sill2_);
        h->addWidget(range2_);
        return w;
    }());
    vf->addRow(tr("Experimental lag / lags"), [&] {
        auto* w = new QWidget(vg);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(lag_);
        h->addWidget(lags_);
        return w;
    }());
    vl->addLayout(vf);
    chart_ = new VariogramChart(vg);
    vl->addWidget(chart_, 1);
    auto* vb = new QHBoxLayout;
    auto* calc = new QPushButton(tr("Experimental variogram and fit"), vg);
    auto* xv = new QPushButton(tr("Cross-validate"), vg);
    vb->addWidget(calc);
    vb->addWidget(xv);
    vb->addStretch(1);
    vl->addLayout(vb);
    tabs->addTab(vg, tr("Variogram"));
    connect(calc, &QPushButton::clicked, this, &EstimationDialog::computeVariogram);
    connect(xv, &QPushButton::clicked, this, &EstimationDialog::crossValidate);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumHeight(140);
    lay->addWidget(log_);
    lay->addWidget(buttons(this, tr("Estimate"), [this] { return start() > 0; }));
}

geology::SampleSet EstimationDialog::samples() const {
    const auto& m = ctx_.project().model();
    geology::CompositeOptions o;
    o.length = compLength_->value();
    const std::string field = sstr(field_->currentText());
    o.fields = {field};
    const std::string codeField = sstr(codeField_->currentData().toString());
    std::vector<geology::Composite> all;
    for (const auto& h : m.drillholes.all()) {
        auto cs = codeField.empty() ? geology::compositeByLength(h, o) : geology::compositeByCode(h, codeField, o);
        all.insert(all.end(), cs.begin(), cs.end());
    }
    return geology::samplesFromComposites(all, field, codeField, sstr(code_->text().trimmed()));
}

geology::EstimationOptions EstimationDialog::options() const {
    geology::EstimationOptions o;
    o.method = static_cast<geology::EstimationMethod>(method_->currentData().toInt());
    o.outputField = sstr(output_->text().trimmed());
    geology::SearchPass pass;
    pass.ellipse = {major_->value(), semi_->value(), minor_->value(), az_->value(), dip_->value(), tilt_->value()};
    pass.minSamples = minS_->value();
    pass.maxSamples = maxS_->value();
    pass.maxPerHole = perHole_->value();
    o.passes = {pass};
    // A second, relaxed pass doubles the search for blocks the first leaves empty.
    geology::SearchPass p2 = pass;
    p2.ellipse.major *= 2;
    p2.ellipse.semi *= 2;
    p2.ellipse.minor *= 2;
    p2.minSamples = std::max(1, pass.minSamples / 2);
    o.passes.push_back(p2);
    o.power = power_->value();
    const double ratioSemi = semi_->value() / major_->value(), ratioMinor = minor_->value() / major_->value();
    o.variogram.nugget = nugget_->value();
    auto structure = [&](QComboBox* t, QDoubleSpinBox* sill, QDoubleSpinBox* range) {
        geology::VariogramStructure s;
        s.type = static_cast<geology::VariogramType>(t->currentData().toInt());
        s.sill = sill->value();
        s.ranges = {range->value(), range->value() * ratioSemi, range->value() * ratioMinor, az_->value(), dip_->value(), tilt_->value()};
        return s;
    };
    o.variogram.structures = {structure(type1_, sill1_, range1_)};
    if (second_->isChecked()) o.variogram.structures.push_back(structure(type2_, sill2_, range2_));
    o.discretisation = {discret_->value(), discret_->value(), discret_->value()};
    o.maskField = sstr(mask_->text().trimmed());
    return o;
}

bool EstimationDialog::computeVariogram() {
    try {
        const auto s = samples();
        if (s.points.size() < 10) throw ValidationError("Fewer than 10 samples");
        double mean = 0;
        for (double v : s.values) mean += v;
        mean /= s.values.size();
        double var = 0;
        for (double v : s.values) var += (v - mean) * (v - mean);
        var /= s.values.size();
        geology::ExperimentalOptions eo;
        eo.lag = lag_->value();
        eo.lags = lags_->value();
        eo.azimuth = az_->value();
        eo.dip = dip_->value();
        eo.angleTolerance = 22.5;
        eo.bandwidth = semi_->value();
        experimental_ = geology::experimentalVariogram(s, eo);
        // Initial model from the dialog (sills scaled to the sample variance when unset).
        auto o = options();
        if (o.variogram.totalSill() <= 0) {
            o.variogram.nugget = 0.1 * var;
            o.variogram.structures.front().sill = 0.9 * var;
        }
        const Vec3 dir = o.variogram.structures.front().ranges.axes()[0];
        const auto fit = geology::fitVariogram(experimental_, o.variogram, dir);
        nugget_->setValue(fit.model.nugget);
        if (!fit.model.structures.empty()) {
            sill1_->setValue(fit.model.structures[0].sill);
            range1_->setValue(fit.model.structures[0].ranges.major);
        }
        if (fit.model.structures.size() > 1) {
            sill2_->setValue(fit.model.structures[1].sill);
            range2_->setValue(fit.model.structures[1].ranges.major);
        }
        auto* chart = static_cast<VariogramChart*>(chart_);
        chart->points = experimental_;
        chart->model = fit.model;
        chart->range = lag_->value() * lags_->value();
        chart->update();
        log_->appendPlainText(tr("%1 samples (variance %2): fitted nugget %3, sill %4, range %5 m (SSE %6, %7)")
                                  .arg(s.points.size())
                                  .arg(var, 0, 'g', 4)
                                  .arg(fit.model.nugget, 0, 'g', 4)
                                  .arg(fit.model.totalSill(), 0, 'g', 4)
                                  .arg(fit.model.structures.empty() ? 0 : fit.model.structures[0].ranges.major, 0, 'f', 1)
                                  .arg(fit.weightedSse, 0, 'g', 3)
                                  .arg(fit.converged ? tr("converged") : tr("not converged")));
        return true;
    } catch (const std::exception& e) {
        log_->appendPlainText(tr("Variogram: %1").arg(qstr(e.what())));
        return false;
    }
}

bool EstimationDialog::crossValidate() {
    try {
        const auto s = samples();
        const auto cv = geology::crossValidate(s, options(), true);
        log_->appendPlainText(tr("Cross-validation (leave one hole out): %1 estimated, mean error %2, MSE %3, correlation %4, slope %5")
                                  .arg(cv.estimated)
                                  .arg(cv.meanError, 0, 'g', 4)
                                  .arg(cv.meanSquaredError, 0, 'g', 4)
                                  .arg(cv.correlation, 0, 'f', 3)
                                  .arg(cv.slope, 0, 'f', 3));
        return true;
    } catch (const std::exception& e) {
        log_->appendPlainText(tr("Cross-validation: %1").arg(qstr(e.what())));
        return false;
    }
}

int EstimationDialog::start() {
    geology::SampleSet s;
    try {
        s = samples();
    } catch (const std::exception& e) {
        ctx_.report(qstr(e.what()), MessageLevel::Error);
        return 0;
    }
    if (s.points.empty()) {
        ctx_.report(tr("No samples for the chosen field and domain"), MessageLevel::Error);
        return 0;
    }
    auto o = options();
    if (o.method == geology::EstimationMethod::SimpleKriging) {
        double mean = 0;
        for (double v : s.values) mean += v;
        o.simpleMean = mean / s.values.size();
    }
    const Uuid id = comboUuid(model_);
    auto snap = ctx_.snapshot();
    AppContext* c = &ctx_;
    const auto* bm = snap->model().blockModels.find(id);
    if (!bm) return 0;
    const auto revision = bm->meta.revision;
    const std::string field = sstr(field_->currentText());
    return ctx_.jobs().start(tr("Estimate %1").arg(qstr(o.outputField)), tr("Geology"), [snap, id, s, o, c, revision, field](jobs::Progress& pr) -> UiApply {
        auto bmd = std::make_shared<geology::BlockModelData>(geology::loadBlockModel(*snap, id));
        const auto sum = geology::estimate(*bmd, s, o, pr);
        return [c, bmd, id, sum, o, revision, field, n = s.points.size()] {
            const auto* cur = c->project().model().blockModels.find(id);
            if (!cur || cur->meta.revision != revision) throw ValidationError("The block model changed during the estimation; run it again");
            CommandStack::MacroScope macro(c->project().commands(), "Estimate " + o.outputField);
            geology::saveBlockModel(c->project(), *bmd, "Estimate " + o.outputField);
            storeAnalysis(c->project(), AnalysisKind::BlockModelEstimate, "Estimate " + o.outputField,
                          {{"blockModel", id.toString()}, {"field", field}, {"samples", n}},
                          {{"method", geology::toString(o.method)}, {"major", o.passes.front().ellipse.major}, {"minSamples", o.passes.front().minSamples},
                           {"maxSamples", o.passes.front().maxSamples}, {"nugget", o.variogram.nugget}, {"sill", o.variogram.totalSill()}},
                          {{"estimated", sum.estimated}, {"notEstimated", sum.notEstimated}, {"skippedMask", sum.skippedMask}, {"singular", sum.singularSystems},
                           {"negativeWeightBlocks", sum.negativeWeightBlocks}});
            macro.commit();
            c->report(QObject::tr("Estimated %1 blocks (%2 not estimated, %3 masked)").arg(sum.estimated).arg(sum.notEstimated).arg(sum.skippedMask));
        };
    });
}

// ---- block display -----------------------------------------------------------------------------------

BlockDisplayDialog::BlockDisplayDialog(AppContext& ctx, SceneCache& scene, const Uuid& blockModelId, QWidget* parent)
    : QDialog(parent), ctx_(ctx), scene_(scene), model_(blockModelId) {
    const auto* b = ctx_.project().model().blockModels.find(blockModelId);
    setWindowTitle(tr("Block model display — %1").arg(b ? qstr(b->name) : QString()));
    auto* form = new QFormLayout(this);
    const auto d = scene_.blockDisplay(blockModelId);
    attribute_ = new QComboBox(this);
    if (b)
        for (const auto& a : b->attributes) attribute_->addItem(qstr(a));
    if (!d.attribute.empty()) attribute_->setCurrentText(qstr(d.attribute));
    hasMin_ = new QCheckBox(tr("Show values ≥"), this);
    hasMax_ = new QCheckBox(tr("Show values ≤"), this);
    min_ = spin(-1e12, 1e12, d.filterMin.value_or(0), 4, {}, this);
    max_ = spin(-1e12, 1e12, d.filterMax.value_or(1), 4, {}, this);
    hasMin_->setChecked(d.filterMin.has_value());
    hasMax_->setChecked(d.filterMax.has_value());
    opacity_ = spin(0.05, 1, d.opacity, 2, {}, this);
    lut_ = new QComboBox(this);
    lut_->addItem(tr("Viridis"), static_cast<int>(Lut::Viridis));
    lut_->addItem(tr("Rainbow"), static_cast<int>(Lut::Rainbow));
    lut_->addItem(tr("Diverging"), static_cast<int>(Lut::Diverging));
    lut_->addItem(tr("Categories"), static_cast<int>(Lut::Categorical));
    lut_->addItem(tr("Grey"), static_cast<int>(Lut::Grey));
    lut_->setCurrentIndex(std::max(0, lut_->findData(static_cast<int>(d.lut))));
    outline_ = new QCheckBox(tr("Model extent outline"), this);
    outline_->setChecked(d.outline);
    form->addRow(tr("Attribute"), attribute_);
    form->addRow(hasMin_, min_);
    form->addRow(hasMax_, max_);
    form->addRow(tr("Opacity"), opacity_);
    form->addRow(tr("Colours"), lut_);
    form->addRow(outline_);
    auto* bb = buttons(this, tr("OK"), [this] {
        apply();
        return true;
    });
    auto* ap = bb->addButton(QDialogButtonBox::Apply);
    connect(ap, &QPushButton::clicked, this, &BlockDisplayDialog::apply);
    form->addRow(bb);
}

void BlockDisplayDialog::apply() {
    BlockModelDisplay d;
    d.attribute = sstr(attribute_->currentText());
    if (hasMin_->isChecked()) d.filterMin = min_->value();
    if (hasMax_->isChecked()) d.filterMax = max_->value();
    d.opacity = opacity_->value();
    d.lut = static_cast<Lut>(lut_->currentData().toInt());
    d.outline = outline_->isChecked();
    scene_.setBlockDisplay(model_, d);
}

// ---- structures ----------------------------------------------------------------------------------

StructureImportDialog::StructureImportDialog(AppContext& ctx, const QString& file, QWidget* parent) : QDialog(parent), ctx_(ctx), file_(file) {
    setWindowTitle(tr("Import structural measurements — %1").arg(QFileInfo(file).fileName()));
    auto* form = new QFormLayout(this);
    util::CsvTable t;
    try {
        t = util::readCsvFile(sstr(file));
    } catch (const std::exception& e) {
        ctx_.report(qstr(e.what()), MessageLevel::Error);
    }
    const char* roles[] = {"Dip", "Dip direction", "Set", "Type", "X", "Y", "Z", "Persistence (m)"};
    static const std::vector<std::vector<std::string>> guess = {{"dip"}, {"dipdir", "dip_direction", "dipdirection", "dd", "azimuth"}, {"set", "set_name", "family"},
                                                                {"type", "kind"}, {"x", "east", "easting"}, {"y", "north", "northing"}, {"z", "elev", "rl"},
                                                                {"persistence", "length", "trace"}};
    for (int i = 0; i < 8; ++i) {
        cols_[i] = new QComboBox(this);
        if (i >= 2) cols_[i]->addItem(tr("(none)"), -1);
        for (std::size_t h = 0; h < t.headers.size(); ++h) {
            cols_[i]->addItem(qstr(t.headers[h]), static_cast<int>(h));
            std::string lower = t.headers[h];
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            for (const auto& g : guess[static_cast<std::size_t>(i)])
                if (lower == g) cols_[i]->setCurrentIndex(cols_[i]->count() - 1);
        }
        form->addRow(tr(roles[i]), cols_[i]);
    }
    development_ = new QComboBox(this);
    fillEntityCombo(development_, ctx_.project().model(), EntityKind::Development, ctx_.selected(EntityKind::Development).empty() ? Uuid{} : ctx_.selected(EntityKind::Development).front(), true);
    form->addRow(tr("Mapped in heading"), development_);
    info_ = new QLabel(tr("%1 rows").arg(t.rows.size()), this);
    form->addRow(info_);
    form->addRow(buttons(this, tr("Import"), [this] { return apply(); }));
}

bool StructureImportDialog::apply() {
    util::CsvTable t;
    try {
        t = util::readCsvFile(sstr(file_));
    } catch (const std::exception& e) {
        ctx_.report(qstr(e.what()), MessageLevel::Error);
        return false;
    }
    int col[8];
    for (int i = 0; i < 8; ++i) col[i] = cols_[i]->currentData().toInt();
    std::vector<StructuralMeasurement> out;
    std::size_t rejected = 0;
    for (const auto& row : t.rows) {
        auto field = [&](int c) { return c >= 0 && static_cast<std::size_t>(c) < row.size() ? row[static_cast<std::size_t>(c)] : std::string(); };
        const auto dip = util::parseNumber(field(col[0])), dd = util::parseNumber(field(col[1]));
        if (!dip || !dd || *dip < 0 || *dip > 90) {
            ++rejected;
            continue;
        }
        StructuralMeasurement s;
        s.dip = *dip;
        s.dipDirection = std::fmod(std::fmod(*dd, 360.0) + 360.0, 360.0);
        s.setName = field(col[2]);
        std::string type = field(col[3]);
        if (!type.empty()) {
            type[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(type[0])));
            s.type = enumFromString<StructureType>(type).value_or(StructureType::Other);
        }
        s.location = {util::parseNumber(field(col[4])).value_or(0), util::parseNumber(field(col[5])).value_or(0), util::parseNumber(field(col[6])).value_or(0)};
        s.persistence = util::parseNumber(field(col[7])).value_or(0);
        s.developmentId = comboUuid(development_);
        out.push_back(s);
    }
    const bool ok = ctx_.tryEdit(tr("Import structures"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Import structural measurements");
        ProjectEditor ed(p);
        for (auto& s : out) ed.add(s);
        macro.commit();
    });
    if (ok) ctx_.report(tr("Imported %1 measurements (%2 rows rejected: missing or invalid dip / dip direction)").arg(out.size()).arg(rejected));
    return ok;
}

// ---- classification ---------------------------------------------------------------------------------

ClassificationDialog::ClassificationDialog(AppContext& ctx, const Uuid& developmentId, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Rock-mass classification"));
    resize(620, 600);
    auto* lay = new QVBoxLayout(this);
    auto* where = new QFormLayout;
    development_ = new QComboBox(this);
    fillEntityCombo(development_, ctx_.project().model(), EntityKind::Development, developmentId);
    chainage_ = spin(-1e6, 1e6, 0, 2, " m", this);
    if (const auto* d = ctx_.project().model().developments.find(developmentId)) chainage_->setValue(d->chainageFrom);
    mappedBy_ = new QLineEdit(qstr(ctx_.project().userName()), this);
    date_ = new QDateEdit(QDate::currentDate(), this);
    date_->setCalendarPopup(true);
    date_->setDisplayFormat("yyyy-MM-dd");
    notes_ = new QLineEdit(this);
    where->addRow(tr("Heading"), development_);
    where->addRow(tr("Chainage"), chainage_);
    where->addRow(tr("Mapped by"), mappedBy_);
    where->addRow(tr("Date"), date_);
    where->addRow(tr("Notes"), notes_);
    lay->addLayout(where);
    tabs_ = new QTabWidget(this);
    auto table = [](QComboBox* c, const std::vector<geotechnical::TableEntry>& t) {
        for (const auto& e : t)
            c->addItem(QStringLiteral("%1  %2 — %3").arg(qstr(e.code)).arg(e.valueMax > e.value ? QStringLiteral("%1–%2").arg(e.value).arg(e.valueMax) : QString::number(e.value)).arg(qstr(e.description)),
                       e.valueMax > e.value ? (e.value + e.valueMax) / 2 : e.value);
    };
    auto* q = new QWidget;
    auto* qf = new QFormLayout(q);
    rqd_ = spin(0, 100, 75, 0, " %", q);
    jn_ = new QComboBox(q);
    jr_ = new QComboBox(q);
    ja_ = new QComboBox(q);
    jw_ = new QComboBox(q);
    srf_ = new QComboBox(q);
    table(jn_, geotechnical::jnTable());
    table(jr_, geotechnical::jrTable());
    table(ja_, geotechnical::jaTable());
    table(jw_, geotechnical::jwTable());
    table(srf_, geotechnical::srfTable());
    span_ = spin(0.5, 100, 5, 2, " m", q);
    esr_ = spin(0.3, 5, 1.6, 2, {}, q);
    qf->addRow(tr("RQD"), rqd_);
    qf->addRow(tr("Jn (joint sets)"), jn_);
    qf->addRow(tr("Jr (roughness)"), jr_);
    qf->addRow(tr("Ja (alteration)"), ja_);
    qf->addRow(tr("Jw (water)"), jw_);
    qf->addRow(tr("SRF (stress)"), srf_);
    qf->addRow(tr("Span"), span_);
    qf->addRow(tr("ESR"), esr_);
    tabs_->addTab(q, tr("Q-system"));
    auto* r = new QWidget;
    auto* rf = new QFormLayout(r);
    ucs_ = spin(0.1, 400, 100, 1, " MPa", r);
    rmrRqd_ = spin(0, 100, 75, 0, " %", r);
    spacing_ = spin(0.01, 10, 0.5, 3, " m", r);
    condition_ = new QComboBox(r);
    condition_->addItem(tr("Very rough, not continuous, no separation"), static_cast<int>(geotechnical::JointCondition::VeryRough));
    condition_->addItem(tr("Slightly rough, separation < 1 mm"), static_cast<int>(geotechnical::JointCondition::SlightlyRough));
    condition_->addItem(tr("Slightly rough, highly weathered walls"), static_cast<int>(geotechnical::JointCondition::SlightlyRoughHighlyWeathered));
    condition_->addItem(tr("Slickensided or gouge < 5 mm"), static_cast<int>(geotechnical::JointCondition::Slickensided));
    condition_->addItem(tr("Soft gouge > 5 mm"), static_cast<int>(geotechnical::JointCondition::SoftGouge));
    water_ = new QComboBox(r);
    water_->addItem(tr("Completely dry"), static_cast<int>(geotechnical::GroundwaterCondition::CompletelyDry));
    water_->addItem(tr("Damp"), static_cast<int>(geotechnical::GroundwaterCondition::Damp));
    water_->addItem(tr("Wet"), static_cast<int>(geotechnical::GroundwaterCondition::Wet));
    water_->addItem(tr("Dripping"), static_cast<int>(geotechnical::GroundwaterCondition::Dripping));
    water_->addItem(tr("Flowing"), static_cast<int>(geotechnical::GroundwaterCondition::Flowing));
    orientation_ = new QComboBox(r);
    orientation_->addItem(tr("(no adjustment)"), -1);
    for (auto oc : {geotechnical::OrientationClass::VeryFavourable, geotechnical::OrientationClass::Favourable, geotechnical::OrientationClass::Fair,
                    geotechnical::OrientationClass::Unfavourable, geotechnical::OrientationClass::VeryUnfavourable})
        orientation_->addItem(qstr(geotechnical::toString(oc)), static_cast<int>(oc));
    rf->addRow(tr("Intact UCS"), ucs_);
    rf->addRow(tr("RQD"), rmrRqd_);
    rf->addRow(tr("Discontinuity spacing"), spacing_);
    rf->addRow(tr("Condition"), condition_);
    rf->addRow(tr("Groundwater"), water_);
    rf->addRow(tr("Orientation (tunnels)"), orientation_);
    tabs_->addTab(r, tr("RMR89"));
    lay->addWidget(tabs_);
    result_ = new QLabel(this);
    result_->setWordWrap(true);
    lay->addWidget(result_);
    lay->addWidget(buttons(this, tr("Record"), [this] { return !record().isNil(); }));
    for (auto* c : findChildren<QComboBox*>()) connect(c, &QComboBox::activated, this, &ClassificationDialog::recompute);
    for (auto* s : findChildren<QDoubleSpinBox*>()) connect(s, &QDoubleSpinBox::valueChanged, this, &ClassificationDialog::recompute);
    connect(tabs_, &QTabWidget::currentChanged, this, &ClassificationDialog::recompute);
    recompute();
}

void ClassificationDialog::recompute() {
    try {
        if (tabs_->currentIndex() == 0) {
            geotechnical::QInputs in{rqd_->value(), jn_->currentData().toDouble(), jr_->currentData().toDouble(), ja_->currentData().toDouble(),
                                     jw_->currentData().toDouble(), srf_->currentData().toDouble()};
            const auto q = geotechnical::computeQ(in);
            const auto s = geotechnical::qSupport(in, span_->value(), span_->value(), esr_->value());
            QString t = tr("<b>Q = %1</b> (%2) — block size %3, shear strength %4, active stress %5<br>Max unsupported span %6 m%7; roof bolts %8 m; RMR ≈ %9")
                            .arg(q.q, 0, 'g', 3)
                            .arg(qstr(q.rockClass))
                            .arg(q.blockSize, 0, 'g', 3)
                            .arg(q.shearStrength, 0, 'g', 3)
                            .arg(q.activeStress, 0, 'g', 3)
                            .arg(s.maxUnsupportedSpan, 0, 'f', 1)
                            .arg(s.unsupportedSpanExceeded ? tr(" — <b>span exceeds it: support required</b>") : QString())
                            .arg(s.roofBoltLength, 0, 'f', 1)
                            .arg(geotechnical::rmrFromQBarton1995(q.q), 0, 'f', 0);
            for (const auto& w : q.warnings) t += "<br><i>" + qstr(w) + "</i>";
            result_->setText(t);
        } else {
            geotechnical::RmrInputs in;
            in.ucsMPa = ucs_->value();
            in.rqd = rmrRqd_->value();
            in.spacingM = spacing_->value();
            in.condition = static_cast<geotechnical::JointCondition>(condition_->currentData().toInt());
            in.groundwater = static_cast<geotechnical::GroundwaterCondition>(water_->currentData().toInt());
            if (orientation_->currentData().toInt() >= 0) in.orientation = static_cast<geotechnical::OrientationClass>(orientation_->currentData().toInt());
            const auto r = geotechnical::computeRmr89(in);
            result_->setText(tr("<b>RMR89 = %1</b> (%2): basic %3, adjustment %4<br>Stand-up time %5; cohesion %6; friction %7; GSI ≈ %8")
                                 .arg(r.rmr, 0, 'f', 0)
                                 .arg(qstr(r.rockClass))
                                 .arg(r.basic, 0, 'f', 0)
                                 .arg(r.adjustment, 0, 'f', 0)
                                 .arg(qstr(r.standUpTime), qstr(r.cohesion), qstr(r.frictionAngle))
                                 .arg(geotechnical::gsiFromRmr89(r), 0, 'f', 0));
        }
    } catch (const std::exception& e) {
        result_->setText(tr("<span style='color:#d32f2f'>%1</span>").arg(qstr(e.what()).toHtmlEscaped()));
    }
}

Uuid ClassificationDialog::record() {
    const Uuid dev = comboUuid(development_);
    Vec3 at;
    if (const auto* d = ctx_.project().model().developments.find(dev)) {
        const auto cl = mine_design::centrelinePoints(ctx_.project(), *d);
        if (auto p = geometry::pointAtChainage(cl, chainage_->value() - d->chainageFrom)) at = *p;
    }
    Uuid id;
    const auto date = fromQDate(date_->date());
    ctx_.tryEdit(tr("Record classification"), [&] {
        if (tabs_->currentIndex() == 0) {
            geotechnical::QInputs in{rqd_->value(), jn_->currentData().toDouble(), jr_->currentData().toDouble(), ja_->currentData().toDouble(),
                                     jw_->currentData().toDouble(), srf_->currentData().toDouble()};
            id = geotechnical::recordQ(ctx_.project(), dev, chainage_->value(), at, in, sstr(mappedBy_->text()), date, sstr(notes_->text()));
        } else {
            geotechnical::RmrInputs in;
            in.ucsMPa = ucs_->value();
            in.rqd = rmrRqd_->value();
            in.spacingM = spacing_->value();
            in.condition = static_cast<geotechnical::JointCondition>(condition_->currentData().toInt());
            in.groundwater = static_cast<geotechnical::GroundwaterCondition>(water_->currentData().toInt());
            if (orientation_->currentData().toInt() >= 0) in.orientation = static_cast<geotechnical::OrientationClass>(orientation_->currentData().toInt());
            id = geotechnical::recordRmr(ctx_.project(), dev, chainage_->value(), at, in, sstr(mappedBy_->text()), date, sstr(notes_->text()));
        }
    });
    return id;
}

} // namespace openmine::app
