// SPDX-License-Identifier: Apache-2.0
#include "PlaybackPanel.h"

#include "../theme/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

namespace openmine::app {

namespace {
constexpr int kSteps = 10000;
}

PlaybackPanel::PlaybackPanel(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    enable_ = new QCheckBox(tr("4D view"), this);
    enable_->setToolTip(tr("Show the design objects as scheduled at the playback time"));
    playBtn_ = new QPushButton(toolIcon("play"), tr("Play"), this);
    auto* stepBack = new QPushButton(tr("◀ Step"), this);
    auto* stepFwd = new QPushButton(tr("Step ▶"), this);
    speed_ = new QComboBox(this);
    speed_->addItem(tr("1 day/s"), 1440.0);
    speed_->addItem(tr("1 week/s"), 7 * 1440.0);
    speed_->addItem(tr("1 month/s"), 30 * 1440.0);
    speed_->addItem(tr("1 quarter/s"), 91 * 1440.0);
    speed_->setCurrentIndex(1);
    auto* rec = new QPushButton(toolIcon("export"), tr("Record frames…"), this);
    top->addWidget(enable_);
    top->addWidget(stepBack);
    top->addWidget(playBtn_);
    top->addWidget(stepFwd);
    top->addWidget(speed_);
    top->addStretch(1);
    top->addWidget(rec);
    lay->addLayout(top);

    auto* row = new QHBoxLayout;
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(0, kSteps);
    timeLabel_ = new QLabel(this);
    timeLabel_->setMinimumWidth(140);
    row->addWidget(slider_, 1);
    row->addWidget(timeLabel_);
    lay->addLayout(row);

    auto* opts = new QHBoxLayout;
    opts->addWidget(new QLabel(tr("Colour by"), this));
    colorMode_ = new QComboBox(this);
    colorMode_->addItem(tr("State"), static_cast<int>(planning::ColorMode::State));
    colorMode_->addItem(tr("Planned start"), static_cast<int>(planning::ColorMode::StartPeriod));
    colorMode_->addItem(tr("Resource"), static_cast<int>(planning::ColorMode::Resource));
    colorMode_->addItem(tr("Activity"), static_cast<int>(planning::ColorMode::Activity));
    opts->addWidget(colorMode_);
    baseline_ = new QCheckBox(tr("Highlight behind baseline"), this);
    ghost_ = new QCheckBox(tr("Ghost future work"), this);
    ghost_->setChecked(true);
    opts->addWidget(baseline_);
    opts->addWidget(ghost_);
    opts->addStretch(1);
    lay->addLayout(opts);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    table_ = new QTreeWidget(this);
    table_->setColumnCount(6);
    table_->setHeaderLabels({tr("Object"), tr("State"), tr("Planned %"), tr("Actual %"), tr("Baseline %"), tr("Resources")});
    table_->setRootIsDecorated(false);
    table_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->setSortingEnabled(true);
    lay->addWidget(table_, 1);

    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, &PlaybackPanel::tick);
    connect(enable_, &QCheckBox::toggled, this, [this](bool on) {
        if (on) setTime(ctx_.playbackTime().value_or(sliderTime(slider_->value())));
        else {
            play(false);
            ctx_.setPlaybackTime(std::nullopt);
        }
    });
    connect(slider_, &QSlider::valueChanged, this, [this](int v) {
        if (updating_) return;
        if (!enable_->isChecked()) enable_->setChecked(true);
        ctx_.setPlaybackTime(sliderTime(v));
    });
    connect(playBtn_, &QPushButton::clicked, this, [this] { play(!playing()); });
    auto step = [this](int dir) {
        const double s = speed_->currentData().toDouble();
        setTime(std::clamp(ctx_.playbackTime().value_or(t0_) + dir * s, t0_, t1_));
    };
    connect(stepBack, &QPushButton::clicked, this, [step] { step(-1); });
    connect(stepFwd, &QPushButton::clicked, this, [step] { step(+1); });
    connect(colorMode_, &QComboBox::activated, this, [this](int) { ctx_.setPlaybackColorMode(static_cast<planning::ColorMode>(colorMode_->currentData().toInt())); });
    connect(baseline_, &QCheckBox::toggled, this, [this](bool on) { ctx_.setPlaybackCompareBaseline(on); });
    connect(ghost_, &QCheckBox::toggled, this, &PlaybackPanel::ghostFutureChanged);
    connect(rec, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder for the frames"));
        if (dir.isEmpty()) return;
        bool ok = false;
        const double days = QInputDialog::getDouble(this, tr("Record frames"), tr("Days between frames:"), 7, 0.04, 365, 2, &ok);
        if (!ok) return;
        const int n = recordFrames(dir, days * 1440.0);
        ctx_.report(tr("Wrote %1 frames to %2").arg(n).arg(dir));
    });
    connect(&ctx_, &AppContext::scheduleRecalculated, this, &PlaybackPanel::rebuildRange);
    connect(&ctx_, &AppContext::playbackChanged, this, &PlaybackPanel::updateTable);
    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
        play(false);
        updating_ = true;
        enable_->setChecked(false);
        updating_ = false;
        rebuildRange();
    });
    rebuildRange();
}

double PlaybackPanel::sliderTime(int v) const { return t0_ + (t1_ - t0_) * v / kSteps; }

void PlaybackPanel::rebuildRange() {
    const auto& r = ctx_.schedule().result;
    const bool ok = ctx_.hasProject() && r.ok && r.projectFinish > r.projectStart;
    setEnabled(ok);
    if (!ok) {
        summary_->setText(ctx_.hasProject() && !r.errors.empty() ? tr("The schedule cannot be calculated: %1").arg(qstr(r.errors.front())) : tr("No calculated schedule"));
        return;
    }
    t0_ = r.projectStart;
    t1_ = r.projectFinish;
    updating_ = true;
    const double t = ctx_.playbackTime().value_or(t0_);
    slider_->setValue(static_cast<int>(std::lround((std::clamp(t, t0_, t1_) - t0_) / (t1_ - t0_) * kSteps)));
    updating_ = false;
    updateTable();
}

void PlaybackPanel::setTime(double minutes) {
    if (!enable_->isChecked()) {
        updating_ = true;
        enable_->setChecked(true);
        updating_ = false;
    }
    updating_ = true;
    if (t1_ > t0_) slider_->setValue(static_cast<int>(std::lround((std::clamp(minutes, t0_, t1_) - t0_) / (t1_ - t0_) * kSteps)));
    updating_ = false;
    ctx_.setPlaybackTime(minutes);
}

void PlaybackPanel::play(bool on) {
    if (on) {
        if (!enable_->isChecked()) enable_->setChecked(true);
        if (ctx_.playbackTime().value_or(t1_) >= t1_) setTime(t0_);
        timer_.start();
        playBtn_->setIcon(toolIcon("pause"));
        playBtn_->setText(tr("Pause"));
    } else {
        timer_.stop();
        playBtn_->setIcon(toolIcon("play"));
        playBtn_->setText(tr("Play"));
    }
}

void PlaybackPanel::tick() {
    const double perSecond = speed_->currentData().toDouble();
    const double t = ctx_.playbackTime().value_or(t0_) + perSecond * timer_.interval() / 1000.0;
    if (t >= t1_) {
        setTime(t1_);
        play(false);
        return;
    }
    setTime(t);
}

void PlaybackPanel::updateTable() {
    const auto t = ctx_.playbackTime();
    timeLabel_->setText(t ? formatDateTimeMinutes(*t) : QString());
    table_->setSortingEnabled(false);
    table_->clear();
    if (!ctx_.hasProject() || !ctx_.planSnapshot()) {
        if (ctx_.hasProject() && ctx_.schedule().result.ok)
            summary_->setText(tr("Schedule %1 → %2 (%3 days). Tick “4D view” or move the slider to play it back.")
                                  .arg(qstr(ctx_.schedule().result.projectStartDate.toString()), qstr(ctx_.schedule().result.projectFinishDate.toString()))
                                  .arg(ctx_.schedule().result.makespanHours() / 24.0, 0, 'f', 1));
        table_->setSortingEnabled(true);
        return;
    }
    const auto& s = *ctx_.planSnapshot();
    const auto& m = ctx_.project().model();
    int inProgress = 0, complete = 0, behind = 0;
    for (const auto& o : s.objects) {
        auto* it = new QTreeWidgetItem(table_);
        it->setText(0, qstr(o.name));
        it->setText(1, qstr(planning::toString(o.state)));
        it->setData(2, Qt::DisplayRole, std::round(o.plannedFraction * 1000) / 10);
        it->setData(3, Qt::DisplayRole, std::round(o.actualFraction * 1000) / 10);
        it->setData(4, Qt::DisplayRole, std::round(o.baselineFraction * 1000) / 10);
        QStringList res;
        for (const auto& r : o.activeResources)
            if (const auto* rr = m.resources.find(r)) res << qstr(rr->name);
        it->setText(5, res.join(", "));
        QPixmap sw(12, 12);
        sw.fill(QColor(static_cast<QRgb>(o.color)));
        it->setIcon(0, QIcon(sw));
        if (o.behindPlan) {
            it->setForeground(1, Theme::colors().warning);
            ++behind;
        }
        inProgress += o.state == planning::ObjectState::InProgress;
        complete += o.state == planning::ObjectState::Complete;
    }
    table_->setSortingEnabled(true);
    summary_->setText(tr("<b>%1</b> — planned %2 m developed, actual %3 m to the status date; %4 objects in progress, %5 complete%6")
                          .arg(formatDateTimeMinutes(s.time))
                          .arg(s.plannedMetres, 0, 'f', 1)
                          .arg(s.actualMetres, 0, 'f', 1)
                          .arg(inProgress)
                          .arg(complete)
                          .arg(behind ? tr(", <span style='color:#d32f2f'>%1 behind the baseline</span>").arg(behind) : QString()));
}

int PlaybackPanel::recordFrames(const QString& folder, double stepMinutes) {
    if (!grab_ || !(stepMinutes > 0) || t1_ <= t0_) return 0;
    const auto frames = planning::playbackFrames(ctx_.schedule().result, stepMinutes);
    QDir().mkpath(folder);
    QProgressDialog progress(tr("Recording 4D frames…"), tr("Cancel"), 0, static_cast<int>(frames.size()), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    int n = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (progress.wasCanceled()) break;
        setTime(frames[i]);
        QApplication::processEvents();
        const QImage img = grab_();
        if (img.isNull()) break;
        if (!img.save(QDir(folder).filePath(QStringLiteral("frame_%1.png").arg(i, 5, 10, QChar('0'))))) break;
        ++n;
        progress.setValue(static_cast<int>(i + 1));
    }
    return n;
}

} // namespace openmine::app
