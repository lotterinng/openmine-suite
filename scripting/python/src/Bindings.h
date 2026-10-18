#pragma once
// SPDX-License-Identifier: Apache-2.0
// Native Python bindings shared by the extension module (openmine._core, for a standalone Python)
// and the embedded interpreter of the desktop application (which binds its *live* project, so
// scripts and the console act on exactly what the user sees, with undo).

#include "openmine/project/Project.h"

#include <pybind11/pybind11.h>

#include <memory>

namespace openmine::python {

/// A project seen from Python: owned (created/loaded by a script) or borrowed (the application's
/// open project; the application guarantees it outlives the script call).
class PyProject {
public:
    explicit PyProject(std::unique_ptr<Project> owned) : owned_(std::move(owned)), p_(owned_.get()) {}
    static PyProject borrow(Project& p) {
        PyProject r;
        r.p_ = &p;
        return r;
    }
    Project& get() const;
    bool owned() const { return owned_ != nullptr; }
    void close() {
        owned_.reset();
        p_ = nullptr;
    }

private:
    PyProject() = default;
    std::shared_ptr<Project> owned_;
    Project* p_ = nullptr;
};

/// Defines every class and function of the native module in `m`.
void defineBindings(pybind11::module_& m);

} // namespace openmine::python
