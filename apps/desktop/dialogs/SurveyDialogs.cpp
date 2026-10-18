// SPDX-License-Identifier: Apache-2.0
#include "SurveyDialogs.h"

#include "DialogUtil.h"
#include "../app/EntityInfo.h"
#include "../viewport/Tools.h"

#include "openmine/geometry/Polygon2D.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/geotechnical/Stereonet.h"
#include "openmine/mine_design/Development.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/reporting/ReportDocument.h"
#include "openmine/reporting/SurveyReports.h"
#include "openmine/survey/AsBuilt.h"
#include "openmine/survey/Conformance.h"
#include "openmine/survey/KdTree.h"
#include "openmine/survey/Meshing.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/Processing.h"
#include "openmine/survey/Registration.h"
#include "openmine/util/Csv.h"

#ifdef OPENMINE_APP_HAVE_PDAL
#include "openmine/pdalio/Pdal.h"
#endif
#ifdef OPENMINE_APP_HAVE_OPEN3D
#include "openmine/o3d/Open3d.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <numbers>

namespace openmine::app {

using nlohmann::json;

namespace {

QDialogButtonBox* okCancel(QDialog* d, const QString& okText, std::function<bool()> onOk) {
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
    bb->button(QDialogButtonBox::Ok)->setText(okText);
    QObject::connect(bb, &QDialogButtonBox::accepted, d, [d, onOk] {
        if (onOk()) d->accept();
    });
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    return bb;
}

bool havePdal() {
#ifdef OPENMINE_APP_HAVE_PDAL
    return true;
#else
    return false;
#endif
}

bool haveOpen3d() {
#ifdef OPENMINE_APP_HAVE_OPEN3D
    return true;
#else
    return false;
#endif
}

/// Engine label recorded in derivations and method attributes.
std::string engineLabel(const QString& engine) {
#ifdef OPENMINE_APP_HAVE_OPEN3D
    if (engine == "open3d") return o3d::engineName();
#endif
#ifdef OPENMINE_APP_HAVE_PDAL
    if (engine == "pdal") return "PDAL " + pdalio::pdalVersion();
#endif
    (void)engine;
    return "OpenMine";
}

/// Derived dataset job: read the parent in project coordinates, process, write the derived octree.
int derivedJob(AppContext& ctx, const QString& title, const Uuid& id, const std::string& name, const std::string& derivation,
               std::function<survey::PointBuffer(survey::PointBuffer&, jobs::Progress&, QString&)> process) {
    auto snap = ctx.writableSnapshot();
    AppContext* c = &ctx;
    return ctx.jobs().start(title, QObject::tr("Point cloud"), [snap, id, name, derivation, process, c, title](jobs::Progress& pr) -> UiApply {
        pr.subRange(0.0, 0.3);
        auto pts = survey::datasetPoints(*snap, id, true, std::nullopt, pr);
        pr.subRange(0.3, 0.8);
        QString message;
        auto out = process(pts, pr, message);
        if (out.empty()) throw ValidationError("The operation produced no points");
        pr.subRange(0.8, 1.0);
        auto prepared = std::make_shared<survey::PreparedDataset>(survey::prepareDerived(*snap, id, out, name, derivation, pr));
        return [c, prepared, message, title] {
            const Uuid nid = survey::commitDataset(c->project(), *prepared, sstr(title));
            c->report(QObject::tr("%1: %L2 points in '%3'%4").arg(title).arg(prepared->dataset.pointCount).arg(qstr(prepared->dataset.name), message.isEmpty() ? QString() : " — " + message));
            c->select(EntityKind::PointCloud, nid);
        };
    }, true);
}

} // namespace

// ---- import ------------------------------------------------------------------------------------------

PointCloudImportDialog::PointCloudImportDialog(AppContext& ctx, QStringList files, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Import point cloud"));
    resize(560, 480);
    auto* form = new QFormLayout(this);
    files_ = new QListWidget(this);
    files_->addItems(files);
    files_->setMaximumHeight(90);
    form->addRow(tr("Files"), files_);
    name_ = new QLineEdit(files.size() == 1 ? QFileInfo(files.front()).completeBaseName() : QString(), this);
    name_->setEnabled(files.size() == 1);
    name_->setPlaceholderText(tr("file name"));
    form->addRow(tr("Dataset name"), name_);
    crs_ = new QLineEdit(this);
    crs_->setPlaceholderText(tr("as declared by the file (e.g. EPSG:22285)"));
    crs_->setToolTip(tr("Recorded as provenance. Coordinates are stored exactly as read; no transformation is applied."));
    form->addRow(tr("Source CRS"), crs_);
    auto* dateRow = new QHBoxLayout;
    hasDate_ = new QCheckBox(tr("Scan date"), this);
    date_ = new QDateEdit(QDate::currentDate(), this);
    date_->setCalendarPopup(true);
    date_->setDisplayFormat("yyyy-MM-dd");
    date_->setEnabled(false);
    connect(hasDate_, &QCheckBox::toggled, date_, &QWidget::setEnabled);
    dateRow->addWidget(hasDate_);
    dateRow->addWidget(date_, 1);
    form->addRow(dateRow);
    scanner_ = new QLineEdit(this);
    form->addRow(tr("Scanner"), scanner_);
    development_ = new QComboBox(this);
    fillEntityCombo(development_, ctx_.project().model(), EntityKind::Development, {}, true);
    form->addRow(tr("Heading scanned"), development_);
    nodePoints_ = ispin(1000, 1000000, 20000, this);
    nodePoints_->setToolTip(tr("Octree nodes with more points are split (smaller = finer level of detail)"));
    grid_ = ispin(16, 512, 128, this);
    grid_->setToolTip(tr("Sampling grid per node (cells per axis)"));
    form->addRow(tr("Max points per node"), nodePoints_);
    form->addRow(tr("Node sampling grid"), grid_);
    QStringList exts;
    for (const auto& e : survey::readableExtensions()) exts << qstr(e);
    auto* note = new QLabel(tr("Readable formats in this build: %1. Points are stored bit-exact in an out-of-core octree in the project's asset folder.").arg(exts.join(" ")), this);
    note->setWordWrap(true);
    form->addRow(note);
    form->addRow(okCancel(this, tr("Import"), [this] { return !start().empty(); }));
}

std::vector<int> PointCloudImportDialog::start() {
    std::vector<int> ids;
    for (int i = 0; i < files_->count(); ++i) {
        const QString file = files_->item(i)->text();
        survey::ImportOptions o;
        o.name = files_->count() == 1 && !name_->text().trimmed().isEmpty() ? sstr(name_->text().trimmed()) : sstr(QFileInfo(file).completeBaseName());
        o.sourceCrs = sstr(crs_->text().trimmed());
        if (hasDate_->isChecked()) o.scanDate = fromQDate(date_->date());
        o.scanner = sstr(scanner_->text());
        o.developmentId = comboUuid(development_);
        o.octree.maxNodePoints = static_cast<std::size_t>(nodePoints_->value());
        o.octree.grid = grid_->value();
        auto snap = ctx_.writableSnapshot();
        AppContext* c = &ctx_;
        ids.push_back(ctx_.jobs().start(tr("Import %1").arg(QFileInfo(file).fileName()), tr("Point cloud"),
                                        [snap, file, o, c](jobs::Progress& pr) -> UiApply {
                                            auto prepared = std::make_shared<survey::PreparedDataset>(
                                                survey::prepareImport(*snap, std::filesystem::path(file.toStdU16String()), o, pr));
                                            return [c, prepared] {
                                                const Uuid id = survey::commitDataset(c->project(), *prepared);
                                                c->report(QObject::tr("Imported '%1': %L2 points (%3)")
                                                              .arg(qstr(prepared->dataset.name))
                                                              .arg(prepared->dataset.pointCount)
                                                              .arg(qstr(prepared->dataset.sourceFormat)));
                                                c->select(EntityKind::PointCloud, id);
                                            };
                                        },
                                        true));
    }
    return ids;
}

// ---- processing --------------------------------------------------------------------------------------

void CloudProcessDialog::setBox(const BoundingBox& box, bool keepInside) {
    if (op_ != Operation::CropBox) return;
    a_->setValue(box.min.x);
    b_->setValue(box.min.y);
    c_->setValue(box.min.z);
    d_->setValue(box.max.x);
    e_->setValue(box.max.y);
    f_->setValue(box.max.z);
    flag_->setChecked(keepInside);
}

bool CloudProcessDialog::setValue(const QString& field, double value) {
    QDoubleSpinBox* d = field == "a" ? a_ : field == "b" ? b_ : field == "c" ? c_ : field == "d" ? d_ : field == "e" ? e_ : field == "f" ? f_ : nullptr;
    if (d) {
        d->setValue(value);
        return std::abs(d->value() - value) <= std::pow(10.0, -d->decimals());
    }
    if (field == "k" && k_) {
        k_->setValue(static_cast<int>(std::lround(value)));
        return k_->value() == static_cast<int>(std::lround(value));
    }
    return false;
}

bool CloudProcessDialog::setMode(int modeData) {
    if (!mode_) return false;
    const int i = mode_->findData(modeData);
    if (i < 0) return false;
    mode_->setCurrentIndex(i);
    return true;
}

bool CloudProcessDialog::setFlag(bool on) {
    if (!flag_) return false;
    flag_->setChecked(on);
    return true;
}

bool CloudProcessDialog::setEngine(const QString& engine) {
    if (!engine_) return false;
    const int i = engine_->findData(engine);
    if (i < 0) return false;
    engine_->setCurrentIndex(i);
    return true;
}

void CloudProcessDialog::setOutputName(const QString& name) { name_->setText(name); }

QString CloudProcessDialog::title(Operation op) {
    switch (op) {
    case Operation::Subsample: return tr("Subsample point cloud");
    case Operation::Outliers: return tr("Remove outliers");
    case Operation::Normals: return tr("Estimate normals");
    case Operation::Ground: return tr("Classify ground (SMRF)");
    case Operation::Delaunay: return tr("Surface (Delaunay 2.5D)");
    case Operation::Poisson: return tr("Surface (Poisson)");
    case Operation::Distance: return tr("Distance to reference");
    case Operation::Statistics: return tr("Point-cloud statistics");
    case Operation::Planes: return tr("Extract planes (RANSAC)");
    case Operation::Segment: return tr("Segment into clusters");
    case Operation::CropBox: return tr("Crop by box");
    }
    return {};
}

CloudProcessDialog::CloudProcessDialog(AppContext& ctx, Operation op, const Uuid& datasetId, QWidget* parent)
    : QDialog(parent), ctx_(ctx), op_(op), dataset_(datasetId) {
    setWindowTitle(title(op));
    const auto& m = ctx_.project().model();
    const auto* d = m.pointClouds.find(datasetId);
    auto* form = new QFormLayout(this);
    form->addRow(new QLabel(tr("<b>%1</b> — %L2 points").arg(d ? qstr(d->name) : QString()).arg(d ? d->pointCount : 0), this));
    name_ = new QLineEdit(this);
    const QString base = d ? qstr(d->name) : QString();
    switch (op) {
    case Operation::Subsample:
        mode_ = new QComboBox(this);
        mode_->addItem(tr("Voxel grid — keep the point nearest each voxel centre"), 0);
        mode_->addItem(tr("Voxel grid — voxel centroids (new coordinates)"), 1);
        mode_->addItem(tr("Minimum spacing"), 2);
        mode_->addItem(tr("Random count"), 3);
        a_ = spin(0.001, 100, 0.05, 3, " m", this);
        k_ = ispin(1, 2000000000, 1000000, this);
        form->addRow(tr("Method"), mode_);
        form->addRow(tr("Voxel size / spacing"), a_);
        form->addRow(tr("Random: points to keep"), k_);
        name_->setText(base + tr(" (subsampled)"));
        break;
    case Operation::Outliers:
        mode_ = new QComboBox(this);
        mode_->addItem(tr("Statistical (mean distance to k neighbours)"), 0);
        mode_->addItem(tr("Radius (neighbours within r)"), 1);
        k_ = ispin(2, 200, 16, this);
        a_ = spin(0.1, 10, 2.0, 2, " σ", this);
        b_ = spin(0.001, 100, 0.1, 3, " m", this);
        form->addRow(tr("Method"), mode_);
        form->addRow(tr("k / minimum neighbours"), k_);
        form->addRow(tr("Std-dev ratio"), a_);
        form->addRow(tr("Radius"), b_);
        name_->setText(base + tr(" (cleaned)"));
        break;
    case Operation::Normals:
        mode_ = new QComboBox(this);
        mode_->addItem(tr("Orient up (+Z)"), 0);
        mode_->addItem(tr("Orient towards the dataset centroid"), 1);
        mode_->addItem(tr("Orient away from the dataset centroid"), 2);
        k_ = ispin(4, 200, 16, this);
        form->addRow(tr("Orientation"), mode_);
        form->addRow(tr("Neighbours (k)"), k_);
        name_->setText(base + tr(" (normals)"));
        break;
    case Operation::Ground:
        a_ = spin(0.1, 50, 1.0, 2, " m", this);
        b_ = spin(0.01, 2, 0.15, 3, "", this);
        c_ = spin(1, 200, 18, 1, " m", this);
        d_ = spin(0.05, 10, 0.5, 2, " m", this);
        form->addRow(tr("Cell size"), a_);
        form->addRow(tr("Slope (rise/run)"), b_);
        form->addRow(tr("Max window"), c_);
        form->addRow(tr("Elevation threshold"), d_);
        name_->setText(base + tr(" (ground classified)"));
        if (!havePdal()) form->addRow(new QLabel(tr("<i>Not available: SMRF ground classification uses PDAL, which this build does not include.</i>"), this));
        break;
    case Operation::Delaunay:
        a_ = spin(0, 100, 0.25, 3, " m", this);
        a_->setToolTip(tr("Voxel subsampling before triangulation (0 = all points)"));
        b_ = spin(0, 1000, 5.0, 2, " m", this);
        b_->setToolTip(tr("Remove triangles with a plan edge longer than this (0 = keep all)"));
        k_ = ispin(0, 50000000, 0, this);
        k_->setToolTip(tr("Decimate to this many triangles (0 = no decimation)"));
        form->addRow(tr("Subsample voxel"), a_);
        form->addRow(tr("Max edge length"), b_);
        form->addRow(tr("Target triangles"), k_);
        name_->setText(base + tr(" surface"));
        break;
    case Operation::Poisson:
        a_ = spin(0, 100, 0.05, 3, " m", this);
        k_ = ispin(5, 13, 9, this);
        mode_ = new QComboBox(this);
        mode_->addItem(tr("Normals up (surfaces)"), 0);
        mode_->addItem(tr("Normals towards the centroid (closed scans of excavations)"), 1);
        form->addRow(tr("Subsample voxel"), a_);
        form->addRow(tr("Octree depth"), k_);
        form->addRow(tr("Normal orientation (when estimated)"), mode_);
        d_ = spin(0, 0.5, 0.02, 3, "", this);
        d_->setToolTip(tr("Open3D: remove the vertices with the lowest sample density (this quantile) — the surface "
                          "Poisson extrapolates away from the scan"));
        form->addRow(tr("Trim low density (Open3D)"), d_);
        name_->setText(base + tr(" mesh"));
        if (!havePdal() && !haveOpen3d())
            form->addRow(new QLabel(tr("<i>Not available: Poisson reconstruction needs PDAL or Open3D, which this build does not include.</i>"), this));
        break;
    case Operation::Distance:
        reference_ = new QComboBox(this);
        for (const auto& c : m.pointClouds.all())
            if (c.id != datasetId) reference_->addItem(tr("Point cloud: %1").arg(qstr(c.name)), "c:" + uuidText(c.id));
        for (const auto& g : m.geometry.all())
            if (g.mesh && !g.mesh->triangles.empty()) reference_->addItem(tr("Mesh: %1").arg(displayName(m, {EntityKind::Geometry, g.id})), "m:" + uuidText(g.id));
        a_ = spin(0, 1e6, 0, 3, " m", this);
        a_->setToolTip(tr("Points farther than this get no value (0 = unlimited)"));
        flag_ = new QCheckBox(tr("Signed (by reference normals / mesh orientation)"), this);
        flag_->setChecked(true);
        form->addRow(tr("Reference"), reference_);
        form->addRow(tr("Max distance"), a_);
        form->addRow(QString(), flag_);
        name_->setText(base + tr(" (distance)"));
        break;
    case Operation::Statistics:
        k_ = ispin(100, 1000000, 10000, this);
        form->addRow(tr("Spacing samples"), k_);
        break;
    case Operation::Planes:
        a_ = spin(0.001, 10, 0.05, 3, " m", this);
        k_ = ispin(1, 50, 6, this);
        b_ = spin(10, 1e8, 500, 0, " pts", this);
        flag_ = new QCheckBox(tr("Store planes as structural measurements (set 'Scan planes')"), this);
        flag_->setChecked(true);
        form->addRow(tr("Distance threshold"), a_);
        form->addRow(tr("Max planes"), k_);
        form->addRow(tr("Min inliers"), b_);
        form->addRow(QString(), flag_);
        break;
    case Operation::Segment:
        mode_ = new QComboBox(this);
        mode_->addItem(tr("Connected components (points within a distance)"), 0);
        mode_->addItem(tr("Region growing on normals (smooth surfaces)"), 1);
        a_ = spin(0.001, 100, 0.25, 3, " m", this);
        a_->setToolTip(tr("Connected components: points closer than this are in the same cluster"));
        b_ = spin(0.1, 90, 5.0, 1, " °", this);
        b_->setToolTip(tr("Region growing: maximum angle between neighbouring normals"));
        k_ = ispin(1, 100000000, 50, this);
        flag_ = new QCheckBox(tr("Drop points in clusters smaller than the minimum size"), this);
        form->addRow(tr("Method"), mode_);
        form->addRow(tr("Distance tolerance"), a_);
        form->addRow(tr("Normal angle"), b_);
        form->addRow(tr("Minimum cluster size"), k_);
        form->addRow(QString(), flag_);
        name_->setText(base + tr(" (clusters)"));
        break;
    case Operation::CropBox: {
        BoundingBox bb = BoundingBox::empty();
        if (d && !d->storedBounds.isEmpty())
            for (int k = 0; k < 8; ++k)
                bb.expand(d->transform.apply({(k & 1) ? d->storedBounds.max.x : d->storedBounds.min.x, (k & 2) ? d->storedBounds.max.y : d->storedBounds.min.y,
                                              (k & 4) ? d->storedBounds.max.z : d->storedBounds.min.z}));
        if (bb.isEmpty()) bb = {{0, 0, 0}, {1, 1, 1}};
        a_ = spin(-1e8, 1e8, bb.min.x, 3, {}, this);
        b_ = spin(-1e8, 1e8, bb.min.y, 3, {}, this);
        c_ = spin(-1e8, 1e8, bb.min.z, 3, {}, this);
        d_ = spin(-1e8, 1e8, bb.max.x, 3, {}, this);
        e_ = spin(-1e8, 1e8, bb.max.y, 3, {}, this);
        f_ = spin(-1e8, 1e8, bb.max.z, 3, {}, this);
        flag_ = new QCheckBox(tr("Keep the points inside the box (otherwise remove them)"), this);
        flag_->setChecked(true);
        form->addRow(tr("Minimum E / X"), a_);
        form->addRow(tr("Minimum N / Y"), b_);
        form->addRow(tr("Minimum Z"), c_);
        form->addRow(tr("Maximum E / X"), d_);
        form->addRow(tr("Maximum N / Y"), e_);
        form->addRow(tr("Maximum Z"), f_);
        form->addRow(QString(), flag_);
        name_->setText(base + tr(" (cropped)"));
        break;
    }
    }
    // Engine: the built-in implementation or Open3D (Poisson: PDAL or Open3D).
    if (op == Operation::Subsample || op == Operation::Outliers || op == Operation::Normals || op == Operation::Poisson) {
        engine_ = new QComboBox(this);
        engine_->setObjectName("engine");
        if (op == Operation::Poisson) {
            if (havePdal()) engine_->addItem(tr("PDAL (filters.poisson)"), "pdal");
        } else {
            engine_->addItem(tr("OpenMine (built-in)"), "openmine");
        }
        if (haveOpen3d()) engine_->addItem(tr("Open3D"), "open3d");
        else engine_->setToolTip(tr("Open3D engines: not available in this build"));
        form->insertRow(1, tr("Engine"), engine_);
        if (op == Operation::Subsample) {
            // Open3D's voxel filter computes voxel centroids only.
            connect(engine_, &QComboBox::currentIndexChanged, this, [this] {
                const bool o3d = engine_->currentData().toString() == "open3d";
                if (o3d) mode_->setCurrentIndex(mode_->findData(1));
                mode_->setEnabled(!o3d);
            });
        }
        if (op == Operation::Poisson)
            connect(engine_, &QComboBox::currentIndexChanged, this, [this] { d_->setEnabled(engine_->currentData().toString() == "open3d"); });
        if (op == Operation::Poisson) d_->setEnabled(engine_->currentData().toString() == "open3d");
    }
    if (op != Operation::Statistics && op != Operation::Planes) form->addRow(op == Operation::Delaunay || op == Operation::Poisson ? tr("Object name") : tr("New dataset"), name_);
    auto* bb = okCancel(this, tr("Run"), [this] { return start() > 0; });
    if ((op == Operation::Ground && !havePdal()) || (op == Operation::Poisson && !havePdal() && !haveOpen3d())) bb->button(QDialogButtonBox::Ok)->setEnabled(false);
    form->addRow(bb);
}

int CloudProcessDialog::start() {
    const std::string name = sstr(name_->text().trimmed());
    const QString t = title(op_);
    auto& ctx = ctx_;
    AppContext* c = &ctx_;
    const Uuid id = dataset_;
    switch (op_) {
    case Operation::Subsample: {
        const int mode = mode_->currentData().toInt();
        const double size = a_->value();
        const auto count = static_cast<std::size_t>(k_->value());
        const QString engine = engine_->currentData().toString();
        if (engine == "open3d" && mode != 1) {
            ctx_.report(tr("Open3D subsamples to voxel centroids only"), MessageLevel::Error);
            return 0;
        }
        std::string deriv = mode == 3 ? "random subsample n=" + std::to_string(count)
                                      : (mode == 2 ? "minimum spacing " : (mode == 1 ? "voxel centroid " : "voxel nearest-to-centre ")) + std::to_string(size) + " m";
        if (engine == "open3d") deriv += " (" + engineLabel(engine) + ")";
        return derivedJob(ctx, t, id, name, deriv, [mode, size, count, engine](survey::PointBuffer& b, jobs::Progress&, QString& msg) {
            survey::PointBuffer out;
#ifdef OPENMINE_APP_HAVE_OPEN3D
            if (engine == "open3d") out = o3d::voxelDownsample(b, size);
            else
#endif
            if (mode == 0 || mode == 1) out = survey::voxelDownsample(b, size, mode == 0 ? survey::VoxelMode::NearestToCentre : survey::VoxelMode::Centroid);
            else if (mode == 2) out = survey::minimumSpacing(b, size);
            else out = survey::randomSubsample(b, count);
            msg = QObject::tr("%L1 of %L2 points kept").arg(static_cast<qulonglong>(out.size())).arg(static_cast<qulonglong>(b.size()));
            return out;
        });
    }
    case Operation::Outliers: {
        const int mode = mode_->currentData().toInt();
        const int k = k_->value();
        const double ratio = a_->value(), r = b_->value();
        const QString engine = engine_->currentData().toString();
        std::string deriv = mode == 0 ? "statistical outlier removal k=" + std::to_string(k) + " n=" + std::to_string(ratio)
                                      : "radius outlier removal r=" + std::to_string(r) + " min=" + std::to_string(k);
        if (engine == "open3d") deriv += " (" + engineLabel(engine) + ")";
        return derivedJob(ctx, t, id, name, deriv, [mode, k, ratio, r, engine](survey::PointBuffer& b, jobs::Progress& pr, QString& msg) {
            std::vector<char> keep;
#ifdef OPENMINE_APP_HAVE_OPEN3D
            if (engine == "open3d") keep = mode == 0 ? o3d::statisticalInliers(b, k, ratio) : o3d::radiusInliers(b, r, k);
            else
#endif
                keep = mode == 0 ? survey::statisticalInliers(b, k, ratio, pr) : survey::radiusInliers(b, r, k, pr);
            auto out = b.filtered(keep);
            msg = QObject::tr("%L1 outliers removed").arg(static_cast<qulonglong>(b.size() - out.size()));
            return out;
        });
    }
    case Operation::Normals: {
        const int mode = mode_->currentData().toInt();
        const int k = k_->value();
        const QString engine = engine_->currentData().toString();
        std::string deriv = "PCA normals k=" + std::to_string(k);
        if (engine == "open3d") deriv += " (" + engineLabel(engine) + ")";
        return derivedJob(ctx, t, id, name, deriv, [mode, k, engine](survey::PointBuffer& b, jobs::Progress& pr, QString&) {
            survey::NormalOptions o;
            o.k = k;
            if (mode != 0) {
                o.orientation = mode == 1 ? survey::NormalOptions::Orientation::TowardsViewpoint : survey::NormalOptions::Orientation::AwayFromViewpoint;
                o.viewpoint = survey::statistics(b, 0).centroid;
            }
#ifdef OPENMINE_APP_HAVE_OPEN3D
            if (engine == "open3d") {
                o3d::estimateNormals(b, o, pr);
                return b;
            }
#endif
            (void)engine;
            survey::estimateNormals(b, o, pr);
            return b;
        });
    }
    case Operation::Ground: {
#ifdef OPENMINE_APP_HAVE_PDAL
        pdalio::SmrfOptions o;
        o.cell = a_->value();
        o.slope = b_->value();
        o.window = c_->value();
        o.threshold = d_->value();
        return derivedJob(ctx, t, id, name, "SMRF ground classification", [o](survey::PointBuffer& b, jobs::Progress&, QString& msg) {
            const auto cls = pdalio::smrfGround(b, o);
            auto schema = b.schema();
            schema.classification = true;
            b.setSchema(schema);
            std::size_t ground = 0;
            for (std::size_t i = 0; i < b.size() && i < cls.size(); ++i) {
                b.classification[i] = cls[i];
                ground += cls[i] == 2;
            }
            msg = QObject::tr("%L1 ground points").arg(static_cast<qulonglong>(ground));
            return b;
        });
#else
        ctx_.report(tr("Ground classification needs PDAL, which this build does not include"), MessageLevel::Error);
        return 0;
#endif
    }
    case Operation::Delaunay:
    case Operation::Poisson: {
        const bool poisson = op_ == Operation::Poisson;
        const double voxel = a_->value();
        const double maxEdge = b_ ? b_->value() : 0.0;
        const int k = k_->value();
        const int orient = mode_ ? mode_->currentData().toInt() : 0;
        const QString engine = engine_ ? engine_->currentData().toString() : QString();
        const double densityQuantile = poisson ? d_->value() : 0.0;
        auto snap = ctx_.snapshot();
        return ctx_.jobs().start(t, tr("Point cloud"), [snap, id, voxel, maxEdge, k, poisson, orient, name, c, engine, densityQuantile](jobs::Progress& pr) -> UiApply {
            pr.subRange(0, 0.3);
            auto pts = survey::datasetPoints(*snap, id, true, std::nullopt, pr);
            if (voxel > 0) pts = survey::voxelDownsample(pts, voxel);
            pr.subRange(0.3, 1.0);
            auto mesh = std::make_shared<MeshData>();
            std::string method;
            if (!poisson) {
                survey::DelaunayOptions o;
                o.maxEdgeLength = maxEdge;
                survey::DelaunayReport rep;
                *mesh = survey::delaunay25D(pts.xyz, o, &rep, pr);
                if (k > 0 && mesh->triangles.size() > static_cast<std::size_t>(k)) {
                    survey::DecimateOptions d;
                    d.targetTriangles = static_cast<std::size_t>(k);
                    *mesh = survey::decimate(*mesh, d, pr);
                }
                method = "Delaunay 2.5D";
            } else {
                if (!pts.schema().normals) {
                    survey::NormalOptions no;
                    if (orient == 1) {
                        no.orientation = survey::NormalOptions::Orientation::TowardsViewpoint;
                        no.viewpoint = survey::statistics(pts, 0).centroid;
                    }
                    survey::estimateNormals(pts, no, pr);
                }
                method = "Poisson depth=" + std::to_string(k) + " (" + engineLabel(engine) + ")";
                if (engine == "open3d") {
#ifdef OPENMINE_APP_HAVE_OPEN3D
                    o3d::PoissonOptions po;
                    po.depth = k;
                    po.densityQuantile = densityQuantile;
                    auto res = o3d::poissonSurface(pts, po);
                    *mesh = std::move(res.mesh);
                    if (densityQuantile > 0) method += ", density trim q=" + std::to_string(densityQuantile);
#endif
                } else {
#ifdef OPENMINE_APP_HAVE_PDAL
                    pdalio::PoissonOptions po;
                    po.depth = k;
                    *mesh = pdalio::poissonSurface(pts, po);
#else
                    throw ValidationError("Poisson reconstruction needs PDAL or Open3D");
#endif
                }
                (void)densityQuantile;
            }
            return [c, mesh, name, method] {
                Uuid gid;
                c->tryEdit(QObject::tr("Add surface"), [&] {
                    auto& p = c->project();
                    CommandStack::MacroScope macro(p.commands(), "Add surface " + name);
                    GeometryObject g;
                    g.type = GeometryType::Mesh;
                    g.name = name;
                    g.layerId = workflowLayer(*c, "Surfaces", 0x9CCC65);
                    g.mesh = mesh;
                    ensureAttribute(p, "GeometryObject", "method", "Method");
                    g.attributes["method"] = method;
                    gid = ProjectEditor(p).addGeometry(std::move(g));
                    macro.commit();
                });
                if (!gid.isNil()) {
                    c->report(QObject::tr("Surface '%1': %L2 triangles (%3)").arg(qstr(name)).arg(static_cast<qulonglong>(mesh->triangles.size())).arg(qstr(method)));
                    c->select(EntityKind::Geometry, gid);
                }
            };
        }, false);
    }
    case Operation::Distance: {
        const QString ref = reference_->currentData().toString();
        if (ref.isEmpty()) {
            ctx_.report(tr("There is no reference point cloud or mesh"), MessageLevel::Error);
            return 0;
        }
        const bool isCloud = ref.startsWith("c:");
        const Uuid refId = uuidFrom(ref.mid(2));
        const double maxD = a_->value() > 0 ? a_->value() : 1e300;
        const bool sign = flag_->isChecked();
        auto snap = ctx_.snapshot();
        MeshPtr mesh;
        if (!isCloud)
            if (const auto* g = snap->model().geometry.find(refId)) mesh = g->mesh;
        const std::string refName = sstr(reference_->currentText());
        auto stats = std::make_shared<json>();
        const int jid = derivedJob(ctx, t, id, name, "distance to " + refName,
                                   [snap, isCloud, refId, mesh, maxD, sign, stats](survey::PointBuffer& b, jobs::Progress& pr, QString& msg) {
                                       std::vector<float> d;
                                       if (isCloud) {
                                           auto refPts = survey::datasetPoints(*snap, refId, true);
                                           if (sign && !refPts.schema().normals) survey::estimateNormals(refPts);
                                           d = survey::cloudToCloud(b, refPts, maxD, sign, pr);
                                       } else {
                                           if (!mesh) throw ValidationError("The reference mesh has no triangles");
                                           d = survey::cloudToMesh(b, *mesh, sign, maxD, pr);
                                       }
                                       const int si = b.scalarIndex("distance", true);
                                       b.scalars[static_cast<std::size_t>(si)] = d;
                                       std::vector<double> v;
                                       for (float x : d)
                                           if (std::isfinite(x)) v.push_back(x);
                                       if (!v.empty()) {
                                           std::sort(v.begin(), v.end());
                                           double mean = 0, sq = 0;
                                           for (double x : v) mean += x;
                                           mean /= v.size();
                                           for (double x : v) sq += (x - mean) * (x - mean);
                                           (*stats) = {{"count", v.size()}, {"mean", mean}, {"std", std::sqrt(sq / v.size())}, {"min", v.front()}, {"max", v.back()},
                                                       {"p05", v[v.size() / 20]}, {"median", v[v.size() / 2]}, {"p95", v[v.size() * 19 / 20]}};
                                           msg = QObject::tr("mean %1 m, σ %2 m, 95 %% within %3 m").arg((*stats)["mean"].get<double>(), 0, 'f', 4).arg((*stats)["std"].get<double>(), 0, 'f', 4).arg((*stats)["p95"].get<double>(), 0, 'f', 4);
                                       }
                                       return b;
                                   });
        // Record the analysis once the derived dataset exists.
        connect(&ctx_.jobs(), &JobManager::jobFinished, c, [c, jid, stats, id, refId, isCloud, maxD, sign, refName](int finished, bool ok, const QString&) {
            if (finished != jid || !ok || stats->is_null()) return;
            c->tryEdit(QObject::tr("Store distance analysis"), [&] {
                storeAnalysis(c->project(), isCloud ? AnalysisKind::DistanceCloudToCloud : AnalysisKind::DistanceCloudToMesh, "Distance to " + refName,
                              {{"compared", id.toString()}, {"reference", refId.toString()}}, {{"maxDistance", maxD < 1e300 ? json(maxD) : json(nullptr)}, {"signed", sign}}, *stats);
            });
        }, Qt::SingleShotConnection);
        return jid;
    }
    case Operation::Statistics: {
        const auto samples = static_cast<std::size_t>(k_->value());
        auto snap = ctx_.snapshot();
        return ctx_.jobs().start(t, tr("Point cloud"), [snap, id, samples, c](jobs::Progress& pr) -> UiApply {
            const auto pts = survey::datasetPoints(*snap, id, true, std::nullopt, pr);
            const auto s = std::make_shared<survey::CloudStatistics>(survey::statistics(pts, samples));
            return [c, s, id] {
                const QString text = QObject::tr("%L1 points; centroid %2, %3, %4; mean spacing %5 m, median %6 m; extent %7 × %8 × %9 m")
                                         .arg(static_cast<qulonglong>(s->count))
                                         .arg(s->centroid.x, 0, 'f', 3).arg(s->centroid.y, 0, 'f', 3).arg(s->centroid.z, 0, 'f', 3)
                                         .arg(s->meanSpacing, 0, 'f', 4).arg(s->medianSpacing, 0, 'f', 4)
                                         .arg(s->bounds.size().x, 0, 'f', 2).arg(s->bounds.size().y, 0, 'f', 2).arg(s->bounds.size().z, 0, 'f', 2);
                c->report(text);
                c->tryEdit(QObject::tr("Store statistics"), [&] {
                    storeAnalysis(c->project(), AnalysisKind::Other, "Point-cloud statistics", {{"dataset", id.toString()}}, json::object(),
                                  {{"count", s->count}, {"meanSpacing", s->meanSpacing}, {"medianSpacing", s->medianSpacing},
                                   {"centroid", {s->centroid.x, s->centroid.y, s->centroid.z}},
                                   {"min", {s->bounds.min.x, s->bounds.min.y, s->bounds.min.z}}, {"max", {s->bounds.max.x, s->bounds.max.y, s->bounds.max.z}}});
                });
            };
        });
    }
    case Operation::Segment: {
        const int mode = mode_->currentData().toInt();
        const double tol = a_->value(), angle = b_->value();
        const auto minSize = static_cast<std::size_t>(k_->value());
        const bool drop = flag_->isChecked();
        const std::string deriv = mode == 0 ? "connected components tol=" + std::to_string(tol) + " min=" + std::to_string(minSize)
                                            : "region growing angle=" + std::to_string(angle) + " min=" + std::to_string(minSize);
        return derivedJob(ctx, t, id, name, deriv, [mode, tol, angle, minSize, drop](survey::PointBuffer& b, jobs::Progress& pr, QString& msg) {
            std::vector<int> labels;
            if (mode == 0) labels = survey::euclideanClusters(b, tol, minSize, SIZE_MAX, pr);
            else {
                if (!b.schema().normals) {
                    survey::NormalOptions no;
                    no.storeCurvature = true;
                    survey::estimateNormals(b, no, pr);
                }
                labels = survey::regionGrowing(b, 16, angle, minSize, pr);
            }
            const int ci = b.scalarIndex("cluster", true);
            int clusters = 0;
            std::size_t unclustered = 0;
            for (std::size_t i = 0; i < b.size(); ++i) {
                b.scalars[static_cast<std::size_t>(ci)][i] = static_cast<float>(labels[i]);
                clusters = std::max(clusters, labels[i] + 1);
                unclustered += labels[i] < 0 ? 1 : 0;
            }
            msg = QObject::tr("%1 clusters; %L2 points in no cluster%3").arg(clusters).arg(static_cast<qulonglong>(unclustered))
                      .arg(drop && unclustered ? QObject::tr(" (dropped)") : QString());
            if (!drop) return b;
            std::vector<char> keep(b.size());
            for (std::size_t i = 0; i < b.size(); ++i) keep[i] = labels[i] >= 0 ? 1 : 0;
            return b.filtered(keep);
        });
    }
    case Operation::CropBox: {
        BoundingBox box{{a_->value(), b_->value(), c_->value()}, {d_->value(), e_->value(), f_->value()}};
        if (!(box.max.x > box.min.x && box.max.y > box.min.y && box.max.z > box.min.z)) {
            ctx_.report(tr("Each maximum must exceed its minimum"), MessageLevel::Error);
            return 0;
        }
        const bool inside = flag_->isChecked();
        const std::string deriv = std::string(inside ? "box crop (inside) " : "box crop (outside) ") + reporting::fmtNumber(box.min.x, 3) + "," +
                                  reporting::fmtNumber(box.min.y, 3) + "," + reporting::fmtNumber(box.min.z, 3) + " – " + reporting::fmtNumber(box.max.x, 3) + "," +
                                  reporting::fmtNumber(box.max.y, 3) + "," + reporting::fmtNumber(box.max.z, 3);
        return derivedJob(ctx, t, id, name, deriv, [box, inside](survey::PointBuffer& b, jobs::Progress&, QString& msg) {
            auto keep = survey::selectBox(b, box);
            if (!inside) keep = survey::invert(std::move(keep));
            auto out = b.filtered(keep);
            if (out.empty()) throw ValidationError("No points remain after the crop");
            msg = QObject::tr("%L1 of %L2 points kept").arg(static_cast<qulonglong>(out.size())).arg(static_cast<qulonglong>(b.size()));
            return out;
        });
    }
    case Operation::Planes: {
        const double thr = a_->value();
        const int maxPlanes = k_->value();
        const auto minIn = static_cast<std::size_t>(b_->value());
        const bool store = flag_->isChecked();
        auto snap = ctx_.snapshot();
        return ctx_.jobs().start(t, tr("Point cloud"), [snap, id, thr, maxPlanes, minIn, store, c](jobs::Progress& pr) -> UiApply {
            auto pts = survey::datasetPoints(*snap, id, true, std::nullopt, pr);
            if (pts.size() > 2'000'000) pts = survey::randomSubsample(pts, 2'000'000);
            auto planes = std::make_shared<std::vector<survey::PlaneFit>>(survey::ransacPlanes(pts, thr, maxPlanes, minIn));
            return [c, planes, store, id] {
                QStringList lines;
                std::vector<StructuralMeasurement> ms;
                for (const auto& pl : *planes) {
                    const auto line = geotechnical::lineOf(pl.normal);
                    const auto plane = geotechnical::planeOfPole(line);
                    lines << QObject::tr("%1/%2 (%L3 points, RMS %4 m)")
                                 .arg(plane.dip, 2, 'f', 0, QChar('0'))
                                 .arg(plane.dipDirection, 3, 'f', 0, QChar('0'))
                                 .arg(static_cast<qulonglong>(pl.inliers.size()))
                                 .arg(pl.rms, 0, 'f', 4);
                    StructuralMeasurement s;
                    s.setName = "Scan planes";
                    s.type = StructureType::Joint;
                    s.dip = plane.dip;
                    s.dipDirection = plane.dipDirection;
                    s.location = pl.centroid;
                    if (const auto* d = c->project().model().pointClouds.find(id)) s.developmentId = d->developmentId;
                    ms.push_back(s);
                }
                c->report(QObject::tr("%1 planes: %2").arg(planes->size()).arg(lines.join("; ")));
                if (store && !ms.empty())
                    c->tryEdit(QObject::tr("Store scan planes"), [&] {
                        auto& p = c->project();
                        CommandStack::MacroScope macro(p.commands(), "Store scan planes as structures");
                        ProjectEditor ed(p);
                        for (auto& s : ms) ed.add(s);
                        macro.commit();
                    });
            };
        });
    }
    }
    return 0;
}

int startCropJob(AppContext& ctx, const Uuid& datasetId, const std::vector<Vec3>& polygon, Vec3 viewNormal, bool keepInside) {
    const auto* d = ctx.project().model().pointClouds.find(datasetId);
    if (!d || polygon.size() < 3) return 0;
    // Polygon in a 2D frame of the view plane: u, v ⟂ viewNormal (plan views: x, y).
    Vec3 n = viewNormal.normalized();
    if (n.length() < 1e-9) n = {0, 0, 1};
    const Vec3 u = std::abs(n.z) > 0.9 ? Vec3{1, 0, 0} : Vec3{-n.y, n.x, 0}.normalized();
    const Vec3 v = cross(n, u);
    geometry::Ring ring;
    for (const auto& p : polygon) ring.push_back({dot(p, u), dot(p, v)});
    const std::string name = d->name + (keepInside ? " (cropped)" : " (polygon removed)");
    return derivedJob(ctx, QObject::tr("Crop point cloud"), datasetId, name, keepInside ? "crop by polygon" : "remove polygon",
                      [ring, u, v, keepInside](survey::PointBuffer& b, jobs::Progress&, QString& msg) {
                          std::vector<char> keep(b.size(), 0);
                          for (std::size_t i = 0; i < b.size(); ++i) {
                              const bool in = geometry::contains(ring, Vec2{dot(b.xyz[i], u), dot(b.xyz[i], v)});
                              keep[i] = in == keepInside ? 1 : 0;
                          }
                          auto out = b.filtered(keep);
                          msg = QObject::tr("%L1 of %L2 points kept").arg(static_cast<qulonglong>(out.size())).arg(static_cast<qulonglong>(b.size()));
                          return out;
                      });
}

// ---- registration by pairs -------------------------------------------------------------------------------

namespace {
enum PairCol { PName, SX, SY, SZ, TX, TY, TZ, PControl, PRole, PResidual, NPair };
}

RegistrationDialog::RegistrationDialog(AppContext& ctx, ToolController& tools, const Uuid& datasetId, QWidget* parent)
    : QDialog(parent), ctx_(ctx), tools_(tools) {
    setWindowTitle(tr("Register point cloud — point pairs / control"));
    resize(980, 520);
    setAttribute(Qt::WA_DeleteOnClose);
    auto* lay = new QVBoxLayout(this);
    auto* top = new QFormLayout;
    dataset_ = new QComboBox(this);
    fillEntityCombo(dataset_, ctx_.project().model(), EntityKind::PointCloud, datasetId);
    top->addRow(tr("Moving dataset"), dataset_);
    scale_ = new QCheckBox(tr("Allow uniform scale (similarity transform)"), this);
    top->addRow(QString(), scale_);
    lay->addLayout(top);
    auto* help = new QLabel(tr("Source points are in the dataset's <i>stored</i> coordinates (pick them on the cloud). Targets are project coordinates or survey "
                               "control points. <b>Fit</b> pairs determine the transform; <b>Check</b> pairs are independent and give the positional accuracy."),
                            this);
    help->setWordWrap(true);
    lay->addWidget(help);
    pairs_ = new QTableWidget(0, NPair, this);
    pairs_->setHorizontalHeaderLabels({tr("Name"), tr("Source X"), tr("Source Y"), tr("Source Z"), tr("Target X"), tr("Target Y"), tr("Target Z"), tr("Control point"),
                                       tr("Role"), tr("Residual (m)")});
    pairs_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    lay->addWidget(pairs_, 1);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add pair"), this);
    auto* pickS = new QPushButton(tr("Pick source on cloud"), this);
    auto* pickT = new QPushButton(tr("Pick target"), this);
    auto* del = new QPushButton(tr("Remove"), this);
    auto* comp = new QPushButton(tr("Compute"), this);
    auto* store = new QPushButton(tr("Store (not applied)"), this);
    auto* apply = new QPushButton(tr("Apply registration"), this);
    for (auto* b : {add, pickS, pickT, del}) row->addWidget(b);
    row->addStretch(1);
    for (auto* b : {comp, store, apply}) row->addWidget(b);
    lay->addLayout(row);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    connect(add, &QPushButton::clicked, this, [this] { addPair(tr("P%1").arg(pairs_->rowCount() + 1), {}, std::nullopt); });
    connect(del, &QPushButton::clicked, this, [this] {
        if (pairs_->currentRow() >= 0) pairs_->removeRow(pairs_->currentRow());
    });
    connect(pickS, &QPushButton::clicked, this, [this] {
        int r = pairs_->currentRow();
        if (r < 0) {
            addPair(tr("P%1").arg(pairs_->rowCount() + 1), {}, std::nullopt);
            r = pairs_->rowCount() - 1;
        }
        QPointer<RegistrationDialog> self(this);
        tools_.requestCloudPoint(tr("REGISTER: pick the source point on the moving cloud"), [self, r](Uuid ds, Vec3 stored, Vec3) {
            if (!self || r >= self->pairs_->rowCount()) return;
            if (ds != comboUuid(self->dataset_)) {
                self->ctx_.report(tr("That point is on another dataset"), MessageLevel::Warning);
                return;
            }
            for (int k = 0; k < 3; ++k) self->pairs_->item(r, SX + k)->setText(QString::number(stored[k], 'f', 4));
            self->raise();
        });
    });
    connect(pickT, &QPushButton::clicked, this, [this] {
        const int r = pairs_->currentRow();
        if (r < 0) return;
        QPointer<RegistrationDialog> self(this);
        tools_.requestPoint(tr("REGISTER: pick the target point (project coordinates)"), [self, r](Vec3 p) {
            if (!self || r >= self->pairs_->rowCount()) return;
            for (int k = 0; k < 3; ++k) self->pairs_->item(r, TX + k)->setText(QString::number(p[k], 'f', 4));
            self->raise();
        });
    });
    connect(comp, &QPushButton::clicked, this, &RegistrationDialog::compute);
    connect(store, &QPushButton::clicked, this, [this] {
        if (commit(false)) close();
    });
    connect(apply, &QPushButton::clicked, this, [this] {
        if (commit(true)) close();
    });
}

void RegistrationDialog::addPair(const QString& name, Vec3 source, std::optional<Vec3> target, const Uuid& controlId, bool check) {
    const int r = pairs_->rowCount();
    pairs_->insertRow(r);
    pairs_->setItem(r, PName, new QTableWidgetItem(name));
    for (int k = 0; k < 3; ++k) {
        pairs_->setItem(r, SX + k, new QTableWidgetItem(QString::number(source[k], 'f', 4)));
        pairs_->setItem(r, TX + k, new QTableWidgetItem(target ? QString::number((*target)[k], 'f', 4) : QString()));
    }
    auto* cp = new QComboBox;
    fillEntityCombo(cp, ctx_.project().model(), EntityKind::ControlPoint, controlId, true);
    connect(cp, &QComboBox::activated, this, [this, cp, r](int) {
        if (const auto* c = ctx_.project().model().controlPoints.find(comboUuid(cp)))
            for (int k = 0; k < 3; ++k) pairs_->item(r, TX + k)->setText(QString::number(c->position[k], 'f', 4));
    });
    pairs_->setCellWidget(r, PControl, cp);
    auto* role = new QComboBox;
    role->addItems({tr("Fit"), tr("Check")});
    role->setCurrentIndex(check ? 1 : 0);
    pairs_->setCellWidget(r, PRole, role);
    auto* res = new QTableWidgetItem;
    res->setFlags(Qt::ItemIsEnabled);
    pairs_->setItem(r, PResidual, res);
    if (const auto* c = ctx_.project().model().controlPoints.find(controlId))
        for (int k = 0; k < 3; ++k) pairs_->item(r, TX + k)->setText(QString::number(c->position[k], 'f', 4));
}

bool RegistrationDialog::compute() {
    std::vector<RegistrationPair> pairs;
    bool allControl = true;
    for (int r = 0; r < pairs_->rowCount(); ++r) {
        RegistrationPair p;
        p.name = sstr(pairs_->item(r, PName)->text());
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            bool a = false, b = false;
            p.source[k] = QLocale::c().toDouble(pairs_->item(r, SX + k)->text(), &a);
            p.target[k] = QLocale::c().toDouble(pairs_->item(r, TX + k)->text(), &b);
            ok = ok && a && b;
        }
        if (!ok) {
            ctx_.report(tr("Pair %1 has missing coordinates").arg(r + 1), MessageLevel::Error);
            return false;
        }
        p.targetControlId = comboUuid(static_cast<QComboBox*>(pairs_->cellWidget(r, PControl)));
        allControl = allControl && !p.targetControlId.isNil();
        p.role = static_cast<QComboBox*>(pairs_->cellWidget(r, PRole))->currentIndex() == 1 ? PairRole::Check : PairRole::Fit;
        pairs.push_back(p);
    }
    try {
        const Uuid ds = comboUuid(dataset_);
        result_ = allControl ? survey::computeControlRegistration(ctx_.project(), ds, pairs, scale_->isChecked())
                             : survey::computePairRegistration(ctx_.project(), ds, pairs, scale_->isChecked());
    } catch (const std::exception& e) {
        result_.reset();
        summary_->setText(tr("<span style='color:#d32f2f'>%1</span>").arg(qstr(e.what()).toHtmlEscaped()));
        return false;
    }
    refreshResiduals();
    return true;
}

void RegistrationDialog::refreshResiduals() {
    if (!result_) return;
    for (int r = 0; r < pairs_->rowCount() && r < static_cast<int>(result_->pairs.size()); ++r)
        pairs_->item(r, PResidual)->setText(QString::number(result_->pairs[static_cast<std::size_t>(r)].residual.length(), 'f', 4));
    QString s = tr("Fit: RMS %1 m, max %2 m over %3 pairs (optimistic). ").arg(result_->fitRms, 0, 'f', 4).arg(result_->fitMax, 0, 'f', 4).arg(result_->fitCount);
    s += result_->checkCount > 0 ? tr("Independent check: RMS %1 m, max %2 m over %3 points.").arg(result_->checkRms, 0, 'f', 4).arg(result_->checkMax, 0, 'f', 4).arg(result_->checkCount)
                                 : tr("<b>No check points</b>: the positional accuracy is not demonstrated.");
    const Vec3 t = result_->transform.translationPart();
    s += tr("<br>Translation %1, %2, %3 m; scale %4").arg(t.x, 0, 'f', 4).arg(t.y, 0, 'f', 4).arg(t.z, 0, 'f', 4).arg(result_->transform.uniformScale(), 0, 'f', 8);
    summary_->setText(s);
}

bool RegistrationDialog::commit(bool apply) {
    if (!result_ && !compute()) return false;
    Uuid id;
    if (!ctx_.tryEdit(apply ? tr("Apply registration") : tr("Store registration"), [&] { id = survey::commitRegistration(ctx_.project(), *result_, apply); })) return false;
    ctx_.report(apply ? tr("Registration applied: the dataset transform changed; stored coordinates are unchanged") : tr("Registration stored (not applied)"));
    ctx_.select(EntityKind::Registration, id);
    return true;
}

// ---- ICP ---------------------------------------------------------------------------------------------

IcpDialog::IcpDialog(AppContext& ctx, const Uuid& movingId, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Register point cloud — ICP"));
    resize(560, 460);
    auto* form = new QFormLayout(this);
    const auto& m = ctx_.project().model();
    moving_ = new QComboBox(this);
    target_ = new QComboBox(this);
    fillEntityCombo(moving_, m, EntityKind::PointCloud, movingId);
    fillEntityCombo(target_, m, EntityKind::PointCloud, {}, false, [movingId](const Uuid& id) { return id != movingId; });
    mode_ = new QComboBox(this);
    mode_->addItem(tr("Point to plane (recommended)"), static_cast<int>(survey::IcpMode::PointToPlane));
    mode_->addItem(tr("Point to point"), static_cast<int>(survey::IcpMode::PointToPoint));
    maxCorr_ = spin(0.001, 100, 0.5, 3, " m", this);
    iterations_ = ispin(1, 1000, 50, this);
    trim_ = spin(0, 0.9, 0.1, 2, "", this);
    sample_ = ispin(0, 10000000, 50000, this);
    engine_ = new QComboBox(this);
    engine_->setObjectName("engine");
    for (const auto& e : survey::icpEngines()) engine_->addItem(qstr(survey::icpEngineDescription(e)), qstr(e));
    connect(engine_, &QComboBox::currentIndexChanged, this, [this] {
        // Open3D has no trimmed ICP.
        const bool builtIn = engine_->currentData().toString() == "OpenMine";
        if (!builtIn) trim_->setValue(0);
        trim_->setEnabled(builtIn);
        trim_->setToolTip(builtIn ? QString() : tr("Not available: this engine has no trimmed ICP"));
    });
    form->addRow(tr("Engine"), engine_);
    form->addRow(tr("Moving"), moving_);
    form->addRow(tr("Target (reference)"), target_);
    form->addRow(tr("Mode"), mode_);
    form->addRow(tr("Max correspondence"), maxCorr_);
    form->addRow(tr("Max iterations"), iterations_);
    form->addRow(tr("Trim worst fraction"), trim_);
    form->addRow(tr("Sample points (0 = all)"), sample_);
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setPlainText(tr("ICP refines an approximate alignment (register by point pairs first when the clouds are far apart). "
                          "Its inlier RMS measures internal consistency, not positional accuracy."));
    form->addRow(log_);
    auto* row = new QHBoxLayout;
    auto* store = new QPushButton(tr("Run and store"), this);
    auto* apply = new QPushButton(tr("Run and apply"), this);
    auto* close = new QPushButton(tr("Close"), this);
    row->addStretch(1);
    row->addWidget(store);
    row->addWidget(apply);
    row->addWidget(close);
    form->addRow(row);
    connect(store, &QPushButton::clicked, this, [this] { start(false); });
    connect(apply, &QPushButton::clicked, this, [this] { start(true); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
}

int IcpDialog::start(bool apply) {
    const Uuid mov = comboUuid(moving_), tgt = comboUuid(target_);
    if (mov.isNil() || tgt.isNil() || mov == tgt) {
        ctx_.report(tr("Choose two different datasets"), MessageLevel::Error);
        return 0;
    }
    survey::IcpOptions o;
    o.mode = static_cast<survey::IcpMode>(mode_->currentData().toInt());
    o.maxCorrespondence = maxCorr_->value();
    o.maxIterations = iterations_->value();
    o.trimFraction = trim_->value();
    o.sampleSize = static_cast<std::size_t>(sample_->value());
    o.engine = sstr(engine_->currentData().toString());
    auto snap = ctx_.snapshot();
    AppContext* c = &ctx_;
    QPointer<QPlainTextEdit> log(log_);
    log_->appendPlainText(tr("\nRunning ICP…"));
    return ctx_.jobs().start(tr("ICP registration"), tr("Registration"), [snap, mov, tgt, o, apply, c, log](jobs::Progress& pr) -> UiApply {
        auto r = std::make_shared<Registration>(survey::computeIcpRegistration(*snap, mov, tgt, o, {}, pr));
        return [c, r, apply, log] {
            const QString text = QObject::tr("Fitness %1, inlier RMS %2 m, %3 iterations, %4")
                                     .arg(r->icpFitness, 0, 'f', 3)
                                     .arg(r->icpInlierRms, 0, 'f', 4)
                                     .arg(r->icpIterations)
                                     .arg(r->icpConverged ? QObject::tr("converged") : QObject::tr("NOT converged"));
            if (log) log->appendPlainText(text);
            Uuid id;
            if (c->tryEdit(QObject::tr("Store ICP registration"), [&] { id = survey::commitRegistration(c->project(), *r, apply); })) {
                c->report(QObject::tr("ICP registration %1: %2").arg(apply ? QObject::tr("applied") : QObject::tr("stored")).arg(text));
                c->select(EntityKind::Registration, id);
            }
        };
    });
}

// ---- as-built ------------------------------------------------------------------------------------------

AsBuiltDialog::AsBuiltDialog(AppContext& ctx, const Uuid& datasetId, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("As-built model from scan"));
    auto* form = new QFormLayout(this);
    const auto& m = ctx_.project().model();
    dataset_ = new QComboBox(this);
    fillEntityCombo(dataset_, m, EntityKind::PointCloud, datasetId);
    development_ = new QComboBox(this);
    Uuid dev;
    if (const auto* d = m.pointClouds.find(datasetId)) dev = d->developmentId;
    fillEntityCombo(development_, m, EntityKind::Development, dev);
    corridor_ = spin(0.5, 50, 6, 2, " m", this);
    corridor_->setToolTip(tr("Scan points within this distance of the centreline are used"));
    voxel_ = spin(0, 1, 0.05, 3, " m", this);
    depth_ = ispin(5, 13, 9, this);
    engine_ = new QComboBox(this);
    engine_->setObjectName("engine");
    if (havePdal()) engine_->addItem(tr("Poisson (PDAL)"), "pdal");
    if (haveOpen3d()) engine_->addItem(tr("Poisson (Open3D)"), "open3d");
    form->addRow(tr("Registered scan"), dataset_);
    form->addRow(tr("Development"), development_);
    form->addRow(tr("Corridor half-width"), corridor_);
    form->addRow(tr("Subsample voxel"), voxel_);
    form->addRow(tr("Poisson depth"), depth_);
    form->addRow(tr("Surface engine"), engine_);
    auto* note = new QLabel(tr("Normals are oriented towards the inside of the heading (its design profile's centroid along the centreline), so the reconstructed surface faces into the excavation. "
                               "Triangles farther from the scan than twice its point spacing (Poisson closes unscanned gaps) are removed."),
                            this);
    note->setWordWrap(true);
    form->addRow(note);
    auto* bb = okCancel(this, tr("Create"), [this] { return start() > 0; });
    if (engine_->count() == 0) {
        bb->button(QDialogButtonBox::Ok)->setEnabled(false);
        form->addRow(new QLabel(tr("<i>Not available: no surface-reconstruction engine in this build (PDAL or Open3D).</i>"), this));
    }
    form->addRow(bb);
}

int AsBuiltDialog::start() {
#if defined(OPENMINE_APP_HAVE_PDAL) || defined(OPENMINE_APP_HAVE_OPEN3D)
    const Uuid ds = comboUuid(dataset_), dev = comboUuid(development_);
    const double corridor = corridor_->value(), voxel = voxel_->value();
    const int depth = depth_->value();
    const QString engine = engine_->currentData().toString();
    auto snap = ctx_.snapshot();
    AppContext* c = &ctx_;
    return ctx_.jobs().start(tr("As-built model"), tr("Survey"), [snap, ds, dev, corridor, voxel, depth, engine, c](jobs::Progress& pr) -> UiApply {
        const auto* d = snap->model().developments.find(dev);
        if (!d) throw ValidationError("Choose a development");
        survey::AsBuiltOptions o;
        o.corridor = corridor;
        o.voxel = voxel;
        o.depth = depth;
        survey::SurfaceEngine surface;
        if (engine == "open3d") {
#ifdef OPENMINE_APP_HAVE_OPEN3D
            surface = [](const survey::PointBuffer& pts, int dep) {
                o3d::PoissonOptions po;
                po.depth = dep;
                return o3d::poissonSurface(pts, po).mesh; // densities are cleared by the pipeline
            };
#endif
        } else {
#ifdef OPENMINE_APP_HAVE_PDAL
            surface = [](const survey::PointBuffer& pts, int dep) {
                pdalio::PoissonOptions po;
                po.depth = dep;
                return pdalio::poissonSurface(pts, po);
            };
#endif
        }
        std::shared_ptr<MeshData> cleaned;
        try {
            cleaned = std::make_shared<MeshData>(survey::buildAsBuiltSurface(*snap, ds, dev, o, surface, pr));
        } catch (const survey::PointCloudError& e) {
            throw ValidationError(e.what());
        }
        const std::string name = d->headingId + " as-built";
        const std::string method = "Poisson depth=" + std::to_string(depth) + " (" + engineLabel(engine) + ")";
        return [c, cleaned, ds, dev, method, name] {
            Uuid id;
            if (c->tryEdit(QObject::tr("Add as-built model"), [&] {
                    auto& p = c->project();
                    CommandStack::MacroScope macro(p.commands(), "Add as-built " + name);
                    const Uuid layer = workflowLayer(*c, "As-built", 0xFFB74D);
                    id = survey::addAsBuiltModel(p, ds, *cleaned, name, layer, dev, method);
                    macro.commit();
                })) {
                c->report(QObject::tr("As-built '%1': %L2 triangles").arg(qstr(name)).arg(static_cast<qulonglong>(cleaned->triangles.size())));
                c->select(EntityKind::AsBuilt, id);
            }
        };
    });
#else
    ctx_.report(tr("No surface-reconstruction engine is available"), MessageLevel::Error);
    return 0;
#endif
}

// ---- conformance -----------------------------------------------------------------------------------

ConformanceDialog::ConformanceDialog(AppContext& ctx, const Uuid& asBuiltId, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Design conformance"));
    auto* form = new QFormLayout(this);
    const auto& m = ctx_.project().model();
    asBuilt_ = new QComboBox(this);
    fillEntityCombo(asBuilt_, m, EntityKind::AsBuilt, asBuiltId);
    development_ = new QComboBox(this);
    Uuid dev;
    if (const auto* a = m.asBuiltModels.find(asBuiltId)) dev = a->developmentId;
    fillEntityCombo(development_, m, EntityKind::Development, dev);
    connect(asBuilt_, &QComboBox::activated, this, [this](int) {
        if (const auto* a = ctx_.project().model().asBuiltModels.find(comboUuid(asBuilt_)))
            development_->setCurrentIndex(std::max(0, development_->findData(uuidText(a->developmentId))));
    });
    interval_ = spin(0.05, 100, 1.0, 2, " m", this);
    range_ = new QCheckBox(tr("Limit to chainages"), this);
    from_ = spin(-1e6, 1e6, 0, 3, " m", this);
    to_ = spin(-1e6, 1e6, 100, 3, " m", this);
    from_->setEnabled(false);
    to_->setEnabled(false);
    connect(range_, &QCheckBox::toggled, from_, &QWidget::setEnabled);
    connect(range_, &QCheckBox::toggled, to_, &QWidget::setEnabled);
    orientation_ = new QComboBox(this);
    orientation_->addItem(tr("Sections perpendicular to the centreline"), static_cast<int>(geometry::SectionOrientation::PerpendicularToAxis));
    orientation_->addItem(tr("Vertical sections"), static_cast<int>(geometry::SectionOrientation::Vertical));
    envelope_ = new QCheckBox(tr("Check clearance of an equipment envelope (rectangle on the floor centre)"), this);
    envW_ = spin(0.1, 50, 3.0, 2, " m", this);
    envH_ = spin(0.1, 50, 3.5, 2, " m", this);
    maxGap_ = spin(0, 10, 0.5, 2, " m", this);
    maxGap_->setToolTip(tr("Holes in the as-built surface up to this size are bridged when a section is assembled; "
                           "larger gaps leave the section open, closed by its chord"));
    report_ = new QCheckBox(tr("Generate the conformance report (PDF)"), this);
    report_->setChecked(true);
    form->addRow(tr("As-built model"), asBuilt_);
    form->addRow(tr("Design (development)"), development_);
    form->addRow(tr("Station interval"), interval_);
    form->addRow(range_);
    form->addRow(tr("From chainage"), from_);
    form->addRow(tr("To chainage"), to_);
    form->addRow(tr("Sections"), orientation_);
    form->addRow(tr("Bridge gaps up to"), maxGap_);
    form->addRow(envelope_);
    form->addRow(tr("Envelope width"), envW_);
    form->addRow(tr("Envelope height"), envH_);
    form->addRow(report_);
    form->addRow(okCancel(this, tr("Analyse"), [this] { return start() > 0; }));
}

int ConformanceDialog::start() {
    const Uuid ab = comboUuid(asBuilt_), dev = comboUuid(development_);
    if (ab.isNil() || dev.isNil()) {
        ctx_.report(tr("Choose an as-built model and a development"), MessageLevel::Error);
        return 0;
    }
    survey::ConformanceOptions o;
    o.interval = interval_->value();
    if (range_->isChecked()) {
        o.fromChainage = from_->value();
        o.toChainage = to_->value();
    }
    o.orientation = static_cast<geometry::SectionOrientation>(orientation_->currentData().toInt());
    o.maxGap = maxGap_->value();
    if (envelope_->isChecked()) {
        const double w = envW_->value() / 2, h = envH_->value();
        o.clearanceEnvelope = {{-w, 0}, {w, 0}, {w, h}, {-w, h}};
    }
    const bool report = report_->isChecked();
    auto snap = ctx_.snapshot();
    AppContext* c = &ctx_;
    return ctx_.jobs().start(tr("Design conformance"), tr("Survey"), [snap, ab, dev, o, report, c](jobs::Progress& pr) -> UiApply {
        auto rep = std::make_shared<survey::ConformanceReport>(survey::analyseAsBuilt(*snap, ab, dev, o, pr));
        return [c, rep, ab, dev, o, report] {
            Uuid id;
            if (!c->tryEdit(QObject::tr("Store conformance"), [&] { id = survey::commitConformance(c->project(), ab, dev, *rep, o); })) return;
            c->report(QObject::tr("Conformance: overbreak %1 m³ (%2 %), underbreak %3 m³, %4 valid stations%5")
                          .arg(rep->overbreakVolume, 0, 'f', 1)
                          .arg(rep->overbreakPercent, 0, 'f', 1)
                          .arg(rep->underbreakVolume, 0, 'f', 2)
                          .arg(rep->validStations)
                          .arg(rep->minClearance ? QObject::tr(", minimum clearance %1 m").arg(*rep->minClearance, 0, 'f', 3) : QString()));
            c->select(EntityKind::Conformance, id);
            if (report)
                c->tryEdit(QObject::tr("Conformance report"), [&] {
                    reporting::storeReport(c->project(), reporting::conformanceReport(c->project(), id, rep.get()), "Survey", "pdf");
                });
        };
    });
}

// ---- face advance ------------------------------------------------------------------------------

FaceAdvanceDialog::FaceAdvanceDialog(AppContext& ctx, const Uuid& developmentId, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Face position and advance reconciliation"));
    auto* form = new QFormLayout(this);
    const auto& m = ctx_.project().model();
    development_ = new QComboBox(this);
    fillEntityCombo(development_, m, EntityKind::Development, developmentId);
    dataset_ = new QComboBox(this);
    fillEntityCombo(dataset_, m, EntityKind::PointCloud, {});
    for (int i = 0; i < dataset_->count(); ++i)
        if (const auto* d = m.pointClouds.find(uuidFrom(dataset_->itemData(i).toString())); d && d->developmentId == developmentId) dataset_->setCurrentIndex(i);
    activity_ = new QComboBox(this);
    auto fillActs = [this] {
        const auto& mm = ctx_.project().model();
        activity_->clear();
        for (const auto& a : mm.activitiesForDevelopment(comboUuid(development_))) activity_->addItem(displayName(mm, {EntityKind::Activity, a}), uuidText(a));
    };
    fillActs();
    connect(development_, &QComboBox::activated, this, fillActs);
    date_ = new QDateEdit(QDate::currentDate(), this);
    date_->setCalendarPopup(true);
    date_->setDisplayFormat("yyyy-MM-dd");
    if (const auto* d = m.pointClouds.find(comboUuid(dataset_)); d && d->scanDate) date_->setDate(toQDate(*d->scanDate));
    corridor_ = spin(0.5, 30, 5, 2, " m", this);
    result_ = new QLabel(this);
    result_->setWordWrap(true);
    form->addRow(tr("Development"), development_);
    form->addRow(tr("Scan"), dataset_);
    form->addRow(tr("Linked activity"), activity_);
    form->addRow(tr("Survey date"), date_);
    form->addRow(tr("Corridor"), corridor_);
    form->addRow(result_);
    form->addRow(okCancel(this, tr("Measure and record"), [this] { return start() > 0; }));
}

int FaceAdvanceDialog::start() {
    const Uuid dev = comboUuid(development_), ds = comboUuid(dataset_), act = comboUuid(activity_);
    if (dev.isNil() || ds.isNil() || act.isNil()) {
        ctx_.report(tr("Choose a development, a scan and the linked activity"), MessageLevel::Error);
        return 0;
    }
    const Date date = fromQDate(date_->date());
    const double corridor = corridor_->value();
    auto snap = ctx_.snapshot();
    AppContext* c = &ctx_;
    return ctx_.jobs().start(tr("Face position"), tr("Survey"), [snap, dev, ds, act, date, corridor, c](jobs::Progress& pr) -> UiApply {
        const auto* d = snap->model().developments.find(dev);
        if (!d) throw ValidationError("The development no longer exists");
        const auto cl = mine_design::centrelinePoints(*snap, *d);
        auto box = BoundingBox::empty();
        for (const auto& p : cl) box.expand(p);
        box.min = box.min - Vec3{corridor, corridor, corridor};
        box.max = box.max + Vec3{corridor, corridor, corridor};
        const auto pts = survey::datasetPoints(*snap, ds, true, box, pr);
        const auto face = survey::facePosition(cl, pts, corridor);
        const double faceChainage = d->chainageFrom + face.faceChainage;
        return [c, dev, ds, act, date, faceChainage, face] {
            const auto r = survey::reconcileAdvance(c->project(), dev, act, faceChainage, date);
            c->report(QObject::tr("Face at %1 (%L2 points): surveyed advance %3 m, reported %4 m, correction %5 m")
                          .arg(qstr(mine_design::formatChainage(faceChainage)))
                          .arg(static_cast<qulonglong>(face.pointsUsed))
                          .arg(r.surveyedAdvance, 0, 'f', 2)
                          .arg(r.reportedAdvance, 0, 'f', 2)
                          .arg(r.correction, 0, 'f', 2));
            c->tryEdit(QObject::tr("Record survey actual"), [&] { survey::commitSurveyActual(c->project(), r, dev, act, ds, date); });
        };
    });
}

// ---- control import --------------------------------------------------------------------------------

ControlImportDialog::ControlImportDialog(AppContext& ctx, const QString& file, QWidget* parent) : QDialog(parent), ctx_(ctx), file_(file) {
    setWindowTitle(tr("Import control points — %1").arg(QFileInfo(file).fileName()));
    auto* form = new QFormLayout(this);
    const char* roles[] = {"Name", "X (east)", "Y (north)", "Z", "Code"};
    util::CsvTable t;
    try {
        t = util::readCsvFile(sstr(file));
    } catch (const std::exception& e) {
        ctx_.report(tr("Cannot read %1: %2").arg(file, qstr(e.what())), MessageLevel::Error);
    }
    for (int i = 0; i < 5; ++i) {
        cols_[i] = new QComboBox(this);
        if (i == 4) cols_[i]->addItem(tr("(none)"), -1);
        for (std::size_t h = 0; h < t.headers.size(); ++h) cols_[i]->addItem(qstr(t.headers[h]), static_cast<int>(h));
        // Guess by header name.
        static const std::vector<std::vector<std::string>> guess = {{"name", "id", "point", "pt", "station"}, {"x", "east", "easting", "e"}, {"y", "north", "northing", "n"},
                                                                    {"z", "elev", "elevation", "rl", "h"}, {"code", "desc", "description"}};
        for (std::size_t h = 0; h < t.headers.size(); ++h) {
            std::string lower = t.headers[h];
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (std::find(guess[static_cast<std::size_t>(i)].begin(), guess[static_cast<std::size_t>(i)].end(), lower) != guess[static_cast<std::size_t>(i)].end())
                cols_[i]->setCurrentIndex(cols_[i]->findData(static_cast<int>(h)));
        }
        form->addRow(tr(roles[i]), cols_[i]);
    }
    role_ = new QComboBox(this);
    for (const auto& n : enumNames<ControlPointRole>()) role_->addItem(qstr(n));
    hSigma_ = spin(0, 10, 0.005, 4, " m", this);
    vSigma_ = spin(0, 10, 0.005, 4, " m", this);
    source_ = new QLineEdit(QFileInfo(file).fileName(), this);
    form->addRow(tr("Role"), role_);
    form->addRow(tr("Horizontal σ"), hSigma_);
    form->addRow(tr("Vertical σ"), vSigma_);
    form->addRow(tr("Source"), source_);
    info_ = new QLabel(tr("%1 rows").arg(t.rows.size()), this);
    form->addRow(info_);
    form->addRow(okCancel(this, tr("Import"), [this] { return apply(); }));
}

bool ControlImportDialog::apply() {
    util::CsvTable t;
    try {
        t = util::readCsvFile(sstr(file_));
    } catch (const std::exception& e) {
        ctx_.report(qstr(e.what()), MessageLevel::Error);
        return false;
    }
    int col[5];
    for (int i = 0; i < 5; ++i) col[i] = cols_[i]->currentData().toInt();
    const auto role = enumFromString<ControlPointRole>(sstr(role_->currentText())).value_or(ControlPointRole::Control);
    std::vector<SurveyControlPoint> pts;
    std::vector<std::string> problems;
    for (std::size_t r = 0; r < t.rows.size(); ++r) {
        const auto& row = t.rows[r];
        auto field = [&](int c) { return c >= 0 && static_cast<std::size_t>(c) < row.size() ? row[static_cast<std::size_t>(c)] : std::string(); };
        SurveyControlPoint p;
        p.name = field(col[0]);
        const auto x = util::parseNumber(field(col[1])), y = util::parseNumber(field(col[2])), z = util::parseNumber(field(col[3]));
        if (p.name.empty() || !x || !y || !z) {
            problems.push_back("row " + std::to_string(r + 2) + ": missing name or coordinate");
            continue;
        }
        p.position = {*x, *y, *z};
        p.code = field(col[4]);
        p.role = role;
        p.horizontalSigma = hSigma_->value();
        p.verticalSigma = vSigma_->value();
        p.source = sstr(source_->text());
        pts.push_back(p);
    }
    const bool ok = ctx_.tryEdit(tr("Import control points"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Import control points");
        ProjectEditor ed(p);
        for (auto& c : pts) ed.add(c);
        macro.commit();
    });
    if (ok) ctx_.report(tr("Imported %1 control points%2").arg(pts.size()).arg(problems.empty() ? QString() : tr("; %1 rows rejected (first: %2)").arg(problems.size()).arg(qstr(problems.front()))));
    return ok;
}

// ---- volumes ---------------------------------------------------------------------------------------

VolumeDialog::VolumeDialog(AppContext& ctx, const Uuid& surfaceId, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Surface volume"));
    auto* form = new QFormLayout(this);
    const auto& m = ctx_.project().model();
    auto meshes = [&m](const Uuid& id) {
        const auto* g = m.geometry.find(id);
        return g && g->mesh && !g->mesh->triangles.empty();
    };
    mode_ = new QComboBox(this);
    mode_->addItem(tr("Surface to elevation (exact, TIN prisms)"), 0);
    mode_->addItem(tr("Between two surfaces (grid)"), 1);
    surface_ = new QComboBox(this);
    fillEntityCombo(surface_, m, EntityKind::Geometry, surfaceId, false, meshes);
    other_ = new QComboBox(this);
    fillEntityCombo(other_, m, EntityKind::Geometry, {}, false, meshes);
    elevation_ = spin(-1e5, 1e5, 0, 3, " m", this);
    if (const auto* g = m.geometry.find(surfaceId); g && g->mesh) {
        double lo = 1e300;
        for (const auto& v : g->mesh->vertices) lo = std::min(lo, v.z);
        if (lo < 1e300) elevation_->setValue(std::floor(lo));
    }
    cell_ = spin(0.01, 100, 0.5, 3, " m", this);
    result_ = new QLabel(this);
    form->addRow(tr("Method"), mode_);
    form->addRow(tr("Surface (later)"), surface_);
    form->addRow(tr("Earlier surface"), other_);
    form->addRow(tr("Reference elevation"), elevation_);
    form->addRow(tr("Grid cell"), cell_);
    form->addRow(result_);
    form->addRow(okCancel(this, tr("Calculate"), [this] { return start() > 0; }));
}

int VolumeDialog::start() {
    const int mode = mode_->currentData().toInt();
    const Uuid a = comboUuid(surface_), b = comboUuid(other_);
    const double z = elevation_->value(), cell = cell_->value();
    auto snap = ctx_.snapshot();
    AppContext* c = &ctx_;
    return ctx_.jobs().start(tr("Volume"), tr("Survey"), [snap, mode, a, b, z, cell, c](jobs::Progress& pr) -> UiApply {
        const auto* ga = snap->model().geometry.find(a);
        if (!ga || !ga->mesh) throw ValidationError("Choose a surface");
        survey::VolumeResult v;
        json params;
        if (mode == 0) {
            v = survey::volumeToPlane(*ga->mesh, z);
            params = {{"elevation", z}};
        } else {
            const auto* gb = snap->model().geometry.find(b);
            if (!gb || !gb->mesh) throw ValidationError("Choose the earlier surface");
            v = survey::volumeBetween(*gb->mesh, *ga->mesh, cell, pr);
            params = {{"cell", cell}};
        }
        return [c, v, params, a, b, mode] {
            const QString text = QObject::tr("Above (fill) %1 m³, below (cut) %2 m³, plan area %3 m²").arg(v.above, 0, 'f', 2).arg(v.below, 0, 'f', 2).arg(v.planArea, 0, 'f', 2);
            c->report(text);
            c->tryEdit(QObject::tr("Store volume"), [&] {
                storeAnalysis(c->project(), mode == 0 ? AnalysisKind::VolumeCalculation : AnalysisKind::SurfaceComparison, mode == 0 ? "Volume to elevation" : "Volume between surfaces",
                              mode == 0 ? json{{"surface", a.toString()}} : json{{"later", a.toString()}, {"earlier", b.toString()}}, params,
                              {{"above", v.above}, {"below", v.below}, {"planArea", v.planArea}});
            });
        };
    });
}

} // namespace openmine::app
