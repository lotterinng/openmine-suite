// SPDX-License-Identifier: Apache-2.0
#include "ProjectExplorer.h"

#include "../theme/Theme.h"
#include "openmine/project/ProjectEditor.h"

#include <QHeaderView>
#include <QTimer>

#include <functional>

namespace openmine::app {

namespace {
constexpr int RoleId = Qt::UserRole + 1;
constexpr int RoleKind = Qt::UserRole + 2;
} // namespace

ProjectExplorer::ProjectExplorer(AppContext& ctx, QWidget* parent) : QTreeWidget(parent), ctx_(ctx) {
    setHeaderHidden(true);
    setUniformRowHeights(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    connect(&ctx_, &AppContext::projectReplaced, this, &ProjectExplorer::rebuild);
    connect(&ctx_, &AppContext::modelChanged, this, &ProjectExplorer::scheduleRebuild);
    connect(&ctx_, &AppContext::selectionChanged, this, &ProjectExplorer::syncSelection);
    connect(this, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (rebuilding_) return;
        auto items = selectedItems();
        if (items.isEmpty()) return;
        auto id = Uuid::parse(items.front()->data(0, RoleId).toString().toStdString());
        if (!id) return;
        ctx_.select(static_cast<EntityKind>(items.front()->data(0, RoleKind).toInt()), *id);
    });
    connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (rebuilding_ || item->data(0, RoleKind).toInt() != static_cast<int>(EntityKind::Layer)) return;
        auto id = Uuid::parse(item->data(0, RoleId).toString().toStdString());
        const auto* layer = id ? ctx_.project().model().layers.find(*id) : nullptr;
        if (!layer) return;
        const bool visible = item->checkState(0) == Qt::Checked;
        if (layer->visible == visible) return;
        CadLayer l = *layer;
        l.visible = visible;
        try {
            ProjectEditor(ctx_.project()).updateLayer(l);
        } catch (const std::exception& e) {
            ctx_.report(e.what(), true);
        }
    });
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

QTreeWidgetItem* ProjectExplorer::findItem(const Uuid& id) const {
    const QString key = qstr(id.toString());
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) -> QTreeWidgetItem* {
        if (it->data(0, RoleId).toString() == key) return it;
        for (int i = 0; i < it->childCount(); ++i)
            if (auto* f = walk(it->child(i))) return f;
        return nullptr;
    };
    for (int i = 0; i < topLevelItemCount(); ++i)
        if (auto* f = walk(topLevelItem(i))) return f;
    return nullptr;
}

void ProjectExplorer::rebuild() {
    rebuilding_ = true;
    // Remember which folders were expanded so the tree doesn't jump around.
    QSet<QString> collapsed;
    std::function<void(QTreeWidgetItem*)> remember = [&](QTreeWidgetItem* it) {
        if (it->childCount() && !it->isExpanded()) collapsed.insert(it->text(0).section(" (", 0, 0));
        for (int i = 0; i < it->childCount(); ++i) remember(it->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i) remember(topLevelItem(i));
    clear();
    if (!ctx_.hasProject()) {
        rebuilding_ = false;
        return;
    }
    const auto& m = ctx_.project().model();
    const auto& c = Theme::colors();

    auto* root = new QTreeWidgetItem(this, {qstr(m.info.name)});
    root->setData(0, RoleId, qstr(m.info.id.toString()));
    root->setData(0, RoleKind, static_cast<int>(EntityKind::ProjectInfo));
    QFont bold = root->font(0);
    bold.setBold(true);
    root->setFont(0, bold);

    auto folder = [&](const QString& name, int count, bool available = true) {
        auto* f = new QTreeWidgetItem(root, {available ? QStringLiteral("%1 (%2)").arg(name).arg(count) : name});
        f->setFlags(Qt::ItemIsEnabled);
        if (!available) {
            f->setForeground(0, c.textMuted);
            f->setToolTip(0, "Not available in this build — see ROADMAP.md");
        }
        return f;
    };
    auto leaf = [&](QTreeWidgetItem* parent, const QString& text, EntityKind kind, const Uuid& id) {
        auto* it = new QTreeWidgetItem(parent, {text});
        it->setData(0, RoleId, qstr(id.toString()));
        it->setData(0, RoleKind, static_cast<int>(kind));
        return it;
    };

    {
        auto* crs = folder("Coordinate Systems", 1);
        auto* it = new QTreeWidgetItem(crs, {qstr(m.info.crs.name) +
                                             (m.info.crs.authorityCode.empty() ? QString() : " [" + qstr(m.info.crs.authorityCode) + "]")});
        it->setFlags(Qt::ItemIsEnabled);
    }
    folder("Survey Control", 0, false);
    folder("Survey Scans", 0, false);
    folder("Point Clouds", 0, false);
    folder("Meshes", 0, false);

    auto* layers = folder("CAD Layers", static_cast<int>(m.layers.size()));
    for (const auto& l : m.layers.all()) {
        auto* li = leaf(layers, qstr(l.name), EntityKind::Layer, l.id);
        li->setFlags(li->flags() | Qt::ItemIsUserCheckable);
        li->setCheckState(0, l.visible ? Qt::Checked : Qt::Unchecked);
        QPixmap sw(12, 12);
        sw.fill(QColor((l.colorRgb >> 16) & 0xFF, (l.colorRgb >> 8) & 0xFF, l.colorRgb & 0xFF));
        li->setIcon(0, QIcon(sw));
        for (const auto& g : m.geometry.all())
            if (g.layerId == l.id) leaf(li, qstr(g.name), EntityKind::Geometry, g.id)->setIcon(0, toolIcon(g.type == GeometryType::Line ? "line" : "polyline"));
    }

    auto* designs = folder("Mine Designs", static_cast<int>(m.developments.size()));
    auto* headings = new QTreeWidgetItem(designs, {QStringLiteral("Development Headings (%1)").arg(m.developments.size())});
    headings->setFlags(Qt::ItemIsEnabled);
    for (const auto& d : m.developments.all()) {
        auto* it = leaf(headings, qstr(d.headingId), EntityKind::Development, d.id);
        it->setIcon(0, toolIcon("heading"));
        it->setToolTip(0, QStringLiteral("%1 — level %2, %3").arg(qstr(d.name), qstr(d.level), qstr(d.status)));
    }
    for (const char* n : {"Levels", "Declines", "Crosscuts", "Stopes"}) {
        auto* f = new QTreeWidgetItem(designs, {n});
        f->setFlags(Qt::ItemIsEnabled);
        f->setForeground(0, c.textMuted);
        f->setToolTip(0, "Dedicated object type planned (Phase 6). Use the Activity Type attribute on headings for now.");
    }
    folder("Drillholes", 0, false);
    folder("Geological Models", 0, false);

    auto* acts = folder("Scheduling Activities", static_cast<int>(m.activities.size()));
    std::function<void(QTreeWidgetItem*, const Uuid&)> addActs = [&](QTreeWidgetItem* parent, const Uuid& parentId) {
        for (const auto& a : m.activities.all()) {
            if (a.parentId != parentId) continue;
            auto* it = leaf(parent, qstr(a.name), EntityKind::Activity, a.id);
            it->setIcon(0, toolIcon("gantt"));
            if (a.designImpactFlag) {
                it->setForeground(0, c.warning);
                it->setToolTip(0, qstr(a.designImpactNote));
            }
            addActs(it, a.id);
        }
    };
    addActs(acts, Uuid{});
    auto* cals = folder("Calendars", static_cast<int>(m.calendars.size()));
    for (const auto& cal : m.calendars.all()) {
        auto* it = new QTreeWidgetItem(cals, {qstr(cal.name) + (cal.id == m.info.defaultCalendarId ? "  (project default)" : "")});
        it->setFlags(Qt::ItemIsEnabled);
    }
    auto* bls = folder("Baselines", static_cast<int>(m.baselines.size()));
    for (const auto& b : m.baselines) {
        auto* it = new QTreeWidgetItem(bls, {qstr(b.name) + "  " + qstr(b.capturedAt)});
        it->setFlags(Qt::ItemIsEnabled);
    }
    folder("Resource Libraries", 0, false);
    folder("Production Actuals", 0, false);
    folder("Reports", 0, false);

    expandAll();
    std::function<void(QTreeWidgetItem*)> restore = [&](QTreeWidgetItem* it) {
        if (collapsed.contains(it->text(0).section(" (", 0, 0))) it->setExpanded(false);
        for (int i = 0; i < it->childCount(); ++i) restore(it->child(i));
    };
    restore(root);
    rebuilding_ = false;
    syncSelection();
}

void ProjectExplorer::syncSelection() {
    rebuilding_ = true;
    clearSelection();
    if (ctx_.hasProject() && !ctx_.selection().isEmpty())
        if (auto* it = findItem(ctx_.selection().id)) {
            it->setSelected(true);
            scrollToItem(it);
        }
    rebuilding_ = false;
}

} // namespace openmine::app
