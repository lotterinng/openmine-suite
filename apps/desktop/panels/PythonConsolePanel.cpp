// SPDX-License-Identifier: Apache-2.0
#include "PythonConsolePanel.h"

#include "../theme/Theme.h"

#ifdef OPENMINE_APP_HAVE_PYTHON
#include "PythonConsole.h"
#endif

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBlock>
#include <QVBoxLayout>
#include <system_error>

namespace openmine::app {

bool PythonConsolePanel::available() {
#ifdef OPENMINE_APP_HAVE_PYTHON
    return true;
#else
    return false;
#endif
}

PythonConsolePanel::PythonConsolePanel(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    auto* bar = new QHBoxLayout;
    auto* runBtn = new QPushButton(toolIcon("python"), tr("Run script…"), this);
    interrupt_ = new QPushButton(toolIcon("stop"), tr("Interrupt"), this);
    interrupt_->setEnabled(false);
    auto* clear = new QPushButton(tr("Clear"), this);
    bar->addWidget(runBtn);
    bar->addWidget(interrupt_);
    bar->addWidget(clear);
    bar->addStretch(1);
    lay->addLayout(bar);
    out_ = new QPlainTextEdit(this);
    out_->setReadOnly(true);
    out_->setObjectName("ConsoleOutput");
    out_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    out_->setMaximumBlockCount(20000);
    lay->addWidget(out_, 1);
    auto* inRow = new QHBoxLayout;
    prompt_ = new QLabel(">>>", this);
    prompt_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    in_ = new QLineEdit(this);
    in_->setObjectName("ConsoleInput");
    in_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    in_->installEventFilter(this);
    inRow->addWidget(prompt_);
    inRow->addWidget(in_, 1);
    lay->addLayout(inRow);

    if (!available()) {
        out_->setPlainText(tr("Python scripting is not available in this build (OpenMine was built without the Python bindings)."));
        in_->setEnabled(false);
        runBtn->setEnabled(false);
        return;
    }
    out_->setPlainText(tr("OpenMine Python console — `project` is the open project, `openmine` the API.\n"
                          "Every change is validated and undoable (Edit ▸ Undo). Type help(openmine) for the reference.\n"));
    connect(in_, &QLineEdit::returnPressed, this, [this] {
        const QString line = in_->text();
        in_->clear();
        executeLine(line);
    });
    connect(runBtn, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Run Python script"), ctx_.settings().value("python/lastDir").toString(), tr("Python (*.py)"));
        if (f.isEmpty()) return;
        ctx_.settings().setValue("python/lastDir", QFileInfo(f).absolutePath());
        runFile(f);
    });
    connect(interrupt_, &QPushButton::clicked, this, [] {
#ifdef OPENMINE_APP_HAVE_PYTHON
        python::PythonConsole::interrupt();
#endif
    });
    connect(clear, &QPushButton::clicked, out_, &QPlainTextEdit::clear);
    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
#ifdef OPENMINE_APP_HAVE_PYTHON
        if (console_) console_->setProject(ctx_.projectPtr());
#endif
    });
    ctx_.jobs().setApplyBlocked([this] { return running_; });
}

PythonConsolePanel::~PythonConsolePanel() { ctx_.jobs().setApplyBlocked({}); }

void PythonConsolePanel::ensureConsole() {
#ifdef OPENMINE_APP_HAVE_PYTHON
    if (console_) return;
    std::vector<std::filesystem::path> paths;
    // Installed package next to the executable, else the build tree (the console's default).
    const QDir appDir(QCoreApplication::applicationDirPath());
    for (const QString& cand : {appDir.filePath("../lib/openmine/python"), appDir.filePath("python")})
        if (QFileInfo::exists(QDir(cand).filePath("openmine/__init__.py"))) paths.emplace_back(cand.toStdString());
    console_ = std::make_unique<python::PythonConsole>(ctx_.projectPtr(), paths);
    console_->setOutputSink([this](int stream, const std::string& text) {
        if (!quiet_) append(qstr(text), stream);
    });
    console_->setPump([this] {
        // Show the modal progress dialog once code has run for a moment, then keep the UI alive.
        if (!progress_ && QDateTime::currentMSecsSinceEpoch() - runStart_ > 600) {
            progress_ = new QProgressDialog(tr("Running Python…"), tr("Interrupt"), 0, 0, this);
            progress_->setWindowModality(Qt::ApplicationModal);
            progress_->setMinimumDuration(0);
            connect(progress_, &QProgressDialog::canceled, this, [] { python::PythonConsole::interrupt(); });
            // Closing the dialog any other way (Esc, the window manager) also stops the code: the
            // dialog is not shown again for this run, so it must not leave the code unstoppable.
            connect(progress_, &QDialog::rejected, this, [] { python::PythonConsole::interrupt(); });
            progress_->show();
        }
        QApplication::processEvents(QEventLoop::AllEvents, 25);
    }, 80);
    append(tr("Python %1\n").arg(qstr(python::PythonConsole::pythonVersion()).section(' ', 0, 0)), 0);
#endif
}

void PythonConsolePanel::append(const QString& text, int stream) {
    QTextCharFormat fmt;
    fmt.setForeground(stream == 1 ? Theme::colors().warning : stream == 2 ? Theme::colors().accent : Theme::colors().text);
    auto cursor = out_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text, fmt);
    out_->verticalScrollBar()->setValue(out_->verticalScrollBar()->maximum());
}

void PythonConsolePanel::setPrompt(bool more) {
    more_ = more;
    prompt_->setText(more ? "..." : ">>>");
}

void PythonConsolePanel::beginRun(const QString&) {
    running_ = true;
    runStart_ = QDateTime::currentMSecsSinceEpoch();
    interrupt_->setEnabled(true);
    in_->setEnabled(false);
}

void PythonConsolePanel::endRun() {
    running_ = false;
    interrupt_->setEnabled(false);
    in_->setEnabled(true);
    in_->setFocus();
    if (progress_) {
        progress_->hide();
        progress_->deleteLater();
        progress_ = nullptr;
    }
}

bool PythonConsolePanel::executeLine(const QString& line) {
#ifdef OPENMINE_APP_HAVE_PYTHON
    if (running_) return false;
    ensureConsole();
    append((more_ ? "... " : ">>> ") + line + "\n", 2);
    if (!line.trimmed().isEmpty() && (history_.isEmpty() || history_.last() != line)) history_ << line;
    historyPos_ = static_cast<int>(history_.size());
    beginRun(line);
    python::PythonConsole::Result r;
    try {
        r = console_->push(sstr(line));
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
        append(qstr(r.error) + "\n", 1);
    }
    endRun();
    setPrompt(r.needsMore);
    return r.ok;
#else
    (void)line;
    return false;
#endif
}

bool PythonConsolePanel::runScript(const QString& source, const QString& name) {
#ifdef OPENMINE_APP_HAVE_PYTHON
    if (running_) return false;
    ensureConsole();
    append(tr("# running %1\n").arg(name), 2);
    beginRun(name);
    python::PythonConsole::Result r;
    try {
        r = console_->run(sstr(source), sstr(name));
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
        append(qstr(r.error) + "\n", 1);
    }
    endRun();
    append(r.ok ? tr("# %1 finished\n").arg(name) : tr("# %1 failed\n").arg(name), r.ok ? 2 : 1);
    if (!r.ok) ctx_.report(tr("Script %1 failed (see the Python console)").arg(name), MessageLevel::Error);
    return r.ok;
#else
    (void)source;
    (void)name;
    return false;
#endif
}

bool PythonConsolePanel::runFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        ctx_.report(tr("Cannot open %1").arg(path), MessageLevel::Error);
        return false;
    }
    return runScript(QString::fromUtf8(f.readAll()), path);
}

QString PythonConsolePanel::transcript() const { return out_->toPlainText(); }

bool PythonConsolePanel::runQuiet(const QString& source, QString* output, QString* error) {
#ifdef OPENMINE_APP_HAVE_PYTHON
    if (running_) {
        if (error) *error = tr("Python is busy");
        return false;
    }
    ensureConsole();
    beginRun(QStringLiteral("<plugin>"));
    quiet_ = true;
    python::PythonConsole::Result r;
    try {
        r = console_->run(sstr(source), "<openmine plugin host>");
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    }
    quiet_ = false;
    endRun();
    if (output) *output = qstr(r.output);
    if (error) *error = qstr(r.error);
    return r.ok;
#else
    (void)source;
    (void)output;
    if (error) *error = tr("This build has no embedded Python");
    return false;
#endif
}

namespace {
/// A JSON array of strings is also a Python list literal: the safe way to pass text into code.
QString pyStrings(const QStringList& values) {
    QJsonArray a;
    for (const auto& v : values) a.append(v);
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}
/// The last line of `output` that is a JSON object; earlier lines go to `rest`.
QJsonObject lastJsonLine(const QString& output, QStringList* rest) {
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    while (!lines.isEmpty()) {
        const auto doc = QJsonDocument::fromJson(lines.last().toUtf8());
        lines.removeLast();
        if (doc.isObject()) {
            if (rest) *rest = lines;
            return doc.object();
        }
    }
    return {};
}
} // namespace

int PythonConsolePanel::discoverPlugins(const std::vector<std::filesystem::path>& folders, QStringList* problems) {
    commands_.clear();
    if (!available()) return 0;
    QStringList paths;
    for (const auto& f : folders) {
        const auto py = f / "python";
        std::error_code ec;
        if (std::filesystem::is_directory(py, ec)) paths << QString::fromStdU16String(py.u16string());
    }
    QString out, err;
    const QString src = QStringLiteral("import openmine.plugins as _omp\nprint(_omp._desktop_discover(%1[0]))\n")
                            .arg(pyStrings({QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(paths)).toJson(QJsonDocument::Compact))}));
    if (!runQuiet(src, &out, &err)) {
        if (problems) *problems << tr("Python plugin discovery failed: %1").arg(err.trimmed());
        return 0;
    }
    const auto o = lastJsonLine(out, nullptr);
    for (const auto& v : o.value("commands").toArray()) {
        const auto c = v.toObject();
        commands_.push_back({sstr(c.value("name").toString()), sstr(c.value("help").toString()), sstr(c.value("plugin").toString())});
    }
    if (problems)
        for (const auto& e : o.value("errors").toArray()) *problems << tr("Python plugin %1").arg(e.toString());
    return static_cast<int>(commands_.size());
}

ScriptPluginHost::Result PythonConsolePanel::runScriptCommand(const std::string& name, const std::string& argumentsJson) {
    Result r;
    QString out, err;
    const QString src = QStringLiteral("import openmine.plugins as _omp\n_a = %1\nprint(_omp._desktop_run(_a[0], _a[1]))\n")
                            .arg(pyStrings({qstr(name), qstr(argumentsJson)}));
    if (!runQuiet(src, &out, &err)) {
        r.error = sstr(err.trimmed());
        return r;
    }
    QStringList rest;
    const auto o = lastJsonLine(out, &rest);
    for (const auto& l : rest) r.log.push_back(sstr(l));
    if (o.isEmpty()) {
        r.error = "The Python plugin host returned no result";
        return r;
    }
    r.ok = o.value("ok").toBool();
    r.text = sstr(o.value("text").toString());
    r.error = sstr(o.value("error").toString());
    r.rolledBack = o.value("rolled_back").toBool();
    return r;
}

void PythonConsolePanel::complete() {
#ifdef OPENMINE_APP_HAVE_PYTHON
    ensureConsole();
    const QString text = in_->text().left(in_->cursorPosition());
    // Complete the last identifier chain (letters, digits, '_' and '.').
    int start = text.size();
    while (start > 0 && (text[start - 1].isLetterOrNumber() || text[start - 1] == '_' || text[start - 1] == '.')) --start;
    const QString word = text.mid(start);
    const auto cands = console_->complete(sstr(word));
    if (cands.empty()) return;
    QString common = qstr(cands.front());
    for (const auto& c : cands) {
        const QString q = qstr(c);
        int i = 0;
        while (i < common.size() && i < q.size() && common[i] == q[i]) ++i;
        common.truncate(i);
    }
    if (common.size() > word.size()) {
        in_->setText(text.left(start) + common + in_->text().mid(in_->cursorPosition()));
        in_->setCursorPosition(start + static_cast<int>(common.size()));
    } else if (cands.size() > 1) {
        QStringList l;
        for (const auto& c : cands) l << qstr(c);
        append(l.join("  ") + "\n", 2);
    }
#endif
}

bool PythonConsolePanel::eventFilter(QObject* o, QEvent* e) {
    if (o == in_ && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Up && !history_.isEmpty()) {
            historyPos_ = std::max(0, historyPos_ - 1);
            in_->setText(history_[historyPos_]);
            return true;
        }
        if (k->key() == Qt::Key_Down && !history_.isEmpty()) {
            historyPos_ = std::min(static_cast<int>(history_.size()), historyPos_ + 1);
            in_->setText(historyPos_ < history_.size() ? history_[historyPos_] : QString());
            return true;
        }
        if (k->key() == Qt::Key_Tab) {
            // Indentation in continuation lines, completion otherwise.
            if (more_ && in_->text().trimmed().isEmpty()) in_->insert("    ");
            else complete();
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

} // namespace openmine::app
