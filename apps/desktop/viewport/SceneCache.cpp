// SPDX-License-Identifier: Apache-2.0
#include "SceneCache.h"

#include "openmine/diagnostics/Log.h"
#include "openmine/geometry/Curves.h"

#include <algorithm>
#include <cmath>

namespace openmine::app {

SceneCache::SceneCache(AppContext& ctx, QObject* parent) : QObject(parent), ctx_(ctx) {
    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
        entries_.clear();
        dirty_.clear();
        removed_.clear();
        hidden_.clear();
        blockCache_.clear();
        geometryBounds_.clear();
        settings_.blockModels.clear();
        originSet_ = false;
        markAll();
    });
    connect(&ctx_, &AppContext::entityChanged, this, &SceneCache::onEntityChanged);
}

void SceneCache::setSettings(const SceneSettings& s) {
    if (s == settings_) return;
    const bool drill = s.drillholeField != settings_.drillholeField;
    settings_ = s;
    if (drill) updateDrillholeRange();
    markAll();
}

BlockModelDisplay SceneCache::blockDisplay(const Uuid& id) const {
    auto it = settings_.blockModels.find(id);
    return it == settings_.blockModels.end() ? BlockModelDisplay{} : it->second;
}

void SceneCache::setBlockDisplay(const Uuid& id, const BlockModelDisplay& d) {
    settings_.blockModels[id] = d;
    markDirty(id, EntityKind::BlockModel);
}

void SceneCache::setHidden(const Uuid& id, bool hidden) {
    const bool was = hidden_.count(id) != 0;
    if (was == hidden) return;
    if (hidden) hidden_.insert(id);
    else hidden_.erase(id);
    // Re-evaluate the entity (hidden entities have no items).
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    EntityKind kind = EntityKind::Geometry;
    if (m.developments.contains(id)) kind = EntityKind::Development;
    else if (m.controlPoints.contains(id)) kind = EntityKind::ControlPoint;
    else if (m.drillholes.contains(id)) kind = EntityKind::Drillhole;
    else if (m.blockModels.contains(id)) kind = EntityKind::BlockModel;
    else if (m.structures.contains(id)) kind = EntityKind::Structure;
    else if (m.geotechClassifications.contains(id)) kind = EntityKind::GeotechClassification;
    else if (m.pointClouds.contains(id)) kind = EntityKind::PointCloud;
    markDirty(id, kind);
    // Hiding a development / stope also hides its shape.
    if (const auto* d = m.developments.find(id)) markDirty(d->geometryId, EntityKind::Geometry);
    if (const auto* s = m.stopes.find(id)) {
        if (hidden) hidden_.insert(s->geometryId);
        else hidden_.erase(s->geometryId);
        markDirty(s->geometryId, EntityKind::Geometry);
    }
}

void SceneCache::showAll() {
    if (hidden_.empty()) return;
    hidden_.clear();
    markAll();
}

void SceneCache::onEntityChanged(int kindInt, int typeInt, const QString& uuid) {
    if (!ctx_.hasProject()) return;
    const auto kind = static_cast<EntityKind>(kindInt);
    const auto type = static_cast<ChangeType>(typeInt);
    const Uuid id = uuidFrom(uuid);
    const auto& m = ctx_.project().model();
    if (type == ChangeType::Reset) {
        markAll();
        return;
    }
    switch (kind) {
    case EntityKind::Geometry:
        markDirty(id, kind);
        for (const auto& d : m.developmentsUsingGeometry(id)) markDirty(d, EntityKind::Development);
        break;
    case EntityKind::Layer:
        markAllOf(EntityKind::Geometry);
        markAllOf(EntityKind::Development);
        break;
    case EntityKind::Development:
        markDirty(id, kind);
        if (const auto* d = m.developments.find(id)) markDirty(d->geometryId, EntityKind::Geometry);
        else markAllOf(EntityKind::Geometry); // owner of its centreline changed
        break;
    case EntityKind::Profile:
    case EntityKind::Material:
    case EntityKind::SupportClass:
    case EntityKind::Level:
    case EntityKind::Decline:
    case EntityKind::DesignLink:
        markAllOf(EntityKind::Development);
        break;
    case EntityKind::Activity:
        if (type == ChangeType::Removed) markAllOf(EntityKind::Development);
        else
            for (const auto& d : m.developmentsForActivity(id)) markDirty(d, EntityKind::Development);
        break;
    case EntityKind::Stope:
    case EntityKind::MiningBlock:
    case EntityKind::GeologicalObject:
    case EntityKind::AsBuilt:
        markAllOf(EntityKind::Geometry); // ownership of shapes changed
        break;
    case EntityKind::ControlPoint:
    case EntityKind::BlockModel:
    case EntityKind::Structure:
    case EntityKind::GeotechClassification:
    case EntityKind::PointCloud:
        markDirty(id, kind);
        break;
    case EntityKind::Drillhole: {
        const auto before = drillholeRange_;
        updateDrillholeRange();
        if (before != drillholeRange_) markAllOf(EntityKind::Drillhole);
        else markDirty(id, kind);
        break;
    }
    default: break;
    }
}

void SceneCache::markDirty(const Uuid& id, EntityKind kind) {
    if (id.isNil()) return;
    dirty_[id] = kind;
    scheduleFlush();
}

void SceneCache::markAllOf(EntityKind kind) {
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    auto add = [&](const auto& store) {
        for (const auto& e : store.all()) dirty_[e.id] = kind;
    };
    switch (kind) {
    case EntityKind::Geometry: add(m.geometry); break;
    case EntityKind::Development: add(m.developments); break;
    case EntityKind::Drillhole: add(m.drillholes); break;
    case EntityKind::BlockModel: add(m.blockModels); break;
    default: break;
    }
    // Entities of this kind that no longer exist.
    for (const auto& [id, e] : entries_)
        if (e.kind == kind && !dirty_.count(id)) dirty_[id] = kind;
    scheduleFlush();
}

void SceneCache::markAll() {
    allDirty_ = true;
    scheduleFlush();
}

void SceneCache::scheduleFlush() {
    if (flushPending_) return;
    flushPending_ = true;
    QTimer::singleShot(0, this, &SceneCache::flush);
}

void SceneCache::chooseOrigin(bool force) {
    if (!ctx_.hasProject()) return;
    const auto& m = ctx_.project().model();
    auto b = BoundingBox::empty();
    for (const auto& g : m.geometry.all()) {
        if (auto ob = geometry::objectBounds(g)) b.expand(*ob);
        else if (g.mesh)
            for (const auto& v : g.mesh->vertices) b.expand(v);
    }
    for (const auto& p : m.controlPoints.all()) b.expand(p.position);
    for (const auto& h : m.drillholes.all()) b.expand(h.collar);
    for (const auto& d : m.pointClouds.all())
        if (!d.storedBounds.isEmpty()) {
            b.expand(d.transform.apply(d.storedBounds.min));
            b.expand(d.transform.apply(d.storedBounds.max));
        }
    for (const auto& bm : m.blockModels.all()) b.expand(bm.origin);
    if (b.isEmpty()) {
        if (!originSet_) {
            origin_ = {};
            originSet_ = true;
        }
        return;
    }
    const Vec3 c = b.center();
    // Re-centre only when content is far from the current origin (keeps actors stable).
    if (originSet_ && !force && (c - origin_.origin()).length() < 20000.0) return;
    const auto old = origin_.origin();
    origin_ = coordinates::RenderOrigin::around(c);
    const bool had = originSet_;
    originSet_ = true;
    if (had && origin_.origin() != old) {
        allDirty_ = true;
        emit originChanged(origin_.origin() - old);
    }
}

void SceneCache::updateDrillholeRange() {
    drillholeRange_.reset();
    if (!ctx_.hasProject() || settings_.drillholeField.empty()) return;
    double lo = 1e300, hi = -1e300;
    for (const auto& h : ctx_.project().model().drillholes.all())
        for (const auto& iv : h.intervals)
            if (auto it = iv.values.find(settings_.drillholeField); it != iv.values.end() && std::isfinite(it->second)) {
                lo = std::min(lo, it->second);
                hi = std::max(hi, it->second);
            }
    if (lo <= hi) drillholeRange_ = std::make_pair(lo, hi);
}

std::vector<std::string> SceneCache::drillholeFields() const {
    std::set<std::string> names;
    if (ctx_.hasProject())
        for (const auto& h : ctx_.project().model().drillholes.all())
            for (const auto& iv : h.intervals)
                for (const auto& [k, v] : iv.values) names.insert(k);
    return {names.begin(), names.end()};
}

const geology::BlockModelData* SceneCache::blockData(const Uuid& id) {
    if (!ctx_.hasProject()) return nullptr;
    const auto* bm = ctx_.project().model().blockModels.find(id);
    if (!bm || bm->dataAsset.isNull()) return nullptr;
    auto& c = blockCache_[id];
    if (c.data && c.assetId == bm->dataAsset.id && c.revision == bm->meta.revision) return c.data.get();
    if (c.failed && c.assetId == bm->dataAsset.id) return nullptr;
    c.assetId = bm->dataAsset.id;
    c.revision = bm->meta.revision;
    try {
        c.data = std::make_shared<geology::BlockModelData>(geology::loadBlockModel(ctx_.project(), id));
        c.failed = false;
    } catch (const std::exception& e) {
        c.data.reset();
        c.failed = true;
        ctx_.report(tr("Block model '%1' values could not be read: %2").arg(qstr(bm->name), qstr(e.what())), MessageLevel::Warning);
    }
    return c.data.get();
}

void SceneCache::rebuild(const Uuid& id, EntityKind kind) {
    const auto& p = ctx_.project();
    const auto& m = p.model();
    scene::BuildContext bc{p, settings_, origin_, std::nullopt, nullptr};
    bc.drillholeRange = drillholeRange_;
    std::vector<SceneItem> items;
    bool exists = false;
    const bool hidden = hidden_.count(id) != 0;
    try {
        switch (kind) {
        case EntityKind::Geometry:
            if (const auto* g = m.geometry.find(id)) {
                exists = true;
                const auto* layer = m.layers.find(g->layerId);
                if (!hidden && (!layer || layer->visible)) {
                    // A hidden owner (e.g. a hidden development) hides its shape too.
                    const auto owner = scene::ownerOfGeometry(m, id);
                    if (!hidden_.count(owner.id)) items = scene::buildGeometry(bc, *g);
                }
                if (auto b = geometry::objectBounds(*g)) geometryBounds_[id] = *b;
                else if (g->mesh) {
                    auto bb = BoundingBox::empty();
                    for (const auto& v : g->mesh->vertices) bb.expand(v);
                    geometryBounds_[id] = bb;
                }
                if (!layer || !layer->visible || hidden) geometryBounds_.erase(id);
            }
            break;
        case EntityKind::Development:
            if (const auto* d = m.developments.find(id)) {
                exists = true;
                if (!hidden) items = scene::buildDevelopment(bc, *d);
            }
            break;
        case EntityKind::ControlPoint:
            if (const auto* c = m.controlPoints.find(id)) {
                exists = true;
                if (!hidden) items = scene::buildControlPoint(bc, *c);
            }
            break;
        case EntityKind::Drillhole:
            if (const auto* h = m.drillholes.find(id)) {
                exists = true;
                if (!hidden) items = scene::buildDrillhole(bc, *h);
            }
            break;
        case EntityKind::BlockModel:
            if (const auto* b = m.blockModels.find(id)) {
                exists = true;
                if (!hidden) {
                    bc.blockData = blockData(id);
                    items = scene::buildBlockModel(bc, *b);
                }
            }
            break;
        case EntityKind::Structure:
            if (const auto* s = m.structures.find(id)) {
                exists = true;
                if (!hidden) items = scene::buildStructure(bc, *s);
            }
            break;
        case EntityKind::GeotechClassification:
            if (const auto* g = m.geotechClassifications.find(id)) {
                exists = true;
                if (!hidden) items = scene::buildClassification(bc, *g);
            }
            break;
        case EntityKind::PointCloud:
            if (const auto* d = m.pointClouds.find(id)) {
                exists = true;
                if (!hidden && d->visible) items = scene::buildPointCloudBox(bc, *d);
            }
            break;
        default: break;
        }
    } catch (const std::exception& e) {
        diag::warn("app.scene", "Display of entity " + id.toString() + " failed: " + e.what());
    }
    if (!exists) {
        entries_.erase(id);
        geometryBounds_.erase(id);
        removed_.insert(id);
        return;
    }
    for (auto& it : items) it.revision = nextRevision_++;
    auto& e = entries_[id];
    e.kind = kind;
    e.items = std::move(items);
}

void SceneCache::flush() {
    flushPending_ = false;
    QSet<QString> changed;
    if (!ctx_.hasProject()) {
        const bool any = !entries_.empty();
        entries_.clear();
        dirty_.clear();
        geometryBounds_.clear();
        allDirty_ = false;
        if (any) emit sceneChanged({}, true);
        return;
    }
    chooseOrigin(false);
    if (allDirty_) {
        allDirty_ = false;
        dirty_.clear();
        removed_.clear();
        entries_.clear();
        geometryBounds_.clear();
        updateDrillholeRange();
        const auto& m = ctx_.project().model();
        for (const auto& g : m.geometry.all()) rebuild(g.id, EntityKind::Geometry);
        for (const auto& d : m.developments.all()) rebuild(d.id, EntityKind::Development);
        for (const auto& c : m.controlPoints.all()) rebuild(c.id, EntityKind::ControlPoint);
        for (const auto& h : m.drillholes.all()) rebuild(h.id, EntityKind::Drillhole);
        for (const auto& b : m.blockModels.all()) rebuild(b.id, EntityKind::BlockModel);
        for (const auto& s : m.structures.all()) rebuild(s.id, EntityKind::Structure);
        for (const auto& g : m.geotechClassifications.all()) rebuild(g.id, EntityKind::GeotechClassification);
        for (const auto& d : m.pointClouds.all()) rebuild(d.id, EntityKind::PointCloud);
        emit sceneChanged({}, true);
        return;
    }
    if (dirty_.empty() && removed_.empty()) return;
    auto dirty = std::move(dirty_);
    dirty_.clear();
    for (const auto& [id, kind] : dirty) {
        rebuild(id, kind);
        changed.insert(uuidText(id));
    }
    for (const auto& id : removed_) changed.insert(uuidText(id));
    removed_.clear();
    emit sceneChanged(changed, false);
}

std::vector<Uuid> SceneCache::entities() const {
    std::vector<Uuid> out;
    out.reserve(entries_.size());
    for (const auto& [id, e] : entries_) out.push_back(id);
    return out;
}

const std::vector<SceneItem>* SceneCache::items(const Uuid& id) const {
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second.items;
}

BoundingBox SceneCache::sceneBounds() const {
    auto b = BoundingBox::empty();
    for (const auto& [id, e] : entries_)
        for (const auto& it : e.items)
            if (!it.bounds.isEmpty()) b.expand(it.bounds);
    return b;
}

} // namespace openmine::app
