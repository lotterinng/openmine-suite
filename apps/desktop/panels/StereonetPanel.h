#pragma once
// SPDX-License-Identifier: Apache-2.0
// Stereonet of structural measurements (modules/geotechnical): poles on an equal-area or
// equal-angle lower-hemisphere net with pole-density contours (Schmidt 1 % or Kamb), joint-set
// identification (k-means on axes with Fisher statistics), great circles of the set planes and
// kinematic analysis (planar sliding, wedge sliding with the Hocking refinement, flexural
// toppling) against a face or the walls of a drive. Identified sets can be written back to the
// measurements (one undo step) and the net exported as SVG.

#include "../app/AppContext.h"

#include "openmine/geotechnical/Stereonet.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QTextBrowser;

namespace openmine::app {

class StereonetView : public QWidget {
    Q_OBJECT
public:
    explicit StereonetView(QWidget* parent = nullptr);
    struct Data {
        std::vector<Vec3> poles;
        std::vector<int> assignment;
        std::vector<geotechnical::JointSet> sets;
        std::optional<geotechnical::DensityGrid> density;
        std::vector<double> levels;
        geotechnical::Projection projection = geotechnical::Projection::EqualArea;
        std::optional<double> frictionDeg;
        std::optional<geotechnical::Plane> face;
        std::vector<std::size_t> critical; ///< highlighted poles
    };
    void setData(Data d);
    const Data& data() const { return d_; }
    QSize sizeHint() const override { return {420, 420}; }

signals:
    void hovered(QString text);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    QPointF toWidget(geotechnical::NetPoint p) const;
    Data d_;
};

class StereonetPanel : public QWidget {
    Q_OBJECT
public:
    explicit StereonetPanel(AppContext& ctx, QWidget* parent = nullptr);
    /// Measurements analysed: the selected structures, else those of the selected development,
    /// else all of the project.
    std::vector<const StructuralMeasurement*> measurements() const;
    void recompute();
    bool writeSets();
    QString exportSvg() const;
    StereonetView* view() const { return view_; }

private:
    AppContext& ctx_;
    StereonetView* view_ = nullptr;
    QComboBox* projection_ = nullptr;
    QComboBox* density_ = nullptr;
    QSpinBox* sets_ = nullptr;
    QDoubleSpinBox* cone_ = nullptr;
    QDoubleSpinBox* friction_ = nullptr;
    QComboBox* faceMode_ = nullptr;
    QDoubleSpinBox* faceDip_ = nullptr;
    QDoubleSpinBox* faceDir_ = nullptr;
    QCheckBox* terzaghi_ = nullptr;
    QTextBrowser* results_ = nullptr;
    QLabel* hover_ = nullptr;
    geotechnical::JointSetResult lastSets_;
    std::vector<Uuid> lastIds_;
};

} // namespace openmine::app
