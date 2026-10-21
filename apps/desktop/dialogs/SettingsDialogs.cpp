// SPDX-License-Identifier: Apache-2.0
#include "SettingsDialogs.h"

#include "DialogUtil.h"
#include "../theme/Theme.h"
#include "../viewport/ViewportArea.h"

#include "openmine/project/ProjectEditor.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace openmine::app {

namespace {
const std::pair<cad::SnapMode, const char*> kSnapModes[] = {
    {cad::SnapMode::Endpoint, "Endpoint"},         {cad::SnapMode::Midpoint, "Midpoint"}, {cad::SnapMode::Center, "Centre"},
    {cad::SnapMode::Intersection, "Intersection"}, {cad::SnapMode::Perpendicular, "Perpendicular"}, {cad::SnapMode::Tangent, "Tangent"},
    {cad::SnapMode::Nearest, "Nearest"},           {cad::SnapMode::Grid, "Grid"},         {cad::SnapMode::Node, "Node (points, vertices)"},
};
} // namespace

PreferencesDialog::PreferencesDialog(AppContext& ctx, ViewportArea& views, std::function<void(bool)> setTheme, QWidget* parent)
    : QDialog(parent), ctx_(ctx), views_(views), setTheme_(std::move(setTheme)) {
    setWindowTitle(tr("Preferences"));
    resize(560, 560);
    auto* lay = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    lay->addWidget(tabs, 1);

    auto* display = new QWidget;
    auto* df = new QFormLayout(display);
    theme_ = new QComboBox(display);
    theme_->addItems({tr("Dark"), tr("Light")});
    chord_ = spin(0.0001, 1, 0.01, 4, " m", display);
    chord_->setToolTip(tr("Arcs and splines are drawn as chords within this tolerance (measurements always use the exact curve)"));
    envelopeOpacity_ = spin(0, 1, 0.35, 2, {}, display);
    stationInterval_ = spin(1, 1000, 20, 1, " m", display);
    discRadius_ = spin(0.1, 100, 2, 2, " m", display);
    drillholeWidth_ = spin(1, 20, 3, 1, " px", display);
    controlLabels_ = new QCheckBox(tr("Label control points"), display);
    drillholeLabels_ = new QCheckBox(tr("Label drillholes"), display);
    df->addRow(tr("Theme"), theme_);
    df->addRow(tr("Curve display tolerance"), chord_);
    df->addRow(tr("Excavation envelope opacity"), envelopeOpacity_);
    df->addRow(tr("Chainage station interval"), stationInterval_);
    df->addRow(tr("Structure disc radius (no persistence)"), discRadius_);
    df->addRow(tr("Drillhole trace width"), drillholeWidth_);
    df->addRow(QString(), controlLabels_);
    df->addRow(QString(), drillholeLabels_);
    tabs->addTab(display, tr("Display"));

    auto* clouds = new QWidget;
    auto* cf = new QFormLayout(clouds);
    pointBudget_ = new QSpinBox(clouds);
    pointBudget_->setRange(1, 100);
    pointBudget_->setSuffix(tr(" million"));
    pointSpacing_ = spin(0.5, 10, 1.5, 1, " px", clouds);
    pointSpacing_->setToolTip(tr("Level of detail: finer nodes load until points are this far apart on screen"));
    cf->addRow(tr("Points displayed per view"), pointBudget_);
    cf->addRow(tr("Target screen spacing"), pointSpacing_);
    tabs->addTab(clouds, tr("Point clouds"));

    auto* drawing = new QWidget;
    auto* dl = new QVBoxLayout(drawing);
    auto* sg = new QGroupBox(tr("Object snaps"), drawing);
    auto* sgl = new QGridLayout(sg);
    int i = 0;
    for (const auto& [mode, label] : kSnapModes) {
        auto* cb = new QCheckBox(tr(label), sg);
        snaps_[static_cast<unsigned>(mode)] = cb;
        sgl->addWidget(cb, i / 3, i % 3);
        ++i;
    }
    dl->addWidget(sg);
    auto* dform = new QFormLayout;
    gridSpacing_ = spin(0.001, 10000, 1, 3, " m", drawing);
    aperture_ = spin(2, 60, 12, 0, " px", drawing);
    textHeight_ = spin(0.01, 1000, 1, 2, " m", drawing);
    filletRadius_ = spin(0, 10000, 5, 3, " m", drawing);
    offsetDistance_ = spin(0.001, 10000, 1, 3, " m", drawing);
    dform->addRow(tr("Grid snap spacing"), gridSpacing_);
    dform->addRow(tr("Snap aperture"), aperture_);
    dform->addRow(tr("Default text height"), textHeight_);
    dform->addRow(tr("Default fillet radius"), filletRadius_);
    dform->addRow(tr("Default offset distance"), offsetDistance_);
    dl->addLayout(dform);
    dl->addStretch(1);
    tabs->addTab(drawing, tr("Drawing"));

    // Plugins: which native plugins load, and whether Python plugins are discovered.
    auto* plugins = new QWidget;
    auto* plg = new QVBoxLayout(plugins);
    plg->addWidget(new QLabel(tr("Native plugins found in the plugin folders (unchecked = not loaded). Changes apply at once."), plugins));
    nativePlugins_ = new QListWidget(plugins);
    plg->addWidget(nativePlugins_, 1);
    pythonPlugins_ = new QCheckBox(tr("Discover Python plugins at start-up (installed distributions and \"python\" plugin sub-folders)"), plugins);
    pythonPlugins_->setToolTip(tr("Python plugins stay loaded for the session once discovered; turning this off takes effect at the next start"));
    plg->addWidget(pythonPlugins_);
    QStringList folders;
    for (const auto& d : ctx_.pluginDirectories()) folders << QString::fromStdU16String(d.u16string());
    auto* where = new QLabel(tr("Plugin folders: %1 (add more with OPENMINE_PLUGIN_PATH)").arg(folders.join("; ")), plugins);
    where->setWordWrap(true);
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    plg->addWidget(where);
    tabs->addTab(plugins, tr("Plugins"));

    auto* project = new QWidget;
    auto* pf = new QFormLayout(project);
    decimals_ = new QSpinBox(project);
    decimals_->setRange(0, 6);
    southAxes_ = new QCheckBox(tr("South-orientated axes (Lo system: Y westing, X southing)"), project);
    pf->addRow(new QLabel(tr("Stored in the open project; coordinates themselves are never changed."), project));
    pf->addRow(tr("Coordinate display decimals"), decimals_);
    pf->addRow(QString(), southAxes_);
    tabs->addTab(project, tr("Project"));
    project->setEnabled(ctx_.hasProject());

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (apply()) accept();
    });
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    load();
}

void PreferencesDialog::load() {
    const auto& s = views_.scene().settings();
    theme_->setCurrentIndex(Theme::current() == ThemeKind::Light ? 1 : 0);
    chord_->setValue(s.chordTolerance);
    envelopeOpacity_->setValue(s.envelopeOpacity);
    stationInterval_->setValue(s.stationInterval);
    discRadius_->setValue(s.structureDiscRadius);
    drillholeWidth_->setValue(s.drillholeWidth);
    controlLabels_->setChecked(s.controlLabels);
    drillholeLabels_->setChecked(s.drillholeLabels);
    const auto& o = views_.active()->options();
    pointBudget_->setValue(static_cast<int>(std::max<std::size_t>(1, o.pointBudget / 1000000)));
    pointSpacing_->setValue(o.pointScreenSpacing);
    auto& tools = views_.tools();
    for (auto& [mode, cb] : snaps_) cb->setChecked(tools.snapSettings().modes & mode);
    gridSpacing_->setValue(tools.snapSettings().gridSpacing);
    aperture_->setValue(tools.snapSettings().aperturePixels);
    textHeight_->setValue(tools.textHeight());
    filletRadius_->setValue(tools.filletRadius());
    offsetDistance_->setValue(tools.offsetDistance());
    const QStringList disabled = ctx_.settings().value("plugins/disabled").toStringList();
    nativePlugins_->clear();
    std::vector<std::string> problems;
    for (const auto& m : ctx_.plugins().discover(ctx_.pluginDirectories(), &problems)) {
        auto* it = new QListWidgetItem(tr("%1 %2 — %3").arg(QString::fromStdString(m.name), QString::fromStdString(m.version), QString::fromStdString(m.id)),
                                       nativePlugins_);
        it->setData(Qt::UserRole, QString::fromStdString(m.id));
        it->setToolTip(QString::fromStdString(m.description));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(disabled.contains(QString::fromStdString(m.id)) ? Qt::Unchecked : Qt::Checked);
    }
    for (const auto& p : problems) {
        auto* it = new QListWidgetItem(QString::fromStdString(p), nativePlugins_);
        it->setFlags(Qt::ItemIsEnabled);
    }
    pythonPlugins_->setChecked(ctx_.settings().value("plugins/python", true).toBool());
    if (ctx_.hasProject()) {
        decimals_->setValue(ctx_.project().model().info.coordinateDisplayDecimals);
        southAxes_->setChecked(ctx_.project().model().info.southOrientatedAxes);
    }
}

bool PreferencesDialog::apply() {
    auto s = views_.scene().settings();
    s.chordTolerance = chord_->value();
    s.envelopeOpacity = envelopeOpacity_->value();
    s.stationInterval = stationInterval_->value();
    s.structureDiscRadius = discRadius_->value();
    s.drillholeWidth = drillholeWidth_->value();
    s.controlLabels = controlLabels_->isChecked();
    s.drillholeLabels = drillholeLabels_->isChecked();
    views_.scene().setSettings(s);
    const std::size_t budget = static_cast<std::size_t>(pointBudget_->value()) * 1000000;
    const double spacing = pointSpacing_->value();
    views_.forEach([&](Viewport3D& v) {
        v.options().pointBudget = budget;
        v.options().pointScreenSpacing = spacing;
        v.applyOptions();
    });
    auto& tools = views_.tools();
    unsigned modes = 0;
    for (const auto& [mode, cb] : snaps_)
        if (cb->isChecked()) modes |= mode;
    tools.snapSettings().modes = modes;
    tools.snapSettings().gridSpacing = gridSpacing_->value();
    tools.snapSettings().aperturePixels = aperture_->value();
    tools.setTextHeight(textHeight_->value());
    tools.setFilletRadius(filletRadius_->value());
    tools.setOffsetDistance(offsetDistance_->value());

    QSettings& st = ctx_.settings();
    st.setValue("display/chordTolerance", s.chordTolerance);
    st.setValue("display/envelopeOpacity", s.envelopeOpacity);
    st.setValue("display/stationInterval", s.stationInterval);
    st.setValue("display/structureDiscRadius", s.structureDiscRadius);
    st.setValue("display/drillholeWidth", s.drillholeWidth);
    st.setValue("display/controlLabels", s.controlLabels);
    st.setValue("display/drillholeLabels", s.drillholeLabels);
    st.setValue("clouds/pointBudget", static_cast<qulonglong>(budget));
    st.setValue("clouds/screenSpacing", spacing);
    st.setValue("snap/modes", modes);
    st.setValue("snap/gridSpacing", gridSpacing_->value());
    st.setValue("snap/aperture", aperture_->value());
    st.setValue("draw/textHeight", textHeight_->value());
    st.setValue("draw/filletRadius", filletRadius_->value());
    st.setValue("draw/offsetDistance", offsetDistance_->value());

    // Plugins: the disabled list keeps ids of plugins not found now (a folder may be unavailable).
    QStringList disabled = st.value("plugins/disabled").toStringList();
    const QStringList before = disabled;
    for (int r = 0; r < nativePlugins_->count(); ++r) {
        const auto* it = nativePlugins_->item(r);
        const QString id = it->data(Qt::UserRole).toString();
        if (id.isEmpty()) continue;
        disabled.removeAll(id);
        if (it->checkState() == Qt::Unchecked) disabled << id;
    }
    disabled.removeDuplicates();
    st.setValue("plugins/disabled", disabled);
    st.setValue("plugins/python", pythonPlugins_->isChecked());
    if (QSet<QString>(disabled.begin(), disabled.end()) != QSet<QString>(before.begin(), before.end())) ctx_.requestPluginRescan();

    const bool dark = theme_->currentIndex() == 0;
    if ((Theme::current() == ThemeKind::Dark) != dark && setTheme_) setTheme_(dark);

    if (ctx_.hasProject()) {
        ProjectInfo info = ctx_.project().model().info;
        if (info.coordinateDisplayDecimals != decimals_->value() || info.southOrientatedAxes != southAxes_->isChecked()) {
            info.coordinateDisplayDecimals = decimals_->value();
            info.southOrientatedAxes = southAxes_->isChecked();
            if (!ctx_.tryEdit(tr("Coordinate display"), [&] { ProjectEditor(ctx_.project()).setProjectInfo(info, "Coordinate display settings"); })) return false;
        }
    }
    ctx_.report(tr("Preferences applied"));
    return true;
}

void PreferencesDialog::applyStored(QSettings& st, ViewportArea& views) {
    auto s = views.scene().settings();
    s.chordTolerance = st.value("display/chordTolerance", s.chordTolerance).toDouble();
    s.envelopeOpacity = st.value("display/envelopeOpacity", s.envelopeOpacity).toDouble();
    s.stationInterval = st.value("display/stationInterval", s.stationInterval).toDouble();
    s.structureDiscRadius = st.value("display/structureDiscRadius", s.structureDiscRadius).toDouble();
    s.drillholeWidth = st.value("display/drillholeWidth", s.drillholeWidth).toDouble();
    s.controlLabels = st.value("display/controlLabels", s.controlLabels).toBool();
    s.drillholeLabels = st.value("display/drillholeLabels", s.drillholeLabels).toBool();
    views.scene().setSettings(s);
    const auto budget = st.value("clouds/pointBudget", 0).toULongLong();
    const double spacing = st.value("clouds/screenSpacing", 0.0).toDouble();
    views.forEach([&](Viewport3D& v) {
        if (budget) v.options().pointBudget = budget;
        if (spacing > 0) v.options().pointScreenSpacing = spacing;
        v.applyOptions();
    });
    auto& tools = views.tools();
    if (st.contains("snap/modes")) tools.snapSettings().modes = st.value("snap/modes").toUInt();
    tools.snapSettings().gridSpacing = st.value("snap/gridSpacing", tools.snapSettings().gridSpacing).toDouble();
    tools.snapSettings().aperturePixels = st.value("snap/aperture", tools.snapSettings().aperturePixels).toDouble();
    tools.setTextHeight(st.value("draw/textHeight", tools.textHeight()).toDouble());
    tools.setFilletRadius(st.value("draw/filletRadius", tools.filletRadius()).toDouble());
    tools.setOffsetDistance(st.value("draw/offsetDistance", tools.offsetDistance()).toDouble());
}

// ---- quick-access toolbar ----------------------------------------------------------------------------

QuickAccessDialog::QuickAccessDialog(const std::map<QString, QAction*>& actions, const QStringList& current, QWidget* parent)
    : QDialog(parent), actions_(actions) {
    setWindowTitle(tr("Customise the quick-access toolbar"));
    resize(720, 520);
    auto* lay = new QVBoxLayout(this);
    auto* filter = new QLineEdit(this);
    filter->setPlaceholderText(tr("Filter commands…"));
    lay->addWidget(filter);
    auto* row = new QHBoxLayout;
    available_ = new QListWidget(this);
    chosen_ = new QListWidget(this);
    auto* mid = new QVBoxLayout;
    auto* add = new QPushButton(tr("Add ▶"), this);
    auto* remove = new QPushButton(tr("◀ Remove"), this);
    auto* up = new QPushButton(tr("Up"), this);
    auto* down = new QPushButton(tr("Down"), this);
    auto* reset = new QPushButton(tr("Defaults"), this);
    mid->addStretch(1);
    for (auto* b : {add, remove, up, down, reset}) mid->addWidget(b);
    mid->addStretch(1);
    row->addWidget(available_, 1);
    row->addLayout(mid);
    row->addWidget(chosen_, 1);
    lay->addLayout(row, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (const auto& [id, a] : actions_) {
        if (a->text().isEmpty()) continue;
        auto* it = new QListWidgetItem(a->icon(), QString(a->text()).remove('&'), available_);
        it->setData(Qt::UserRole, id);
        it->setToolTip(id);
    }
    available_->sortItems();
    connect(filter, &QLineEdit::textChanged, this, [this](const QString& t) {
        for (int i = 0; i < available_->count(); ++i)
            available_->item(i)->setHidden(!t.isEmpty() && !available_->item(i)->text().contains(t, Qt::CaseInsensitive));
    });
    connect(add, &QPushButton::clicked, this, [this] {
        QStringList ids = chosen();
        for (auto* it : available_->selectedItems())
            if (!ids.contains(it->data(Qt::UserRole).toString())) ids << it->data(Qt::UserRole).toString();
        setChosen(ids);
    });
    connect(available_, &QListWidget::itemDoubleClicked, add, &QPushButton::click);
    connect(remove, &QPushButton::clicked, this, [this] { delete chosen_->currentItem(); });
    connect(up, &QPushButton::clicked, this, [this] {
        const int r = chosen_->currentRow();
        if (r <= 0) return;
        chosen_->insertItem(r - 1, chosen_->takeItem(r));
        chosen_->setCurrentRow(r - 1);
    });
    connect(down, &QPushButton::clicked, this, [this] {
        const int r = chosen_->currentRow();
        if (r < 0 || r + 1 >= chosen_->count()) return;
        chosen_->insertItem(r + 1, chosen_->takeItem(r));
        chosen_->setCurrentRow(r + 1);
    });
    connect(reset, &QPushButton::clicked, this, [this] { setChosen({"file.new", "file.open", "file.save", "edit.undo", "edit.redo", "view.fit"}); });
    setChosen(current);
}

void QuickAccessDialog::setChosen(const QStringList& ids) {
    chosen_->clear();
    for (const auto& id : ids) {
        auto it = actions_.find(id);
        if (it == actions_.end()) continue;
        auto* item = new QListWidgetItem(it->second->icon(), QString(it->second->text()).remove('&'), chosen_);
        item->setData(Qt::UserRole, id);
    }
}

QStringList QuickAccessDialog::chosen() const {
    QStringList ids;
    for (int i = 0; i < chosen_->count(); ++i) ids << chosen_->item(i)->data(Qt::UserRole).toString();
    return ids;
}

} // namespace openmine::app
