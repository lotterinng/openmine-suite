#pragma once
// SPDX-License-Identifier: Apache-2.0
// Display items of every entity, shared by all viewports. Items are rebuilt lazily for the
// entities a change affects (an edited centreline rebuilds its development's envelope, an edited
// layer rebuilds the objects on it, …); viewports are told which entities changed so they only
// replace those actors. Also owns the render origin and the per-scene display settings.

#include "SceneBuilder.h"

#include <QObject>
#include <QSet>
#include <QTimer>

#include <set>
#include <unordered_map>
#include <unordered_set>

namespace openmine::app {

class SceneCache : public QObject {
    Q_OBJECT
public:
    explicit SceneCache(AppContext& ctx, QObject* parent = nullptr);

    const coordinates::RenderOrigin& origin() const { return origin_; }
    const SceneSettings& settings() const { return settings_; }
    void setSettings(const SceneSettings& s);
    BlockModelDisplay blockDisplay(const Uuid& blockModelId) const;
    void setBlockDisplay(const Uuid& blockModelId, const BlockModelDisplay& d);

    /// Builds dirty entities now (normally done on the next event-loop turn).
    void flush();
    /// Entities that have display items.
    std::vector<Uuid> entities() const;
    const std::vector<SceneItem>* items(const Uuid& entity) const;
    /// Every item, for whole-scene queries (bounds, box selection).
    template <class F> void forEachItem(F&& f) const {
        for (const auto& [id, e] : entries_)
            for (const auto& it : e.items) f(it);
    }
    /// World bounds of everything displayed (including point-cloud datasets).
    BoundingBox sceneBounds() const;

    /// Display-only hiding of entities (not saved; layer and dataset visibility are model data).
    bool isHidden(const Uuid& id) const { return hidden_.count(id) != 0; }
    void setHidden(const Uuid& id, bool hidden);
    void showAll();

    /// Numeric drillhole interval fields present in the project (for colouring choices).
    std::vector<std::string> drillholeFields() const;

    /// Loaded block-model values (cached by data asset), nullptr when they cannot be read.
    const geology::BlockModelData* blockData(const Uuid& blockModelId);

    /// Geometry objects with a display (for snapping and box selection), with world bounds.
    const std::unordered_map<Uuid, BoundingBox>& geometryBounds() const { return geometryBounds_; }

signals:
    /// Entities whose items were rebuilt or removed (`all` after a reset or origin change).
    void sceneChanged(QSet<QString> changed, bool all);
    /// The render origin moved; `delta` = new − old origin (world metres).
    void originChanged(openmine::Vec3 delta);

private:
    struct Entry {
        EntityKind kind = EntityKind::Geometry;
        std::vector<SceneItem> items;
    };
    void onEntityChanged(int kind, int type, const QString& uuid);
    void markDirty(const Uuid& id, EntityKind kind);
    void markAllOf(EntityKind kind);
    void markAll();
    void scheduleFlush();
    void rebuild(const Uuid& id, EntityKind kind);
    void chooseOrigin(bool force);
    void updateDrillholeRange();

    AppContext& ctx_;
    SceneSettings settings_;
    coordinates::RenderOrigin origin_;
    bool originSet_ = false;
    std::unordered_map<Uuid, Entry> entries_;
    std::unordered_map<Uuid, EntityKind> dirty_;
    std::unordered_set<Uuid> removed_;
    bool allDirty_ = true;
    bool flushPending_ = false;
    std::set<Uuid> hidden_;
    std::optional<std::pair<double, double>> drillholeRange_;
    struct BlockCache {
        Uuid assetId;
        std::int64_t revision = 0;
        std::shared_ptr<geology::BlockModelData> data;
        bool failed = false;
    };
    std::unordered_map<Uuid, BlockCache> blockCache_;
    std::unordered_map<Uuid, BoundingBox> geometryBounds_;
    std::uint64_t nextRevision_ = 1;
};

} // namespace openmine::app
