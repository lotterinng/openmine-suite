#pragma once
// SPDX-License-Identifier: Apache-2.0
// Bridges the toolkit-independent project model to Qt: owns the open Project,
// the current selection, and the latest schedule calculation, and re-emits
// model events as Qt signals for the panels.

#include "openmine/project/Project.h"
#include "openmine/scheduling/Cpm.h"

#include <QObject>
#include <QString>

#include <memory>

namespace openmine::app {

struct Selection {
    EntityKind kind = EntityKind::ProjectInfo;
    Uuid id;
    bool isEmpty() const { return id.isNil(); }
    friend bool operator==(const Selection&, const Selection&) = default;
};

class AppContext : public QObject {
    Q_OBJECT
public:
    explicit AppContext(QObject* parent = nullptr);
    ~AppContext() override;

    bool hasProject() const { return static_cast<bool>(project_); }
    Project& project() { return *project_; }
    const Project& project() const { return *project_; }

    void newProject(const QString& name, Date start);
    /// Throws persistence::PersistenceError.
    void openProject(const QString& path);
    /// Throws persistence::PersistenceError.
    void saveProject(const QString& path);
    void closeProject();

    const Selection& selection() const { return selection_; }
    void select(EntityKind kind, const Uuid& id);
    void clearSelection() { select(EntityKind::ProjectInfo, Uuid{}); }

    /// The latest CPM result (recalculated automatically after every model change).
    const scheduling::ScheduleResult& schedule() const { return schedule_; }
    /// Recalculate immediately (normally coalesced to the next event-loop turn).
    void recalculateSchedule();

    /// Message routed to the console/status bar.
    void report(const QString& text, bool error = false) { emit message(text, error); }

signals:
    void projectReplaced();
    void modelChanged(int entityKind, int changeType, QString uuid);
    void selectionChanged();
    void scheduleRecalculated();
    void undoStackChanged();
    void message(const QString& text, bool error);

private:
    void attach();

    std::unique_ptr<Project> project_;
    Selection selection_;
    scheduling::ScheduleResult schedule_;
    EventBus::Token token_ = 0;
    bool recalcPending_ = false;
};

QString qstr(const std::string& s);
std::string sstr(const QString& s);
QString formatNumber(double v, int decimals = 3);

} // namespace openmine::app
