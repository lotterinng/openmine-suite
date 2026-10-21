// SPDX-License-Identifier: Apache-2.0
#include "AppContext.h"

#include "openmine/scheduling/ScenarioSchedule.h"

#include "openmine/diagnostics/Log.h"
#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <set>

namespace openmine::app {

QString qstr(const std::string& s) { return QString::fromStdString(s); }
std::string sstr(const QString& s) { return s.toStdString(); }
QString formatNumber(double v, int decimals) {
    if (!std::isfinite(v)) return QStringLiteral("—");
    return QString::number(v, 'f', decimals);
}
QString formatDateTimeMinutes(double minutes) {
    return qstr(DateTime(static_cast<std::int64_t>(std::floor(minutes))).toString());
}

AppContext::AppContext(QObject* parent)
    : QObject(parent), plugins_(std::make_unique<plugins::PluginManager>()), settings_(QStringLiteral("OpenMine"), QStringLiteral("OpenMine Suite")) {
    plugins_->setLogSink([this](int level, const std::string& plugin, const std::string& msg) {
        const auto lvl = level >= 3 ? MessageLevel::Error : level == 2 ? MessageLevel::Warning : MessageLevel::Info;
        // Plugin calls run on the UI thread; queue anyway in case a plugin logs from a worker.
        QMetaObject::invokeMethod(
            this, [this, lvl, t = QStringLiteral("[%1] %2").arg(qstr(plugin), qstr(msg))] { report(t, lvl); }, Qt::QueuedConnection);
    });
}

AppContext::~AppContext() {
    jobs_.cancelAll();
    jobs_.waitAll(10000);
    if (project_) project_->events().unsubscribe(token_);
}

// ---- project ---------------------------------------------------------------------------------------

void AppContext::attach() {
    project_->commands().onChanged = [this] { emit undoStackChanged(); };
    token_ = project_->events().subscribe([this](const ChangeEvent& e) {
        if (e.type == ChangeType::Removed) {
            const auto before = selection_.size();
            std::erase_if(selection_, [&](const SelectionItem& s) { return s.id == e.id; });
            if (selection_.size() != before) emit selectionChanged();
            if (e.id == currentLayer_) {
                currentLayer_ = {};
                emit currentLayerChanged();
            }
        }
        if (e.type == ChangeType::Reset) {
            selection_.clear();
            emit selectionChanged();
        }
        emit entityChanged(static_cast<int>(e.kind), static_cast<int>(e.type), uuidText(e.id));
        if (!changePending_) {
            changePending_ = true;
            QTimer::singleShot(0, this, &AppContext::coalescedChange);
        }
    });
    selection_.clear();
    currentLayer_ = {};
    activeScenario_ = {};
    comparisonBaseline_ = {};
    playbackTime_.reset();
    snapshot_.reset();
    recalculateSchedule();
    emit projectReplaced();
    emit selectionChanged();
    emit undoStackChanged();
    emit currentLayerChanged();
    emit scenarioChanged();
    emit playbackChanged();
}

void AppContext::coalescedChange() {
    changePending_ = false;
    if (!project_) return;
    if (!activeScenario_.isNil() && !project_->model().scenarios.contains(activeScenario_)) {
        activeScenario_ = {};
        emit scenarioChanged();
    }
    recalculateSchedule();
    emit modelChanged();
}

void AppContext::newProject(const QString& name, Date start) {
    jobs_.cancelAll();
    jobs_.waitAll(10000);
    closeProject();
    project_ = Project::createNew(sstr(name), start);
    attach();
    report(tr("New project '%1' created (schedule start %2).").arg(name, qstr(start.toString())));
}

void AppContext::openProject(const QString& path) {
    persistence::LoadReport rep;
    auto loaded = persistence::loadProject(std::filesystem::path(path.toStdU16String()), &rep);
    jobs_.cancelAll();
    jobs_.waitAll(10000);
    closeProject();
    project_ = std::move(loaded);
    attach();
    report(tr("Opened %1 (schema v%2%3)").arg(QFileInfo(path).fileName()).arg(rep.fileSchemaVersion)
               .arg(rep.migrated ? tr(", migrated — it will be saved in the current schema") : QString()));
    for (const auto& w : rep.warnings) report(qstr(w), MessageLevel::Warning);
    settings_.setValue("recent/last", path);
    auto recent = settings_.value("recent/files").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 10) recent.removeLast();
    settings_.setValue("recent/files", recent);
}

void AppContext::adoptProject(std::unique_ptr<Project> project, const QString& description) {
    jobs_.cancelAll();
    jobs_.waitAll(10000);
    closeProject();
    project_ = std::move(project);
    pendingWorkingCopy_.clear();
    attach();
    report(description);
}

void AppContext::saveProject(const QString& path) {
    persistence::saveProject(*project_, std::filesystem::path(path.toStdU16String()));
    report(tr("Saved %1").arg(QFileInfo(path).fileName()));
    auto recent = settings_.value("recent/files").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    settings_.setValue("recent/files", recent);
}

void AppContext::closeProject() {
    if (!project_) return;
    project_->events().unsubscribe(token_);
    project_->commands().onChanged = nullptr;
    project_.reset();
    selection_.clear();
    schedule_ = {};
    snapshot_.reset();
    playbackTime_.reset();
    emit projectReplaced();
    emit selectionChanged();
    emit undoStackChanged();
    emit playbackChanged();
}

std::shared_ptr<const Project> AppContext::snapshot() const { return writableSnapshot(); }

std::shared_ptr<Project> AppContext::writableSnapshot() const {
    auto s = std::make_shared<Project>();
    s->mutableModel() = project_->model();
    s->assets() = AssetStore(project_->assets().root());
    s->setFilePath(project_->filePath());
    s->setUserName(project_->userName());
    return s;
}

// ---- selection -------------------------------------------------------------------------------------

bool AppContext::isSelected(EntityKind kind, const Uuid& id) const {
    return std::find(selection_.begin(), selection_.end(), SelectionItem{kind, id}) != selection_.end();
}

void AppContext::select(EntityKind kind, const Uuid& id, SelectMode mode) {
    if (id.isNil()) {
        if (mode == SelectMode::Replace) clearSelection();
        return;
    }
    selectMany({SelectionItem{kind, id}}, mode);
}

void AppContext::selectMany(const std::vector<SelectionItem>& items, SelectMode mode) {
    auto before = selection_;
    if (mode == SelectMode::Replace) selection_.clear();
    for (const auto& it : items) {
        if (it.id.isNil()) continue;
        auto pos = std::find(selection_.begin(), selection_.end(), it);
        switch (mode) {
        case SelectMode::Replace:
        case SelectMode::Add:
            if (pos != selection_.end()) selection_.erase(pos);
            selection_.push_back(it); // most recent last (primary)
            break;
        case SelectMode::Toggle:
            if (pos != selection_.end()) selection_.erase(pos);
            else selection_.push_back(it);
            break;
        case SelectMode::Remove:
            if (pos != selection_.end()) selection_.erase(pos);
            break;
        }
    }
    if (selection_ != before) emit selectionChanged();
}

void AppContext::clearSelection() {
    if (selection_.empty()) return;
    selection_.clear();
    emit selectionChanged();
}

std::vector<Uuid> AppContext::selected(EntityKind kind) const {
    std::vector<Uuid> out;
    for (const auto& s : selection_)
        if (s.kind == kind) out.push_back(s.id);
    return out;
}

std::vector<Uuid> AppContext::selectedGeometry() const {
    std::vector<Uuid> out;
    if (!project_) return out;
    const auto& m = project_->model();
    auto push = [&](const Uuid& id) {
        if (!id.isNil() && m.geometry.contains(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };
    for (const auto& s : selection_) {
        switch (s.kind) {
        case EntityKind::Geometry: push(s.id); break;
        case EntityKind::Development:
            if (const auto* d = m.developments.find(s.id)) push(d->geometryId);
            break;
        case EntityKind::Stope:
            if (const auto* st = m.stopes.find(s.id)) push(st->geometryId);
            break;
        case EntityKind::MiningBlock:
            if (const auto* b = m.miningBlocks.find(s.id)) push(b->geometryId);
            break;
        case EntityKind::GeologicalObject:
            if (const auto* g = m.geologicalObjects.find(s.id)) push(g->geometryId);
            break;
        case EntityKind::AsBuilt:
            if (const auto* a = m.asBuiltModels.find(s.id)) push(a->geometryId);
            break;
        default: break;
        }
    }
    return out;
}

// ---- layers ----------------------------------------------------------------------------------------

Uuid AppContext::currentLayer() const {
    if (!project_) return {};
    const auto& layers = project_->model().layers;
    if (const auto* l = layers.find(currentLayer_); l && !l->locked) return currentLayer_;
    for (const auto& l : layers.all())
        if (!l.locked && l.visible) return l.id;
    for (const auto& l : layers.all())
        if (!l.locked) return l.id;
    return {};
}

void AppContext::setCurrentLayer(const Uuid& id) {
    if (id == currentLayer_) return;
    currentLayer_ = id;
    emit currentLayerChanged();
}

// ---- schedule --------------------------------------------------------------------------------------

const Scenario* AppContext::activeScenario() const {
    if (!project_ || activeScenario_.isNil()) return nullptr;
    return project_->model().scenarios.find(activeScenario_);
}

void AppContext::setActiveScenario(const Uuid& scenarioId) {
    if (scenarioId == activeScenario_) return;
    activeScenario_ = scenarioId;
    recalculateSchedule();
    emit scenarioChanged();
    const auto* s = activeScenario();
    report(s ? tr("Active scenario: %1").arg(qstr(s->name)) : tr("Active scenario: base schedule"));
}

const ScheduleBaseline* AppContext::comparisonBaseline() const {
    if (!project_) return nullptr;
    const auto& bl = project_->model().baselines;
    if (const auto* b = bl.find(comparisonBaseline_)) return b;
    for (auto it = bl.all().rbegin(); it != bl.all().rend(); ++it)
        if (it->approved) return &*it;
    return bl.empty() ? nullptr : &bl.all().back();
}

void AppContext::setComparisonBaseline(const Uuid& id) {
    comparisonBaseline_ = id;
    updateSnapshot();
    emit scheduleRecalculated();
    emit playbackChanged();
}

void AppContext::recalculateSchedule() {
    if (!project_) return;
    QElapsedTimer t;
    t.start();
    const Scenario* scenario = activeScenario();
    ScheduleState s;
    s.scenarioId = scenario ? scenario->id : Uuid{};
    auto calc = scheduling::calculateScenario(*project_, scenario);
    s.input = std::move(calc.input);
    s.cpm = std::move(calc.cpm);
    s.result = std::move(calc.result);
    s.levelling = calc.levelling;
    s.milliseconds = static_cast<double>(t.nsecsElapsed()) / 1e6;
    schedule_ = std::move(s);
    updateSnapshot();
    emit scheduleRecalculated();
    if (playbackTime_) emit playbackChanged();
}

// ---- 4D ------------------------------------------------------------------------------------------

void AppContext::updateSnapshot() {
    snapshot_.reset();
    if (!project_ || !playbackTime_ || !schedule_.result.ok) return;
    try {
        snapshot_ = planning::snapshot(*project_, schedule_.input, schedule_.result, *playbackTime_, colorMode_,
                                       compareBaseline_ ? comparisonBaseline() : nullptr);
    } catch (const std::exception& e) {
        report(tr("4D snapshot failed: %1").arg(qstr(e.what())), MessageLevel::Error);
    }
}

void AppContext::setPlaybackTime(std::optional<double> minutes) {
    playbackTime_ = minutes;
    updateSnapshot();
    emit playbackChanged();
}

void AppContext::setPlaybackColorMode(planning::ColorMode m) {
    colorMode_ = m;
    updateSnapshot();
    emit playbackChanged();
}

void AppContext::setPlaybackCompareBaseline(bool on) {
    compareBaseline_ = on;
    updateSnapshot();
    emit playbackChanged();
}

// ---- services --------------------------------------------------------------------------------------

std::vector<PluginCommandInfo> AppContext::pluginCommands() const {
    std::vector<PluginCommandInfo> out;
    std::set<std::string> names;
    for (const auto& c : plugins_->commands()) {
        out.push_back({c.name, c.description, c.pluginId, false});
        names.insert(c.name);
    }
    if (scriptPlugins_)
        for (const auto& c : scriptPlugins_->scriptCommands())
            if (!names.count(c.name)) out.push_back({c.name, c.help, c.plugin, true});
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

plugins::PluginCallResult AppContext::runPluginCommand(const std::string& name, const std::string& argumentsJson) {
    for (const auto& c : plugins_->commands())
        if (c.name == name) return plugins_->runCommand(project(), name, argumentsJson);
    plugins::PluginCallResult r;
    if (scriptPlugins_)
        for (const auto& c : scriptPlugins_->scriptCommands())
            if (c.name == name) {
                const auto s = scriptPlugins_->runScriptCommand(name, argumentsJson);
                r.ok = s.ok;
                r.text = s.text;
                r.error = s.error;
                r.rolledBack = s.rolledBack;
                r.log = s.log;
                return r;
            }
    r.error = "Unknown plugin command '" + name + "'";
    return r;
}

std::vector<std::filesystem::path> AppContext::pluginDirectories() const {
    std::vector<std::filesystem::path> dirs;
    auto add = [&](const QString& d) {
        if (d.isEmpty()) return;
        const std::filesystem::path p(d.toStdU16String());
        if (std::find(dirs.begin(), dirs.end(), p) == dirs.end()) dirs.push_back(p);
    };
    const QString appDir = QCoreApplication::applicationDirPath();
    add(QDir(appDir).filePath("plugins"));
    add(QDir(appDir).filePath("../lib/openmine/plugins"));
    add(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("plugins"));
    const QByteArray env = qgetenv("OPENMINE_PLUGIN_PATH");
    for (const auto& part : QString::fromLocal8Bit(env).split(QDir::listSeparator(), Qt::SkipEmptyParts)) add(part);
    return dirs;
}

void AppContext::report(const QString& text, MessageLevel level) {
    const auto lvl = level == MessageLevel::Error ? diag::Level::Error : level == MessageLevel::Warning ? diag::Level::Warning : diag::Level::Info;
    diag::log(lvl, "app", text.toStdString());
    emit message(text, static_cast<int>(level));
}

bool AppContext::tryEdit(const QString& what, const std::function<void()>& fn) {
    if (!project_) {
        report(tr("%1: no project is open").arg(what), MessageLevel::Error);
        return false;
    }
    try {
        fn();
        return true;
    } catch (const ValidationError& e) {
        report(tr("%1 rejected: %2").arg(what, qstr(e.what())), MessageLevel::Error);
    } catch (const std::exception& e) {
        report(tr("%1 failed: %2").arg(what, qstr(e.what())), MessageLevel::Error);
    }
    return false;
}

} // namespace openmine::app
