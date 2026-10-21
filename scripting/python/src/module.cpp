// SPDX-License-Identifier: Apache-2.0
// The extension module openmine._core for a standalone Python interpreter.
#include "openmine/Version.h"
#include "Bindings.h"

PYBIND11_MODULE(_core, m) {
    m.doc() = "OpenMine Suite native core (internal; use the 'openmine' package)";
    m.attr("__version__") = OPENMINE_VERSION_STRING;
    openmine::python::defineBindings(m);
}
