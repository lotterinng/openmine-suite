// SPDX-License-Identifier: Apache-2.0
#include "ProjectExplorer.h"

#include "../app/EntityInfo.h"
#include "../theme/Theme.h"
#include "../viewport/SceneCache.h"

#include "openmine/objects/EnumNames.h"
#include "openmine/persistence/EntityCodec.h"
#include "openmine/project/ProjectEditor.h"

#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>

namespace openmine::app {

namespace {
constexpr int kKindRole = Qt::UserRole;
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kGroupRole = Qt::UserRole + 2;
constexpr int kGroupKind = -1;

bool hasVisibility(EntityKind k) {
    switch (k) {
    case EntityKind::Layer:
    case EntityKind::PointCloud:
    case EntityKind::Geometry:
    case EntityKind::Development:
    case EntityKind::Stope:
    case EntityKind::ControlPoint:
    case EntityKind::Drillhole:
    case EntityKind::BlockModel:
    case EntityKind::Structure:
    case EntityKind::GeotechClassification: return true;
    default: return false;
    }
}
} // namespace

ProjectExplorer::ProjectExplorer(AppContext& ctx, SceneCache& scene, QWidget* parent) : QWidget(parent), ctx_(ctx), scene_(scene) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter (name, id, kind)…"));
    filter_->setClearButtonEnabled(true);
    lay->addWidget(filter_);
    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setUniformRowHeights(true);
    tree_->setExpandsOnDoubleClick(true);
    tree_->setIconSize(QSize(16, 16));
    lay->addWidget(tree_, 1);

    connect(filter_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &ProjectExplorer::syncSelectionToContext);
    connect(tree_, &QTreeWidget::itemChanged, this, &ProjectExplorer::onItemChanged);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &ProjectExplorer::contextMenu);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
        if (it->data(0, kKindRole).toInt() == kGroupKind) return;
        emit actionRequested("zoom", {SelectionItem{static_cast<EntityKind>(it->data(0, kKindRole).toInt()), uuidFrom(it->data(0, kIdRole).toString())}});
    });
    connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* it) {
        if (!building_) expanded_.insert(it->data(0, kGroupRole).toString() + it->data(0, kIdRole).toString());
    });
    connect(tree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* it) {
        if (!building_) expanded_.erase(it->data(0, kGroupRole).toString() + it->data(0, kIdRole).toString());
    });
    connect(&ctx_, &AppContext::modelChanged, this, &ProjectExplorer::scheduleRebuild);
    connect(&ctx_, &AppContext::projectReplaced, this, [this] {
        expanded_ = {"g:survey", "g:cad", "g:layers", "g:design", "g:schedule"};
        scheduleRebuild();
    });
    connect(&ctx_, &AppContext::selectionChanged, this, &ProjectExplorer::syncSelectionFromContext);
    connect(&scene_, &SceneCache::sceneChanged, this, [this](const QSet<QString>&, bool) {
        // Hidden state may have changed (display-only visibility).
        building_ = true;
        for (auto& [id, it] : items_) {
            const auto kind = static_cast<EntityKind>(it->data(0, kKindRole).toInt());
            if (!hasVisibility(kind) || kind == EntityKind::Layer || kind == EntityKind::PointCloud) continue;
            it->setCheckState(0, scene_.isHidden(id) ? Qt::Unchecked : Qt::Checked);
        }
        building_ = false;
    });
    expanded_ = {"g:survey", "g:cad", "g:layers", "g:design", "g:schedule"};
    rebuild();
}

void ProjectExplorer::scheduleRebuild() {
    if (pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        rebuild();
    });
}

QTreeWidgetItem* ProjectExplorer::group(QTreeWidgetItem* parent, const QString& key, const QString& title, const QString& icon) {
    auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
    it->setText(0, title);
    it->setData(0, kKindRole, kGroupKind);
    it->setData(0, kGroupRole, key);
    it->setData(0, Qt::UserRole + 3, title);
    it->setIcon(0, toolIcon(icon.isEmpty() ? QStringLiteral("folder") : icon));
    QFont f = it->font(0);
    f.setBold(parent == nullptr);
    it->setFont(0, f);
    it->setFlags(Qt::ItemIsEnabled);
    groups_.push_back(it);
    return it;
}

QTreeWidgetItem* ProjectExplorer::entity(QTreeWidgetItem* parent, EntityKind kind, const Uuid& id, const QString& text, const QString& icon) {
    auto* it = new QTreeWidgetItem(parent);
    it->setText(0, text.isEmpty() ? displayName(ctx_.project().model(), {kind, id}) : text);
    it->setData(0, kKindRole, static_cast<int>(kind));
    it->setData(0, kIdRole, uuidText(id));
    it->setIcon(0, toolIcon(icon.isEmpty() ? kindIcon(kind) : icon));
    it->setToolTip(0, QStringLiteral("%1\n%2").arg(kindLabel(kind), uuidText(id)));
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (hasVisibility(kind)) flags |= Qt::ItemIsUserCheckable;
    it->setFlags(flags);
    if (hasVisibility(kind)) {
        bool visible = !scene_.isHidden(id);
        const auto& m = ctx_.project().model();
        if (kind == EntityKind::Layer)
            if (const auto* l = m.layers.find(id)) visible = l->visible;
        if (kind == EntityKind::PointCloud)
            if (const auto* d = m.pointClouds.find(id)) visible = d->visible;
        it->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
    }
    items_[id] = it;
    return it;
}

void ProjectExplorer::finishGroups() {
    // Counts in group titles; drop empty sub-groups (top-level groups stay as orientation).
    for (auto it = groups_.rbegin(); it != groups_.rend(); ++it) {
        auto* g = *it;
        int n = 0;
        std::function<void(QTreeWidgetItem*)> count = [&](QTreeWidgetItem* x) {
            for (int i = 0; i < x->childCount(); ++i) {
                auto* c = x->child(i);
                if (c->data(0, kKindRole).toInt() != kGroupKind) ++n;
                count(c);
            }
        };
        count(g);
        const QString title = g->data(0, Qt::UserRole + 3).toString();
        g->setText(0, n ? QStringLiteral("%1 (%2)").arg(title).arg(n) : title);
        if (n == 0 && g->parent() && g->childCount() == 0) {
            delete g;
            *it = nullptr;
        }
    }
    std::erase(groups_, nullptr);
}

void ProjectExplorer::rebuild() {
    building_ = true;
    const int scroll = tree_->verticalScrollBar()->value();
    tree_->clear();
    items_.clear();
    groups_.clear();
    if (!ctx_.hasProject()) {
        building_ = false;
        return;
    }
    const auto& m = ctx_.project().model();

    // ---- Project ------------------------------------------------------------------------------
    auto* proj = group(nullptr, "g:project", tr("Project"), "info");
    entity(proj, EntityKind::ProjectInfo, m.info.id, tr("Settings — %1").arg(qstr(m.info.name)), "settings");
    {
        auto* g = group(proj, "g:crs", tr("Coordinate systems"), "crs");
        for (const auto& c : m.crs.all()) {
            auto* it = entity(g, EntityKind::Crs, c.id);
            if (c.id == m.info.projectCrsId) it->setText(0, it->text(0) + tr("  [project CRS]"));
        }
        auto* lg = group(proj, "g:localgrids", tr("Local grids"), "grid");
        for (const auto& c : m.localGrids.all()) entity(lg, EntityKind::LocalGrid, c.id);
        auto* tr2 = group(proj, "g:transformations", tr("Transformation records"), "crs");
        for (const auto& c : m.transformations.all()) entity(tr2, EntityKind::Transformation, c.id);
        auto* mat = group(proj, "g:materials", tr("Materials"), "entity");
        for (const auto& c : m.materials.all()) entity(mat, EntityKind::Material, c.id);
        auto* ad = group(proj, "g:attrdefs", tr("Attribute definitions"), "properties");
        for (const auto& c : m.attributeDefinitions.all()) entity(ad, EntityKind::AttributeDefinition, c.id);
    }

    // ---- Survey ------------------------------------------------------------------------------
    auto* survey = group(nullptr, "g:survey", tr("Survey"), "control");
    {
        auto* cp = group(survey, "g:control", tr("Control points"), "control");
        for (const auto& c : m.controlPoints.all())
            entity(cp, EntityKind::ControlPoint, c.id, QStringLiteral("%1  (%2)").arg(qstr(c.name), qstr(toString(c.role))));
        auto* pc = group(survey, "g:clouds", tr("Point clouds"), "cloud");
        // Derived datasets under their parent.
        std::map<Uuid, QTreeWidgetItem*> cloudItems;
        std::vector<const PointCloudDataset*> pending;
        for (const auto& d : m.pointClouds.all()) pending.push_back(&d);
        for (int pass = 0; pass < 8 && !pending.empty(); ++pass) {
            std::vector<const PointCloudDataset*> next;
            for (const auto* d : pending) {
                QTreeWidgetItem* parent = pc;
                if (!d->parentDatasetId.isNil() && m.pointClouds.contains(d->parentDatasetId)) {
                    auto it = cloudItems.find(d->parentDatasetId);
                    if (it == cloudItems.end()) {
                        next.push_back(d);
                        continue;
                    }
                    parent = it->second;
                }
                cloudItems[d->id] = entity(parent, EntityKind::PointCloud, d->id,
                                           QStringLiteral("%1  (%L2 pts)").arg(qstr(d->name)).arg(d->pointCount));
            }
            pending = std::move(next);
        }
        for (const auto* d : pending) entity(pc, EntityKind::PointCloud, d->id);
        auto* rg = group(survey, "g:registrations", tr("Registrations"), "register");
        for (const auto& r : m.registrations.all()) entity(rg, EntityKind::Registration, r.id);
        auto* ab = group(survey, "g:asbuilt", tr("As-built models"), "mesh");
        for (const auto& a : m.asBuiltModels.all()) entity(ab, EntityKind::AsBuilt, a.id);
        auto* cf = group(survey, "g:conformance", tr("Conformance results"), "conformance");
        for (const auto& c : m.conformanceResults.all())
            entity(cf, EntityKind::Conformance, c.id,
                   QStringLiteral("%1 — overbreak %2 m³, underbreak %3 m³")
                       .arg(displayName(m, {EntityKind::Conformance, c.id}))
                       .arg(c.overbreakVolume, 0, 'f', 1)
                       .arg(c.underbreakVolume, 0, 'f', 1));
    }

    // ---- CAD ---------------------------------------------------------------------------------
    auto* cad = group(nullptr, "g:cad", tr("CAD"), "polyline");
    {
        auto* layers = group(cad, "g:layers", tr("Layers"), "layer");
        std::map<Uuid, QTreeWidgetItem*> layerItems;
        const Uuid current = ctx_.currentLayer();
        for (const auto& l : m.layers.all()) {
            auto* it = entity(layers, EntityKind::Layer, l.id, qstr(l.name) + (l.locked ? tr("  [locked]") : QString()) + (l.id == current ? tr("  [current]") : QString()));
            QPixmap sw(12, 12);
            sw.fill(QColor(static_cast<QRgb>(l.colorRgb)));
            it->setIcon(0, QIcon(sw));
            it->setData(0, kGroupRole, QStringLiteral("layer"));
            layerItems[l.id] = it;
        }
        for (const auto& g : m.geometry.all()) {
            auto lit = layerItems.find(g.layerId);
            QTreeWidgetItem* parent = lit == layerItems.end() ? layers : lit->second;
            QString icon = "polyline";
            switch (g.type) {
            case GeometryType::Point: icon = "point"; break;
            case GeometryType::Line: icon = "line"; break;
            case GeometryType::Arc: icon = "arc"; break;
            case GeometryType::Circle: icon = "circle"; break;
            case GeometryType::Spline: icon = "spline"; break;
            case GeometryType::Text: icon = "text"; break;
            case GeometryType::Dimension: icon = "dimension"; break;
            case GeometryType::Hatch: icon = "hatch"; break;
            case GeometryType::Mesh: icon = "mesh"; break;
            case GeometryType::Solid: icon = "solid"; break;
            default: break;
            }
            entity(parent, EntityKind::Geometry, g.id, {}, icon);
        }
    }

    // ---- Mine design ---------------------------------------------------------------------------
    auto* design = group(nullptr, "g:design", tr("Mine design"), "heading");
    {
        auto* lv = group(design, "g:levels", tr("Levels and developments"), "level");
        std::map<Uuid, QTreeWidgetItem*> levelItems;
        for (const auto& l : m.levels.all())
            levelItems[l.id] = entity(lv, EntityKind::Level, l.id, QStringLiteral("%1  (RL %2)").arg(qstr(l.name)).arg(l.elevation, 0, 'f', 1));
        QTreeWidgetItem* noLevel = nullptr;
        for (const auto& d : m.developments.all()) {
            auto it = levelItems.find(d.levelId);
            QTreeWidgetItem* parent;
            if (it != levelItems.end()) parent = it->second;
            else {
                if (!noLevel) noLevel = group(lv, "g:nolevel", tr("Not assigned to a level"), "heading");
                parent = noLevel;
            }
            auto* item = entity(parent, EntityKind::Development, d.id);
            bool flagged = false;
            for (const auto& a : m.activitiesForDevelopment(d.id))
                if (const auto* ac = m.activities.find(a)) flagged = flagged || ac->designImpactFlag;
            if (flagged) {
                item->setForeground(0, Theme::colors().warning);
                item->setToolTip(0, item->toolTip(0) + tr("\nDesign changed: linked schedule quantities are flagged"));
            }
        }
        auto* dc = group(design, "g:declines", tr("Declines"), "decline");
        for (const auto& d : m.declines.all()) entity(dc, EntityKind::Decline, d.id);
        auto* st = group(design, "g:stopes", tr("Stopes"), "stope");
        std::map<Uuid, QTreeWidgetItem*> stopeItems;
        for (const auto& s : m.stopes.all()) stopeItems[s.id] = entity(st, EntityKind::Stope, s.id);
        for (const auto& b : m.miningBlocks.all()) {
            auto it = stopeItems.find(b.stopeId);
            entity(it == stopeItems.end() ? st : it->second, EntityKind::MiningBlock, b.id);
        }
        auto* pr = group(design, "g:profiles", tr("Tunnel profiles"), "heading");
        for (const auto& p : m.profiles.all()) entity(pr, EntityKind::Profile, p.id);
        auto* sc = group(design, "g:support", tr("Ground support classes"), "entity");
        for (const auto& s : m.supportClasses.all()) entity(sc, EntityKind::SupportClass, s.id);
        auto* rv = group(design, "g:revisions", tr("Design revisions"), "revision");
        for (const auto& r : m.designRevisions.all()) {
            const auto* d = m.developments.find(r.entityId);
            const auto* s = m.stopes.find(r.entityId);
            const QString owner = d ? qstr(d->headingId) : s ? qstr(s->name) : QStringLiteral("?");
            entity(rv, EntityKind::DesignRevision, r.id, owner + " — " + displayName(m, {EntityKind::DesignRevision, r.id}));
        }
    }

    // ---- Geology / geotechnical -----------------------------------------------------------------
    auto* geo = group(nullptr, "g:geology", tr("Geology"), "drillhole");
    {
        auto* dh = group(geo, "g:drillholes", tr("Drillholes"), "drillhole");
        for (const auto& h : m.drillholes.all())
            entity(dh, EntityKind::Drillhole, h.id, QStringLiteral("%1  (%2 m, %3 intervals)").arg(qstr(h.holeId)).arg(h.length, 0, 'f', 1).arg(h.intervals.size()));
        auto* bm = group(geo, "g:blockmodels", tr("Block models"), "blocks");
        for (const auto& b : m.blockModels.all())
            entity(bm, EntityKind::BlockModel, b.id, QStringLiteral("%1  (%2×%3×%4)").arg(qstr(b.name)).arg(b.nx).arg(b.ny).arg(b.nz));
        auto* go = group(geo, "g:geoobjects", tr("Geological objects"), "solid");
        for (const auto& g : m.geologicalObjects.all()) entity(go, EntityKind::GeologicalObject, g.id);
    }
    auto* gt = group(nullptr, "g:geotech", tr("Geotechnical"), "geotech");
    {
        auto* sg = group(gt, "g:structures", tr("Structures"), "structure");
        std::map<std::string, QTreeWidgetItem*> sets;
        for (const auto& s : m.structures.all()) {
            const std::string key = s.setName.empty() ? std::string("(no set)") : s.setName;
            auto& g = sets[key];
            if (!g) g = group(sg, "g:set:" + qstr(key), qstr(key), "structure");
            entity(g, EntityKind::Structure, s.id);
        }
        auto* cl = group(gt, "g:classifications", tr("Rock-mass classifications"), "geotech");
        for (const auto& c : m.geotechClassifications.all()) entity(cl, EntityKind::GeotechClassification, c.id);
    }

    // ---- Schedule ------------------------------------------------------------------------------
    auto* sch = group(nullptr, "g:schedule", tr("Schedule"), "gantt");
    {
        auto* acts = group(sch, "g:activities", tr("Activities"), "task");
        std::map<Uuid, QTreeWidgetItem*> actItems;
        std::vector<const ScheduleActivity*> ordered;
        for (const auto& a : m.activities.all()) ordered.push_back(&a);
        std::stable_sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->sortOrder < b->sortOrder; });
        std::vector<const ScheduleActivity*> pendingA = ordered;
        for (int pass = 0; pass < 32 && !pendingA.empty(); ++pass) {
            std::vector<const ScheduleActivity*> next;
            for (const auto* a : pendingA) {
                QTreeWidgetItem* parent = acts;
                if (!a->parentId.isNil() && m.activities.contains(a->parentId)) {
                    auto it = actItems.find(a->parentId);
                    if (it == actItems.end()) {
                        next.push_back(a);
                        continue;
                    }
                    parent = it->second;
                }
                const QString icon = a->kind == ActivityKind::Milestone ? "milestone" : a->kind == ActivityKind::Summary ? "summary" : "task";
                auto* it = entity(parent, EntityKind::Activity, a->id, {}, icon);
                if (a->designImpactFlag) it->setForeground(0, Theme::colors().warning);
                actItems[a->id] = it;
            }
            pendingA = std::move(next);
        }
        auto* deps = group(sch, "g:dependencies", tr("Dependencies"), "dependency");
        for (const auto& d : m.dependencies.all()) entity(deps, EntityKind::Dependency, d.id);
        auto* res = group(sch, "g:resources", tr("Resources"), "resource");
        std::map<Uuid, QTreeWidgetItem*> resItems;
        for (const auto& r : m.resources.all()) resItems[r.id] = entity(res, EntityKind::Resource, r.id);
        for (const auto& a : m.assignments.all()) {
            auto it = resItems.find(a.resourceId);
            entity(it == resItems.end() ? res : it->second, EntityKind::Assignment, a.id);
        }
        auto* cal = group(sch, "g:calendars", tr("Calendars"), "calendar");
        for (const auto& c : m.calendars.all()) {
            auto* it = entity(cal, EntityKind::Calendar, c.id);
            if (c.id == m.info.defaultCalendarId) it->setText(0, it->text(0) + tr("  [default]"));
        }
        auto* cyc = group(sch, "g:cycles", tr("Development cycles"), "calc");
        for (const auto& c : m.cycles.all()) entity(cyc, EntityKind::Cycle, c.id);
        auto* act = group(sch, "g:actuals", tr("Production actuals"), "actual");
        for (const auto& a : m.actuals.all()) {
            const auto* ac = m.activities.find(a.activityId);
            entity(act, EntityKind::Actual, a.id, QStringLiteral("%1 — %2").arg(ac ? qstr(ac->name) : QStringLiteral("?"), displayName(m, {EntityKind::Actual, a.id})));
        }
        auto* tg = group(sch, "g:targets", tr("Production targets"), "chart");
        for (const auto& t : m.targets.all()) entity(tg, EntityKind::Target, t.id);
        auto* bl = group(sch, "g:baselines", tr("Baselines"), "baseline");
        for (const auto& b : m.baselines.all())
            entity(bl, EntityKind::Baseline, b.id, QStringLiteral("%1  (%2, %3 activities)").arg(qstr(b.name), qstr(b.capturedAt.substr(0, 10))).arg(b.activities.size()));
        auto* sc = group(sch, "g:scenarios", tr("Scenarios"), "scenario");
        for (const auto& s : m.scenarios.all()) {
            auto* it = entity(sc, EntityKind::Scenario, s.id);
            if (ctx_.activeScenario() && ctx_.activeScenario()->id == s.id) {
                it->setText(0, it->text(0) + tr("  [active]"));
                QFont f = it->font(0);
                f.setBold(true);
                it->setFont(0, f);
            }
        }
    }

    // ---- Reports / records ------------------------------------------------------------------------
    auto* rep = group(nullptr, "g:reports", tr("Reports and records"), "report");
    {
        auto* r = group(rep, "g:reportlist", tr("Reports"), "report");
        for (const auto& x : m.reports.all()) entity(r, EntityKind::Report, x.id, {}, x.format == "pdf" ? "pdf" : "report");
        auto* a = group(rep, "g:analysis", tr("Analysis results"), "chart");
        for (const auto& x : m.analysisResults.all()) entity(a, EntityKind::AnalysisResult, x.id);
        auto* at = group(rep, "g:attachments", tr("Attachments"), "entity");
        for (const auto& x : m.attachments.all()) entity(at, EntityKind::Attachment, x.id);
        auto* as = group(rep, "g:assets", tr("Assets"), "folder");
        for (const auto& x : m.assets.all())
            entity(as, EntityKind::Asset, x.id, QStringLiteral("%1  (%L2 bytes)").arg(qstr(x.relativePath)).arg(x.sizeBytes));
    }
    finishGroups();

    // Restore expansion.
    std::function<void(QTreeWidgetItem*)> restore = [&](QTreeWidgetItem* it) {
        const QString key = it->data(0, kGroupRole).toString() + it->data(0, kIdRole).toString();
        if (expanded_.count(key)) it->setExpanded(true);
        for (int i = 0; i < it->childCount(); ++i) restore(it->child(i));
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) restore(tree_->topLevelItem(i));
    building_ = false;
    applyFilter();
    syncSelectionFromContext();
    tree_->verticalScrollBar()->setValue(scroll);
}

QTreeWidgetItem* ProjectExplorer::itemFor(const Uuid& id) const {
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : it->second;
}

int ProjectExplorer::entityItemCount() const { return static_cast<int>(items_.size()); }

void ProjectExplorer::setFilter(const QString& text) { filter_->setText(text); }

void ProjectExplorer::applyFilter() {
    const QString f = filter_->text().trimmed();
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* it) {
        bool any = false;
        for (int i = 0; i < it->childCount(); ++i) any = visit(it->child(i)) || any;
        const bool isGroup = it->data(0, kKindRole).toInt() == kGroupKind;
        const bool self = !f.isEmpty() && !isGroup &&
                          (it->text(0).contains(f, Qt::CaseInsensitive) || it->data(0, kIdRole).toString().startsWith(f, Qt::CaseInsensitive) ||
                           kindLabel(static_cast<EntityKind>(it->data(0, kKindRole).toInt())).contains(f, Qt::CaseInsensitive));
        const bool show = f.isEmpty() || self || any;
        it->setHidden(!show);
        if (!f.isEmpty() && any) it->setExpanded(true);
        return self || any;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) visit(tree_->topLevelItem(i));
}

void ProjectExplorer::syncSelectionFromContext() {
    if (syncing_) return;
    syncing_ = true;
    tree_->clearSelection();
    QTreeWidgetItem* last = nullptr;
    for (const auto& s : ctx_.selection())
        if (auto* it = itemFor(s.id)) {
            it->setSelected(true);
            for (auto* p = it->parent(); p; p = p->parent()) p->setExpanded(true);
            last = it;
        }
    if (last) tree_->scrollToItem(last);
    syncing_ = false;
}

void ProjectExplorer::syncSelectionToContext() {
    if (syncing_ || building_) return;
    syncing_ = true;
    std::vector<SelectionItem> sel;
    for (auto* it : tree_->selectedItems()) {
        const int k = it->data(0, kKindRole).toInt();
        if (k == kGroupKind) continue;
        sel.push_back({static_cast<EntityKind>(k), uuidFrom(it->data(0, kIdRole).toString())});
    }
    // Keep the current item last (it becomes the primary selection).
    if (auto* cur = tree_->currentItem(); cur && cur->isSelected() && cur->data(0, kKindRole).toInt() != kGroupKind) {
        SelectionItem c{static_cast<EntityKind>(cur->data(0, kKindRole).toInt()), uuidFrom(cur->data(0, kIdRole).toString())};
        std::erase(sel, c);
        sel.push_back(c);
    }
    ctx_.selectMany(sel, SelectMode::Replace);
    syncing_ = false;
}

void ProjectExplorer::onItemChanged(QTreeWidgetItem* item, int) {
    if (building_ || !ctx_.hasProject()) return;
    const int k = item->data(0, kKindRole).toInt();
    if (k == kGroupKind) return;
    const auto kind = static_cast<EntityKind>(k);
    if (!hasVisibility(kind)) return;
    const Uuid id = uuidFrom(item->data(0, kIdRole).toString());
    const bool visible = item->checkState(0) == Qt::Checked;
    auto& p = ctx_.project();
    if (kind == EntityKind::Layer) {
        if (const auto* l = p.model().layers.find(id); l && l->visible != visible) {
            CadLayer n = *l;
            n.visible = visible;
            ctx_.tryEdit(visible ? tr("Show layer") : tr("Hide layer"), [&] { ProjectEditor(p).updateLayer(n); });
        }
    } else if (kind == EntityKind::PointCloud) {
        if (const auto* d = p.model().pointClouds.find(id); d && d->visible != visible) {
            PointCloudDataset n = *d;
            n.visible = visible;
            ctx_.tryEdit(visible ? tr("Show point cloud") : tr("Hide point cloud"), [&] { ProjectEditor(p).update(n, visible ? "Show point cloud" : "Hide point cloud"); });
        }
    } else {
        scene_.setHidden(id, !visible);
    }
}

void ProjectExplorer::deleteSelected() {
    std::vector<SelectionItem> sel = ctx_.selection();
    std::erase_if(sel, [](const SelectionItem& s) { return s.kind == EntityKind::ProjectInfo; });
    if (sel.empty()) return;
    const auto& m = ctx_.project().model();
    const QString what = sel.size() == 1 ? describe(m, sel.front()) : tr("%1 items").arg(sel.size());
    if (!property("nonInteractive").toBool() &&
        QMessageBox::question(this, tr("Delete"), tr("Delete %1? Dependent items are removed or the deletion is refused, as the model's rules require. This can be undone.").arg(what)) !=
            QMessageBox::Yes)
        return;
    ctx_.tryEdit(tr("Delete %1").arg(what), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), sstr(tr("Delete %1").arg(what)));
        for (const auto& s : sel)
            if (findKind(p.model(), s.id)) persistence::removeDocument(p, documentType(s.kind), s.id.toString());
        macro.commit();
    });
}

void ProjectExplorer::renameItem(const SelectionItem& s) {
    const auto& m = ctx_.project().model();
    const std::string type = documentType(s.kind);
    auto doc = persistence::encodeDocument(m, type, s.id.toString());
    if (!doc) return;
    auto j = nlohmann::json::parse(*doc);
    auto& cols = j["columns"];
    std::string col = cols.contains("name") ? "name" : cols.contains("hole_id") ? "hole_id" : cols.contains("heading_id") ? "heading_id" : "";
    if (col.empty()) {
        ctx_.report(tr("%1 has no name to edit").arg(kindLabel(s.kind)), MessageLevel::Warning);
        return;
    }
    bool ok = false;
    const QString old = cols[col].is_string() ? qstr(cols[col].get<std::string>()) : QString();
    const QString n = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal, old, &ok);
    if (!ok || n == old) return;
    cols[col] = sstr(n);
    ctx_.tryEdit(tr("Rename"), [&] { persistence::putDocument(ctx_.project(), type, j.dump(), "Rename"); });
}

void ProjectExplorer::contextMenu(const QPoint& pos) {
    auto* item = tree_->itemAt(pos);
    if (!ctx_.hasProject()) return;
    QMenu menu(this);
    const auto sel = ctx_.selection();
    const int k = item ? item->data(0, kKindRole).toInt() : kGroupKind;
    if (item && k == kGroupKind) {
        const QString key = item->data(0, kGroupRole).toString();
        menu.addAction(tr("Expand all"), [item] { item->setExpanded(true); for (int i = 0; i < item->childCount(); ++i) item->child(i)->setExpanded(true); });
        menu.addAction(tr("Collapse"), [item] { item->setExpanded(false); });
        auto add = [&](const QString& text, const QString& action) { menu.addAction(text, [this, action] { emit actionRequested(action, {}); }); };
        if (key == "g:layers") add(tr("New layer…"), "new-layer");
        if (key == "g:clouds") add(tr("Import point cloud…"), "import-cloud");
        if (key == "g:control") add(tr("Import control points…"), "import-control");
        if (key == "g:drillholes") add(tr("Import drillholes…"), "import-drillholes");
        if (key == "g:activities") add(tr("New task"), "new-task");
        if (key == "g:scenarios") add(tr("New scenario…"), "new-scenario");
        if (key == "g:levels") add(tr("New level…"), "new-level");
        if (key == "g:resources") add(tr("Resources and assignments…"), "resources");
        if (key == "g:calendars") add(tr("Working calendars…"), "calendars");
        if (key == "g:cycles") add(tr("Development cycles…"), "cycles");
        if (key == "g:targets") add(tr("Production targets…"), "targets");
        if (key == "g:structures") add(tr("Import structures…"), "import-structures");
        menu.exec(tree_->viewport()->mapToGlobal(pos));
        return;
    }
    if (sel.empty()) return;
    const auto primary = ctx_.primary();
    auto act = [&](const QString& text, const QString& action, const QString& icon = {}) {
        auto* a = menu.addAction(icon.isEmpty() ? QIcon() : toolIcon(icon), text, [this, action, sel] { emit actionRequested(action, sel); });
        return a;
    };
    act(tr("Zoom to"), "zoom", "fit");
    act(tr("Properties"), "properties", "properties");
    if (hasVisibility(primary.kind)) {
        menu.addAction(toolIcon("eye"), tr("Show only these"), [this, sel] {
            for (const auto& id : scene_.entities()) scene_.setHidden(id, true);
            for (const auto& s : sel) scene_.setHidden(s.id, false);
        });
        menu.addAction(tr("Show all"), [this] { scene_.showAll(); });
    }
    menu.addSeparator();
    switch (primary.kind) {
    case EntityKind::Layer:
        menu.addAction(tr("Set as current layer"), [this, primary] { ctx_.setCurrentLayer(primary.id); scheduleRebuild(); });
        act(tr("Select objects on layer"), "select-layer-objects");
        break;
    case EntityKind::PointCloud:
        act(tr("Point-cloud report"), "report-cloud", "report");
        act(tr("Export…"), "export-cloud", "export");
        act(tr("Statistics"), "cloud-stats", "chart");
        act(tr("Subsample…"), "cloud-subsample", "filter");
        act(tr("Remove outliers…"), "cloud-outliers", "filter");
        act(tr("Estimate normals…"), "cloud-normals", "normals");
        act(tr("Classify ground (SMRF)…"), "cloud-ground", "ground");
        act(tr("Crop by polygon"), "cloud-crop", "polygon");
        act(tr("Crop by box…"), "cloud-crop-box", "crop");
        act(tr("Segment into clusters…"), "cloud-segment", "cluster");
        act(tr("Extract planes…"), "cloud-planes", "structure");
        act(tr("Surface (Delaunay 2.5D)…"), "cloud-delaunay", "surface");
        act(tr("Surface (Poisson)…"), "cloud-poisson", "mesh");
        act(tr("Register (point pairs / control)…"), "register-pairs", "register");
        act(tr("Register (ICP)…"), "register-icp", "icp");
        act(tr("Distance to…"), "cloud-distance", "distance");
        break;
    case EntityKind::Registration:
        act(tr("Registration report"), "report-registration", "report");
        act(tr("Revert registration"), "revert-registration", "undo");
        break;
    case EntityKind::AsBuilt:
        act(tr("Run conformance…"), "conformance", "conformance");
        break;
    case EntityKind::Conformance:
        act(tr("Conformance report"), "report-conformance", "report");
        act(tr("Export station table (CSV)…"), "export-conformance", "export");
        break;
    case EntityKind::Development:
        act(tr("Create linked activity…"), "linked-activity", "link");
        act(tr("Section at chainage…"), "section-chainage", "section");
        act(tr("Crosscuts…"), "crosscuts", "crosscut");
        act(tr("Capture revision…"), "capture-revision", "revision");
        act(tr("Apply design quantities"), "apply-quantities", "calc");
#ifdef OPENMINE_APP_HAVE_OCCT
        act(tr("Create excavation solid"), "development-solid", "solid");
#endif
        break;
    case EntityKind::Stope:
        act(tr("Subdivide into blocks…"), "subdivide-stope", "stope");
        act(tr("Capture revision…"), "capture-revision", "revision");
        break;
    case EntityKind::DesignRevision:
        act(tr("Compare with current"), "compare-revision", "revision");
        act(tr("Approve"), "approve-revision", "baseline");
        act(tr("Restore"), "restore-revision", "undo");
        break;
    case EntityKind::BlockModel:
        act(tr("Display settings…"), "block-display", "blocks");
        act(tr("Grade–tonnage report…"), "grade-tonnage", "chart");
        act(tr("Export CSV…"), "export-blocks", "export");
        act(tr("Estimate grades…"), "estimate", "estimate");
        break;
    case EntityKind::Drillhole:
        act(tr("Composite…"), "composite", "drillhole");
        break;
    case EntityKind::Geometry:
        act(tr("Create heading from centreline…"), "heading-from-geometry", "heading");
        act(tr("Create stope from shape…"), "stope-from-geometry", "stope");
        act(tr("Volume to elevation…"), "surface-volume", "volume");
#ifdef OPENMINE_APP_HAVE_OCCT
        act(tr("Convert mesh to solid"), "mesh-to-solid", "solid");
        if (sel.size() == 2) {
            act(tr("Union"), "union", "boolean");
            act(tr("Subtract"), "subtract", "boolean");
            act(tr("Intersect"), "intersect", "boolean");
        }
#endif
        act(tr("Join"), "join", "join");
        act(tr("Explode"), "explode", "explode");
        act(tr("Check solid / surface"), "solid-check", "info");
        act(tr("Section by plane"), "section-objects", "section");
#ifdef OPENMINE_APP_HAVE_OCCT
        act(tr("Heal solid"), "solid-heal", "repair");
        act(tr("Fillet solid edges…"), "solid-fillet", "fillet");
        act(tr("Offset solid…"), "solid-offset", "offset");
#endif
        break;
    case EntityKind::Calendar:
        act(tr("Edit calendar…"), "edit-calendar", "calendar");
        break;
    case EntityKind::Resource:
        act(tr("Resources and assignments…"), "resources", "resource");
        break;
    case EntityKind::Cycle:
        act(tr("Development cycles…"), "cycles", "calc");
        break;
    case EntityKind::Target:
        act(tr("Production targets…"), "targets", "chart");
        break;
    case EntityKind::Report:
        act(tr("Open"), "open-report", "report");
        act(tr("Save as…"), "save-report", "save");
        break;
    case EntityKind::Scenario:
        menu.addAction(toolIcon("scenario"), tr("Make active"), [this, primary] { ctx_.setActiveScenario(primary.id); });
        act(tr("Optimise sequence…"), "optimise", "optimise");
        act(tr("Compare with base"), "compare-scenario", "chart");
        break;
    case EntityKind::Baseline:
        menu.addAction(toolIcon("baseline"), tr("Compare against this baseline"), [this, primary] { ctx_.setComparisonBaseline(primary.id); });
        break;
    case EntityKind::Activity:
        act(tr("Add dependency…"), "add-dependency", "dependency");
        act(tr("Record actual…"), "record-actual", "actual");
        act(tr("Apply design quantities"), "apply-quantities", "calc");
        break;
    case EntityKind::Structure:
    case EntityKind::GeotechClassification:
        act(tr("Show stereonet"), "stereonet", "stereonet");
        break;
    default: break;
    }
    menu.addSeparator();
    menu.addAction(tr("Rename…"), [this, primary] { renameItem(primary); })->setEnabled(sel.size() == 1);
    menu.addAction(toolIcon("delete"), tr("Delete"), [this] { deleteSelected(); })->setEnabled(primary.kind != EntityKind::ProjectInfo);
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

} // namespace openmine::app
