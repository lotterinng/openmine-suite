#pragma once
// SPDX-License-Identifier: Apache-2.0
// Thread-safe diagnostic logging with pluggable sinks (rotating file, UI console, stderr).

#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

namespace openmine::diag {

enum class Level { Trace, Debug, Info, Warning, Error };
const char* toString(Level);

struct Record {
    Level level;
    std::string timestamp; ///< UTC ISO-8601 with milliseconds
    std::string category;  ///< e.g. "persistence", "survey.icp"
    std::string message;
    std::string thread;
};

class Logger {
public:
    using Sink = std::function<void(const Record&)>;
    static Logger& instance();

    void setLevel(Level l) { level_ = l; }
    Level level() const { return level_; }
    bool enabled(Level l) const { return l >= level_; }

    int addSink(Sink s);
    void removeSink(int token);
    /// Appends to `file`; rotates to `file.1` … `file.<keep>` when it exceeds maxBytes.
    int addFileSink(const std::filesystem::path& file, std::size_t maxBytes = 5u << 20, int keep = 3);
    int addStderrSink(Level minimum = Level::Warning);

    void log(Level l, std::string category, std::string message);

private:
    Logger() = default;
    Level level_ = Level::Info;
    std::mutex mutex_;
    std::vector<std::pair<int, Sink>> sinks_;
    int next_ = 1;
};

inline void log(Level l, const std::string& cat, const std::string& msg) {
    auto& lg = Logger::instance();
    if (lg.enabled(l)) lg.log(l, cat, msg);
}
inline void info(const std::string& cat, const std::string& msg) { log(Level::Info, cat, msg); }
inline void warn(const std::string& cat, const std::string& msg) { log(Level::Warning, cat, msg); }
inline void error(const std::string& cat, const std::string& msg) { log(Level::Error, cat, msg); }
inline void debug(const std::string& cat, const std::string& msg) { log(Level::Debug, cat, msg); }

} // namespace openmine::diag
