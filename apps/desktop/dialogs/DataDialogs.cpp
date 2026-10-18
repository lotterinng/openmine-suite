// SPDX-License-Identifier: Apache-2.0
#include "DataDialogs.h"

#include "DialogUtil.h"
#include "../app/EntityInfo.h"

#include "openmine/geometry/LocalGrid.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/project/ProjectEditor.h"

#ifdef OPENMINE_APP_HAVE_CRS
#include "openmine/crs/Crs.h"
#endif
#ifdef OPENMINE_APP_HAVE_REPOSITORY
#include "openmine/repository/PgRepository.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace openmine::app {

// ---- CRS -------------------------------------------------------------------------------------------

bool CrsDialog::available() {
#ifdef OPENMINE_APP_HAVE_CRS
    return true;
#else
    return false;
#endif
}

CrsDialog::CrsDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Coordinate reference systems"));
    resize(820, 560);
    auto* lay = new QVBoxLayout(this);
    const auto* pc = ctx_.project().model().projectCrs();
    auto* current = new QLabel(tr("Project CRS: <b>%1</b> %2").arg(pc ? qstr(pc->name) : tr("not declared"), pc ? qstr(pc->authorityCode) : QString()), this);
    lay->addWidget(current);
    auto* note = new QLabel(tr("Declaring or changing the project CRS states what the stored coordinates <i>are</i>; it never transforms them. "
                               "Use Transform objects for an explicit, recorded transformation."),
                            this);
    note->setWordWrap(true);
    lay->addWidget(note);
    auto* row = new QHBoxLayout;
    query_ = new QLineEdit(this);
    query_->setPlaceholderText(tr("name fragment (e.g. 'Lo25', 'UTM zone 35S') or EPSG:xxxx / WKT / PROJ string"));
    auto* go = new QPushButton(tr("Search"), this);
    row->addWidget(query_, 1);
    row->addWidget(go);
    lay->addLayout(row);
    results_ = new QTableWidget(0, 4, this);
    results_->setHorizontalHeaderLabels({tr("Code"), tr("Name"), tr("Kind"), tr("Area of use")});
    results_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    results_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(results_, 1);
    details_ = new QLabel(this);
    details_->setWordWrap(true);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(details_);
    setProject_ = new QCheckBox(tr("Make it the project CRS"), this);
    setProject_->setChecked(!pc || pc->kind == CrsKind::Unknown);
    lay->addWidget(setProject_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* decl = bb->addButton(tr("Declare"), QDialogButtonBox::AcceptRole);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(go, &QPushButton::clicked, this, &CrsDialog::search);
    connect(query_, &QLineEdit::returnPressed, this, &CrsDialog::search);
    connect(decl, &QPushButton::clicked, this, [this] {
        const int r = results_->currentRow();
        const QString def = r >= 0 ? results_->item(r, 0)->data(Qt::UserRole).toString() : query_->text().trimmed();
        if (!def.isEmpty() && !declare(def, setProject_->isChecked()).isNil()) accept();
    });
    connect(results_, &QTableWidget::currentCellChanged, this, [this](int r, int, int, int) {
#ifdef OPENMINE_APP_HAVE_CRS
        if (r < 0) return;
        try {
            const auto info = crs::describe(sstr(results_->item(r, 0)->data(Qt::UserRole).toString()));
            details_->setText(tr("<b>%1</b> (%2)<br>Datum: %3 · unit: %4 · axes: %5<br>PROJ: <code>%6</code>")
                                  .arg(qstr(info.name), qstr(info.authorityCode), qstr(info.horizontalDatum), qstr(info.linearUnit), qstr(info.axisDescription()),
                                       qstr(info.projString).toHtmlEscaped()));
        } catch (const std::exception& e) {
            details_->setText(qstr(e.what()));
        }
#else
        (void)r;
#endif
    });
    if (!available()) {
        go->setEnabled(false);
        decl->setEnabled(false);
        details_->setText(tr("Not available: this build has no PROJ."));
    }
}

void CrsDialog::search() {
#ifdef OPENMINE_APP_HAVE_CRS
    results_->setRowCount(0);
    const QString q = query_->text().trimmed();
    std::vector<crs::CrsInfo> found;
    try {
        if (q.contains(':') || q.startsWith('+') || q.contains('[')) found = {crs::describe(sstr(q))};
        else found = crs::search(sstr(q), 200);
    } catch (const std::exception& e) {
        details_->setText(qstr(e.what()));
        return;
    }
    for (const auto& c : found) {
        const int r = results_->rowCount();
        results_->insertRow(r);
        auto* code = new QTableWidgetItem(qstr(c.authorityCode));
        code->setData(Qt::UserRole, qstr(c.authorityCode.empty() ? c.wkt : c.authorityCode));
        results_->setItem(r, 0, code);
        results_->setItem(r, 1, new QTableWidgetItem(qstr(c.name) + (c.deprecated ? tr(" (deprecated)") : QString())));
        results_->setItem(r, 2, new QTableWidgetItem(qstr(toString(c.kind))));
        results_->setItem(r, 3, new QTableWidgetItem(qstr(c.areaName)));
    }
    details_->setText(tr("%1 results").arg(found.size()));
#endif
}

Uuid CrsDialog::declare(const QString& definition, bool makeProject) {
    Uuid id;
#ifdef OPENMINE_APP_HAVE_CRS
    ctx_.tryEdit(tr("Declare CRS"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Declare CRS " + sstr(definition));
        id = crs::declareCrs(p, sstr(definition));
        if (makeProject) crs::setProjectCrs(p, id);
        macro.commit();
    });
    if (!id.isNil() && makeProject) {
        // Warn when the data lies outside the CRS's area of use (a sign of a wrong declaration).
        std::vector<Vec3> pts;
        for (const auto& c : ctx_.project().model().controlPoints.all()) pts.push_back(c.position);
        for (const auto& g : ctx_.project().model().geometry.all())
            if (!g.vertices.empty() && pts.size() < 500) pts.push_back(g.vertices.front());
        try {
            for (const auto& w : crs::checkAreaOfUse(sstr(definition), pts)) ctx_.report(qstr(w), MessageLevel::Warning);
        } catch (const std::exception&) {
        }
    }
#else
    (void)definition;
    (void)makeProject;
#endif
    return id;
}

// ---- local grid -----------------------------------------------------------------------------------

LocalGridDialog::LocalGridDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Define a local mine grid"));
    resize(860, 520);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(tr("Mine grid"), this);
    base_ = new QComboBox(this);
    fillEntityCombo(base_, ctx_.project().model(), EntityKind::Crs, ctx_.project().model().info.projectCrsId);
    method_ = new QComboBox(this);
    for (const auto& n : enumNames<LocalGridMethod>()) method_->addItem(qstr(n));
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Base CRS"), base_);
    form->addRow(tr("Method"), method_);
    lay->addLayout(form);
    pairs_ = new QTableWidget(0, 9, this);
    pairs_->setHorizontalHeaderLabels({tr("Name"), tr("Local X"), tr("Local Y"), tr("Local Z"), tr("Base X"), tr("Base Y"), tr("Base Z"), tr("Use in fit"), tr("Residual (m)")});
    pairs_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    lay->addWidget(pairs_, 1);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add pair"), this);
    auto* del = new QPushButton(tr("Remove"), this);
    auto* fitB = new QPushButton(tr("Fit"), this);
    row->addWidget(add);
    row->addWidget(del);
    row->addStretch(1);
    row->addWidget(fitB);
    lay->addLayout(row);
    result_ = new QLabel(this);
    result_->setWordWrap(true);
    lay->addWidget(result_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* st = bb->addButton(tr("Store local grid"), QDialogButtonBox::AcceptRole);
    lay->addWidget(bb);
    connect(add, &QPushButton::clicked, this, [this] {
        const int r = pairs_->rowCount();
        pairs_->insertRow(r);
        pairs_->setItem(r, 0, new QTableWidgetItem(tr("C%1").arg(r + 1)));
        for (int c = 1; c < 7; ++c) pairs_->setItem(r, c, new QTableWidgetItem("0"));
        auto* use = new QTableWidgetItem;
        use->setCheckState(Qt::Checked);
        pairs_->setItem(r, 7, use);
        pairs_->setItem(r, 8, new QTableWidgetItem);
    });
    connect(del, &QPushButton::clicked, this, [this] {
        if (pairs_->currentRow() >= 0) pairs_->removeRow(pairs_->currentRow());
    });
    connect(fitB, &QPushButton::clicked, this, &LocalGridDialog::fit);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(st, &QPushButton::clicked, this, [this] {
        if (!store().isNil()) accept();
    });
    // Pre-fill from control points named in both systems is not inferable: the user enters local
    // coordinates; base coordinates default to the control points' (project) positions.
    for (const auto& c : ctx_.project().model().controlPoints.all()) {
        const int r = pairs_->rowCount();
        pairs_->insertRow(r);
        pairs_->setItem(r, 0, new QTableWidgetItem(qstr(c.name)));
        for (int k = 0; k < 3; ++k) {
            pairs_->setItem(r, 1 + k, new QTableWidgetItem(QString()));
            pairs_->setItem(r, 4 + k, new QTableWidgetItem(QString::number(c.position[k], 'f', 4)));
        }
        auto* use = new QTableWidgetItem;
        use->setCheckState(c.role == ControlPointRole::Check ? Qt::Unchecked : Qt::Checked);
        pairs_->setItem(r, 7, use);
        pairs_->setItem(r, 8, new QTableWidgetItem);
    }
}

namespace {
std::optional<std::vector<geometry::GridControlPair>> readPairs(QTableWidget* t, QString& error) {
    std::vector<geometry::GridControlPair> out;
    for (int r = 0; r < t->rowCount(); ++r) {
        geometry::GridControlPair p;
        p.name = sstr(t->item(r, 0)->text());
        bool all = true, any = false;
        for (int k = 0; k < 3; ++k) {
            bool a = false, b = false;
            p.local[k] = QLocale::c().toDouble(t->item(r, 1 + k)->text(), &a);
            p.base[k] = QLocale::c().toDouble(t->item(r, 4 + k)->text(), &b);
            all = all && a && b;
            any = any || (!t->item(r, 1 + k)->text().isEmpty());
        }
        if (!any) continue; // rows without local coordinates are ignored
        if (!all) {
            error = QObject::tr("Pair %1 has invalid coordinates").arg(qstr(p.name));
            return std::nullopt;
        }
        p.use = t->item(r, 7)->checkState() == Qt::Checked;
        out.push_back(p);
    }
    return out;
}
} // namespace

bool LocalGridDialog::fit() {
    QString err;
    auto pairs = readPairs(pairs_, err);
    if (!pairs) {
        result_->setText(err);
        return false;
    }
    try {
        const auto f = geometry::fitLocalGrid(*pairs, enumFromString<LocalGridMethod>(sstr(method_->currentText())).value_or(LocalGridMethod::Helmert2D));
        int fi = 0;
        for (int r = 0; r < pairs_->rowCount(); ++r) {
            if (pairs_->item(r, 1)->text().isEmpty()) continue;
            if (fi < static_cast<int>(f.pairs.size())) pairs_->item(r, 8)->setText(QString::number(f.pairs[static_cast<std::size_t>(fi)].residual.length(), 'f', 4));
            ++fi;
        }
        result_->setText(tr("Fit RMS %1 m, max %2 m over %3 pairs; vertical RMS %4 m%5")
                             .arg(f.rms, 0, 'f', 4)
                             .arg(f.max, 0, 'f', 4)
                             .arg(f.fitCount)
                             .arg(f.verticalRms, 0, 'f', 4)
                             .arg(f.checkCount > 0 ? tr("; independent check RMS %1 m over %2").arg(f.checkRms, 0, 'f', 4).arg(f.checkCount) : tr("; no check pairs")));
        return true;
    } catch (const std::exception& e) {
        result_->setText(qstr(e.what()));
        return false;
    }
}

Uuid LocalGridDialog::store() {
    QString err;
    auto pairs = readPairs(pairs_, err);
    if (!pairs) {
        ctx_.report(err, MessageLevel::Error);
        return {};
    }
    Uuid id;
#ifdef OPENMINE_APP_HAVE_CRS
    ctx_.tryEdit(tr("Define local grid"), [&] {
        const auto f = geometry::fitLocalGrid(*pairs, enumFromString<LocalGridMethod>(sstr(method_->currentText())).value_or(LocalGridMethod::Helmert2D));
        id = crs::defineLocalGrid(ctx_.project(), sstr(name_->text()), comboUuid(base_), f);
    });
#else
    ctx_.report(tr("Local grids need PROJ, which this build does not include"), MessageLevel::Error);
#endif
    return id;
}

// ---- transform objects --------------------------------------------------------------------------------

TransformDialog::TransformDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Transform objects to another CRS"));
    auto* form = new QFormLayout(this);
    const auto& m = ctx_.project().model();
    source_ = new QComboBox(this);
    target_ = new QComboBox(this);
    fillEntityCombo(source_, m, EntityKind::Crs, m.info.projectCrsId);
    fillEntityCombo(target_, m, EntityKind::Crs);
    operation_ = new QComboBox(this);
    layer_ = new QComboBox(this);
    fillEntityCombo(layer_, m, EntityKind::Layer, ctx_.currentLayer());
    ballpark_ = new QCheckBox(tr("Allow ballpark (approximate) operations"), this);
    info_ = new QLabel(tr("%1 selected objects are copied into the target CRS; the originals are not changed. The operation, its accuracy and grids are recorded.")
                           .arg(ctx_.selectedGeometry().size()),
                       this);
    info_->setWordWrap(true);
    form->addRow(tr("Source CRS"), source_);
    form->addRow(tr("Target CRS"), target_);
    form->addRow(tr("Operation"), operation_);
    form->addRow(tr("Copies on layer"), layer_);
    form->addRow(ballpark_);
    form->addRow(info_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Transform"));
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (!apply().empty()) accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(source_, &QComboBox::activated, this, &TransformDialog::operations);
    connect(target_, &QComboBox::activated, this, &TransformDialog::operations);
    operations();
}

void TransformDialog::operations() {
    operation_->clear();
#ifdef OPENMINE_APP_HAVE_CRS
    const auto& m = ctx_.project().model();
    const auto* s = m.crs.find(comboUuid(source_));
    const auto* t = m.crs.find(comboUuid(target_));
    if (!s || !t || s->authorityCode.empty() || t->authorityCode.empty()) {
        operation_->addItem(tr("(determined by the local-grid definition)"), 0);
        return;
    }
    try {
        int i = 0;
        for (const auto& op : crs::candidateOperations(s->authorityCode, t->authorityCode))
            operation_->addItem(QStringLiteral("%1 — %2%3%4")
                                    .arg(qstr(op.name))
                                    .arg(op.accuracy >= 0 ? tr("±%1 m").arg(op.accuracy) : tr("accuracy unknown"))
                                    .arg(op.ballpark ? tr(", ballpark") : QString())
                                    .arg(op.gridsAvailable ? QString() : tr(", GRIDS MISSING")),
                                i++);
    } catch (const std::exception& e) {
        info_->setText(qstr(e.what()));
    }
#endif
}

std::vector<Uuid> TransformDialog::apply() {
    std::vector<Uuid> ids;
#ifdef OPENMINE_APP_HAVE_CRS
    crs::TransformerOptions o;
    o.operationIndex = operation_->currentData().toInt();
    o.allowBallpark = ballpark_->isChecked();
    const auto objs = ctx_.selectedGeometry();
    if (objs.empty()) {
        ctx_.report(tr("Select the objects to transform"), MessageLevel::Warning);
        return ids;
    }
    ctx_.tryEdit(tr("Transform objects"), [&] { ids = crs::transformObjects(ctx_.project(), objs, comboUuid(source_), comboUuid(target_), comboUuid(layer_), o); });
    if (!ids.empty()) ctx_.report(tr("%1 transformed copies created; a transformation record was stored").arg(ids.size()));
#else
    ctx_.report(tr("Transformations need PROJ, which this build does not include"), MessageLevel::Error);
#endif
    return ids;
}

// ---- plugins ---------------------------------------------------------------------------------------

PluginsDialog::PluginsDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Plugins"));
    resize(820, 560);
    auto* lay = new QVBoxLayout(this);
    QStringList dirs;
    for (const auto& d : ctx_.pluginDirectories()) dirs << QString::fromStdU16String(d.u16string());
    auto* info = new QLabel(tr("Native plugins (C ABI v%1) are loaded from: %2").arg(plugins::PluginManager::apiVersion()).arg(dirs.join("; ")), this);
    info->setWordWrap(true);
    lay->addWidget(info);
    list_ = new QTreeWidget(this);
    list_->setColumnCount(5);
    list_->setHeaderLabels({tr("Plugin"), tr("Version"), tr("Status"), tr("Provides"), tr("Folder")});
    list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    lay->addWidget(list_, 1);
    auto* row = new QHBoxLayout;
    auto* reload = new QPushButton(tr("Rescan and load"), this);
    auto* unload = new QPushButton(tr("Unload selected"), this);
    row->addWidget(reload);
    row->addWidget(unload);
    row->addStretch(1);
    lay->addLayout(row);
    auto* run = new QHBoxLayout;
    command_ = new QComboBox(this);
    args_ = new QLineEdit("{}", this);
    auto* go = new QPushButton(tr("Run command"), this);
    run->addWidget(new QLabel(tr("Command"), this));
    run->addWidget(command_, 1);
    run->addWidget(new QLabel(tr("Arguments (JSON)"), this));
    run->addWidget(args_, 1);
    run->addWidget(go);
    lay->addLayout(run);
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumHeight(160);
    lay->addWidget(log_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(reload, &QPushButton::clicked, this, [this] {
        std::vector<std::string> problems;
        ctx_.plugins().discover(ctx_.pluginDirectories(), &problems);
        const int n = ctx_.plugins().loadAll(ctx_.pluginDirectories(), {});
        log_->appendPlainText(tr("%1 plugins loaded").arg(n));
        for (const auto& p : problems) log_->appendPlainText(qstr(p));
        refresh();
    });
    connect(unload, &QPushButton::clicked, this, [this] {
        if (auto* it = list_->currentItem()) {
            ctx_.plugins().unload(sstr(it->data(0, Qt::UserRole).toString()));
            refresh();
        }
    });
    connect(go, &QPushButton::clicked, this, [this] { runCommand(command_->currentText(), args_->text()); });
    refresh();
}

void PluginsDialog::refresh() {
    list_->clear();
    command_->clear();
    for (const auto& s : ctx_.plugins().plugins()) {
        auto* it = new QTreeWidgetItem(list_);
        it->setText(0, qstr(s.manifest.name.empty() ? s.manifest.id : s.manifest.name));
        it->setData(0, Qt::UserRole, qstr(s.manifest.id));
        it->setToolTip(0, qstr(s.manifest.description));
        it->setText(1, qstr(s.manifest.version));
        it->setText(2, s.loaded ? tr("Loaded") : tr("Not loaded: %1").arg(qstr(s.error)));
        QStringList prov;
        for (const auto& c : s.commands) prov << tr("command %1").arg(qstr(c));
        for (const auto& c : s.importers) prov << tr("import %1").arg(qstr(c));
        for (const auto& c : s.exporters) prov << tr("export %1").arg(qstr(c));
        it->setText(3, prov.join(", "));
        it->setText(4, QString::fromStdU16String(s.manifest.folder.u16string()));
    }
    for (const auto& c : ctx_.plugins().commands()) command_->addItem(qstr(c.name));
}

bool PluginsDialog::runCommand(const QString& name, const QString& args) {
    if (name.isEmpty()) return false;
    const auto r = ctx_.plugins().runCommand(ctx_.project(), sstr(name), sstr(args));
    for (const auto& l : r.log) log_->appendPlainText(qstr(l));
    if (r.ok) log_->appendPlainText(tr("%1: %2").arg(name, qstr(r.text)));
    else log_->appendPlainText(tr("%1 failed (%2): %3%4").arg(name).arg(r.code).arg(qstr(r.error), r.rolledBack ? tr(" — its open edit was rolled back") : QString()));
    return r.ok;
}

// ---- repository ---------------------------------------------------------------------------------------

bool RepositoryDialog::available() {
#ifdef OPENMINE_APP_HAVE_REPOSITORY
    return true;
#else
    return false;
#endif
}

RepositoryDialog::RepositoryDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Shared repository (PostgreSQL / PostGIS)"));
    resize(860, 620);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    dsn_ = new QLineEdit(ctx_.settings().value("repository/dsn", "host=localhost dbname=openmine").toString(), this);
    dsn_->setToolTip(tr("libpq connection string; passwords are taken from ~/.pgpass or PGPASSWORD, never stored by OpenMine"));
    author_ = new QLineEdit(qstr(ctx_.project().userName()), this);
    message_ = new QLineEdit(this);
    policy_ = new QComboBox(this);
    policy_->addItem(tr("Abort on conflicts"), 0);
    policy_->addItem(tr("Take the repository's version"), 1);
    policy_->addItem(tr("Keep my version"), 2);
    form->addRow(tr("Connection"), dsn_);
    form->addRow(tr("Author"), author_);
    form->addRow(tr("Message"), message_);
    form->addRow(tr("Pull conflicts"), policy_);
    lay->addLayout(form);
    auto* row = new QHBoxLayout;
    auto* conn = new QPushButton(tr("Connect"), this);
    auto* init = new QPushButton(tr("Initialise schema"), this);
    auto* create = new QPushButton(tr("Share this project"), this);
    auto* checkout = new QPushButton(tr("Check out…"), this);
    auto* pull = new QPushButton(tr("Pull"), this);
    auto* publish = new QPushButton(tr("Publish"), this);
    for (auto* b : {conn, init, create, checkout, pull, publish}) row->addWidget(b);
    lay->addLayout(row);
    history_ = new QTreeWidget(this);
    history_->setColumnCount(5);
    history_->setHeaderLabels({tr("Revision"), tr("Author"), tr("Date"), tr("Changes"), tr("Message")});
    lay->addWidget(history_, 1);
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumHeight(170);
    lay->addWidget(log_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
#ifdef OPENMINE_APP_HAVE_REPOSITORY
    using namespace repository;
    auto repo = [this]() -> PgRepository* { return static_cast<PgRepository*>(repo_.get()); };
    auto projectFile = [this]() -> std::optional<std::filesystem::path> {
        if (ctx_.project().filePath().empty()) {
            log_->appendPlainText(tr("Save the project first: the working-copy state is kept next to the project file."));
            return std::nullopt;
        }
        return ctx_.project().filePath();
    };
    auto guarded = [this](const QString& what, const std::function<void()>& f) {
        try {
            f();
        } catch (const std::exception& e) {
            log_->appendPlainText(tr("%1 failed: %2").arg(what, qstr(e.what())));
        }
    };
    connect(conn, &QPushButton::clicked, this, &RepositoryDialog::connectRepo);
    connect(init, &QPushButton::clicked, this, [this, repo, guarded] {
        if (!repo()) connectRepo();
        if (!repo()) return;
        guarded(tr("Initialise"), [&] {
            repo()->initialise();
            log_->appendPlainText(tr("Schema version %1 ready").arg(repo()->schemaVersion()));
        });
    });
    connect(create, &QPushButton::clicked, this, [this, repo, guarded, projectFile] {
        if (!repo()) connectRepo();
        auto f = projectFile();
        if (!repo() || !f) return;
        guarded(tr("Share"), [&] {
            auto wc = repo()->createProject(ctx_.project(), sstr(author_->text()), message_->text().isEmpty() ? "Initial import" : sstr(message_->text()));
            wc.save(*f);
            log_->appendPlainText(tr("Project shared at revision %1").arg(wc.baseRevision));
            showStatus();
        });
    });
    connect(checkout, &QPushButton::clicked, this, [this, repo, guarded] {
        if (!repo()) connectRepo();
        if (!repo()) return;
        guarded(tr("Check out"), [&] {
            QStringList names;
            const auto projects = repo()->projects();
            for (const auto& [id, name] : projects) names << QStringLiteral("%1  (%2)").arg(qstr(name), uuidText(id));
            if (names.isEmpty()) {
                log_->appendPlainText(tr("The repository has no projects"));
                return;
            }
            bool ok = false;
            const QString pick = QInputDialog::getItem(this, tr("Check out"), tr("Project:"), names, 0, false, &ok);
            if (!ok) return;
            const Uuid id = projects[static_cast<std::size_t>(names.indexOf(pick))].first;
            WorkingCopy wc;
            auto project = repo()->checkout(id, wc);
            log_->appendPlainText(tr("Checked out revision %1; save it to a project file to keep the working copy").arg(wc.baseRevision));
            ctx_.adoptProject(std::move(project), tr("Checked out from the repository (revision %1)").arg(wc.baseRevision));
            ctx_.setPendingWorkingCopy(wc.toJson());
        });
    });
    connect(pull, &QPushButton::clicked, this, [this, repo, guarded, projectFile] {
        if (!repo()) connectRepo();
        auto f = projectFile();
        if (!repo() || !f) return;
        guarded(tr("Pull"), [&] {
            auto wc = WorkingCopy::load(*f);
            if (!wc) throw RepositoryError("This project is not a working copy of a repository");
            const auto policy = static_cast<ConflictPolicy>(policy_->currentData().toInt());
            const auto r = repo()->pull(ctx_.project(), *wc, policy);
            for (const auto& c : r.conflicts) log_->appendPlainText(tr("Conflict: %1 %2 — %3").arg(qstr(c.type), qstr(c.name), qstr(c.reason)));
            if (r.aborted) log_->appendPlainText(tr("Pull aborted: nothing changed"));
            else {
                wc->save(*f);
                log_->appendPlainText(tr("Pulled revisions %1 → %2: %3 changes applied").arg(r.fromRevision).arg(r.toRevision).arg(r.applied));
            }
            showStatus();
        });
    });
    connect(publish, &QPushButton::clicked, this, [this, repo, guarded, projectFile] {
        if (!repo()) connectRepo();
        auto f = projectFile();
        if (!repo() || !f) return;
        guarded(tr("Publish"), [&] {
            auto wc = WorkingCopy::load(*f);
            if (!wc) throw RepositoryError("This project is not a working copy of a repository");
            if (message_->text().trimmed().isEmpty()) throw RepositoryError("Enter a message describing the change");
            const auto r = repo()->publish(ctx_.project(), *wc, sstr(author_->text()), sstr(message_->text()));
            if (!r.published) {
                log_->appendPlainText(tr("Not published: %1").arg(qstr(r.refusal)));
                for (const auto& c : r.conflicts) log_->appendPlainText(tr("Conflict: %1 %2 — %3").arg(qstr(c.type), qstr(c.name), qstr(c.reason)));
                return;
            }
            wc->save(*f);
            log_->appendPlainText(tr("Published revision %1: %2 changes, %3 assets uploaded").arg(r.revision).arg(r.changes).arg(r.assetsUploaded));
            showStatus();
        });
    });
#else
    for (auto* b : {conn, init, create, checkout, pull, publish}) b->setEnabled(false);
    log_->setPlainText(tr("Not available: this build has no PostgreSQL client (libpq)."));
#endif
}

void RepositoryDialog::connectRepo() {
#ifdef OPENMINE_APP_HAVE_REPOSITORY
    try {
        repo_ = std::make_shared<repository::PgRepository>(sstr(dsn_->text()));
        auto* r = static_cast<repository::PgRepository*>(repo_.get());
        ctx_.settings().setValue("repository/dsn", dsn_->text());
        log_->appendPlainText(tr("Connected: %1, PostGIS %2").arg(qstr(r->serverVersion()), qstr(r->postgisVersion())));
        showStatus();
    } catch (const std::exception& e) {
        repo_.reset();
        log_->appendPlainText(tr("Connection failed: %1").arg(qstr(e.what())));
    }
#endif
}

void RepositoryDialog::showStatus() {
#ifdef OPENMINE_APP_HAVE_REPOSITORY
    history_->clear();
    auto* r = static_cast<repository::PgRepository*>(repo_.get());
    if (!r || ctx_.project().filePath().empty()) return;
    const auto wc = repository::WorkingCopy::load(ctx_.project().filePath());
    if (!wc) return;
    try {
        for (const auto& rev : r->revisions(wc->projectId)) {
            auto* it = new QTreeWidgetItem(history_);
            it->setText(0, QString::number(rev.revision));
            it->setText(1, qstr(rev.author));
            it->setText(2, qstr(rev.createdAt));
            it->setText(3, QString::number(rev.changes));
            it->setText(4, qstr(rev.message));
            if (rev.revision == wc->baseRevision) it->setText(0, it->text(0) + tr("  ← working copy"));
        }
        const auto changes = repository::localChanges(ctx_.project(), *wc);
        log_->appendPlainText(tr("Working copy at revision %1 (head %2), %3 local changes").arg(wc->baseRevision).arg(r->headRevision(wc->projectId)).arg(changes.size()));
        for (const auto& lock : r->locks(wc->projectId)) log_->appendPlainText(tr("Locked: %1 %2 by %3 until %4").arg(qstr(lock.type), qstr(lock.key), qstr(lock.holder), qstr(lock.expiresAt)));
    } catch (const std::exception& e) {
        log_->appendPlainText(qstr(e.what()));
    }
#endif
}

} // namespace openmine::app
