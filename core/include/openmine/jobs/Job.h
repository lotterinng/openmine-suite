#pragma once
// SPDX-License-Identifier: Apache-2.0
// Background jobs with progress reporting and cooperative cancellation.
//
// Long-running algorithms (point-cloud import/filters, registration, meshing, optimisation)
// take a `Progress&` and call `progress.update(...)` regularly; `update` throws
// `CancelledError` once cancellation was requested. Results are applied to the project on
// the caller's (UI) thread through commands, so jobs never mutate the model directly.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace openmine::jobs {

class CancelledError : public std::runtime_error {
public:
    CancelledError() : std::runtime_error("Operation cancelled by user") {}
};

/// Shared progress/cancellation state of one job. Thread-safe.
class Progress {
public:
    /// Fraction in [0,1] of the *current stage* and an optional stage label.
    void update(double fraction, const std::string& stage = {});
    void setStage(const std::string& stage);
    /// Throws CancelledError if cancellation was requested.
    void checkCancelled() const {
        if (cancelled_.load(std::memory_order_relaxed)) throw CancelledError();
    }
    void cancel() { cancelled_ = true; }
    bool isCancelled() const { return cancelled_.load(); }
    double fraction() const { return fraction_.load(); }
    std::string stage() const;
    /// Sub-range helper: maps [0,1] of a sub-task onto [from,to] of this job.
    void subRange(double from, double to) {
        from_ = from;
        to_ = to;
    }

private:
    std::atomic<bool> cancelled_{false};
    std::atomic<double> fraction_{0.0};
    double from_ = 0.0, to_ = 1.0;
    mutable std::mutex mutex_;
    std::string stage_;
};

/// A no-op progress object for synchronous calls.
Progress& nullProgress();

enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };

class JobHandle {
public:
    std::string name;
    std::shared_ptr<Progress> progress = std::make_shared<Progress>();
    std::atomic<JobState> state{JobState::Queued};
    std::string error;
    std::function<void()> onFinished; ///< called on the worker thread after completion
    std::shared_future<void> done;
};
using JobPtr = std::shared_ptr<JobHandle>;

/// Fixed-size worker pool.
class JobRunner {
public:
    explicit JobRunner(unsigned threads = 0);
    ~JobRunner();
    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    /// Queues `work`. The returned handle tracks progress, state and errors.
    JobPtr submit(std::string name, std::function<void(Progress&)> work);
    std::vector<JobPtr> active() const;
    unsigned threadCount() const { return static_cast<unsigned>(threads_.size()); }

    static JobRunner& global();

private:
    void loop();
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<JobPtr, std::function<void(Progress&)>>> queue_;
    std::vector<JobPtr> running_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

/// Splits [0, n) into chunks processed in parallel by `threads` std::threads; `fn(begin, end)`.
/// Exceptions propagate (first one wins). Progress is checked for cancellation between chunks.
void parallelFor(std::size_t n, const std::function<void(std::size_t, std::size_t)>& fn, Progress* progress = nullptr,
                 unsigned threads = 0);

} // namespace openmine::jobs
