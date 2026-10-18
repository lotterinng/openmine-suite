#pragma once
// SPDX-License-Identifier: Apache-2.0
// 4D schedule playback: a time slider over the calculated schedule of the active scenario, play /
// pause / step, colouring (by state, start period, resource or activity), behind-baseline
// highlighting, the state of every scheduled design object at the instant, and recording of the
// playback as a numbered PNG frame sequence from the active view.

#include "../app/AppContext.h"

#include <QTimer>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTreeWidget;

namespace openmine::app {

class PlaybackPanel : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Grabs the active view's image (set by the main window).
    void setFrameGrabber(std::function<QImage()> grab) { grab_ = std::move(grab); }
    void setTime(double minutes);
    void play(bool on);
    bool playing() const { return timer_.isActive(); }
    /// Writes PNG frames every `stepMinutes` from the schedule start to its finish. Returns the
    /// number of frames written.
    int recordFrames(const QString& folder, double stepMinutes);

signals:
    void ghostFutureChanged(bool on);

private:
    void rebuildRange();
    void updateTable();
    void tick();
    double sliderTime(int v) const;

    AppContext& ctx_;
    QSlider* slider_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* playBtn_ = nullptr;
    QComboBox* speed_ = nullptr;
    QComboBox* colorMode_ = nullptr;
    QCheckBox* enable_ = nullptr;
    QCheckBox* baseline_ = nullptr;
    QCheckBox* ghost_ = nullptr;
    QTreeWidget* table_ = nullptr;
    QTimer timer_;
    double t0_ = 0, t1_ = 0;
    std::function<QImage()> grab_;
    bool updating_ = false;
};

} // namespace openmine::app
