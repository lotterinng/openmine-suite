// SPDX-License-Identifier: Apache-2.0
#include "PropertiesPanel.h"

#include "../app/EntityInfo.h"
#include "../theme/Theme.h"

#include "openmine/geology/BlockModel.h"
#include "openmine/geology/Desurvey.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polygon2D.h"
#include "openmine/mine_design/Development.h"
#include "openmine/mine_design/Headings.h"
#include "openmine/mine_design/Stopes.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/persistence/EntityCodec.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Levelling.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDateEdit>
#include <QDoubleValidator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <numbers>

namespace openmine::app {

using nlohmann::json;
using K = FieldInfo::Kind;

namespace {

QString jsonText(const json& v) {
    if (v.is_null()) return {};
    if (v.is_string()) return qstr(v.get<std::string>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return QString::number(v.get<long long>());
    if (v.is_number_float()) return QString::number(v.get<double>(), 'g', 15);
    if (v.is_object() && v.contains("$f")) return qstr(v["$f"].get<std::string>());
    if (v.is_object() && v.contains("$b64")) return QStringLiteral("binary (%L1 bytes)").arg(static_cast<qulonglong>(v["$b64"].get<std::string>().size() * 3 / 4));
    return qstr(v.dump());
}

QString colorText(std::uint32_t c) { return QStringLiteral("#%1").arg(c, 6, 16, QChar('0')).toUpper(); }

QString num(double v, int d = 3) { return formatNumber(v, d); }

/// Entities of a document type for reference editors: (id, display name).
std::vector<std::pair<Uuid, QString>> candidates(const ProjectModel& m, const std::string& type) {
    std::vector<std::pair<Uuid, QString>> out;
    const auto kind = kindForDocument(type);
    if (!kind) return out;
#define OPENMINE_CANDIDATES(T, member, Kind)                                                  \
    if (*kind == EntityKind::Kind)                                                            \
        for (const auto& e : m.member.all()) out.emplace_back(e.id, displayName(m, {EntityKind::Kind, e.id}));
    OPENMINE_ENTITY_TYPES(OPENMINE_CANDIDATES)
#undef OPENMINE_CANDIDATES
    return out;
}

} // namespace

PropertiesPanel::PropertiesPanel(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    header_ = new QLabel(this);
    header_->setWordWrap(true);
    header_->setObjectName("panelHeader");
    lay->addWidget(header_);
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("Property"), tr("Value")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree_->header()->setStretchLastSection(true);
    tree_->setColumnWidth(0, 170);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(false);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::NoSelection);
    lay->addWidget(tree_, 1);

    vertexBox_ = new QWidget(this);
    auto* vl = new QVBoxLayout(vertexBox_);
    vl->setContentsMargins(0, 0, 0, 0);
    auto* vh = new QHBoxLayout;
    vh->addWidget(new QLabel(tr("Vertices (X, Y, Z)"), vertexBox_));
    vh->addStretch(1);
    auto* add = new QToolButton(vertexBox_);
    add->setText("+");
    add->setToolTip(tr("Insert a vertex after the current one (midpoint)"));
    auto* del = new QToolButton(vertexBox_);
    del->setText("−");
    del->setToolTip(tr("Delete the current vertex"));
    vh->addWidget(add);
    vh->addWidget(del);
    vl->addLayout(vh);
    vertices_ = new QTableWidget(vertexBox_);
    vertices_->setColumnCount(3);
    vertices_->setHorizontalHeaderLabels({"X", "Y", "Z"});
    vertices_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    vertices_->verticalHeader()->setDefaultSectionSize(20);
    vertices_->setMinimumHeight(120);
    vl->addWidget(vertices_);
    lay->addWidget(vertexBox_);
    vertexBox_->hide();

    connect(vertices_, &QTableWidget::cellChanged, this, [this](int, int) {
        if (!building_) commitVertices();
    });
    connect(add, &QToolButton::clicked, this, [this] {
        const int r = std::max(0, vertices_->currentRow());
        if (vertices_->rowCount() == 0) return;
        building_ = true;
        const int next = std::min(r + 1, vertices_->rowCount() - 1);
        vertices_->insertRow(r + 1);
        for (int c = 0; c < 3; ++c) {
            const double a = vertices_->item(r, c)->text().toDouble();
            const double b = vertices_->item(next == r ? r : next + 1, c) ? vertices_->item(next == r ? r : next + 1, c)->text().toDouble() : a;
            vertices_->setItem(r + 1, c, new QTableWidgetItem(QString::number((a + b) / 2, 'f', 4)));
        }
        building_ = false;
        commitVertices();
    });
    connect(del, &QToolButton::clicked, this, [this] {
        const int r = vertices_->currentRow();
        if (r < 0) return;
        building_ = true;
        vertices_->removeRow(r);
        building_ = false;
        commitVertices();
    });

    connect(&ctx_, &AppContext::selectionChanged, this, &PropertiesPanel::scheduleRebuild);
    connect(&ctx_, &AppContext::modelChanged, this, &PropertiesPanel::scheduleRebuild);
    connect(&ctx_, &AppContext::projectReplaced, this, &PropertiesPanel::scheduleRebuild);
    rebuild();
}

void PropertiesPanel::scheduleRebuild() {
    if (pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        rebuild();
    });
}

std::vector<SelectionItem> PropertiesPanel::editedItems() const {
    std::vector<SelectionItem> out;
    const auto prim = ctx_.primary();
    for (const auto& s : ctx_.selection())
        if (s.kind == prim.kind) out.push_back(s);
    return out;
}

QTreeWidgetItem* PropertiesPanel::section(const QString& title) {
    auto* it = new QTreeWidgetItem(tree_, {title});
    QFont f = it->font(0);
    f.setBold(true);
    it->setFont(0, f);
    it->setFirstColumnSpanned(true);
    it->setExpanded(true);
    return it;
}

void PropertiesPanel::addReadOnly(QTreeWidgetItem* parent, const QString& label, const QString& value, const QString& tooltip) {
    auto* it = new QTreeWidgetItem(parent, {label, value});
    it->setForeground(1, Theme::colors().textMuted);
    if (!tooltip.isEmpty()) {
        it->setToolTip(0, tooltip);
        it->setToolTip(1, tooltip);
    }
    displayed_[label] = value;
}

void PropertiesPanel::rebuild() {
    building_ = true;
    tree_->clear();
    displayed_.clear();
    docs_.clear();
    type_.clear();
    vertexBox_->hide();
    if (!ctx_.hasProject()) {
        header_->setText(tr("No project"));
        building_ = false;
        return;
    }
    const auto& m = ctx_.project().model();
    auto items = editedItems();
    SelectionItem prim = ctx_.primary();
    if (items.empty()) {
        // Nothing selected: the project settings.
        prim = {EntityKind::ProjectInfo, m.info.id};
        items = {prim};
    }
    type_ = documentType(prim.kind);
    for (const auto& it : items) {
        const std::string key = it.kind == EntityKind::ProjectInfo ? std::string("project") : it.id.toString();
        if (auto d = persistence::encodeDocument(m, type_, key)) docs_.push_back(json::parse(*d));
    }
    if (docs_.empty()) {
        header_->setText(tr("The selected item no longer exists"));
        building_ = false;
        return;
    }
    if (items.size() > 1) header_->setText(tr("<b>%1 %2</b> — edits apply to all").arg(items.size()).arg(kindLabel(prim.kind, true)));
    else header_->setText(QStringLiteral("<b>%1</b>  %2").arg(kindLabel(prim.kind), displayName(m, prim).toHtmlEscaped()));

    buildFields(type_, docs_);
    if (items.size() == 1) buildDerived(prim);
    buildAttributes(type_, docs_);
    if (items.size() == 1 && prim.kind == EntityKind::Geometry)
        if (const auto* g = m.geometry.find(prim.id)) buildVertices(*g);
    building_ = false;
}

void PropertiesPanel::buildFields(const std::string& type, const std::vector<json>& docs) {
    const auto& first = docs.front();
    const json& cols = first.contains("columns") ? first["columns"] : first;
    std::map<int, QTreeWidgetItem*> sections;
    auto sec = [&](int group) {
        if (auto it = sections.find(group); it != sections.end()) return it->second;
        const QString title = group == 0 ? tr("Identity") : group == 3 ? tr("Links") : group == 9 ? tr("Record") : tr("Properties");
        return sections[group] = section(title);
    };
    // Fixed section order: identity, properties, links, record.
    for (int g : {0, 1, 3, 9}) sec(g);
    for (auto it = cols.begin(); it != cols.end(); ++it) {
        const std::string col = it.key();
        if (type == "ProjectInfo" && (col == "uuid" || col == "meta")) continue;
        const FieldInfo fi = fieldInfo(type, col);
        bool varies = false;
        for (std::size_t i = 1; i < docs.size(); ++i) {
            const json& c2 = docs[i].contains("columns") ? docs[i]["columns"] : docs[i];
            if (!c2.contains(col) || c2[col] != it.value()) varies = true;
        }
        auto* parent = sec(fi.group == 2 ? 1 : fi.group);
        QString label = fi.label;
        if (!fi.unit.isEmpty()) label += QStringLiteral(" (%1)").arg(fi.unit);
        auto* item = new QTreeWidgetItem(parent, {label});
        item->setData(0, Qt::UserRole, qstr(col));
        QString shown = varies ? tr("(varies)") : jsonText(it.value());
        if (fi.kind == K::Reference && !varies && it.value().is_string()) {
            if (auto id = Uuid::parse(it.value().get<std::string>()))
                if (auto k = kindForDocument(fi.referenceType)) shown = displayName(ctx_.project().model(), {*k, *id});
        }
        if (fi.kind == K::Color && it.value().is_number()) shown = colorText(it.value().get<std::uint32_t>());
        displayed_[qstr(col)] = shown;
        displayed_[fi.label] = shown;
        const bool editable = !fi.readOnly && fi.kind != K::Binary && fi.kind != K::ReadOnly && !(it.value().is_object() && it.value().contains("$b64"));
        if (!fi.tooltip.isEmpty() || fi.readOnly) {
            const QString tip = fi.readOnly ? fi.readOnlyReason : fi.tooltip;
            item->setToolTip(0, tip);
            item->setToolTip(1, tip);
        }
        if (!editable) {
            if (!varies && it.value().is_number_float()) {
                // Display six decimals at most; the exact stored value is in the tooltip.
                const double v = it.value().get<double>();
                QString t = QString::number(v, 'f', 6);
                while (t.contains('.') && (t.endsWith('0') || t.endsWith('.'))) t.chop(1);
                item->setText(1, t);
                item->setToolTip(1, QStringLiteral("%1\n%2").arg(QString::number(v, 'g', 17), item->toolTip(1)).trimmed());
                item->setForeground(1, Theme::colors().textMuted);
                continue;
            }
            item->setText(1, shown);
            item->setForeground(1, Theme::colors().textMuted);
            continue;
        }
        if (QWidget* ed = editorFor(type, col, it.value(), varies)) tree_->setItemWidget(item, 1, ed);
        else item->setText(1, shown);
    }
    for (auto& [g, s] : sections)
        if (s->childCount() == 0) delete s;
}

QWidget* PropertiesPanel::editorFor(const std::string& type, const std::string& col, const json& value, bool varies) {
    const FieldInfo fi = fieldInfo(type, col);
    const QString what = tr("Edit %1").arg(fi.label.toLower());
    switch (fi.kind) {
    case K::Bool: {
        auto* cb = new QCheckBox;
        if (varies) {
            cb->setTristate(true);
            cb->setCheckState(Qt::PartiallyChecked);
        }
        else cb->setChecked(value.is_number() ? value.get<double>() != 0 : (value.is_boolean() && value.get<bool>()));
        const bool asBool = value.is_boolean();
        connect(cb, &QCheckBox::clicked, this, [this, col, cb, what, asBool] {
            commit(col, asBool ? json(cb->isChecked()) : json(cb->isChecked() ? 1 : 0), what);
        });
        return cb;
    }
    case K::Enum: {
        auto* combo = new QComboBox;
        combo->addItems(fi.choices);
        if (varies) {
            combo->insertItem(0, tr("(varies)"));
            combo->setCurrentIndex(0);
        } else {
            combo->setCurrentText(jsonText(value));
        }
        connect(combo, &QComboBox::activated, this, [this, col, combo, what, varies](int idx) {
            if (varies && idx == 0) return;
            commit(col, sstr(combo->currentText()), what);
        });
        return combo;
    }
    case K::Reference: {
        auto* combo = new QComboBox;
        combo->addItem(tr("(none)"), QString());
        int current = 0;
        const std::string cur = value.is_string() ? value.get<std::string>() : std::string();
        for (const auto& [id, name] : candidates(ctx_.project().model(), fi.referenceType)) {
            combo->addItem(name, uuidText(id));
            if (id.toString() == cur) current = combo->count() - 1;
        }
        if (varies) {
            combo->insertItem(0, tr("(varies)"), QStringLiteral("__varies"));
            current = 0;
        }
        combo->setCurrentIndex(current);
        connect(combo, &QComboBox::activated, this, [this, col, combo, what](int) {
            const QString v = combo->currentData().toString();
            if (v == "__varies") return;
            commit(col, v.isEmpty() ? json(nullptr) : json(sstr(v)), what);
        });
        return combo;
    }
    case K::Color: {
        auto* b = new QPushButton;
        const std::uint32_t c = value.is_number() ? value.get<std::uint32_t>() : 0xFFFFFF;
        b->setText(value.is_null() ? tr("By layer") : varies ? tr("(varies)") : colorText(c));
        if (!value.is_null() && !varies) b->setStyleSheet(QStringLiteral("background:%1; color:%2").arg(colorText(c), (((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)) > 380 ? "black" : "white"));
        connect(b, &QPushButton::clicked, this, [this, col, c, what, nullable = col == "color_override"] {
            QColorDialog dlg(QColor(static_cast<QRgb>(c)), this);
            dlg.setWindowTitle(what);
            if (nullable) dlg.setOption(QColorDialog::NoButtons, false);
            if (dlg.exec() != QDialog::Accepted) return;
            const QColor q = dlg.currentColor();
            commit(col, (static_cast<std::uint32_t>(q.red()) << 16) | (static_cast<std::uint32_t>(q.green()) << 8) | static_cast<std::uint32_t>(q.blue()), what);
        });
        if (col == "color_override" && !value.is_null()) {
            auto* w = new QWidget;
            auto* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(b, 1);
            auto* clear = new QToolButton;
            clear->setText(tr("By layer"));
            connect(clear, &QToolButton::clicked, this, [this, col, what] { commit(col, nullptr, what); });
            h->addWidget(clear);
            return w;
        }
        return b;
    }
    case K::Date:
    case K::DateTime: {
        auto* e = new QLineEdit(varies ? QString() : jsonText(value));
        e->setPlaceholderText(varies ? tr("(varies)") : fi.kind == K::Date ? QStringLiteral("YYYY-MM-DD (empty = none)") : QStringLiteral("YYYY-MM-DD HH:MM (empty = none)"));
        connect(e, &QLineEdit::editingFinished, this, [this, col, e, what, k = fi.kind, old = jsonText(value)] {
            const QString t = e->text().trimmed();
            if (t == old) return;
            if (t.isEmpty()) {
                commit(col, nullptr, what);
                return;
            }
            const bool ok = k == K::Date ? Date::parse(sstr(t)).has_value() : DateTime::parse(sstr(t)).has_value();
            if (!ok) {
                ctx_.report(tr("'%1' is not a valid %2").arg(t, k == K::Date ? tr("date (YYYY-MM-DD)") : tr("date-time (YYYY-MM-DD HH:MM)")), MessageLevel::Error);
                e->setText(old);
                return;
            }
            commit(col, sstr(t), what);
        });
        return e;
    }
    case K::Json:
    case K::MultiLine: {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        auto* l = new QLabel(varies ? tr("(varies)") : jsonText(value).left(60).replace('\n', ' '));
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        h->addWidget(l, 1);
        auto* b = new QToolButton;
        b->setText("…");
        b->setToolTip(tr("Edit"));
        h->addWidget(b);
        connect(b, &QToolButton::clicked, this, [this, col, value, what, isJson = fi.kind == K::Json, label = fi.label] {
            bool ok = false;
            QString t = jsonText(value);
            if (isJson && !t.isEmpty()) {
                try {
                    t = qstr(json::parse(sstr(t)).dump(2));
                } catch (...) {
                }
            }
            const QString n = QInputDialog::getMultiLineText(this, what, label, t, &ok);
            if (!ok) return;
            if (isJson && !n.trimmed().isEmpty()) {
                try {
                    commit(col, json::parse(sstr(n)).dump(), what);
                } catch (const std::exception& e) {
                    ctx_.report(tr("Invalid JSON: %1").arg(qstr(e.what())), MessageLevel::Error);
                }
                return;
            }
            commit(col, sstr(n), what);
        });
        return w;
    }
    case K::Text:
    case K::Real:
    case K::Integer:
    default: {
        const bool numeric = value.is_number();
        if (fi.editableChoices) {
            auto* combo = new QComboBox;
            combo->setEditable(true);
            combo->addItems(fi.choices);
            combo->setCurrentText(varies ? QString() : jsonText(value));
            if (varies) combo->lineEdit()->setPlaceholderText(tr("(varies)"));
            connect(combo->lineEdit(), &QLineEdit::editingFinished, this, [this, col, combo, what, old = jsonText(value)] {
                if (combo->currentText() == old) return;
                commit(col, sstr(combo->currentText()), what);
            });
            connect(combo, &QComboBox::activated, this, [this, col, combo, what, old = jsonText(value)](int) {
                if (combo->currentText() == old) return;
                commit(col, sstr(combo->currentText()), what);
            });
            return combo;
        }
        auto* e = new QLineEdit(varies ? QString() : jsonText(value));
        if (varies) e->setPlaceholderText(tr("(varies)"));
        if (numeric) {
            e->setAlignment(Qt::AlignRight);
            auto* v = new QDoubleValidator(e);
            v->setLocale(QLocale::c());
            v->setNotation(QDoubleValidator::ScientificNotation);
            if (!value.is_number_float()) v->setDecimals(0);
            e->setValidator(v);
        }
        connect(e, &QLineEdit::editingFinished, this, [this, col, e, what, numeric, isInt = value.is_number_integer(), old = jsonText(value)] {
            const QString t = e->text().trimmed();
            if (t == old || (t.isEmpty() && numeric)) {
                e->setText(old);
                return;
            }
            if (numeric) {
                bool ok = false;
                const double d = QLocale::c().toDouble(t, &ok);
                if (!ok || !std::isfinite(d)) {
                    ctx_.report(tr("'%1' is not a number").arg(t), MessageLevel::Error);
                    e->setText(old);
                    return;
                }
                if (isInt) commit(col, static_cast<long long>(std::llround(d)), what);
                else commit(col, d, what);
            } else {
                commit(col, sstr(t), what);
            }
        });
        return e;
    }
    }
}

bool PropertiesPanel::commit(const std::string& column, const json& value, const QString& what) {
    if (!ctx_.hasProject() || docs_.empty()) return false;
    const std::string type = type_;
    auto docs = docs_;
    const bool ok = ctx_.tryEdit(what, [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), sstr(what));
        for (auto& d : docs) {
            json& cols = d.contains("columns") ? d["columns"] : d;
            cols[column] = value;
            if (type == "ProjectInfo") {
                ProjectInfo info = p.model().info;
                auto optUuid = [](const json& v) { return v.is_string() ? Uuid::parse(v.get<std::string>()).value_or(Uuid{}) : Uuid{}; };
                if (column == "name") info.name = value.get<std::string>();
                else if (column == "description") info.description = value.get<std::string>();
                else if (column == "schedule_start") {
                    auto t = value.is_string() ? DateTime::parse(value.get<std::string>()) : std::nullopt;
                    if (!t) throw ValidationError("The schedule start is required");
                    info.scheduleStart = *t;
                } else if (column == "status_date") {
                    info.statusDate = value.is_string() ? DateTime::parse(value.get<std::string>()) : std::nullopt;
                } else if (column == "project_crs") info.projectCrsId = optUuid(value);
                else if (column == "default_calendar") info.defaultCalendarId = optUuid(value);
                else if (column == "coordinate_display_decimals") info.coordinateDisplayDecimals = static_cast<int>(value.get<double>());
                else if (column == "linear_unit") info.linearUnit = value.get<std::string>();
                else if (column == "south_orientated_axes") info.southOrientatedAxes = value.is_boolean() ? value.get<bool>() : value.get<int>() != 0;
                else throw ValidationError("Field '" + column + "' cannot be edited");
                ProjectEditor(p).setProjectInfo(info, sstr(what));
            } else if (type == "GeometryObject") {
                // Centreline edits flag the linked schedule quantities (never change them).
                auto g = persistence::decodeEntity<GeometryObject>(d.dump());
                mine_design::updateGeometryWithImpact(p, g, sstr(what));
            } else {
                persistence::putDocument(p, type, d.dump(), sstr(what));
            }
        }
        macro.commit();
    });
    if (!ok) scheduleRebuild(); // restore the displayed values
    return ok;
}

// ---- derived values --------------------------------------------------------------------------------

void PropertiesPanel::buildDerived(const SelectionItem& s) {
    const auto& p = ctx_.project();
    const auto& m = p.model();
    QTreeWidgetItem* sec = nullptr;
    auto add = [&](const QString& label, const QString& value, const QString& tip = {}) {
        if (!sec) sec = section(tr("Derived (read-only)"));
        addReadOnly(sec, label, value, tip);
    };
    try {
        switch (s.kind) {
        case EntityKind::Geometry: {
            const auto* g = m.geometry.find(s.id);
            if (!g) break;
            if (geometry::isCurve(g->type)) {
                add(tr("Length 3D (m)"), num(geometry::curveLength(*g)));
                add(tr("Plan length (m)"), num(geometry::planLength(*g)));
                if (auto s0 = geometry::curveStart(*g), s1 = geometry::curveEnd(*g); s0 && s1) {
                    const double pl = geometry::planLength(*g);
                    if (pl > 1e-9) add(tr("Overall grade (%)"), num((s1->z - s0->z) / pl * 100.0, 2));
                }
                const bool closed = g->closed || g->type == GeometryType::Circle;
                if (closed) {
                    geometry::Ring ring;
                    for (const auto& v : geometry::curvePolyline(*g)) ring.push_back(xy(v));
                    add(tr("Plan area (m²)"), num(geometry::area(ring), 2));
                }
                add(tr("Vertices"), QString::number(g->vertices.size()));
            }
            if (g->mesh) {
                const auto t = geometry::topology(*g->mesh);
                add(tr("Triangles"), QStringLiteral("%L1").arg(static_cast<qulonglong>(t.triangleCount)));
                add(tr("Surface area (m²)"), num(geometry::meshArea(*g->mesh), 2));
                add(tr("Watertight"), t.watertight() ? tr("yes") : tr("no (%1 boundary, %2 non-manifold edges)").arg(t.boundaryEdges).arg(t.nonManifoldEdges));
                if (t.closed()) add(tr("Enclosed volume (m³)"), num(std::abs(geometry::meshSignedVolume(*g->mesh)), 2));
                if (!g->mesh->scalarName.empty()) add(tr("Vertex scalar"), qstr(g->mesh->scalarName));
            }
            if (auto b = geometry::objectBounds(*g)) {
                add(tr("Min X, Y, Z"), QStringLiteral("%1, %2, %3").arg(num(b->min.x), num(b->min.y), num(b->min.z)));
                add(tr("Max X, Y, Z"), QStringLiteral("%1, %2, %3").arg(num(b->max.x), num(b->max.y), num(b->max.z)));
            }
            const auto refs = m.referencesToGeometry(s.id);
            if (!refs.empty()) {
                QStringList l;
                for (const auto& [t, id] : refs) l << qstr(t);
                add(tr("Referenced by"), l.join(", "));
            }
            break;
        }
        case EntityKind::Development: {
            const auto* d = m.developments.find(s.id);
            if (!d) break;
            const auto met = mine_design::computeMetrics(p, *d);
            add(tr("Length 3D (m)"), num(met.length3d));
            add(tr("Plan length (m)"), num(met.horizontalLength));
            add(tr("Gradient (%)"), met.gradientPercent ? num(*met.gradientPercent, 2) : tr("vertical"));
            add(tr("Chainage to (m)"), mine_design::formatChainage(met.chainageTo).c_str());
            add(tr("Section area (m²)"), num(met.sectionArea, 2));
            add(tr("Back and walls perimeter (m)"), num(met.backAndWallsPerimeter, 2));
            add(tr("Volume in situ (m³)"), num(met.volume, 1));
            add(tr("Tonnes"), met.tonnes > 0 ? num(met.tonnes, 1) : tr("— (no material)"));
            add(tr("Loose volume (m³)"), num(met.looseVolume, 1));
            const double delta = met.length3d - d->approvedDesignLength;
            if (d->approvedDesignLength > 0 && std::abs(delta) > mine_design::kDesignChangeTolerance)
                add(tr("Design change"), tr("%1 m since the approved length — linked schedule quantities are flagged").arg(num(delta)));
            if (const auto* g = m.geometry.find(d->geometryId)) {
                const auto v = mine_design::checkDesignRules(g->vertices, g->bulges, mine_design::DesignRules{});
                add(tr("Design-rule violations"), v.empty() ? tr("none") : QString::number(v.size()),
                    tr("Default rules: max grade 15 % (12.5 % on curves), min radius 20 m, max grade change 5 %"));
            }
            QStringList acts;
            for (const auto& a : m.activitiesForDevelopment(d->id))
                if (const auto* ac = m.activities.find(a)) acts << qstr(ac->name);
            add(tr("Linked activities"), acts.isEmpty() ? tr("none") : acts.join(", "));
            break;
        }
        case EntityKind::Stope: {
            const auto* st = m.stopes.find(s.id);
            if (!st) break;
            const auto sm = mine_design::stopeMetrics(p, *st);
            add(tr("Volume (m³)"), num(sm.volume, 1));
            add(tr("Tonnes in situ"), num(sm.tonnes, 1));
            add(tr("Diluted tonnes"), num(sm.dilutedTonnes, 1));
            add(tr("Recovered tonnes"), num(sm.recoveredTonnes, 1));
            add(tr("Mined grade"), num(sm.minedGrade, 3) + " " + qstr(st->gradeUnit));
            add(tr("Contained metal"), num(sm.containedMetal, 1));
            add(tr("Watertight shape"), sm.watertight ? tr("yes") : tr("no — volume is unreliable"));
            int blocks = 0;
            for (const auto& b : m.miningBlocks.all()) blocks += b.stopeId == st->id ? 1 : 0;
            add(tr("Mining blocks"), QString::number(blocks));
            break;
        }
        case EntityKind::PointCloud: {
            const auto* d = m.pointClouds.find(s.id);
            if (!d) break;
            add(tr("Points"), QStringLiteral("%L1").arg(d->pointCount));
            QStringList attrs;
            if (d->attributes.intensity) attrs << "intensity";
            if (d->attributes.rgb) attrs << "RGB";
            if (d->attributes.classification) attrs << "classification";
            if (d->attributes.normals) attrs << "normals";
            if (d->attributes.returnInfo) attrs << "returns";
            if (d->attributes.gpsTime) attrs << "GPS time";
            for (const auto& f : d->attributes.scalarFields) attrs << qstr(f);
            add(tr("Attributes"), attrs.isEmpty() ? tr("XYZ only") : attrs.join(", "));
            auto pb = BoundingBox::empty();
            for (int i = 0; i < 8; ++i)
                pb.expand(d->transform.apply({(i & 1) ? d->storedBounds.max.x : d->storedBounds.min.x, (i & 2) ? d->storedBounds.max.y : d->storedBounds.min.y,
                                              (i & 4) ? d->storedBounds.max.z : d->storedBounds.min.z}));
            add(tr("Project bounds min"), QStringLiteral("%1, %2, %3").arg(num(pb.min.x), num(pb.min.y), num(pb.min.z)));
            add(tr("Project bounds max"), QStringLiteral("%1, %2, %3").arg(num(pb.max.x), num(pb.max.y), num(pb.max.z)));
            add(tr("Registered"), d->transform.isIdentity() ? tr("no (stored = project coordinates)") : tr("yes — transform applied on display and export"));
            QStringList chain;
            for (const auto* c = d; c && chain.size() < 20; c = m.pointClouds.find(c->parentDatasetId))
                chain << (c == d ? qstr(c->derivation.empty() ? c->sourceFile : c->derivation) : qstr(c->name));
            if (chain.size() > 1) add(tr("Derivation chain"), chain.join(" ← "));
            int regs = 0, applied = 0;
            for (const auto& r : m.registrations.all())
                if (r.datasetId == d->id) {
                    ++regs;
                    applied += r.applied ? 1 : 0;
                }
            add(tr("Registrations"), tr("%1 (%2 applied)").arg(regs).arg(applied));
            break;
        }
        case EntityKind::Registration: {
            const auto* r = m.registrations.find(s.id);
            if (!r) break;
            const Vec3 e = r->transform.eulerZYX();
            const double deg = 180.0 / std::numbers::pi;
            add(tr("Translation (m)"), QStringLiteral("%1, %2, %3").arg(num(r->transform.translationPart().x), num(r->transform.translationPart().y), num(r->transform.translationPart().z)));
            add(tr("Rotation roll/pitch/yaw (°)"), QStringLiteral("%1, %2, %3").arg(num(e.x * deg, 4), num(e.y * deg, 4), num(e.z * deg, 4)));
            add(tr("Scale"), QString::number(r->transform.uniformScale(), 'f', 8));
            if (r->fitCount > 0) add(tr("Fit residuals"), tr("RMS %1 m, max %2 m over %3 pairs (optimistic: these pairs determined the fit)").arg(num(r->fitRms, 4), num(r->fitMax, 4)).arg(r->fitCount));
            if (r->checkCount > 0) add(tr("Independent check"), tr("RMS %1 m, max %2 m over %3 check points").arg(num(r->checkRms, 4), num(r->checkMax, 4)).arg(r->checkCount));
            else add(tr("Independent check"), tr("none — positional accuracy is not demonstrated"));
            if (r->icpFitness >= 0) add(tr("ICP"), tr("fitness %1, inlier RMS %2 m, %3 iterations%4").arg(num(r->icpFitness, 3), num(r->icpInlierRms, 4)).arg(r->icpIterations).arg(r->icpConverged ? "" : tr(", NOT converged")));
            break;
        }
        case EntityKind::BlockModel: {
            const auto* b = m.blockModels.find(s.id);
            if (!b) break;
            const geology::BlockGrid g(*b);
            add(tr("Blocks"), QStringLiteral("%L1 (%2 × %3 × %4)").arg(static_cast<qulonglong>(g.count())).arg(b->nx).arg(b->ny).arg(b->nz));
            add(tr("Block volume (m³)"), num(g.blockVolume(), 2));
            add(tr("Extent (m)"), QStringLiteral("%1 × %2 × %3").arg(num(b->nx * b->blockSize.x, 1), num(b->ny * b->blockSize.y, 1), num(b->nz * b->blockSize.z, 1)));
            add(tr("Attributes"), b->attributes.empty() ? tr("none") : qstr([&] {
                std::string s2;
                for (const auto& a : b->attributes) s2 += (s2.empty() ? "" : ", ") + a;
                return s2;
            }()));
            if (!b->dataAsset.isNull() && g.count() <= 20'000'000) {
                const auto bmData = geology::loadBlockModel(p, b->id);
                for (const auto& a : b->attributes) {
                    const auto* v = bmData.find(a);
                    if (!v) continue;
                    double lo = 1e300, hi = -1e300, sum = 0;
                    std::size_t n = 0;
                    for (double x : *v)
                        if (std::isfinite(x)) {
                            lo = std::min(lo, x);
                            hi = std::max(hi, x);
                            sum += x;
                            ++n;
                        }
                    add(qstr(a), n ? tr("%L1 values, min %2, max %3, mean %4").arg(static_cast<qulonglong>(n)).arg(lo, 0, 'g', 5).arg(hi, 0, 'g', 5).arg(sum / n, 0, 'g', 5)
                                   : tr("no values"));
                }
            }
            break;
        }
        case EntityKind::Drillhole: {
            const auto* h = m.drillholes.find(s.id);
            if (!h) break;
            const geology::DesurveyedHole dh(*h);
            const Vec3 eoh = dh.at(dh.length());
            add(tr("End of hole X, Y, Z"), QStringLiteral("%1, %2, %3").arg(num(eoh.x), num(eoh.y), num(eoh.z)));
            add(tr("Surveys"), QString::number(h->surveys.size()));
            add(tr("Intervals"), QString::number(h->intervals.size()));
            std::set<std::string> fields;
            for (const auto& iv : h->intervals) {
                for (const auto& [k, v] : iv.values) fields.insert(k);
                for (const auto& [k, v] : iv.codes) fields.insert(k);
            }
            QStringList fl;
            for (const auto& f : fields) fl << qstr(f);
            add(tr("Fields"), fl.join(", "));
            const auto dls = geology::doglegSeverity(dh);
            if (!dls.empty()) add(tr("Max dogleg severity (°/30 m)"), num(*std::max_element(dls.begin(), dls.end()), 2));
            break;
        }
        case EntityKind::Activity: {
            const auto& sch = ctx_.schedule();
            auto it = sch.result.timings.find(s.id);
            if (!sch.result.ok) {
                add(tr("Schedule"), tr("not calculated: %1").arg(sch.result.errors.empty() ? QString() : qstr(sch.result.errors.front())));
                break;
            }
            if (it == sch.result.timings.end()) break;
            const auto& t = it->second;
            add(tr("Start"), formatDateTimeMinutes(t.es));
            add(tr("Finish"), formatDateTimeMinutes(t.ef));
            add(tr("Latest start"), formatDateTimeMinutes(t.ls));
            add(tr("Latest finish"), formatDateTimeMinutes(t.lf));
            add(tr("Working hours"), num(t.totalHours, 2) + (t.started && !t.completed ? tr(" (%1 remaining)").arg(num(t.durationHours, 2)) : QString()));
            add(tr("Total float (h)"), num(t.totalFloatHours, 2));
            add(tr("Free float (h)"), num(t.freeFloatHours, 2));
            add(tr("Critical"), t.critical ? tr("yes") : tr("no"));
            add(tr("Percent complete"), num(t.percentComplete, 1));
            add(tr("Duration basis"), qstr(t.durationBasis));
            if (const auto* bl = ctx_.comparisonBaseline())
                for (const auto& ba : bl->activities)
                    if (ba.activityId == s.id) {
                        add(tr("Baseline finish"), qstr(ba.finish.toString()));
                        add(tr("Finish variance (h)"), num((t.ef - static_cast<double>(ba.finish.minutes())) / 60.0, 1));
                    }
            break;
        }
        case EntityKind::Calendar: {
            const auto* c = m.calendars.find(s.id);
            if (!c) break;
            double weekly = 0;
            for (const auto& sh : c->shifts)
                for (int d = 0; d < 7; ++d)
                    if (sh.weekdays & (1u << d)) weekly += sh.durationMinutes / 60.0;
            add(tr("Working hours per week"), num(weekly, 1));
            add(tr("Exceptions"), QString::number(c->exceptions.size()));
            break;
        }
        case EntityKind::Resource: {
            const auto& sch = ctx_.schedule();
            if (!sch.result.ok) break;
            for (const auto& pr : scheduling::resourceProfiles(sch.input, sch.result))
                if (pr.resourceId == s.id) {
                    add(tr("Peak usage"), tr("%1 of %2").arg(pr.peak).arg(pr.capacity));
                    add(tr("Over-allocated"), pr.overallocated ? tr("yes, %1 periods").arg(pr.overallocatedSpans.size()) : tr("no"));
                }
            break;
        }
        case EntityKind::Structure: {
            const auto* st = m.structures.find(s.id);
            if (!st) break;
            add(tr("Pole trend/plunge (°)"), QStringLiteral("%1 / %2").arg(std::fmod(st->dipDirection + 180.0, 360.0), 0, 'f', 1).arg(90.0 - st->dip, 0, 'f', 1));
            break;
        }
        case EntityKind::Layer: {
            int n = 0;
            for (const auto& g : m.geometry.all()) n += g.layerId == s.id ? 1 : 0;
            add(tr("Objects"), QString::number(n));
            break;
        }
        case EntityKind::Level: {
            const auto* l = m.levels.find(s.id);
            if (!l) break;
            int n = 0;
            double len = 0;
            for (const auto& d : m.developments.all())
                if (d.levelId == s.id) {
                    ++n;
                    len += mine_design::computeMetrics(p, d).length3d;
                }
            add(tr("Developments"), tr("%1, %2 m").arg(n).arg(num(len, 1)));
            break;
        }
        default: break;
        }
    } catch (const std::exception& e) {
        add(tr("Error"), qstr(e.what()));
    }
}

// ---- custom attributes -----------------------------------------------------------------------------

void PropertiesPanel::buildAttributes(const std::string& type, const std::vector<json>& docs) {
    const auto& first = docs.front();
    if (!first.contains("children") || !first["children"].contains("attributes")) return;
    auto* sec = section(tr("Custom attributes"));
    const auto& m = ctx_.project().model();
    std::map<std::string, const AttributeDefinition*> defs;
    for (const auto& d : m.attributeDefinitions.all())
        if (d.entityType == type) defs[d.key] = &d;
    std::set<std::string> keys;
    for (const auto& [k, d] : defs) keys.insert(k);
    // Child rows: {"key": ..., "value": ...} or a key → value object.
    auto valuesOf = [](const json& doc) {
        std::map<std::string, std::string> v;
        const auto& a = doc["children"]["attributes"];
        if (a.is_object())
            for (auto it = a.begin(); it != a.end(); ++it) v[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
        else if (a.is_array())
            for (const auto& row : a)
                if (row.contains("key")) v[row["key"].get<std::string>()] = row.value("value", std::string());
        return v;
    };
    const auto vals = valuesOf(first);
    for (const auto& [k, v] : vals) keys.insert(k);
    for (const auto& k : keys) {
        bool varies = false;
        const auto mine = vals.count(k) ? std::optional<std::string>(vals.at(k)) : std::nullopt;
        for (std::size_t i = 1; i < docs.size(); ++i) {
            const auto o = valuesOf(docs[i]);
            const auto ov = o.count(k) ? std::optional<std::string>(o.at(k)) : std::nullopt;
            if (ov != mine) varies = true;
        }
        const auto* def = defs.count(k) ? defs.at(k) : nullptr;
        QString label = def && !def->displayName.empty() ? qstr(def->displayName) : qstr(k);
        if (def && !def->unit.empty()) label += QStringLiteral(" (%1)").arg(qstr(def->unit));
        if (def && def->required) label += " *";
        auto* item = new QTreeWidgetItem(sec, {label});
        item->setToolTip(0, def ? tr("%1 (%2)").arg(qstr(k), qstr(toString(def->valueType))) : tr("%1 — no definition").arg(qstr(k)));
        auto* e = new QLineEdit(varies ? QString() : qstr(mine.value_or("")));
        if (varies) e->setPlaceholderText(tr("(varies)"));
        else if (!mine && def) e->setPlaceholderText(qstr(def->defaultValue));
        connect(e, &QLineEdit::editingFinished, this, [this, k, e, old = qstr(mine.value_or(""))] {
            if (e->text() == old) return;
            commitAttribute(k, e->text().isEmpty() ? std::nullopt : std::optional<std::string>(sstr(e->text())));
        });
        tree_->setItemWidget(item, 1, e);
        displayed_[qstr(k)] = qstr(mine.value_or(""));
    }
    auto* addItem = new QTreeWidgetItem(sec, {tr("Add attribute…")});
    auto* b = new QToolButton;
    b->setText(tr("Add"));
    connect(b, &QToolButton::clicked, this, [this] {
        bool ok = false;
        const QString key = QInputDialog::getText(this, tr("Add attribute"), tr("Key (letters, digits, _):"), QLineEdit::Normal, {}, &ok);
        if (!ok || key.isEmpty()) return;
        const QString val = QInputDialog::getText(this, tr("Add attribute"), tr("Value of %1:").arg(key), QLineEdit::Normal, {}, &ok);
        if (!ok) return;
        commitAttribute(sstr(key), sstr(val));
    });
    tree_->setItemWidget(addItem, 1, b);
}

bool PropertiesPanel::commitAttribute(const std::string& key, const std::optional<std::string>& value) {
    const std::string type = type_;
    auto docs = docs_;
    const QString what = tr("Edit attribute %1").arg(qstr(key));
    const bool ok = ctx_.tryEdit(what, [&] {
        if (!isValidAttributeKey(key)) throw ValidationError("Invalid attribute key '" + key + "'");
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), sstr(what));
        for (auto& d : docs) {
            auto& a = d["children"]["attributes"];
            if (a.is_array()) {
                json rows = json::array();
                bool done = false;
                for (auto row : a) {
                    if (row.value("key", std::string()) == key) {
                        done = true;
                        if (!value) continue;
                        row["value"] = *value;
                    }
                    rows.push_back(row);
                }
                if (!done && value) rows.push_back({{"key", key}, {"value", *value}});
                a = rows;
            } else {
                if (value) a[key] = *value;
                else a.erase(key);
            }
            if (type == "GeometryObject") {
                auto g = persistence::decodeEntity<GeometryObject>(d.dump());
                mine_design::updateGeometryWithImpact(p, g, sstr(what));
            } else {
                persistence::putDocument(p, type, d.dump(), sstr(what));
            }
        }
        macro.commit();
    });
    if (!ok) scheduleRebuild();
    return ok;
}

// ---- vertices -----------------------------------------------------------------------------------

void PropertiesPanel::buildVertices(const GeometryObject& g) {
    const bool editable = g.type == GeometryType::Point || g.type == GeometryType::Line || g.type == GeometryType::Polyline || g.type == GeometryType::Spline;
    if (!editable || g.vertices.size() > 5000) return;
    const auto* layer = ctx_.project().model().layers.find(g.layerId);
    vertexBox_->show();
    vertices_->setRowCount(static_cast<int>(g.vertices.size()));
    for (int i = 0; i < static_cast<int>(g.vertices.size()); ++i) {
        const auto& v = g.vertices[static_cast<std::size_t>(i)];
        const double c[3] = {v.x, v.y, v.z};
        for (int k = 0; k < 3; ++k) {
            auto* it = new QTableWidgetItem(QString::number(c[k], 'f', 4));
            it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            vertices_->setItem(i, k, it);
        }
    }
    vertices_->setEnabled(!(layer && layer->locked));
    if (layer && layer->locked) vertices_->setToolTip(tr("The layer is locked"));
}

bool PropertiesPanel::commitVertices() {
    const auto prim = ctx_.primary();
    if (prim.kind != EntityKind::Geometry) return false;
    const auto* g = ctx_.project().model().geometry.find(prim.id);
    if (!g) return false;
    GeometryObject n = *g;
    n.vertices.clear();
    for (int r = 0; r < vertices_->rowCount(); ++r) {
        Vec3 v;
        for (int k = 0; k < 3; ++k) {
            bool ok = false;
            const double d = vertices_->item(r, k) ? QLocale::c().toDouble(vertices_->item(r, k)->text(), &ok) : 0.0;
            if (!ok || !std::isfinite(d)) {
                ctx_.report(tr("Vertex %1: '%2' is not a number").arg(r + 1).arg(vertices_->item(r, k) ? vertices_->item(r, k)->text() : QString()), MessageLevel::Error);
                scheduleRebuild();
                return false;
            }
            v[k] = d;
        }
        n.vertices.push_back(v);
    }
    // Bulges follow their vertices; inserted vertices get straight segments.
    if (!n.bulges.empty()) n.bulges.resize(n.vertices.size(), 0.0);
    const bool ok = ctx_.tryEdit(tr("Edit vertices"), [&] { mine_design::updateGeometryWithImpact(ctx_.project(), n, "Edit vertices"); });
    if (!ok) scheduleRebuild();
    return ok;
}

// ---- programmatic access ------------------------------------------------------------------------

bool PropertiesPanel::setField(const QString& column, const QString& value) {
    if (docs_.empty()) rebuild();
    if (docs_.empty()) return false;
    const std::string col = sstr(column);
    const json& cols = docs_.front().contains("columns") ? docs_.front()["columns"] : docs_.front();
    if (!cols.contains(col)) {
        // Custom attribute?
        if (docs_.front().contains("children") && docs_.front()["children"].contains("attributes"))
            return commitAttribute(col, value.isEmpty() ? std::nullopt : std::optional<std::string>(sstr(value)));
        ctx_.report(tr("No field '%1' on %2").arg(column, qstr(type_)), MessageLevel::Error);
        return false;
    }
    const FieldInfo fi = fieldInfo(type_, col);
    if (fi.readOnly) {
        ctx_.report(tr("'%1' is read-only: %2").arg(fi.label, fi.readOnlyReason), MessageLevel::Error);
        return false;
    }
    const json& cur = cols[col];
    json v;
    const QString t = value.trimmed();
    if (t.isEmpty() && (fi.kind == K::Reference || fi.kind == K::Date || fi.kind == K::DateTime || col == "color_override")) v = nullptr;
    else if (fi.kind == K::Bool) v = (t == "1" || t.compare("true", Qt::CaseInsensitive) == 0 || t.compare("yes", Qt::CaseInsensitive) == 0) ? 1 : 0;
    else if (fi.kind == K::Color) {
        bool ok = false;
        const auto c = QString(t).remove('#').toUInt(&ok, 16);
        if (!ok) return false;
        v = c;
    } else if (cur.is_number_integer()) {
        bool ok = false;
        const long long i = t.toLongLong(&ok);
        if (!ok) {
            ctx_.report(tr("'%1' is not an integer").arg(t), MessageLevel::Error);
            return false;
        }
        v = i;
    } else if (cur.is_number()) {
        bool ok = false;
        const double d = QLocale::c().toDouble(t, &ok);
        if (!ok) {
            ctx_.report(tr("'%1' is not a number").arg(t), MessageLevel::Error);
            return false;
        }
        v = d;
    } else if (fi.kind == K::Reference) {
        // Accept a uuid or a display name.
        if (auto id = Uuid::parse(sstr(t))) v = id->toString();
        else {
            for (const auto& [cid, name] : candidates(ctx_.project().model(), fi.referenceType))
                if (name == t) v = cid.toString();
            if (v.is_null()) {
                ctx_.report(tr("No %1 named '%2'").arg(qstr(fi.referenceType), t), MessageLevel::Error);
                return false;
            }
        }
    } else if (fi.kind == K::Enum && !fi.choices.contains(t)) {
        ctx_.report(tr("'%1' is not one of: %2").arg(t, fi.choices.join(", ")), MessageLevel::Error);
        return false;
    } else {
        v = sstr(t);
    }
    return commit(col, v, tr("Edit %1").arg(fi.label.toLower()));
}

QString PropertiesPanel::value(const QString& key) const {
    auto it = displayed_.find(key);
    return it == displayed_.end() ? QString() : it->second;
}

QStringList PropertiesPanel::columns() const {
    QStringList out;
    if (docs_.empty()) return out;
    const json& cols = docs_.front().contains("columns") ? docs_.front()["columns"] : docs_.front();
    for (auto it = cols.begin(); it != cols.end(); ++it) out << qstr(it.key());
    return out;
}

} // namespace openmine::app
