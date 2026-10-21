// SPDX-License-Identifier: Apache-2.0
#include "ReportsPanel.h"

#include "../app/EntityInfo.h"
#include "../theme/Theme.h"
#include "../viewport/SceneTypes.h"

#include "openmine/geology/BlockModel.h"
#include "openmine/mine_design/Development.h"
#include "openmine/reporting/ReportDocument.h"
#include "openmine/reporting/ScheduleExport.h"
#include "openmine/reporting/SurveyReports.h"
#include "openmine/scheduling/Levelling.h"
#include "openmine/scheduling/ScenarioSchedule.h"
#include "openmine/scheduling/Periods.h"
#include "openmine/survey/Conformance.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>

#include <cstring>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#ifdef OPENMINE_APP_HAVE_QTPDF
#include <QPdfDocument>
#include <QPdfView>
#endif

#include <cmath>
#include <fstream>
#include <sstream>

namespace openmine::app {

namespace {

std::string readFile(const std::filesystem::path& f) {
    std::ifstream in(f, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::filesystem::path tempFile(const std::string& ext) {
    const auto dir = std::filesystem::temp_directory_path() / "openmine-reports";
    std::filesystem::create_directories(dir);
    return dir / (Uuid::generate().toString() + "." + ext);
}

/// Schedule input and result of a snapshot for the given scenario (jobs must not use the live
/// schedule state, which points into the live model).
struct SnapshotSchedule {
    scheduling::ScheduleInput input;
    scheduling::ScheduleResult result;
};
SnapshotSchedule scheduleOf(const Project& p, const Uuid& scenarioId) {
    SnapshotSchedule s;
    auto calc = scheduling::calculateScenario(p, p.model().scenarios.find(scenarioId));
    s.input = std::move(calc.input);
    s.result = std::move(calc.result);
    return s;
}

const ScheduleBaseline* latestBaseline(const ProjectModel& m) {
    for (auto it = m.baselines.all().rbegin(); it != m.baselines.all().rend(); ++it)
        if (it->approved) return &*it;
    return nullptr;
}

} // namespace

ReportsPanel::ReportsPanel(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* gen = new QHBoxLayout;
    kind_ = new QComboBox(this);
    kind_->setMinimumContentsLength(12);
    kind_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    kind_->setToolTip(tr("Kind of report to generate"));
    format_ = new QComboBox(this);
    format_->addItems({"pdf", "html"});
    auto* go = new QPushButton(toolIcon("report"), tr("Generate"), this);
    gen->addWidget(kind_, 1);
    gen->addWidget(format_);
    gen->addWidget(go);
    lay->addLayout(gen);
    // Stored reports above the preview: the panel lives in a narrow side dock.
    auto* split = new QSplitter(Qt::Vertical, this);
    list_ = new QListWidget(split);
    list_->setMinimumWidth(180);
    auto* right = new QWidget(split);
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QHBoxLayout;
    info_ = new QLabel(right);
    info_->setWordWrap(true);
    auto* open = new QPushButton(tr("Open"), right);
    auto* save = new QPushButton(tr("Save copy…"), right);
    bar->addWidget(info_, 1);
    bar->addWidget(open);
    bar->addWidget(save);
    rl->addLayout(bar);
    stack_ = new QStackedWidget(right);
    html_ = new QTextBrowser(stack_);
    html_->setOpenExternalLinks(true);
    stack_->addWidget(html_);
#ifdef OPENMINE_APP_HAVE_QTPDF
    auto* doc = new QPdfDocument(this);
    pdfDocument_ = doc;
    auto* view = new QPdfView(stack_);
    view->setDocument(doc);
    view->setPageMode(QPdfView::PageMode::MultiPage);
    view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    pdfView_ = view;
    stack_->addWidget(view);
#endif
    rl->addWidget(stack_, 1);
    split->addWidget(list_);
    split->addWidget(right);
    split->setStretchFactor(1, 3);
    lay->addWidget(split, 1);

    connect(go, &QPushButton::clicked, this, [this] { generate(kind_->currentData().toString(), format_->currentText()); });
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) {
        if (it) preview(uuidFrom(it->data(Qt::UserRole).toString()));
    });
    connect(open, &QPushButton::clicked, this, [this] {
        if (auto* it = list_->currentItem()) openExternally(uuidFrom(it->data(Qt::UserRole).toString()));
    });
    connect(save, &QPushButton::clicked, this, [this] {
        auto* it = list_->currentItem();
        if (!it) return;
        const Uuid id = uuidFrom(it->data(Qt::UserRole).toString());
        const auto* r = ctx_.project().model().reports.find(id);
        if (!r) return;
        const QString f = QFileDialog::getSaveFileName(this, tr("Save report"), qstr(r->name + "." + r->format));
        if (!f.isEmpty() && saveCopy(id, f)) ctx_.report(tr("Saved %1").arg(f));
    });
    connect(&ctx_, &AppContext::modelChanged, this, [this] {
        rebuild();
        updateKinds();
    });
    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
        rebuild();
        updateKinds();
    });
    connect(&ctx_, &AppContext::selectionChanged, this, &ReportsPanel::updateKinds);
    rebuild();
    updateKinds();
}

void ReportsPanel::rebuild() {
    const QString keep = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString();
    list_->clear();
    if (!ctx_.hasProject()) return;
    for (const auto& r : ctx_.project().model().reports.all()) {
        auto* it = new QListWidgetItem(toolIcon(r.format == "pdf" ? "pdf" : r.format == "xlsx" ? "xlsx" : "report"),
                                       QStringLiteral("%1\n%2 · %3").arg(qstr(r.name), qstr(r.kind), qstr(r.createdAt.substr(0, 16))), list_);
        it->setData(Qt::UserRole, uuidText(r.id));
        if (uuidText(r.id) == keep) list_->setCurrentItem(it);
    }
}

QStringList ReportsPanel::availableKinds() const {
    QStringList k;
    for (int i = 0; i < kind_->count(); ++i) k << kind_->itemData(i).toString();
    return k;
}

void ReportsPanel::updateKinds() {
    const QString cur = kind_->currentData().toString();
    kind_->clear();
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    const auto prim = ctx_.primary();
    if (prim.kind == EntityKind::Conformance) kind_->addItem(tr("Design conformance (selected result)"), "conformance");
    if (prim.kind == EntityKind::Registration) kind_->addItem(tr("Registration (selected)"), "registration");
    if (prim.kind == EntityKind::PointCloud) kind_->addItem(tr("Point-cloud dataset (selected)"), "pointcloud");
    if (prim.kind == EntityKind::BlockModel) kind_->addItem(tr("Grade–tonnage (selected block model)"), "gradetonnage");
    if (!m.activities.empty()) {
        kind_->addItem(tr("Schedule Gantt chart (PDF)"), "gantt");
        kind_->addItem(tr("Schedule workbook (XLSX)"), "schedule-xlsx");
        kind_->addItem(tr("Production by month"), "production");
        if (!m.scenarios.empty()) kind_->addItem(tr("Scenario comparison"), "scenarios");
    }
    if (!m.developments.empty()) {
        kind_->addItem(tr("Development quantities"), "quantities");
        kind_->addItem(tr("Development quantities (XLSX)"), "quantities-xlsx");
    }
    const int i = kind_->findData(cur);
    if (i >= 0) kind_->setCurrentIndex(i);
}

Uuid ReportsPanel::generate(const QString& kind, const QString& format) {
    if (!ctx_.hasProject()) return {};
    const auto snap = ctx_.snapshot();
    const auto prim = ctx_.primary();
    const Uuid scenario = ctx_.schedule().scenarioId;
    const std::string fmt = sstr(format);
    // Builds a document from the snapshot (worker thread), stores it on the UI thread.
    auto documentJob = [this, snap, fmt](const QString& name, const std::string& reportKind, std::function<reporting::ReportDocument(const Project&, jobs::Progress&)> build) {
        ctx_.jobs().start(name, tr("Report"), [this, snap, fmt, reportKind, build](jobs::Progress& pr) -> UiApply {
            auto doc = std::make_shared<reporting::ReportDocument>(build(*snap, pr));
            return [this, doc, fmt, reportKind] {
                const Uuid id = reporting::storeReport(ctx_.project(), *doc, reportKind, fmt);
                ctx_.report(tr("Report '%1' stored in the project").arg(qstr(doc->title)));
                preview(id);
            };
        });
    };
    auto fileJob = [this, snap](const QString& name, const std::string& reportKind, const std::string& ext,
                                std::function<void(const Project&, const std::filesystem::path&, jobs::Progress&)> write) {
        ctx_.jobs().start(name, tr("Report"), [this, snap, name, reportKind, ext, write](jobs::Progress& pr) -> UiApply {
            const auto f = tempFile(ext);
            write(*snap, f, pr);
            auto bytes = std::make_shared<std::string>(readFile(f));
            std::filesystem::remove(f);
            return [this, bytes, name, reportKind, ext] {
                const Uuid id = reporting::storeReportBytes(ctx_.project(), *bytes, sstr(name), reportKind, ext);
                ctx_.report(tr("Report '%1' stored in the project").arg(name));
                preview(id);
            };
        });
    };

    if (kind == "conformance") {
        const Uuid id = prim.id;
        documentJob(tr("Conformance report"), "Survey", [id](const Project& p, jobs::Progress& pr) {
            const auto* c = p.model().conformanceResults.find(id);
            if (!c) throw ValidationError("The conformance result no longer exists");
            // Re-run the analysis for the worst cross-sections (the stored table has no sections).
            survey::ConformanceOptions o;
            o.interval = c->stationInterval;
            o.fromChainage = c->chainageFrom;
            o.toChainage = c->chainageTo;
            std::optional<survey::ConformanceReport> full;
            try {
                full = survey::analyseAsBuilt(p, c->asBuiltId, c->developmentId, o, pr);
            } catch (const std::exception&) {
                // Inputs changed or were removed: report the stored result without sections.
            }
            return reporting::conformanceReport(p, id, full ? &*full : nullptr);
        });
    } else if (kind == "registration") {
        const Uuid id = prim.id;
        documentJob(tr("Registration report"), "Survey", [id](const Project& p, jobs::Progress&) { return reporting::registrationReport(p, id); });
    } else if (kind == "pointcloud") {
        const Uuid id = prim.id;
        documentJob(tr("Point-cloud report"), "Survey", [id](const Project& p, jobs::Progress&) { return reporting::pointCloudReport(p, id, true); });
    } else if (kind == "gradetonnage") {
        const Uuid id = prim.id;
        documentJob(tr("Grade–tonnage report"), "Resource", [id](const Project& p, jobs::Progress&) {
            const auto bmd = geology::loadBlockModel(p, id);
            if (bmd.header.attributes.empty()) throw ValidationError("The block model has no attributes");
            geology::GradeTonnageOptions o;
            // Grade: the first attribute that is not an estimation diagnostic (_NS, _DIST, _PASS,
            // _KV, _SLOPE, _NEGW) nor a density.
            auto diagnostic = [](const std::string& a) {
                for (const char* suffix : {"_NS", "_DIST", "_PASS", "_KV", "_SLOPE", "_NEGW"})
                    if (a.size() > std::strlen(suffix) && a.compare(a.size() - std::strlen(suffix), std::string::npos, suffix) == 0) return true;
                return false;
            };
            for (const auto& a : bmd.header.attributes)
                if (o.gradeField.empty() && !diagnostic(a) && a != "density" && a != "sg") o.gradeField = a;
            if (o.gradeField.empty()) throw ValidationError("The block model has no grade attribute");
            for (const auto& a : bmd.header.attributes)
                if (a == "density" || a == "sg") o.densityField = a;
                else if (a != o.gradeField && !diagnostic(a)) o.otherFields.push_back(a);
            // Cut-offs: 0 and the 10…90 % quantiles of the grade.
            std::vector<double> g;
            for (double v : *bmd.find(o.gradeField))
                if (std::isfinite(v)) g.push_back(v);
            if (g.empty()) throw ValidationError("The grade field has no values");
            std::sort(g.begin(), g.end());
            o.cutoffs = {0.0};
            for (int q = 1; q < 10; ++q) o.cutoffs.push_back(g[g.size() * q / 10]);
            o.cutoffs.erase(std::unique(o.cutoffs.begin(), o.cutoffs.end()), o.cutoffs.end());
            const auto rows = geology::gradeTonnage(bmd, o);
            reporting::ReportDocument d;
            d.title = "Grade–tonnage: " + bmd.header.name;
            d.subtitle = "Cut-off on " + o.gradeField + (o.densityField.empty() ? " (density 2.7 t/m³)" : " (density from " + o.densityField + ")");
            d.meta = reporting::standardMeta(p);
            reporting::ReportTable t;
            t.header = {"Cut-off", "Blocks", "Volume (m³)", "Tonnes", "Grade " + o.gradeField, "Metal"};
            reporting::LineChart chart;
            chart.title = "Tonnes and grade above cut-off";
            chart.xLabel = "Cut-off";
            chart.yLabel = "Tonnes (Mt)";
            reporting::ChartSeries ts{"Tonnes (Mt)", {}, 0x1F77B4, false};
            for (const auto& r : rows) {
                const double grade = r.grades.count(o.gradeField) ? r.grades.at(o.gradeField) : 0.0;
                const double metal = r.metal.count(o.gradeField) ? r.metal.at(o.gradeField) : 0.0;
                t.rows.push_back({reporting::fmtNumber(r.cutoff, 3), std::to_string(r.blocks), reporting::fmtNumber(r.volume, 0, true),
                                  reporting::fmtNumber(r.tonnes, 0, true), reporting::fmtNumber(grade, 3), reporting::fmtNumber(metal, 1, true)});
                ts.points.push_back({r.cutoff, r.tonnes / 1e6});
            }
            chart.series.push_back(ts);
            d.heading("Grade–tonnage table");
            d.table(t);
            d.chart(chart);
            d.note("Tonnages use whole blocks (no partial-block proportions unless a proportion field is set). Values are as estimated in the block model.");
            return d;
        });
    } else if (kind == "gantt") {
        fileJob(tr("Gantt chart"), "Schedule", "pdf", [scenario](const Project& p, const std::filesystem::path& f, jobs::Progress&) {
            const auto s = scheduleOf(p, scenario);
            if (!s.result.ok) throw ValidationError("The schedule cannot be calculated: " + (s.result.errors.empty() ? std::string() : s.result.errors.front()));
            reporting::writeGanttPdf(p, s.input, s.result, latestBaseline(p.model()), f);
        });
    } else if (kind == "schedule-xlsx") {
        fileJob(tr("Schedule workbook"), "Schedule", "xlsx", [scenario](const Project& p, const std::filesystem::path& f, jobs::Progress&) {
            const auto s = scheduleOf(p, scenario);
            if (!s.result.ok) throw ValidationError("The schedule cannot be calculated");
            reporting::writeScheduleXlsx(p, scheduling::scheduleTable(p, s.input, s.result, latestBaseline(p.model())), f);
        });
    } else if (kind == "production") {
        documentJob(tr("Production report"), "Schedule", [scenario](const Project& p, jobs::Progress&) {
            const auto s = scheduleOf(p, scenario);
            if (!s.result.ok) throw ValidationError("The schedule cannot be calculated");
            const auto q = scheduling::quantitiesByPeriod(s.input, s.result, scheduling::PeriodKind::Month);
            reporting::ReportDocument d;
            d.title = "Development production by month";
            d.subtitle = p.model().info.name;
            d.meta = reporting::standardMeta(p);
            reporting::ReportTable t;
            t.header = {"Month", "Scheduled (m)", "Actual (m)", "Target (m)", "Scheduled − target (m)"};
            reporting::LineChart c;
            c.title = "Metres per month";
            c.xLabel = "Month index";
            c.yLabel = "m";
            reporting::ChartSeries sch{"Scheduled", {}, 0x1F77B4, false}, act{"Actual", {}, 0x2CA02C, false}, tgt{"Target", {}, 0xD62728, true};
            double cumS = 0, cumA = 0;
            for (std::size_t i = 0; i < q.size(); ++i) {
                t.rows.push_back({q[i].period.label, reporting::fmtNumber(q[i].scheduled, 1), reporting::fmtNumber(q[i].actual, 1), reporting::fmtNumber(q[i].target, 1),
                                  reporting::fmtNumber(q[i].scheduled - q[i].target, 1)});
                sch.points.push_back({static_cast<double>(i), q[i].scheduled});
                act.points.push_back({static_cast<double>(i), q[i].actual});
                tgt.points.push_back({static_cast<double>(i), q[i].target});
                cumS += q[i].scheduled;
                cumA += q[i].actual;
            }
            c.series = {sch, act, tgt};
            d.keyValues({{"Total scheduled (m)", reporting::fmtNumber(cumS, 1, true)}, {"Total actual (m)", reporting::fmtNumber(cumA, 1, true)},
                         {"Schedule finish", s.result.projectFinishDate.toString()}});
            d.table(t);
            d.chart(c);
            d.note("Scheduled quantities are the remaining quantities spread over the remaining working time of each activity; actuals are dated production records.");
            return d;
        });
    } else if (kind == "scenarios") {
        documentJob(tr("Scenario comparison"), "Schedule", [](const Project& p, jobs::Progress& pr) {
            reporting::ReportDocument d;
            d.title = "Schedule scenario comparison";
            d.subtitle = p.model().info.name;
            d.meta = reporting::standardMeta(p);
            reporting::ReportTable t;
            t.header = {"Scenario", "Finish", "Duration (days)", "Critical activities", "Scheduled (m)", "Levelling"};
            reporting::LineChart c;
            c.title = "Scheduled metres per month";
            c.xLabel = "Month index";
            c.yLabel = "m";
            std::vector<const Scenario*> list{nullptr};
            for (const auto& s : p.model().scenarios.all()) list.push_back(&s);
            std::size_t k = 0;
            for (const auto* sc : list) {
                pr.update(static_cast<double>(k) / list.size(), "Calculating scenarios");
                const auto calc = scheduling::calculateScenario(p, sc);
                const auto sum = scheduling::summarise(sc ? sc->name : std::string("Base schedule"), calc.input, calc.result, scheduling::PeriodKind::Month);
                t.rows.push_back({sum.name, sum.ok ? sum.finish.toString() : "—", sum.ok ? reporting::fmtNumber(sum.makespanDays, 1) : sum.error,
                                  std::to_string(sum.criticalCount), reporting::fmtNumber(sum.scheduledQuantity, 1, true), sum.levellingSummary});
                reporting::ChartSeries ser{sum.name, {}, static_cast<std::uint32_t>(categoricalColor(k)), false};
                for (std::size_t i = 0; i < sum.quantities.size(); ++i) ser.points.push_back({static_cast<double>(i), sum.quantities[i].scheduled});
                c.series.push_back(ser);
                ++k;
            }
            d.table(t);
            d.chart(c);
            return d;
        });
    } else if (kind == "quantities" || kind == "quantities-xlsx") {
        if (kind == "quantities-xlsx") {
            fileJob(tr("Development quantities"), "Design", "xlsx", [](const Project& p, const std::filesystem::path& f, jobs::Progress&) {
                reporting::Sheet s;
                s.name = "Quantities";
                s.header = {"Heading", "Level", "Zone", "Type", "Status", "Profile", "Material", "Length (m)", "Plan length (m)", "Gradient (%)", "Area (m²)",
                            "Volume (m³)", "Tonnes", "Loose (m³)", "Scheduled (m)", "Schedule matches design"};
                for (const auto& r : mine_design::developmentQuantities(p))
                    s.rows.push_back({r.headingId, r.level, r.zone, r.activityType, r.status, r.profile, r.material, r.length, r.planLength, r.gradient, r.area,
                                      r.volume, r.tonnes, r.looseVolume, r.scheduledQuantity, std::string(r.scheduleMatchesDesign ? "yes" : "NO")});
                reporting::writeXlsx(f, {s});
            });
        } else {
            documentJob(tr("Development quantities"), "Design", [](const Project& p, jobs::Progress&) {
                reporting::ReportDocument d;
                d.title = "Development quantities";
                d.subtitle = p.model().info.name;
                d.meta = reporting::standardMeta(p);
                reporting::ReportTable t;
                t.header = {"Heading", "Level", "Type", "Length (m)", "Area (m²)", "Volume (m³)", "Tonnes", "Scheduled (m)"};
                double len = 0, vol = 0, ton = 0;
                int i = 0;
                for (const auto& r : mine_design::developmentQuantities(p)) {
                    t.rows.push_back({r.headingId, r.level, r.activityType, reporting::fmtNumber(r.length, 1), reporting::fmtNumber(r.area, 2),
                                      reporting::fmtNumber(r.volume, 0, true), reporting::fmtNumber(r.tonnes, 0, true), reporting::fmtNumber(r.scheduledQuantity, 1)});
                    if (!r.scheduleMatchesDesign) t.highlightRows.push_back(i);
                    len += r.length;
                    vol += r.volume;
                    ton += r.tonnes;
                    ++i;
                }
                d.keyValues({{"Total length (m)", reporting::fmtNumber(len, 1, true)}, {"Total volume (m³)", reporting::fmtNumber(vol, 0, true)},
                             {"Total tonnes", reporting::fmtNumber(ton, 0, true)}});
                d.table(t);
                if (!t.highlightRows.empty()) d.note("Highlighted headings: the scheduled quantity differs from the current design length (design changed since approval).");
                return d;
            });
        }
    } else {
        ctx_.report(tr("No report of kind '%1'").arg(kind), MessageLevel::Error);
    }
    return {};
}

QString ReportsPanel::materialise(const Uuid& id) const {
    if (!ctx_.hasProject()) return {};
    const auto& m = ctx_.project().model();
    const auto* r = m.reports.find(id);
    if (!r) return {};
    const auto* a = m.assets.find(r->asset.id);
    if (!a) return {};
    const auto src = ctx_.project().assets().pathOf(*a);
    // A copy named after the report (external viewers show the file name).
    QString safe = qstr(r->name);
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9 _.-]")), "_");
    const auto dst = std::filesystem::temp_directory_path() / "openmine-reports" / (sstr(safe) + "-" + id.toString().substr(0, 8) + "." + r->format);
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return {};
    return QString::fromStdU16String(dst.u16string());
}

void ReportsPanel::preview(const Uuid& id) {
    const auto* r = ctx_.hasProject() ? ctx_.project().model().reports.find(id) : nullptr;
    if (!r) return;
    info_->setText(QStringLiteral("<b>%1</b> — %2, %3, %4").arg(qstr(r->name).toHtmlEscaped(), qstr(r->kind), qstr(r->format), qstr(r->createdAt)));
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(Qt::UserRole).toString() == uuidText(id)) {
            const QSignalBlocker b(list_);
            list_->setCurrentRow(i);
        }
    const QString path = materialise(id);
    if (path.isEmpty()) {
        html_->setPlainText(tr("The report file is missing from the asset folder."));
        stack_->setCurrentWidget(html_);
        return;
    }
#ifdef OPENMINE_APP_HAVE_QTPDF
    if (r->format == "pdf") {
        auto* doc = static_cast<QPdfDocument*>(pdfDocument_);
        doc->load(path);
        stack_->setCurrentWidget(pdfView_);
        return;
    }
#endif
    if (r->format == "html") {
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        QString text = QString::fromUtf8(f.readAll());
        // Qt's rich text has no SVG: charts are shown when the report is opened in a browser.
        text.replace(QRegularExpression(QStringLiteral("<svg[\\s\\S]*?</svg>")), tr("<p><i>[chart — open the report to view]</i></p>"));
        html_->setHtml(text);
    } else {
        html_->setHtml(tr("<p>%1 file (%2).</p><p>Use <b>Open</b> to view it in the system viewer.</p>").arg(qstr(r->format).toUpper(), path.toHtmlEscaped()));
    }
    stack_->setCurrentWidget(html_);
}

bool ReportsPanel::openExternally(const Uuid& id) {
    const QString path = materialise(id);
    if (path.isEmpty()) {
        ctx_.report(tr("The report file is missing"), MessageLevel::Error);
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool ReportsPanel::saveCopy(const Uuid& id, const QString& target) {
    const QString path = materialise(id);
    if (path.isEmpty()) return false;
    QFile::remove(target);
    if (!QFile::copy(path, target)) {
        ctx_.report(tr("Could not write %1").arg(target), MessageLevel::Error);
        return false;
    }
    return true;
}

} // namespace openmine::app
