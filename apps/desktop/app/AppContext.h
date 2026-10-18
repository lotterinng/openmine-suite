#pragma once
// SPDX-License-Identifier: Apache-2.0
// Bridges the toolkit-independent project model to Qt. Owns the open Project, the selection set,
// the current layer, the calculated schedule of the active scenario, the 4D playback state, the
// background-job manager and the plugin manager, and re-emits model events as Qt signals.
//
// Model change notifications are delivered twice: immediately per entity (`entityChanged`, for
// incremental viewport updates) and coalesced once per event-loop turn (`modelChanged`, for
// panels), after the schedule of the active scenario has been recalculated — so a panel that
// reacts to `modelChanged` always sees a schedule consistent with the model.

#include "JobManager.h"

#include "openmine/planning/FourD.h"
#include "openmine/plugins/PluginManager.h"
#include "openmine/project/Project.h"
#include "openmine/scheduling/Cpm.h"
#include "openmine/scheduling/Levelling.h"

#include <QMetaType>
#include <QObject>
#include <QSettings>
#include <QString>

#include <memory>
#include <optional>
#include <set>
#include <vector>

Q_DECLARE_METATYPE(openmine::Vec3)

namespace openmine::app {

struct SelectionItem {
    EntityKind kind = EntityKind::ProjectInfo;
    Uuid id;
    friend bool operator==(const SelectionItem&, const SelectionItem&) = default;
    friend bool operator<(const SelectionItem& a, const SelectionItem& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.id < b.id;
    }
};

enum class SelectMode { Replace, Add, Toggle, Remove };

/// The calculated schedule of the active scenario.
struct ScheduleState {
    scheduling::ScheduleInput input;          ///< points into the live model: valid until the next change
    scheduling::ScheduleResult result;        ///< levelled when the scenario asks for it
    scheduling::ScheduleResult cpm;           ///< unlevelled CPM (for levelling delays)
    std::optional<scheduling::LevellingReport> levelling;
    Uuid scenarioId;                          ///< nil = base schedule
    double milliseconds = 0;
};

enum class MessageLevel { Info, Warning, Error };

/// Commands of script (Python) plugins, provided by the embedded interpreter when the build has
/// one (PythonConsolePanel).
class ScriptPluginHost {
public:
    struct Command {
        std::string name, help, plugin;
    };
    struct Result {
        bool ok = false;
        std::string text, error;
        bool rolledBack = false;
        std::vector<std::string> log; ///< other output of the command
    };
    virtual ~ScriptPluginHost() = default;
    virtual std::vector<Command> scriptCommands() const = 0;
    virtual Result runScriptCommand(const std::string& name, const std::string& argumentsJson) = 0;
};

/// A plugin command of either kind.
struct PluginCommandInfo {
    std::string name, description, plugin;
    bool python = false;
};

class AppContext : public QObject {
    Q_OBJECT
public:
    explicit AppContext(QObject* parent = nullptr);
    ~AppContext() override;

    // ---- project -------------------------------------------------------------------------------
    bool hasProject() const { return static_cast<bool>(project_); }
    Project& project() { return *project_; }
    const Project& project() const { return *project_; }
    Project* projectPtr() { return project_.get(); }

    void newProject(const QString& name, Date start);
    /// Throws persistence::PersistenceError. Load warnings are reported as messages.
    void openProject(const QString& path);
    /// Throws persistence::PersistenceError.
    void saveProject(const QString& path);
    void closeProject();
    /// Makes an already loaded project the open one (e.g. a repository checkout).
    void adoptProject(std::unique_ptr<Project> project, const QString& description);
    /// Repository working-copy state of a checked-out project not yet saved to a file (JSON);
    /// written next to the project file by the first save.
    const std::string& pendingWorkingCopy() const { return pendingWorkingCopy_; }
    void setPendingWorkingCopy(std::string json) { pendingWorkingCopy_ = std::move(json); }
    /// A consistent, read-only copy of the model sharing the asset store root (for jobs).
    std::shared_ptr<const Project> snapshot() const;
    /// The same, writable: for jobs that create new assets (they land in the live asset folder,
    /// and the job's UI step adds them to the live project).
    std::shared_ptr<Project> writableSnapshot() const;

    // ---- selection -----------------------------------------------------------------------------
    const std::vector<SelectionItem>& selection() const { return selection_; }
    /// The most recently selected item (empty item when nothing is selected).
    SelectionItem primary() const { return selection_.empty() ? SelectionItem{} : selection_.back(); }
    bool isSelected(EntityKind kind, const Uuid& id) const;
    void select(EntityKind kind, const Uuid& id, SelectMode mode = SelectMode::Replace);
    void selectMany(const std::vector<SelectionItem>& items, SelectMode mode = SelectMode::Replace);
    void clearSelection();
    /// Ids of the selected entities of one kind.
    std::vector<Uuid> selected(EntityKind kind) const;
    /// Geometry objects of the selection, including centrelines of selected developments and the
    /// shapes of selected stopes/blocks.
    std::vector<Uuid> selectedGeometry() const;

    // ---- layers --------------------------------------------------------------------------------
    /// Layer new CAD objects go to (first unlocked visible layer when unset or gone).
    Uuid currentLayer() const;
    void setCurrentLayer(const Uuid& id);

    // ---- schedule ------------------------------------------------------------------------------
    const ScheduleState& schedule() const { return schedule_; }
    const Scenario* activeScenario() const;
    void setActiveScenario(const Uuid& scenarioId);
    /// Baseline compared against (Gantt, 4D behind-plan colouring); default the latest approved.
    const ScheduleBaseline* comparisonBaseline() const;
    void setComparisonBaseline(const Uuid& id);
    void recalculateSchedule();

    // ---- 4D playback ---------------------------------------------------------------------------
    bool playbackActive() const { return playbackTime_.has_value(); }
    std::optional<double> playbackTime() const { return playbackTime_; }
    void setPlaybackTime(std::optional<double> minutes);
    planning::ColorMode playbackColorMode() const { return colorMode_; }
    void setPlaybackColorMode(planning::ColorMode m);
    bool playbackCompareBaseline() const { return compareBaseline_; }
    void setPlaybackCompareBaseline(bool on);
    /// Snapshot of the active schedule at the playback time (nullopt when playback is off or the
    /// schedule could not be calculated).
    const std::optional<planning::PlanSnapshot>& planSnapshot() const { return snapshot_; }

    // ---- services ------------------------------------------------------------------------------
    JobManager& jobs() { return jobs_; }
    plugins::PluginManager& plugins() { return *plugins_; }
    void setScriptPlugins(ScriptPluginHost* host) { scriptPlugins_ = host; }
    /// Asks the main window to reload native plugins and rediscover Python plugins.
    void requestPluginRescan() { emit pluginRescanRequested(); }
    /// Native and Python plugin commands, sorted by name. A native command shadows a Python
    /// command of the same name (reported once by the main window when plugins are loaded).
    std::vector<PluginCommandInfo> pluginCommands() const;
    /// Runs a plugin command of either kind on the project (one undo step; an edit left open or
    /// a failing Python command is rolled back).
    plugins::PluginCallResult runPluginCommand(const std::string& name, const std::string& argumentsJson = "{}");
    /// Plugin folders searched at start-up (application dir, user dir, OPENMINE_PLUGIN_PATH).
    std::vector<std::filesystem::path> pluginDirectories() const;
    QSettings& settings() { return settings_; }

    /// Message routed to the console, the status bar and the log.
    void report(const QString& text, MessageLevel level = MessageLevel::Info);
    void report(const QString& text, bool error) { report(text, error ? MessageLevel::Error : MessageLevel::Info); }

    /// Runs `fn` reporting a ValidationError / other exception instead of throwing. Returns success.
    bool tryEdit(const QString& what, const std::function<void()>& fn);

signals:
    void projectReplaced();
    /// Immediate, per entity (also for undo/redo). Do not read the schedule here.
    void entityChanged(int kind, int changeType, QString uuid);
    /// Coalesced once per event-loop turn, after the schedule was recalculated.
    void modelChanged();
    void selectionChanged();
    void scheduleRecalculated();
    void undoStackChanged();
    void currentLayerChanged();
    void scenarioChanged();
    void playbackChanged();
    void message(QString text, int level);
    void pluginRescanRequested();

private:
    void attach();
    void coalescedChange();
    void updateSnapshot();

    std::unique_ptr<Project> project_;
    std::vector<SelectionItem> selection_;
    Uuid currentLayer_;
    ScheduleState schedule_;
    Uuid activeScenario_;
    Uuid comparisonBaseline_;
    std::optional<double> playbackTime_;
    planning::ColorMode colorMode_ = planning::ColorMode::State;
    bool compareBaseline_ = false;
    std::optional<planning::PlanSnapshot> snapshot_;
    std::string pendingWorkingCopy_;
    EventBus::Token token_ = 0;
    bool changePending_ = false;
    JobManager jobs_;
    std::unique_ptr<plugins::PluginManager> plugins_;
    ScriptPluginHost* scriptPlugins_ = nullptr;
    QSettings settings_;
};

QString qstr(const std::string& s);
std::string sstr(const QString& s);
QString formatNumber(double v, int decimals = 3);
QString formatDateTimeMinutes(double minutes);
inline QString uuidText(const Uuid& id) { return QString::fromStdString(id.toString()); }
inline Uuid uuidFrom(const QString& s) { return Uuid::parse(s.toStdString()).value_or(Uuid{}); }

} // namespace openmine::app
