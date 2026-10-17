// SPDX-License-Identifier: Apache-2.0
#include "openmine/diagnostics/Log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

namespace openmine::diag {

const char* toString(Level l) {
    switch (l) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warning: return "WARN";
    case Level::Error: return "ERROR";
    }
    return "?";
}

namespace {
std::string nowIso() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    char out[48];
    std::snprintf(out, sizeof out, "%s.%03dZ", buf, static_cast<int>(ms));
    return out;
}
std::string format(const Record& r) {
    return r.timestamp + " " + toString(r.level) + " [" + r.category + "] (" + r.thread + ") " + r.message;
}
} // namespace

Logger& Logger::instance() {
    static Logger l;
    return l;
}

int Logger::addSink(Sink s) {
    std::lock_guard lock(mutex_);
    sinks_.emplace_back(next_, std::move(s));
    return next_++;
}

void Logger::removeSink(int token) {
    std::lock_guard lock(mutex_);
    std::erase_if(sinks_, [&](const auto& p) { return p.first == token; });
}

int Logger::addFileSink(const std::filesystem::path& file, std::size_t maxBytes, int keep) {
    struct State {
        std::filesystem::path path;
        std::ofstream out;
        std::size_t maxBytes;
        int keep;
    };
    auto st = std::make_shared<State>();
    st->path = file;
    st->maxBytes = maxBytes;
    st->keep = keep;
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    st->out.open(file, std::ios::app);
    return addSink([st](const Record& r) {
        if (!st->out) return;
        st->out << format(r) << '\n';
        st->out.flush();
        std::error_code e;
        if (std::filesystem::file_size(st->path, e) > st->maxBytes && !e) {
            st->out.close();
            for (int i = st->keep - 1; i >= 1; --i) {
                auto from = st->path;
                from += "." + std::to_string(i);
                auto to = st->path;
                to += "." + std::to_string(i + 1);
                std::filesystem::rename(from, to, e);
            }
            auto first = st->path;
            first += ".1";
            std::filesystem::rename(st->path, first, e);
            st->out.open(st->path, std::ios::app);
        }
    });
}

int Logger::addStderrSink(Level minimum) {
    return addSink([minimum](const Record& r) {
        if (r.level >= minimum) std::cerr << format(r) << std::endl;
    });
}

void Logger::log(Level l, std::string category, std::string message) {
    std::ostringstream tid;
    tid << std::this_thread::get_id();
    Record r{l, nowIso(), std::move(category), std::move(message), tid.str()};
    std::vector<Sink> copy;
    {
        std::lock_guard lock(mutex_);
        for (auto& [_, s] : sinks_) copy.push_back(s);
    }
    for (auto& s : copy) s(r);
}

} // namespace openmine::diag
