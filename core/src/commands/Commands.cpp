// SPDX-License-Identifier: Apache-2.0
#include "openmine/commands/CommandStack.h"
#include "openmine/commands/EntityCommands.h"
#include "openmine/project/Project.h"

#include <algorithm>

namespace openmine {

void Command::audit(Project& p) const { p.appendAudit("Command", "", Uuid{}, text()); }

// ---- MacroCommand --------------------------------------------------------
void MacroCommand::redo(Project& p) {
    std::size_t done = 0;
    try {
        for (auto& c : children_) {
            c->redo(p);
            ++done;
        }
    } catch (...) {
        // Roll back the children that already ran so the model stays consistent.
        for (std::size_t i = done; i-- > 0;) children_[i]->undo(p);
        throw;
    }
}
void MacroCommand::undo(Project& p) {
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) (*it)->undo(p);
}
void MacroCommand::audit(Project& p) const {
    for (auto& c : children_) c->audit(p);
}

// ---- CommandStack --------------------------------------------------------
void CommandStack::execute(std::unique_ptr<Command> cmd) {
    cmd->redo(project_); // may throw: nothing pushed
    cmd->audit(project_);
    if (openMacro_) {
        openMacro_->add(std::move(cmd));
        return;
    }
    commands_.resize(index_); // discard redo branch
    if (cleanIndex_ > static_cast<long>(index_)) cleanIndex_ = -1; // clean state unreachable
    commands_.push_back(std::move(cmd));
    index_ = commands_.size();
    notify();
}

void CommandStack::beginMacro(const std::string& text) {
    if (macroDepth_++ == 0) openMacro_ = std::make_unique<MacroCommand>(text);
}

void CommandStack::endMacro() {
    if (macroDepth_ == 0) return;
    if (--macroDepth_ > 0) return;
    auto macro = std::move(openMacro_);
    if (macro->empty()) return;
    commands_.resize(index_);
    if (cleanIndex_ > static_cast<long>(index_)) cleanIndex_ = -1;
    commands_.push_back(std::move(macro));
    index_ = commands_.size();
    notify();
}

void CommandStack::cancelMacro() {
    if (macroDepth_ == 0) return;
    macroDepth_ = 0;
    auto macro = std::move(openMacro_);
    if (macro && !macro->empty()) {
        macro->undo(project_);
        project_.appendAudit("Cancel", "", Uuid{}, macro->text() + " (rolled back)");
    }
    notify();
}

void CommandStack::undo() {
    if (inMacro() || !canUndo()) return;
    auto& cmd = commands_[--index_];
    cmd->undo(project_);
    project_.appendAudit("Undo", "", Uuid{}, cmd->text());
    notify();
}

void CommandStack::redo() {
    if (inMacro() || !canRedo()) return;
    auto& cmd = commands_[index_++];
    cmd->redo(project_);
    project_.appendAudit("Redo", "", Uuid{}, cmd->text());
    notify();
}

std::string CommandStack::undoText() const { return canUndo() ? commands_[index_ - 1]->text() : ""; }
std::string CommandStack::redoText() const { return canRedo() ? commands_[index_]->text() : ""; }

void CommandStack::clear() {
    commands_.clear();
    index_ = 0;
    cleanIndex_ = 0;
    notify();
}

// ---- DesignLinkCommand ---------------------------------------------------
void DesignLinkCommand::set(Project& p, bool present) {
    auto& links = p.mutableModel().designLinks;
    auto it = std::find(links.begin(), links.end(), link_);
    if (present && it == links.end()) {
        links.push_back(link_);
        p.events().publish({EntityKind::DesignLink, ChangeType::Added, link_.activityId});
    } else if (!present && it != links.end()) {
        links.erase(it);
        p.events().publish({EntityKind::DesignLink, ChangeType::Removed, link_.activityId});
    }
}
void DesignLinkCommand::audit(Project& p) const {
    p.appendAudit(add_ ? "Link" : "Unlink", "ActivityDesignLink", link_.activityId,
                  "activity " + link_.activityId.toString() + " <-> " + link_.designType + " " + link_.designId.toString());
}

// ---- ProjectInfoCommand --------------------------------------------------
void ProjectInfoCommand::redo(Project& p) {
    p.mutableModel().info = after_;
    p.events().publish({EntityKind::ProjectInfo, ChangeType::Modified, after_.id});
}
void ProjectInfoCommand::undo(Project& p) {
    p.mutableModel().info = before_;
    p.events().publish({EntityKind::ProjectInfo, ChangeType::Modified, before_.id});
}
void ProjectInfoCommand::audit(Project& p) const { p.appendAudit("Modify", "Project", after_.id, text_); }

} // namespace openmine
