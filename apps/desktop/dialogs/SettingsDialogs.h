#pragma once
// SPDX-License-Identifier: Apache-2.0
// Settings: preferences (display, drawing and snapping, point clouds, plugins, the open project's
// coordinate display) persisted per user, and the customisable quick-access toolbar.

#include "../app/AppContext.h"

#include <QDialog>

#include <functional>
#include <map>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QListWidget;
class QSpinBox;

namespace openmine::app {

class ViewportArea;

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    PreferencesDialog(AppContext& ctx, ViewportArea& views, std::function<void(bool dark)> setTheme, QWidget* parent = nullptr);
    /// Applies the edited values to the views, tools and project, and stores them.
    bool apply();
    /// Applies the stored user preferences (start-up).
    static void applyStored(QSettings& s, ViewportArea& views);

private:
    void load();
    AppContext& ctx_;
    ViewportArea& views_;
    std::function<void(bool)> setTheme_;
    QComboBox* theme_;
    QDoubleSpinBox* chord_;
    QDoubleSpinBox* envelopeOpacity_;
    QDoubleSpinBox* stationInterval_;
    QDoubleSpinBox* discRadius_;
    QDoubleSpinBox* drillholeWidth_;
    QCheckBox* controlLabels_;
    QCheckBox* drillholeLabels_;
    QSpinBox* pointBudget_;
    QDoubleSpinBox* pointSpacing_;
    QDoubleSpinBox* gridSpacing_;
    QDoubleSpinBox* aperture_;
    std::map<unsigned, QCheckBox*> snaps_;
    QDoubleSpinBox* textHeight_;
    QDoubleSpinBox* filletRadius_;
    QDoubleSpinBox* offsetDistance_;
    QSpinBox* decimals_;
    QCheckBox* southAxes_;
    QCheckBox* pythonPlugins_;
    QListWidget* nativePlugins_;
};

/// Chooses the actions of the quick-access toolbar (stored as action ids).
class QuickAccessDialog : public QDialog {
    Q_OBJECT
public:
    QuickAccessDialog(const std::map<QString, QAction*>& actions, const QStringList& current, QWidget* parent = nullptr);
    QStringList chosen() const;
    void setChosen(const QStringList& ids);

private:
    const std::map<QString, QAction*>& actions_;
    QListWidget* available_;
    QListWidget* chosen_;
};

} // namespace openmine::app
