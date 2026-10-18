#pragma once
// SPDX-License-Identifier: Apache-2.0
// Embedded Python console (scripting/python/embed): an interactive prompt with continuation
// lines, history, Tab completion and streamed output, plus script execution — both act on the
// application's live project (`project` in the console, `openmine.current_project()` in scripts),
// so every change goes through the same validated, undoable editing operations as the UI.
// While code runs, window events keep being processed so the Interrupt button works; other input
// is blocked by a modal progress dialog to keep the model consistent.

#include "../app/AppContext.h"

#include <QWidget>

#include <memory>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QProgressDialog;

namespace openmine::python {
class PythonConsole;
}

namespace openmine::app {

class PythonConsolePanel : public QWidget {
    Q_OBJECT
public:
    explicit PythonConsolePanel(AppContext& ctx, QWidget* parent = nullptr);
    ~PythonConsolePanel() override;

    static bool available();
    /// Executes one interactive line (as if typed). Returns false on an exception.
    bool executeLine(const QString& line);
    /// Runs a script (source text or a file). Returns false on an exception.
    bool runScript(const QString& source, const QString& name = QStringLiteral("<script>"));
    bool runFile(const QString& path);
    QString transcript() const;
    bool busy() const { return running_; }

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void ensureConsole();
    void append(const QString& text, int stream);
    void setPrompt(bool more);
    void beginRun(const QString& what);
    void endRun();
    void complete();

    AppContext& ctx_;
#ifdef OPENMINE_APP_HAVE_PYTHON
    std::unique_ptr<python::PythonConsole> console_;
#endif
    QPlainTextEdit* out_ = nullptr;
    QLineEdit* in_ = nullptr;
    QLabel* prompt_ = nullptr;
    QPushButton* interrupt_ = nullptr;
    QProgressDialog* progress_ = nullptr;
    QStringList history_;
    int historyPos_ = 0;
    bool running_ = false;
    bool more_ = false;
    qint64 runStart_ = 0;
};

} // namespace openmine::app
