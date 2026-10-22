#pragma once
// SPDX-License-Identifier: Apache-2.0
// Undo/redo command infrastructure. Every model change goes through a Command so that
// it is reversible, notifies observers and is written to the audit log.

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace openmine {

class Project;

class Command {
public:
    virtual ~Command() = default;
    virtual void redo(Project& p) = 0;
    virtual void undo(Project& p) = 0;
    virtual std::string text() const = 0;
    /// Called once, after the first successful redo, to record audit entries.
    virtual void audit(Project& p) const;
};

/// Groups several commands into one undoable step (executed in order, undone in reverse).
class MacroCommand : public Command {
public:
    explicit MacroCommand(std::string text) : text_(std::move(text)) {}
    void add(std::unique_ptr<Command> c) { children_.push_back(std::move(c)); }
    bool empty() const { return children_.empty(); }
    void redo(Project& p) override;
    void undo(Project& p) override;
    void audit(Project& p) const override;
    std::string text() const override { return text_; }

private:
    std::string text_;
    std::vector<std::unique_ptr<Command>> children_;
};

class CommandStack {
public:
    explicit CommandStack(Project& owner) : project_(owner) {}

    /// Execute and push. If redo() throws, nothing is pushed and the exception propagates.
    void execute(std::unique_ptr<Command> cmd);

    bool canUndo() const { return index_ > 0; }
    bool canRedo() const { return index_ < commands_.size(); }
    void undo();
    void redo();
    std::string undoText() const;
    std::string redoText() const;

    bool isClean() const { return cleanIndex_ == static_cast<long>(index_); }
    void setClean() { cleanIndex_ = static_cast<long>(index_); notify(); }
    void clear();

    std::size_t size() const { return commands_.size(); }
    std::size_t index() const { return index_; }

    /// Group subsequent execute() calls into one undo step. Nestable; only the
    /// outermost begin/end pair creates the step. cancelMacro() reverts the
    /// commands executed since the outermost beginMacro().
    void beginMacro(const std::string& text);
    void endMacro();
    void cancelMacro();
    bool inMacro() const { return macroDepth_ > 0; }

    /// RAII helper: commits on commit(), otherwise cancels in the destructor.
    class MacroScope {
    public:
        MacroScope(CommandStack& s, const std::string& text) : s_(s) { s_.beginMacro(text); }
        ~MacroScope() {
            if (!done_) s_.cancelMacro();
        }
        void commit() {
            s_.endMacro();
            done_ = true;
        }
        MacroScope(const MacroScope&) = delete;
        MacroScope& operator=(const MacroScope&) = delete;

    private:
        CommandStack& s_;
        bool done_ = false;
    };

    /// Observer called after any stack change (execute/undo/redo/clean).
    std::function<void()> onChanged;

private:
    void notify() {
        if (onChanged) onChanged();
    }
    Project& project_;
    std::unique_ptr<MacroCommand> openMacro_;
    int macroDepth_ = 0;
    std::vector<std::unique_ptr<Command>> commands_;
    std::size_t index_ = 0;
    long cleanIndex_ = 0;
};

} // namespace openmine
