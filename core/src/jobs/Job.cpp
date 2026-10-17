// SPDX-License-Identifier: Apache-2.0
#include "openmine/jobs/Job.h"

#include "openmine/diagnostics/Log.h"

#include <algorithm>

namespace openmine::jobs {

void Progress::update(double fraction, const std::string& stage) {
    checkCancelled();
    fraction = std::clamp(fraction, 0.0, 1.0);
    fraction_ = from_ + (to_ - from_) * fraction;
    if (!stage.empty()) setStage(stage);
}

void Progress::setStage(const std::string& stage) {
    std::lock_guard lock(mutex_);
    stage_ = stage;
}

std::string Progress::stage() const {
    std::lock_guard lock(mutex_);
    return stage_;
}

Progress& nullProgress() {
    thread_local Progress p;
    return p;
}

JobRunner::JobRunner(unsigned threads) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { loop(); });
}

JobRunner::~JobRunner() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        for (auto& [j, _] : queue_) j->progress->cancel();
        for (auto& j : running_) j->progress->cancel();
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

JobRunner& JobRunner::global() {
    static JobRunner r(std::max(2u, std::thread::hardware_concurrency()));
    return r;
}

JobPtr JobRunner::submit(std::string name, std::function<void(Progress&)> work) {
    auto job = std::make_shared<JobHandle>();
    job->name = std::move(name);
    auto promise = std::make_shared<std::promise<void>>();
    job->done = promise->get_future().share();
    auto wrapped = [job, promise, w = std::move(work)](Progress& p) {
        job->state = JobState::Running;
        diag::info("jobs", "started: " + job->name);
        try {
            w(p);
            job->state = JobState::Succeeded;
            diag::info("jobs", "finished: " + job->name);
        } catch (const CancelledError&) {
            job->state = JobState::Cancelled;
            diag::info("jobs", "cancelled: " + job->name);
        } catch (const std::exception& e) {
            job->error = e.what();
            job->state = JobState::Failed;
            diag::error("jobs", "failed: " + job->name + ": " + e.what());
        } catch (...) {
            job->error = "unknown error";
            job->state = JobState::Failed;
        }
        if (job->onFinished) job->onFinished();
        promise->set_value();
    };
    {
        std::lock_guard lock(mutex_);
        queue_.emplace_back(job, std::move(wrapped));
    }
    cv_.notify_one();
    return job;
}

std::vector<JobPtr> JobRunner::active() const {
    std::lock_guard lock(mutex_);
    std::vector<JobPtr> out = running_;
    for (auto& [j, _] : queue_) out.push_back(j);
    return out;
}

void JobRunner::loop() {
    while (true) {
        std::pair<JobPtr, std::function<void(Progress&)>> item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            item = std::move(queue_.front());
            queue_.pop_front();
            running_.push_back(item.first);
        }
        item.second(*item.first->progress);
        std::lock_guard lock(mutex_);
        std::erase(running_, item.first);
    }
}

void parallelFor(std::size_t n, const std::function<void(std::size_t, std::size_t)>& fn, Progress* progress, unsigned threads) {
    if (n == 0) return;
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = static_cast<unsigned>(std::min<std::size_t>(threads, (n + 1023) / 1024));
    if (threads <= 1) {
        const std::size_t chunk = 1 << 16;
        for (std::size_t b = 0; b < n; b += chunk) {
            if (progress) progress->update(static_cast<double>(b) / static_cast<double>(n));
            fn(b, std::min(n, b + chunk));
        }
        return;
    }
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> doneCount{0};
    const std::size_t chunk = std::max<std::size_t>(1024, n / (threads * 16));
    std::exception_ptr err;
    std::mutex errMutex;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            while (true) {
                {
                    std::lock_guard l(errMutex);
                    if (err) return;
                }
                if (progress && progress->isCancelled()) return;
                const std::size_t b = next.fetch_add(chunk);
                if (b >= n) return;
                try {
                    fn(b, std::min(n, b + chunk));
                } catch (...) {
                    std::lock_guard l(errMutex);
                    if (!err) err = std::current_exception();
                    return;
                }
                doneCount += std::min(n, b + chunk) - b;
            }
        });
    while (doneCount < n) {
        {
            std::lock_guard l(errMutex);
            if (err) break;
        }
        if (progress && progress->isCancelled()) break;
        if (progress) {
            try {
                progress->update(static_cast<double>(doneCount.load()) / static_cast<double>(n));
            } catch (...) {
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (auto& th : pool) th.join();
    if (err) std::rethrow_exception(err);
    if (progress) progress->checkCancelled();
}

} // namespace openmine::jobs
