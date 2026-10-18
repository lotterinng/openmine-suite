#pragma once
// SPDX-License-Identifier: Apache-2.0
// Embedded Python for the application: an interactive console (continuation lines, captured
// output, completion, interruption) and script execution, both acting on the application's *live*
// project — `project` in the console namespace and `openmine.current_project()` in scripts — so
// every change is the user's own, validated and undoable.
//
// One interpreter per process (created by the first console, finalised at exit). Calls must come
// from one thread (the UI thread); long work inside scripts can use worker threads in Python.

#include "openmine/project/Project.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace openmine::python {

class PythonConsole {
public:
    /// `packagePaths` are prepended to sys.path (where the pure-Python `openmine` package lives;
    /// the build-tree package is used when empty).
    explicit PythonConsole(Project* project = nullptr, std::vector<std::filesystem::path> packagePaths = {});
    ~PythonConsole();
    PythonConsole(const PythonConsole&) = delete;
    PythonConsole& operator=(const PythonConsole&) = delete;

    /// Rebinds the live project (e.g. after File ▸ Open); nullptr when none is open.
    void setProject(Project* project);

    struct Result {
        std::string output;      ///< captured stdout
        std::string error;       ///< captured stderr (tracebacks)
        bool needsMore = false;  ///< the line is incomplete (e.g. after "for …:")
        bool ok = true;          ///< no exception was raised
    };
    /// One interactive line (as typed at the prompt).
    Result push(const std::string& line);
    /// A whole script (its globals are a fresh namespace with `project` and `openmine`).
    Result run(const std::string& source, const std::string& filename = "<script>");
    Result runFile(const std::filesystem::path& file);
    /// Completion candidates for the text before the cursor.
    std::vector<std::string> complete(const std::string& text);
    /// Raises KeyboardInterrupt in the running code (callable from another thread).
    static void interrupt();
    /// Python version string of the embedded interpreter.
    static std::string pythonVersion();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openmine::python
