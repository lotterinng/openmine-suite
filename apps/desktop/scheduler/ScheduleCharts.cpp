// SPDX-License-Identifier: Apache-2.0
#include "ScheduleCharts.h"

#include "../theme/Theme.h"
#include "../viewport/SceneTypes.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

namespace openmine::app {

namespace {
double niceCeil(double v) {
    if (!(v > 0)) return 1;
    const double mag = std::pow(10.0, std::floor(std::log10(v)));
    for (double f : {1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0})
        if (v <= f * mag) return f * mag;
    return 10 * mag;
}
} // namespace

// ---- resource histogram -----------------------------------------------------------------------------

ResourceHistogram::ResourceHistogram(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    setMouseTracking(true);
    setMinimumHeight(120);
    connect(&ctx_, &AppContext::scheduleRecalculated, this, &ResourceHistogram::recompute);
    connect(&ctx_, &AppContext::selectionChanged, this, qOverload<>(&QWidget::update));
    recompute();
}

void ResourceHistogram::recompute() {
    profiles_.clear();
    const auto& s = ctx_.schedule();
    if (ctx_.hasProject() && s.result.ok) {
        profiles_ = scheduling::resourceProfiles(s.input, s.result);
        t0_ = s.result.projectStart;
        t1_ = std::max(s.result.projectFinish, t0_ + 1440);
    }
    update();
}

void ResourceHistogram::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& c = Theme::colors();
    p.fillRect(rect(), c.panel);
    if (profiles_.empty()) {
        p.setPen(c.textMuted);
        p.drawText(rect(), Qt::AlignCenter, ctx_.hasProject() && !ctx_.schedule().result.ok ? tr("Schedule not calculated") : tr("No resources assigned"));
        return;
    }
    // Selected resources first; otherwise every resource as a small multiple.
    std::vector<const scheduling::ResourceProfile*> shown;
    for (const auto& pr : profiles_)
        if (ctx_.isSelected(EntityKind::Resource, pr.resourceId)) shown.push_back(&pr);
    if (shown.empty())
        for (const auto& pr : profiles_) shown.push_back(&pr);
    const int left = 130, right = 12;
    const double rowH = std::max(36.0, (height() - 22.0) / shown.size());
    const double w = width() - left - right;
    auto xOf = [&](double t) { return left + (t - t0_) / (t1_ - t0_) * w; };
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const auto& pr = *shown[i];
        const double top = 4 + i * rowH, bottom = top + rowH - 8;
        const double ymax = niceCeil(std::max(pr.peak, pr.capacity) * 1.1);
        auto yOf = [&](double u) { return bottom - u / ymax * (bottom - top); };
        p.setPen(c.text);
        p.drawText(QRectF(4, top, left - 8, rowH - 8), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap,
                   QStringLiteral("%1\npeak %2 / %3").arg(qstr(pr.name)).arg(pr.peak, 0, 'g', 3).arg(pr.capacity, 0, 'g', 3));
        p.setPen(c.border);
        p.drawLine(QPointF(left, bottom), QPointF(left + w, bottom));
        for (const auto& s : pr.steps) {
            if (s.units <= 0) continue;
            const QRectF r(QPointF(xOf(s.start), yOf(s.units)), QPointF(xOf(s.end), bottom));
            p.fillRect(r, s.units > pr.capacity + 1e-9 ? c.ganttCritical : c.ganttTask);
        }
        p.setPen(QPen(c.accent, 1.5, Qt::DashLine));
        p.drawLine(QPointF(left, yOf(pr.capacity)), QPointF(left + w, yOf(pr.capacity)));
    }
    // Time axis labels (months).
    p.setPen(c.textMuted);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    const Date d0 = DateTime(static_cast<std::int64_t>(t0_)).date(), d1 = DateTime(static_cast<std::int64_t>(t1_)).date();
    const long days = d1.days() - d0.days();
    for (Date d = d0; d <= d1; d = d.addDays(1)) {
        int y;
        unsigned mo, da;
        d.toYmd(y, mo, da);
        if (da == 1 && (days < 800 || mo % 3 == 1)) p.drawText(QPointF(xOf(d.days() * 1440.0) + 2, height() - 4), QStringLiteral("%1-%2").arg(y).arg(mo, 2, 10, QChar('0')));
    }
}

void ResourceHistogram::mouseMoveEvent(QMouseEvent* e) {
    if (profiles_.empty()) return;
    const int left = 130, right = 12;
    const double w = width() - left - right;
    const double t = t0_ + (e->position().x() - left) / w * (t1_ - t0_);
    QStringList lines{formatDateTimeMinutes(t)};
    for (const auto& pr : profiles_) {
        double u = 0;
        for (const auto& s : pr.steps)
            if (t >= s.start && t < s.end) u = s.units;
        lines << QStringLiteral("%1: %2 / %3").arg(qstr(pr.name)).arg(u).arg(pr.capacity);
    }
    QToolTip::showText(e->globalPosition().toPoint(), lines.join("\n"), this);
}

// ---- production chart -------------------------------------------------------------------------------

ProductionChart::ProductionChart(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    setMouseTracking(true);
    setMinimumHeight(150);
    auto* lay = new QHBoxLayout;
    period_ = new QComboBox(this);
    period_->addItem(tr("Week"), static_cast<int>(scheduling::PeriodKind::Week));
    period_->addItem(tr("Month"), static_cast<int>(scheduling::PeriodKind::Month));
    period_->addItem(tr("Quarter"), static_cast<int>(scheduling::PeriodKind::Quarter));
    period_->addItem(tr("Year"), static_cast<int>(scheduling::PeriodKind::Year));
    period_->setCurrentIndex(1);
    unit_ = new QComboBox(this);
    unit_->setEditable(true);
    unit_->addItems({"m", "t", "m3"});
    zone_ = new QLineEdit(this);
    zone_->setPlaceholderText(tr("zone (all)"));
    level_ = new QLineEdit(this);
    level_->setPlaceholderText(tr("level (all)"));
    cumulative_ = new QCheckBox(tr("Cumulative"), this);
    lay->addWidget(new QLabel(tr("Per"), this));
    lay->addWidget(period_);
    lay->addWidget(new QLabel(tr("unit"), this));
    lay->addWidget(unit_);
    lay->addWidget(zone_);
    lay->addWidget(level_);
    lay->addWidget(cumulative_);
    lay->addStretch(1);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 2, 4, 2);
    v->addLayout(lay);
    v->addStretch(1);
    connect(period_, &QComboBox::activated, this, &ProductionChart::recompute);
    connect(unit_, &QComboBox::currentTextChanged, this, &ProductionChart::recompute);
    connect(zone_, &QLineEdit::editingFinished, this, &ProductionChart::recompute);
    connect(level_, &QLineEdit::editingFinished, this, &ProductionChart::recompute);
    connect(cumulative_, &QCheckBox::toggled, this, qOverload<>(&QWidget::update));
    connect(&ctx_, &AppContext::scheduleRecalculated, this, &ProductionChart::recompute);
    recompute();
}

void ProductionChart::recompute() {
    q_.clear();
    const auto& s = ctx_.schedule();
    if (ctx_.hasProject() && s.result.ok) {
        scheduling::QuantityFilter f;
        f.unit = sstr(unit_->currentText());
        f.zone = sstr(zone_->text().trimmed());
        f.level = sstr(level_->text().trimmed());
        q_ = scheduling::quantitiesByPeriod(s.input, s.result, static_cast<scheduling::PeriodKind>(period_->currentData().toInt()), f);
    }
    update();
}

void ProductionChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& c = Theme::colors();
    p.fillRect(rect(), c.panel);
    const double top = 34, left = 56, right = 12, bottom = height() - 34.0;
    plot_ = QRectF(QPointF(left, top), QPointF(width() - right, bottom));
    if (q_.empty()) {
        p.setPen(c.textMuted);
        p.drawText(plot_, Qt::AlignCenter, tr("No production quantities in %1").arg(unit_->currentText()));
        return;
    }
    const bool cum = cumulative_->isChecked();
    std::vector<double> sch, act, tgt;
    double a = 0, b = 0, t = 0;
    for (const auto& x : q_) {
        a = cum ? a + x.scheduled : x.scheduled;
        b = cum ? b + x.actual : x.actual;
        t = cum ? t + x.target : x.target;
        sch.push_back(a);
        act.push_back(b);
        tgt.push_back(t);
    }
    double ymax = 0;
    for (std::size_t i = 0; i < q_.size(); ++i) ymax = std::max({ymax, sch[i], act[i], tgt[i]});
    ymax = niceCeil(ymax * 1.05);
    auto yOf = [&](double v) { return bottom - v / ymax * (bottom - top); };
    const double bw = plot_.width() / q_.size();
    // Axes and grid.
    p.setPen(c.ganttGrid);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    for (int i = 0; i <= 4; ++i) {
        const double v = ymax * i / 4, y = yOf(v);
        p.setPen(c.ganttGrid);
        p.drawLine(QPointF(left, y), QPointF(plot_.right(), y));
        p.setPen(c.textMuted);
        p.drawText(QRectF(0, y - 8, left - 4, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'g', 4));
    }
    const int labelEvery = std::max<int>(1, static_cast<int>(std::ceil(60.0 / bw)));
    for (std::size_t i = 0; i < q_.size(); ++i) {
        const double x = left + i * bw;
        p.fillRect(QRectF(x + bw * 0.12, yOf(sch[i]), bw * 0.38, bottom - yOf(sch[i])), c.ganttTask);
        p.fillRect(QRectF(x + bw * 0.5, yOf(act[i]), bw * 0.38, bottom - yOf(act[i])), c.ganttProgress);
        if (static_cast<int>(i) % labelEvery == 0) {
            p.setPen(c.textMuted);
            p.drawText(QRectF(x, bottom + 2, bw * labelEvery, 16), Qt::AlignLeft, qstr(q_[i].period.label));
        }
    }
    // Targets as a step line.
    p.setPen(QPen(c.ganttCritical, 2));
    for (std::size_t i = 0; i < q_.size(); ++i) {
        if (tgt[i] <= 0 && (!cum || i == 0)) continue;
        const double x = left + i * bw;
        p.drawLine(QPointF(x, yOf(tgt[i])), QPointF(x + bw, yOf(tgt[i])));
    }
    // Legend.
    p.setPen(c.text);
    double lx = left;
    for (auto [label, col] : {std::pair{tr("Scheduled"), c.ganttTask}, {tr("Actual"), c.ganttProgress}, {tr("Target"), c.ganttCritical}}) {
        p.fillRect(QRectF(lx, 18, 10, 10), col);
        p.drawText(QPointF(lx + 14, 27), label);
        lx += 90;
    }
}

void ProductionChart::mouseMoveEvent(QMouseEvent* e) {
    if (q_.empty() || !plot_.contains(e->position())) return;
    const auto i = static_cast<std::size_t>((e->position().x() - plot_.left()) / plot_.width() * q_.size());
    if (i >= q_.size()) return;
    const auto& x = q_[i];
    QToolTip::showText(e->globalPosition().toPoint(),
                       tr("%1\nscheduled %2\nactual %3\ntarget %4").arg(qstr(x.period.label)).arg(x.scheduled, 0, 'f', 1).arg(x.actual, 0, 'f', 1).arg(x.target, 0, 'f', 1),
                       this);
}

// ---- variance -----------------------------------------------------------------------------------

VarianceTable::VarianceTable(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Compare with baseline"), this));
    baseline_ = new QComboBox(this);
    row->addWidget(baseline_, 1);
    lay->addLayout(row);
    table_ = new QTreeWidget(this);
    table_->setColumnCount(6);
    table_->setHeaderLabels({tr("Activity"), tr("Start variance (h)"), tr("Finish variance (h)"), tr("Duration variance (h)"), tr("Quantity variance"), tr("In baseline")});
    table_->setRootIsDecorated(false);
    table_->setSortingEnabled(true);
    table_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    lay->addWidget(table_, 1);
    connect(baseline_, &QComboBox::activated, this, [this](int) { ctx_.setComparisonBaseline(uuidFrom(baseline_->currentData().toString())); });
    connect(&ctx_, &AppContext::scheduleRecalculated, this, &VarianceTable::recompute);
    recompute();
}

void VarianceTable::recompute() {
    baseline_->clear();
    table_->clear();
    if (!ctx_.hasProject()) return;
    const auto* current = ctx_.comparisonBaseline();
    for (const auto& b : ctx_.project().model().baselines.all()) {
        baseline_->addItem(QStringLiteral("%1  (%2)").arg(qstr(b.name), qstr(b.capturedAt.substr(0, 16))), uuidText(b.id));
        if (current && current->id == b.id) baseline_->setCurrentIndex(baseline_->count() - 1);
    }
    const auto& s = ctx_.schedule();
    if (!current || !s.result.ok) return;
    table_->setSortingEnabled(false);
    for (const auto& v : scheduling::compareToBaseline(ctx_.project(), s.result, *current)) {
        auto* it = new QTreeWidgetItem(table_);
        it->setText(0, qstr(v.name));
        it->setData(1, Qt::DisplayRole, std::round(v.startVarianceHours * 10) / 10);
        it->setData(2, Qt::DisplayRole, std::round(v.finishVarianceHours * 10) / 10);
        it->setData(3, Qt::DisplayRole, std::round(v.durationVarianceHours * 10) / 10);
        it->setData(4, Qt::DisplayRole, std::round(v.quantityVariance * 100) / 100);
        it->setText(5, v.inBaseline ? tr("yes") : tr("new"));
        if (v.finishVarianceHours > 0.01) it->setForeground(2, Theme::colors().warning);
    }
    table_->setSortingEnabled(true);
    table_->sortByColumn(2, Qt::DescendingOrder);
}

} // namespace openmine::app
