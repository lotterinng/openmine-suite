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
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBlock>
#include <QVBoxLayout>

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
    console_->setOutputSink([this](int stream, const std::string& text) { append(qstr(text), stream); });
    console_->setPump([this] {
        // Show the modal progress dialog once code has run for a moment, then keep the UI alive.
        if (!progress_ && QDateTime::currentMSecsSinceEpoch() - runStart_ > 600) {
            progress_ = new QProgressDialog(tr("Running Python…"), tr("Interrupt"), 0, 0, this);
            progress_->setWindowModality(Qt::ApplicationModal);
            progress_->setMinimumDuration(0);
            connect(progress_, &QProgressDialog::canceled, this, [] { python::PythonConsole::interrupt(); });
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
