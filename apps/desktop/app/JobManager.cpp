// SPDX-License-Identifier: Apache-2.0
#include "JobManager.h"

#include "openmine/diagnostics/Log.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <mutex>

namespace openmine::app {

struct JobManager::Pending {
    int id = 0;
    std::mutex mutex;
    UiApply apply;              ///< set by the worker on success
    bool hasResult = false;
    std::function<void(const QString&)> onError;
};

bool JobInfo::done() const {
    const auto s = state();
    return s == jobs::JobState::Succeeded || s == jobs::JobState::Failed || s == jobs::JobState::Cancelled;
}

JobManager::JobManager(QObject* parent) : QObject(parent) {
    timer_.setInterval(80);
    connect(&timer_, &QTimer::timeout, this, &JobManager::poll);
}

JobManager::~JobManager() {
    cancelAll();
    for (auto& j : jobs_)
        if (j.handle && j.handle->done.valid()) j.handle->done.wait();
}

int JobManager::start(const QString& name, const QString& category, Work work, bool writesAssets,
                      std::function<void(const QString&)> onError) {
    auto pending = std::make_shared<Pending>();
    pending->id = nextId_++;
    pending->onError = std::move(onError);
    auto handle = jobs::JobRunner::global().submit(name.toStdString(), [pending, work = std::move(work)](jobs::Progress& p) {
        UiApply apply = work(p);
        std::lock_guard lk(pending->mutex);
        pending->apply = std::move(apply);
        pending->hasResult = true;
    });
    JobInfo info;
    info.id = pending->id;
    info.name = name;
    info.category = category;
    info.handle = handle;
    info.started = QDateTime::currentDateTime();
    info.writesAssets = writesAssets;
    jobs_.push_back(info);
    pending_.push_back(pending);
    diag::info("app.jobs", "Started job '" + name.toStdString() + "'");
    if (!timer_.isActive()) timer_.start();
    emit jobAdded(info.id);
    emit jobsChanged();
    return info.id;
}

const JobInfo* JobManager::find(int id) const {
    for (const auto& j : jobs_)
        if (j.id == id) return &j;
    return nullptr;
}

void JobManager::cancel(int id) {
    for (auto& j : jobs_)
        if (j.id == id && j.handle && !j.done()) j.handle->progress->cancel();
}

void JobManager::cancelAll() {
    for (auto& j : jobs_)
        if (j.handle && !j.done()) j.handle->progress->cancel();
}

int JobManager::runningCount() const {
    int n = 0;
    for (const auto& j : jobs_)
        if (!j.done()) ++n;
    return n;
}

bool JobManager::assetJobsRunning() const {
    for (const auto& j : jobs_)
        if (j.writesAssets && !j.done()) return true;
    return false;
}

void JobManager::clearFinished() {
    std::vector<JobInfo> keep;
    for (auto& j : jobs_)
        if (!j.done() || (j.state() == jobs::JobState::Succeeded && !j.applied)) keep.push_back(j);
    jobs_ = std::move(keep);
    emit jobsChanged();
}

void JobManager::setResultText(int id, const QString& text) {
    for (auto& j : jobs_)
        if (j.id == id) j.resultText = text;
    emit jobsChanged();
}

void JobManager::poll() {
    bool anyRunning = false;
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        auto& j = jobs_[i];
        if (!j.done()) {
            anyRunning = true;
            emit jobProgress(j.id);
            continue;
        }
        if (j.finished.isValid()) continue; // already handled
        if (applyBlocked_ && applyBlocked_()) {
            anyRunning = true; // retry on the next tick
            continue;
        }
        j.finished = QDateTime::currentDateTime();
        std::shared_ptr<Pending> pending;
        for (auto it = pending_.begin(); it != pending_.end(); ++it)
            if ((*it)->id == j.id) {
                pending = *it;
                pending_.erase(it);
                break;
            }
        const auto st = j.state();
        if (st == jobs::JobState::Succeeded) {
            UiApply apply;
            if (pending) {
                std::lock_guard lk(pending->mutex);
                apply = std::move(pending->apply);
            }
            try {
                if (apply) apply();
                j.applied = true;
                if (j.resultText.isEmpty()) j.resultText = tr("Completed");
                diag::info("app.jobs", "Job '" + j.name.toStdString() + "' completed");
                emit jobFinished(j.id, true, j.resultText);
            } catch (const std::exception& e) {
                j.resultText = tr("Result rejected: %1").arg(QString::fromStdString(e.what()));
                diag::error("app.jobs", "Job '" + j.name.toStdString() + "' result rejected: " + e.what());
                if (pending && pending->onError) pending->onError(j.resultText);
                emit jobFinished(j.id, false, j.resultText);
            }
        } else if (st == jobs::JobState::Cancelled) {
            j.resultText = tr("Cancelled — nothing was changed");
            diag::info("app.jobs", "Job '" + j.name.toStdString() + "' cancelled");
            emit jobFinished(j.id, false, j.resultText);
        } else {
            j.resultText = QString::fromStdString(j.handle->error);
            diag::error("app.jobs", "Job '" + j.name.toStdString() + "' failed: " + j.handle->error);
            if (pending && pending->onError) pending->onError(j.resultText);
            emit jobFinished(j.id, false, j.resultText);
        }
        emit jobsChanged();
    }
    if (!anyRunning) {
        bool unhandled = false;
        for (const auto& j : jobs_)
            if (!j.finished.isValid()) unhandled = true;
        if (!unhandled) timer_.stop();
    }
}

bool JobManager::waitAll(int timeoutMs) {
    QElapsedTimer t;
    t.start();
    for (;;) {
        bool busy = false;
        for (const auto& j : jobs_)
            if (!j.done() || !j.finished.isValid()) busy = true;
        if (!busy) return true;
        if (timeoutMs >= 0 && t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        poll();
    }
}

} // namespace openmine::app
