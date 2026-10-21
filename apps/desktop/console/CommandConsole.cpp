// SPDX-License-Identifier: Apache-2.0
#include "CommandConsole.h"

#include "../theme/Theme.h"
#include "../viewport/Tools.h"

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTime>
#include <QVBoxLayout>

namespace openmine::app {

namespace {
class HistoryLineEdit : public QLineEdit {
public:
    std::function<void(int)> onHistory;
    using QLineEdit::QLineEdit;

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Up && onHistory) return onHistory(-1);
        if (e->key() == Qt::Key_Down && onHistory) return onHistory(+1);
        QLineEdit::keyPressEvent(e);
    }
};
} // namespace

CommandConsole::CommandConsole(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    out_ = new QPlainTextEdit(this);
    out_->setObjectName("ConsoleOutput");
    out_->setReadOnly(true);
    out_->setMaximumBlockCount(5000);
    lay->addWidget(out_, 1);
    auto* row = new QHBoxLayout;
    auto* prompt = new QLabel("Command:", this);
    auto* edit = new HistoryLineEdit(this);
    in_ = edit;
    in_->setObjectName("ConsoleInput");
    in_->setPlaceholderText("Type a command (HELP lists them) — e.g. POLYLINE 0,0,1000 90,0,1000 @0,60,0");
    row->addWidget(prompt);
    row->addWidget(in_, 1);
    lay->addLayout(row);

    edit->onHistory = [this](int dir) {
        if (history_.isEmpty()) return;
        historyPos_ = std::clamp(historyPos_ + dir, 0, static_cast<int>(history_.size()));
        in_->setText(historyPos_ < history_.size() ? history_[historyPos_] : QString());
    };
    connect(in_, &QLineEdit::returnPressed, this, [this] {
        const QString line = in_->text().trimmed();
        in_->clear();
        if (line.isEmpty()) return;
        history_ << line;
        historyPos_ = static_cast<int>(history_.size());
        execute(line);
    });
    connect(&ctx_, &AppContext::message, this, [this](const QString& t, int level) { print(t, level == static_cast<int>(MessageLevel::Error)); });

    registerCommand("HELP", "HELP [command]", "List commands or show help for one command",
                    [this](const QStringList& a) { printHelp(a.isEmpty() ? QString() : a.front().toUpper()); }, {"?"});
}

void CommandConsole::registerCommand(const QString& name, const QString& usage, const QString& help, Handler handler,
                                     const QStringList& aliases) {
    commands_[name.toUpper()] = {usage, help, std::move(handler)};
    for (const auto& a : aliases) aliases_[a.toUpper()] = name.toUpper();
}

QStringList CommandConsole::commandNames() const {
    QStringList out;
    for (const auto& [k, _] : commands_) out << k;
    return out;
}

QString CommandConsole::referenceMarkdown() const {
    std::map<QString, QStringList> aliasesOf;
    for (const auto& [alias, name] : aliases_) aliasesOf[name] << alias;
    auto cell = [](QString t) { return t.replace('|', "\\|").replace('\n', ' '); };
    QString md;
    md += "| Command | Usage | Description | Aliases |\n|---|---|---|---|\n";
    for (const auto& [name, e] : commands_)
        md += QStringLiteral("| `%1` | `%2` | %3 | %4 |\n")
                  .arg(name, cell(e.usage), cell(e.help), aliasesOf.count(name) ? "`" + aliasesOf[name].join("`, `") + "`" : QString());
    return md;
}

void CommandConsole::print(const QString& text, bool error) {
    lastOutput_ = text;
    if (error) lastFailed_ = true;
    const auto& c = Theme::colors();
    const QString stamp = QTime::currentTime().toString("HH:mm:ss");
    out_->appendHtml(QStringLiteral("<span style='color:%1'>%2</span> <span style='color:%3'>%4</span>")
                         .arg(c.textMuted.name(), stamp, (error ? c.ganttCritical : c.text).name(), text.toHtmlEscaped()));
}

bool CommandConsole::execute(const QString& line) {
    const auto& c = Theme::colors();
    out_->appendHtml(QStringLiteral("<span style='color:%1'>&gt; %2</span>").arg(c.accent.name(), line.toHtmlEscaped()));
    if (tools_ && tools_->isActive() && tools_->acceptInput(line)) return true;
    QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    QString name = parts.takeFirst().toUpper();
    if (auto a = aliases_.find(name); a != aliases_.end()) name = a->second;
    auto it = commands_.find(name);
    if (it == commands_.end()) {
        print(QStringLiteral("Unknown command '%1'. Type HELP for a list.").arg(name), true);
        return false;
    }
    lastFailed_ = false;
    try {
        it->second.handler(parts);
    } catch (const std::exception& e) {
        print(QStringLiteral("%1 failed: %2").arg(name, e.what()), true);
        return false;
    }
    return !lastFailed_;
}

void CommandConsole::printHelp(const QString& name) {
    if (!name.isEmpty()) {
        auto it = commands_.find(aliases_.count(name) ? aliases_[name] : name);
        if (it == commands_.end()) {
            print("No such command: " + name, true);
            return;
        }
        print(it->second.usage + " — " + it->second.help);
        return;
    }
    QStringList available;
    for (const auto& [k, e] : commands_) available << k;
    print("Commands: " + available.join(", "));
    print("While a drawing tool runs: x,y,z | x,y (z = work plane) | @dx,dy[,dz] relative | @distance<bearing polar | a number (distance, radius, angle, factor)");
}

} // namespace openmine::app
