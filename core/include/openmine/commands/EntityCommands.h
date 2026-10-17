#pragma once
// SPDX-License-Identifier: Apache-2.0
// Generic snapshot commands: each stores the full before/after state of one entity,
// which makes create, modify and delete trivially reversible.

#include "openmine/commands/CommandStack.h"
#include "openmine/project/Project.h"

#include <optional>

namespace openmine {

template <class T> struct EntityTraits;

#define OPENMINE_ENTITY_TRAITS(T, member, Kind)                                            \
    template <> struct EntityTraits<T> {                                                    \
        static constexpr EntityKind kind = EntityKind::Kind;                                \
        static constexpr const char* name = #T;                                             \
        static EntityStore<T>& store(ProjectModel& m) { return m.member; }                  \
        static const EntityStore<T>& store(const ProjectModel& m) { return m.member; }      \
    };
OPENMINE_ENTITY_TYPES(OPENMINE_ENTITY_TRAITS)
#undef OPENMINE_ENTITY_TRAITS

/// Sets an entity from `before` to `after`. nullopt before ⇒ create; nullopt after ⇒ delete.
template <class T>
class SetEntityCommand : public Command {
public:
    SetEntityCommand(std::optional<T> before, std::optional<T> after, std::string text,
                     std::optional<std::size_t> originalPosition = std::nullopt)
        : before_(std::move(before)), after_(std::move(after)), text_(std::move(text)), position_(originalPosition) {}

    void redo(Project& p) override { apply(p, before_, after_); }
    void undo(Project& p) override { apply(p, after_, before_); }
    std::string text() const override { return text_; }

    void audit(Project& p) const override {
        const char* action = !before_ ? "Create" : (!after_ ? "Delete" : "Modify");
        const Uuid id = after_ ? after_->id : before_->id;
        p.appendAudit(action, EntityTraits<T>::name, id, text_);
    }

private:
    void apply(Project& p, const std::optional<T>& from, const std::optional<T>& to) {
        auto& store = EntityTraits<T>::store(p.mutableModel());
        if (to) {
            const bool existed = store.contains(to->id);
            store.upsert(*to, existed ? std::nullopt : position_);
            p.events().publish({EntityTraits<T>::kind, existed ? ChangeType::Modified : ChangeType::Added, to->id});
        } else if (from) {
            store.remove(from->id);
            p.events().publish({EntityTraits<T>::kind, ChangeType::Removed, from->id});
        }
    }

    std::optional<T> before_;
    std::optional<T> after_;
    std::string text_;
    std::optional<std::size_t> position_;
};

class DesignLinkCommand : public Command {
public:
    DesignLinkCommand(ActivityDesignLink link, bool add) : link_(std::move(link)), add_(add) {}
    void redo(Project& p) override { set(p, add_); }
    void undo(Project& p) override { set(p, !add_); }
    std::string text() const override { return add_ ? "Link activity to design" : "Unlink activity from design"; }
    void audit(Project& p) const override;

private:
    void set(Project& p, bool present);
    ActivityDesignLink link_;
    bool add_;
};

class ProjectInfoCommand : public Command {
public:
    ProjectInfoCommand(ProjectInfo before, ProjectInfo after, std::string text)
        : before_(std::move(before)), after_(std::move(after)), text_(std::move(text)) {}
    void redo(Project& p) override;
    void undo(Project& p) override;
    std::string text() const override { return text_; }
    void audit(Project& p) const override;

private:
    ProjectInfo before_, after_;
    std::string text_;
};

} // namespace openmine
