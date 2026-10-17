#pragma once
// SPDX-License-Identifier: Apache-2.0
// CAD-style command console with an extensible registry. Commands call the same
// validated editing operations as the UI — they never print fake results.

#include "../AppContext.h"

#include <QStringList>
#include <QWidget>

#include <functional>
#include <map>

class QLineEdit;
class QPlainTextEdit;

namespace openmine::app {

class CommandConsole : public QWidget {
    Q_OBJECT
public:
    using Handler = std::function<void(const QStringList& args)>;

    explicit CommandConsole(AppContext& ctx, QWidget* parent = nullptr);

    void registerCommand(const QString& name, const QString& usage, const QString& help, Handler handler,
                         const QStringList& aliases = {});
    /// Registered for discoverability; reports clearly that it is not yet available.
    void registerPlanned(const QString& name, const QString& plannedIn);

    /// Execute one command line. Returns false if the command failed or is unknown.
    bool execute(const QString& line);

    void print(const QString& text, bool error = false);
    QString lastOutput() const { return lastOutput_; }
    QStringList commandNames() const;

private:
    struct Entry {
        QString usage, help, planned;
        Handler handler;
    };
    void printHelp(const QString& name);

    AppContext& ctx_;
    QPlainTextEdit* out_;
    QLineEdit* in_;
    std::map<QString, Entry> commands_;
    std::map<QString, QString> aliases_;
    QStringList history_;
    int historyPos_ = 0;
    QString lastOutput_;
    bool lastFailed_ = false;
};

} // namespace openmine::app
