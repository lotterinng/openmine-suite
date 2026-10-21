// SPDX-License-Identifier: Apache-2.0
#include "PythonConsole.h"

#include "Bindings.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace py = pybind11;

namespace openmine::python {

namespace {
Project* gCurrent = nullptr;
std::function<void(int, const std::string&)>* gSink = nullptr;
std::atomic<bool> gRunning{false};
std::function<void()>* gPump = nullptr;
std::atomic<bool> gInterruptRequested{false};

int interruptTrampoline(void*) {
    // Runs on the interpreter's thread between bytecodes: raising here interrupts the running code.
    // (PyErr_SetInterrupt would depend on Python's SIGINT handler, which Python does not install
    // when the process starts with SIGINT ignored, as under some CI runners and launchers.)
    if (!gInterruptRequested.exchange(false) || !gRunning.load()) return 0;
    PyErr_SetNone(PyExc_KeyboardInterrupt);
    return -1;
}

int pumpTrampoline(void*) {
    // Runs on the interpreter's thread with the GIL held, between bytecodes.
    if (gRunning.load() && gPump && *gPump) {
        try {
            (*gPump)();
        } catch (...) {
        }
    }
    return 0;
}

/// Schedules `fn` to run on the interpreter's thread between bytecodes. The caller takes the GIL
/// first: Python 3.12 rejects Py_AddPendingCall from a thread without a thread state (it returns
/// -1 every time), although the documentation allows it; 3.13 accepts both.
void schedule(int (*fn)(void*)) {
    const PyGILState_STATE g = PyGILState_Ensure();
    Py_AddPendingCall(fn, nullptr);
    PyGILState_Release(g);
}

/// While alive, schedules the pump periodically from a helper thread.
class PumpTicker {
public:
    explicit PumpTicker(int intervalMs) : interval_(intervalMs) {
        gInterruptRequested = false;
        gRunning = true;
        if (gPump && *gPump && interval_ > 0)
            thread_ = std::thread([this] {
                std::unique_lock lk(m_);
                while (!cv_.wait_for(lk, std::chrono::milliseconds(interval_), [this] { return stop_; })) {
                    lk.unlock(); // the GIL may take a while: never hold m_ while waiting for it
                    schedule(&pumpTrampoline);
                    lk.lock();
                }
            });
    }
    ~PumpTicker() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            // The helper may be waiting for the GIL, which this (interpreter) thread holds.
            py::gil_scoped_release release;
            thread_.join();
        }
        gRunning = false;
        gInterruptRequested = false;
    }

private:
    int interval_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

std::unique_ptr<py::scoped_interpreter>& interpreter() {
    static std::unique_ptr<py::scoped_interpreter> i;
    return i;
}
} // namespace

PYBIND11_EMBEDDED_MODULE(_openmine_embedded, m) {
    m.doc() = "OpenMine native core (embedded in the application)";
    m.attr("__version__") = "0.3.0";
    defineBindings(m);
    m.def("_emit", [](int stream, const std::string& text) {
        if (gSink && *gSink) (*gSink)(stream, text);
    }, "Forwards console output to the application while code runs.");
    m.def("current_project", []() -> py::object {
        if (!gCurrent) return py::none();
        return py::cast(PyProject::borrow(*gCurrent));
    }, "The project open in the application (borrowed; valid while it stays open).");
}

constexpr const char* kBootstrap = R"PY(
import sys, io, code, traceback, rlcompleter
import _openmine_embedded
sys.modules['openmine._core'] = _openmine_embedded
import openmine

class _OpenMineConsole(code.InteractiveConsole):
    def __init__(self, ns):
        super().__init__(ns, filename='<console>')
        self.ok = True
    def showtraceback(self):
        self.ok = False
        super().showtraceback()
    def showsyntaxerror(self, filename=None, **kw):
        self.ok = False
        super().showsyntaxerror(filename)

class _Tee(io.StringIO):
    def __init__(self, stream):
        super().__init__()
        self._stream = stream
    def write(self, s):
        n = super().write(s)
        if s:
            _openmine_embedded._emit(self._stream, s)
        return n

def _capture(fn):
    out, err = _Tee(0), _Tee(1)
    so, se = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = out, err
    try:
        r = fn()
    finally:
        sys.stdout, sys.stderr = so, se
    return r, out.getvalue(), err.getvalue()

def _namespace():
    return {'openmine': openmine, 'project': openmine.current_project(), '__name__': '__console__'}

_console = _OpenMineConsole(_namespace())

def _push(line):
    _console.ok = True
    more, out, err = _capture(lambda: _console.push(line))
    return bool(more), out, err, _console.ok

def _run(source, filename):
    ns = {'openmine': openmine, 'project': openmine.current_project(), '__name__': '__main__', '__file__': filename}
    ok = [True]
    def go():
        try:
            exec(compile(source, filename, 'exec'), ns)
        except SystemExit:
            pass
        except BaseException:
            ok[0] = False
            traceback.print_exc()
    _, out, err = _capture(go)
    return out, err, ok[0]

def _rebind():
    _console.locals['project'] = openmine.current_project()

def _complete(text):
    c = rlcompleter.Completer(_console.locals)
    out, i = [], 0
    while i < 200:
        s = c.complete(text, i)
        if s is None:
            break
        out.append(s)
        i += 1
    return out
)PY";

struct PythonConsole::Impl {
    py::object push, run, rebind, complete;
    std::function<void(int, const std::string&)> sink;
    std::function<void()> pump;
    int interval = 100;
    /// Makes this console's sink and pump the active ones for the duration of a call.
    struct Active {
        explicit Active(Impl& i) : prevSink(gSink), prevPump(gPump) {
            gSink = &i.sink;
            gPump = &i.pump;
        }
        ~Active() {
            gSink = prevSink;
            gPump = prevPump;
        }
        std::function<void(int, const std::string&)>* prevSink;
        std::function<void()>* prevPump;
    };
};

PythonConsole::PythonConsole(Project* project, std::vector<std::filesystem::path> paths) : impl_(std::make_unique<Impl>()) {
    if (!interpreter() && !Py_IsInitialized()) interpreter() = std::make_unique<py::scoped_interpreter>();
    gCurrent = project;
    if (paths.empty()) paths.push_back(OPENMINE_PYTHON_PACKAGE_DIR);
    auto sys = py::module_::import("sys");
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) sys.attr("path").attr("insert")(0, it->string());
    py::dict ns;
    py::exec(kBootstrap, ns);
    impl_->push = ns["_push"];
    impl_->run = ns["_run"];
    impl_->rebind = ns["_rebind"];
    impl_->complete = ns["_complete"];
}

PythonConsole::~PythonConsole() = default;

void PythonConsole::setProject(Project* p) {
    gCurrent = p;
    impl_->rebind();
}

void PythonConsole::setOutputSink(std::function<void(int, const std::string&)> sink) { impl_->sink = std::move(sink); }

void PythonConsole::setPump(std::function<void()> pump, int intervalMs) {
    impl_->pump = std::move(pump);
    impl_->interval = intervalMs;
}

bool PythonConsole::isRunning() { return gRunning.load(); }

PythonConsole::Result PythonConsole::push(const std::string& line) {
    Result r;
    Impl::Active active(*impl_);
    PumpTicker ticker(impl_->interval);
    const auto t = impl_->push(line).cast<py::tuple>();
    r.needsMore = t[0].cast<bool>();
    r.output = t[1].cast<std::string>();
    r.error = t[2].cast<std::string>();
    r.ok = t[3].cast<bool>();
    return r;
}

PythonConsole::Result PythonConsole::run(const std::string& source, const std::string& filename) {
    Result r;
    Impl::Active active(*impl_);
    PumpTicker ticker(impl_->interval);
    const auto t = impl_->run(source, filename).cast<py::tuple>();
    r.output = t[0].cast<std::string>();
    r.error = t[1].cast<std::string>();
    r.ok = t[2].cast<bool>();
    return r;
}

PythonConsole::Result PythonConsole::runFile(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        Result r;
        r.ok = false;
        r.error = "Cannot open " + file.string();
        return r;
    }
    std::ostringstream s;
    s << in.rdbuf();
    return run(s.str(), file.string());
}

std::vector<std::string> PythonConsole::complete(const std::string& text) { return impl_->complete(text).cast<std::vector<std::string>>(); }

void PythonConsole::interrupt() {
    if (!gRunning.load()) return;
    gInterruptRequested = true;
    schedule(&interruptTrampoline); // from the pump (GIL held) or from another thread
}

std::string PythonConsole::pythonVersion() { return Py_GetVersion(); }

} // namespace openmine::python
