#pragma once
// SPDX-License-Identifier: Apache-2.0

#include "openmine/objects/Uuid.h"

#include <optional>
#include <unordered_map>
#include <vector>
#include <utility>

namespace openmine {

/// Insertion-ordered collection of entities keyed by Uuid.
///
/// Pointers and references returned by find() and all() stay valid only until the store is next
/// changed by inserting or removing an entity (replacing one in place keeps them): code that adds
/// entities of a kind while it still needs others of that kind keeps copies or ids, not pointers.
/// Builds with OPENMINE_ENTITYSTORE_ALWAYS_REALLOCATE move every entity on every insertion, so
/// that a sanitizer reports any stale pointer (see DEVELOPMENT.md).
template <class T>
class EntityStore {
public:
    const std::vector<T>& all() const { return items_; }
    std::size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }

    const T* find(const Uuid& id) const {
        auto it = index_.find(id);
        return it == index_.end() ? nullptr : &items_[it->second];
    }
    bool contains(const Uuid& id) const { return index_.count(id) != 0; }

    /// Insert or replace. When inserting, `position` (if valid) restores the original
    /// order so that undo of a delete puts the entity back where it was.
    void upsert(const T& value, std::optional<std::size_t> position = std::nullopt) {
        auto it = index_.find(value.id);
        if (it != index_.end()) {
            items_[it->second] = value;
            return;
        }
        if (position && *position < items_.size()) {
            items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(*position), value);
            reindex();
        } else {
            index_[value.id] = items_.size();
            items_.push_back(value);
        }
#ifdef OPENMINE_ENTITYSTORE_ALWAYS_REALLOCATE
        std::vector<T> moved;
        moved.reserve(items_.size() + 1);
        moved.assign(items_.begin(), items_.end());
        items_.swap(moved); // the old storage is freed: any pointer into it is now detectably stale
#endif
    }

    std::optional<std::size_t> positionOf(const Uuid& id) const {
        auto it = index_.find(id);
        if (it == index_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<T> remove(const Uuid& id) {
        auto it = index_.find(id);
        if (it == index_.end()) return std::nullopt;
        T removed = items_[it->second];
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(it->second));
        reindex();
        return removed;
    }

    /// Reorders the items: those listed in `order` first (in that order), then the others in their
    /// current relative order. Unknown ids in `order` are ignored.
    void reorder(const std::vector<Uuid>& order) {
        std::vector<T> out;
        out.reserve(items_.size());
        std::unordered_map<Uuid, bool> used;
        for (const auto& id : order) {
            auto it = index_.find(id);
            if (it == index_.end() || used[id]) continue;
            used[id] = true;
            out.push_back(items_[it->second]);
        }
        for (const auto& e : items_)
            if (!used[e.id]) out.push_back(e);
        items_ = std::move(out);
        reindex();
    }

    void clear() {
        items_.clear();
        index_.clear();
    }

private:
    void reindex() {
        index_.clear();
        for (std::size_t i = 0; i < items_.size(); ++i) index_[items_[i].id] = i;
    }
    std::vector<T> items_;
    std::unordered_map<Uuid, std::size_t> index_;
};

} // namespace openmine
