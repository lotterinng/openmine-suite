#pragma once
// SPDX-License-Identifier: Apache-2.0
// Charts under the Gantt: resource usage profiles against capacity (over-allocation highlighted),
// production quantities per period (scheduled, actual and target, per period and cumulative)
// and the baseline variance table.

#include "../app/AppContext.h"

#include "openmine/scheduling/Levelling.h"
#include "openmine/scheduling/Periods.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QTreeWidget;

namespace openmine::app {

class ResourceHistogram : public QWidget {
    Q_OBJECT
public:
    explicit ResourceHistogram(AppContext& ctx, QWidget* parent = nullptr);
    const std::vector<scheduling::ResourceProfile>& profiles() const { return profiles_; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    void recompute();
    AppContext& ctx_;
    std::vector<scheduling::ResourceProfile> profiles_;
    double t0_ = 0, t1_ = 1;
};

class ProductionChart : public QWidget {
    Q_OBJECT
public:
    explicit ProductionChart(AppContext& ctx, QWidget* parent = nullptr);
    const std::vector<scheduling::PeriodQuantity>& quantities() const { return q_; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    void recompute();
    AppContext& ctx_;
    QComboBox* period_ = nullptr;
    QComboBox* unit_ = nullptr;
    QLineEdit* zone_ = nullptr;
    QLineEdit* level_ = nullptr;
    QCheckBox* cumulative_ = nullptr;
    std::vector<scheduling::PeriodQuantity> q_;
    QRectF plot_;
};

class VarianceTable : public QWidget {
    Q_OBJECT
public:
    explicit VarianceTable(AppContext& ctx, QWidget* parent = nullptr);
    QTreeWidget* table() const { return table_; }

private:
    void recompute();
    AppContext& ctx_;
    QTreeWidget* table_ = nullptr;
    QComboBox* baseline_ = nullptr;
};

} // namespace openmine::app
