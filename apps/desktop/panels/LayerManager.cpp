// SPDX-License-Identifier: Apache-2.0
#include "LayerManager.h"

#include "../theme/Theme.h"

#include "openmine/persistence/EntityCodec.h"
#include "openmine/project/ProjectEditor.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace openmine::app {

namespace {
enum Col { Current, Name, Visible, Locked, Color, Transparency, Width, Type, Objects, NCols };
}

LayerManager::LayerManager(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    auto* bar = new QHBoxLayout;
    auto button = [&](const QString& icon, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(toolIcon(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        bar->addWidget(b);
        return b;
    };
    auto* add = button("new", tr("New layer"));
    auto* del = button("delete", tr("Delete layer (refused while objects use it)"));
    auto* up = button("redo", tr("Move up (drawn later)"));
    auto* down = button("undo", tr("Move down"));
    auto* sel = button("select", tr("Select the objects on the layer"));
    bar->addStretch(1);
    lay->addLayout(bar);
    table_ = new QTableWidget(this);
    table_->setColumnCount(NCols);
    table_->setHorizontalHeaderLabels({tr("Cur"), tr("Name"), tr("On"), tr("Lock"), tr("Colour"), tr("Transp."), tr("Width"), tr("Line type"), tr("Objects")});
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setSectionResizeMode(Name, QHeaderView::Stretch);
    table_->verticalHeader()->setDefaultSectionSize(24);
    lay->addWidget(table_, 1);

    connect(add, &QToolButton::clicked, this, [this] {
        bool ok = false;
        const QString n = QInputDialog::getText(this, tr("New layer"), tr("Name:"), QLineEdit::Normal, tr("Layer %1").arg(ctx_.project().model().layers.size() + 1), &ok);
        if (!ok || n.isEmpty()) return;
        const QColor c = QColorDialog::getColor(QColor(0x7F, 0xB3, 0xD5), this, tr("Layer colour"));
        if (!c.isValid()) return;
        addLayer(n, (static_cast<std::uint32_t>(c.red()) << 16) | (static_cast<std::uint32_t>(c.green()) << 8) | static_cast<std::uint32_t>(c.blue()));
    });
    connect(del, &QToolButton::clicked, this, [this] {
        const Uuid id = layerAt(table_->currentRow());
        if (id.isNil()) return;
        ctx_.tryEdit(tr("Delete layer"), [&] { ProjectEditor(ctx_.project()).remove<CadLayer>(id); });
    });
    connect(up, &QToolButton::clicked, this, [this] { moveLayer(layerAt(table_->currentRow()), +1); });
    connect(down, &QToolButton::clicked, this, [this] { moveLayer(layerAt(table_->currentRow()), -1); });
    connect(sel, &QToolButton::clicked, this, [this] {
        const Uuid id = layerAt(table_->currentRow());
        std::vector<SelectionItem> items;
        for (const auto& g : ctx_.project().model().geometry.all())
            if (g.layerId == id) items.push_back({EntityKind::Geometry, g.id});
        ctx_.selectMany(items);
    });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (building_ || it->column() != Name) return;
        const Uuid id = layerAt(it->row());
        const QString n = it->text().trimmed();
        edit(id, [n](CadLayer& l) { l.name = sstr(n); }, tr("Rename layer"));
    });
    connect(&ctx_, &AppContext::modelChanged, this, &LayerManager::scheduleRebuild);
    connect(&ctx_, &AppContext::projectReplaced, this, &LayerManager::scheduleRebuild);
    connect(&ctx_, &AppContext::currentLayerChanged, this, &LayerManager::scheduleRebuild);
    rebuild();
}

Uuid LayerManager::layerAt(int row) const {
    if (row < 0 || row >= table_->rowCount() || !table_->item(row, Name)) return {};
    return uuidFrom(table_->item(row, Name)->data(Qt::UserRole).toString());
}

void LayerManager::scheduleRebuild() {
    if (pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        rebuild();
    });
}

void LayerManager::edit(const Uuid& id, const std::function<void(CadLayer&)>& change, const QString& what) {
    if (!ctx_.hasProject()) return;
    const auto* l = ctx_.project().model().layers.find(id);
    if (!l) return;
    CadLayer n = *l;
    change(n);
    if (!ctx_.tryEdit(what, [&] { ProjectEditor(ctx_.project()).update(n, sstr(what)); })) scheduleRebuild();
}

bool LayerManager::addLayer(const QString& name, std::uint32_t color) {
    Uuid id;
    const bool ok = ctx_.tryEdit(tr("New layer"), [&] { id = ProjectEditor(ctx_.project()).addLayer(sstr(name), color); });
    if (ok) ctx_.setCurrentLayer(id);
    return ok;
}

bool LayerManager::moveLayer(const Uuid& id, int delta) {
    if (id.isNil()) return false;
    const auto& layers = ctx_.project().model().layers.all();
    std::vector<std::string> order;
    for (const auto& l : layers) order.push_back(l.id.toString());
    auto it = std::find(order.begin(), order.end(), id.toString());
    if (it == order.end()) return false;
    const auto i = static_cast<long>(it - order.begin());
    const long j = std::clamp<long>(i + delta, 0, static_cast<long>(order.size()) - 1);
    if (i == j) return false;
    std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(j)]);
    return ctx_.tryEdit(tr("Reorder layers"), [&] {
        auto& p = ctx_.project();
        p.commands().execute(persistence::reorderCommand(p.model(), "CadLayer", order, "Reorder layers"));
    });
}

void LayerManager::rebuild() {
    building_ = true;
    const Uuid keep = layerAt(table_->currentRow());
    table_->clearContents();
    table_->setRowCount(0);
    if (!ctx_.hasProject()) {
        building_ = false;
        return;
    }
    const auto& m = ctx_.project().model();
    std::map<Uuid, int> counts;
    for (const auto& g : m.geometry.all()) ++counts[g.layerId];
    const Uuid current = ctx_.currentLayer();
    table_->setRowCount(static_cast<int>(m.layers.size()));
    int row = 0;
    for (const auto& l : m.layers.all()) {
        const Uuid id = l.id;
        auto* cur = new QRadioButton;
        cur->setChecked(id == current);
        cur->setToolTip(tr("New objects are drawn on the current layer"));
        connect(cur, &QRadioButton::clicked, this, [this, id] { ctx_.setCurrentLayer(id); });
        table_->setCellWidget(row, Current, cur);
        auto* name = new QTableWidgetItem(qstr(l.name));
        name->setData(Qt::UserRole, uuidText(id));
        table_->setItem(row, Name, name);
        auto* vis = new QCheckBox;
        vis->setChecked(l.visible);
        connect(vis, &QCheckBox::clicked, this, [this, id](bool on) { edit(id, [on](CadLayer& x) { x.visible = on; }, on ? tr("Show layer") : tr("Hide layer")); });
        table_->setCellWidget(row, Visible, vis);
        auto* lock = new QCheckBox;
        lock->setChecked(l.locked);
        connect(lock, &QCheckBox::clicked, this, [this, id](bool on) { edit(id, [on](CadLayer& x) { x.locked = on; }, on ? tr("Lock layer") : tr("Unlock layer")); });
        table_->setCellWidget(row, Locked, lock);
        auto* col = new QPushButton;
        col->setFixedWidth(46);
        const QColor qc(static_cast<QRgb>(l.colorRgb));
        col->setStyleSheet(QStringLiteral("background:%1; border:1px solid #444;").arg(qc.name()));
        col->setToolTip(qc.name().toUpper());
        connect(col, &QPushButton::clicked, this, [this, id, qc] {
            const QColor c = QColorDialog::getColor(qc, this, tr("Layer colour"));
            if (!c.isValid()) return;
            const std::uint32_t v = (static_cast<std::uint32_t>(c.red()) << 16) | (static_cast<std::uint32_t>(c.green()) << 8) | static_cast<std::uint32_t>(c.blue());
            edit(id, [v](CadLayer& x) { x.colorRgb = v; }, tr("Layer colour"));
        });
        table_->setCellWidget(row, Color, col);
        auto* tr2 = new QDoubleSpinBox;
        tr2->setRange(0, 0.95);
        tr2->setSingleStep(0.05);
        tr2->setDecimals(2);
        tr2->setValue(l.transparency);
        connect(tr2, &QDoubleSpinBox::editingFinished, this, [this, id, tr2] {
            const double v = tr2->value();
            edit(id, [v](CadLayer& x) { x.transparency = v; }, tr("Layer transparency"));
        });
        table_->setCellWidget(row, Transparency, tr2);
        auto* w = new QDoubleSpinBox;
        w->setRange(0.5, 12);
        w->setSingleStep(0.5);
        w->setDecimals(1);
        w->setValue(l.lineWidth);
        connect(w, &QDoubleSpinBox::editingFinished, this, [this, id, w] {
            const double v = w->value();
            edit(id, [v](CadLayer& x) { x.lineWidth = v; }, tr("Layer line width"));
        });
        table_->setCellWidget(row, Width, w);
        auto* lt = new QComboBox;
        lt->addItems({"Continuous", "Dashed", "Dotted", "DashDot"});
        lt->setCurrentText(qstr(l.lineType));
        connect(lt, &QComboBox::activated, this, [this, id, lt](int) {
            const std::string v = sstr(lt->currentText());
            edit(id, [v](CadLayer& x) { x.lineType = v; }, tr("Layer line type"));
        });
        table_->setCellWidget(row, Type, lt);
        auto* n = new QTableWidgetItem(QString::number(counts[id]));
        n->setFlags(Qt::ItemIsEnabled);
        n->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, Objects, n);
        if (id == keep) table_->setCurrentCell(row, Name);
        ++row;
    }
    table_->resizeColumnToContents(Current);
    table_->resizeColumnToContents(Visible);
    table_->resizeColumnToContents(Locked);
    building_ = false;
}

} // namespace openmine::app
