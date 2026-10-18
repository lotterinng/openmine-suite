#pragma once
// SPDX-License-Identifier: Apache-2.0
// Background jobs for the application. Work runs on the core worker pool (jobs::JobRunner) against
// a read-only *snapshot* of the project, so the user can keep working while it runs; the work
// returns a function that is executed on the UI thread to apply its result to the live project
// (through ProjectEditor / module commit functions: one validated undo step). Progress, stage
// text and cancellation are shown in the Jobs panel; cancelling throws jobs::CancelledError
// inside the work at its next progress update and nothing is applied.

#include "openmine/jobs/Job.h"
#include "openmine/project/Project.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace openmine::app {

/// What a job hands back to the UI thread.
using UiApply = std::function<void()>;

struct JobInfo {
    int id = 0;
    QString name;
    QString category;              ///< "Point cloud", "Registration", "Schedule", ...
    jobs::JobPtr handle;
    QDateTime started, finished;
    bool writesAssets = false;     ///< creates files in the project asset store
    bool applied = false;          ///< the result was applied to the project
    QString resultText;            ///< summary after apply, or the error
    bool done() const;
    jobs::JobState state() const { return handle ? handle->state.load() : jobs::JobState::Queued; }
};

class JobManager : public QObject {
    Q_OBJECT
public:
    explicit JobManager(QObject* parent = nullptr);
    ~JobManager() override;

    using Work = std::function<UiApply(jobs::Progress&)>;
    /// Starts a job. `work` runs on a worker thread; its returned function (may be empty) runs on
    /// the UI thread when the work succeeded. `onError` (UI thread) receives the error text of a
    /// failed job (not called on cancellation).
    int start(const QString& name, const QString& category, Work work, bool writesAssets = false,
              std::function<void(const QString&)> onError = {});

    void cancel(int id);
    void cancelAll();
    /// Processes the event loop until every job has finished (or `timeoutMs` elapsed). Returns
    /// true when nothing is running any more. Used on close and by automation.
    bool waitAll(int timeoutMs = -1);

    const std::vector<JobInfo>& jobs() const { return jobs_; }
    const JobInfo* find(int id) const;
    int runningCount() const;
    bool assetJobsRunning() const;
    /// Removes finished jobs from the list.
    void clearFinished();

    /// Set by a job's UI apply to describe what was done (shown in the Jobs panel).
    void setResultText(int id, const QString& text);
    /// While this returns true, finished jobs are not applied yet (e.g. while a script runs, so a
    /// job's result never lands inside the script's undo step). They are applied afterwards.
    void setApplyBlocked(std::function<bool()> blocked) { applyBlocked_ = std::move(blocked); }

signals:
    void jobAdded(int id);
    void jobProgress(int id);
    void jobFinished(int id, bool succeeded, QString message);
    void jobsChanged();

private:
    void poll();
    struct Pending;
    std::vector<JobInfo> jobs_;
    std::vector<std::shared_ptr<Pending>> pending_;
    QTimer timer_;
    int nextId_ = 1;
    std::function<bool()> applyBlocked_;
};

} // namespace openmine::app
