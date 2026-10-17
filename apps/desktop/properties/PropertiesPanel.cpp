// SPDX-License-Identifier: Apache-2.0
#include "PropertiesPanel.h"

#include "../theme/Theme.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"
#ifdef OPENMINE_HAS_OCCT
#include "openmine/occt/SweptSolid.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QTimer>

namespace openmine::app {

namespace {
const QString kSection = QStringLiteral("#section");

std::optional<double> toNumber(const QString& s) {
    bool ok = false;
    const double v = s.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(v)) return std::nullopt;
    return v;
}
double requireNumber(const QString& s, const QString& what) {
    auto v = toNumber(s);
    if (!v) throw ValidationError(sstr(what + " must be a number"));
    return *v;
}
bool toBool(const QString& s) { return s == "true" || s == "1" || s.compare("yes", Qt::CaseInsensitive) == 0; }
QString yesNo(bool b) { return b ? "true" : "false"; }
QString hexColor(std::uint32_t rgb) { return QStringLiteral("#%1").arg(rgb, 6, 16, QLatin1Char('0')).toUpper(); }
} // namespace

PropertiesPanel::PropertiesPanel(AppContext& ctx, QWidget* parent) : QTableWidget(parent), ctx_(ctx) {
    setColumnCount(2);
    setHorizontalHeaderLabels({"Property", "Value"});
    horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    horizontalHeader()->setStretchLastSection(true);
    verticalHeader()->hide();
    verticalHeader()->setDefaultSectionSize(24);
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked |
                    QAbstractItemView::AnyKeyPressed);
    connect(&ctx_, &AppContext::selectionChanged, this, &PropertiesPanel::rebuild);
    connect(&ctx_, &AppContext::projectReplaced, this, &PropertiesPanel::rebuild);
    connect(&ctx_, &AppContext::modelChanged, this, &PropertiesPanel::scheduleRebuild);
    connect(&ctx_, &AppContext::scheduleRecalculated, this, &PropertiesPanel::scheduleRebuild);
    connect(this, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (building_ || item->column() != 1) return;
        const auto& r = rows_[static_cast<std::size_t>(item->row())];
        if (r.kind == Kind::ReadOnly) return;
        if (item->text() == r.value) return;
        commit(r.key, item->text());
    });
    rebuild();
}

void PropertiesPanel::scheduleRebuild() {
    if (pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        if (state() != QAbstractItemView::EditingState) rebuild();
    });
}

void PropertiesPanel::section(const QString& title) { rows_.push_back({kSection, title, {}, Kind::ReadOnly, {}, {}}); }
void PropertiesPanel::row(const QString& key, const QString& label, const QString& value, Kind kind,
                          const QStringList& choices, const QString& tooltip) {
    rows_.push_back({key, label, value, kind, choices, tooltip});
}
void PropertiesPanel::readOnly(const QString& label, const QString& value, const QString& tooltip) {
    rows_.push_back({{}, label, value, Kind::ReadOnly, {}, tooltip});
}

QString PropertiesPanel::value(const QString& key) const {
    for (const auto& r : rows_)
        if (r.key == key) return r.value;
    return {};
}

QStringList PropertiesPanel::keys() const {
    QStringList out;
    for (const auto& r : rows_)
        if (!r.key.isEmpty() && r.key != kSection && r.kind != Kind::ReadOnly) out << r.key;
    return out;
}

void PropertiesPanel::rebuild() {
    building_ = true;
    rows_.clear();
    clearContents();
    setRowCount(0);
    if (!ctx_.hasProject()) {
        building_ = false;
        return;
    }
    const auto& m = ctx_.project().model();
    const auto& sel = ctx_.selection();

    auto meta = [&](const EntityMetadata& md) {
        section("Metadata");
        readOnly("Created", qstr(md.createdAt));
        readOnly("Modified", qstr(md.modifiedAt));
        readOnly("Revision", QString::number(md.revision));
    };
    auto attributes = [&](const char* entityType, const AttributeMap& attrs) {
        bool any = false;
        for (const auto& def : m.attributeDefinitions.all()) {
            if (def.entityType != entityType) continue;
            if (!any) section("Custom attributes");
            any = true;
            auto it = attrs.find(def.key);
            Kind k = def.valueType == AttributeValueType::Boolean ? Kind::Bool
                     : def.valueType == AttributeValueType::Real || def.valueType == AttributeValueType::Integer ? Kind::Number
                     : def.valueType == AttributeValueType::Date ? Kind::Date : Kind::Text;
            row("attr." + qstr(def.key), qstr(def.displayName) + (def.unit.empty() ? "" : " [" + qstr(def.unit) + "]"),
                it == attrs.end() ? QString() : qstr(it->second), k, {}, qstr(toString(def.valueType)));
        }
    };

    if (sel.isEmpty() || sel.kind == EntityKind::ProjectInfo) {
        const auto& i = m.info;
        section("Project");
        readOnly("Project ID", qstr(i.id.toString()));
        row("project.name", "Name", qstr(i.name), Kind::Text);
        row("project.description", "Description", qstr(i.description), Kind::Text);
        readOnly("File", ctx_.project().filePath().empty() ? "(not saved)" : QString::fromStdU16String(ctx_.project().filePath().u16string()));
        section("Schedule");
        row("project.start", "Schedule start", qstr(i.scheduleStart.toString()), Kind::Date);
        row("project.status", "Status (data) date", i.statusDate ? qstr(i.statusDate->toString()) : QString(), Kind::Date, {},
            "When set, remaining work is reforecast from this date");
        QStringList cals;
        QString current;
        for (const auto& c : m.calendars.all()) {
            cals << qstr(c.name);
            if (c.id == i.defaultCalendarId) current = qstr(c.name);
        }
        row("project.calendar", "Default calendar", current, Kind::Choice, cals);
        section("Coordinate reference system (declared)");
        row("crs.name", "CRS name", qstr(i.crs.name), Kind::Text, {}, "Declared by the user; never inferred from coordinates");
        row("crs.code", "Authority code", qstr(i.crs.authorityCode), Kind::Text, {}, "e.g. EPSG:2051 (Hartebeesthoek94 / Lo25)");
        row("crs.vertical", "Vertical datum", qstr(i.crs.verticalDatum), Kind::Text);
        readOnly("Linear unit", qstr(i.crs.linearUnit));
        row("crs.localGrid", "Local mine grid", yesNo(i.crs.isLocalMineGrid), Kind::Bool);
        row("crs.southAxes", "South-orientated (Lo) axes", yesNo(i.crs.southOrientatedAxes), Kind::Bool);
        meta(i.meta);
    } else if (sel.kind == EntityKind::Geometry) {
        const auto* g = m.geometry.find(sel.id);
        if (g) {
            section("Identification");
            readOnly("Object ID", qstr(g->id.toString()));
            readOnly("Geometry type", toString(g->type));
            row("name", "Name", qstr(g->name), Kind::Text);
            QStringList layerNames;
            QString layerName;
            for (const auto& l : m.layers.all()) {
                layerNames << qstr(l.name);
                if (l.id == g->layerId) layerName = qstr(l.name);
            }
            row("layer", "Layer", layerName, Kind::Choice, layerNames);
            if (g->type == GeometryType::Polyline) row("closed", "Closed", yesNo(g->closed), Kind::Bool);
            section("Dimensions (m)");
            readOnly("Length (3D)", formatNumber(geometry::length3d(g->vertices, g->closed)));
            readOnly("Plan length", formatNumber(geometry::horizontalLength(g->vertices, g->closed)));
            auto grad = geometry::gradientPercent(g->vertices);
            readOnly("Gradient", grad ? formatNumber(*grad, 2) + " %" : "n/a (vertical/degenerate)");
            if (const auto* dev = m.developmentForGeometry(g->id)) readOnly("Centreline of", qstr(dev->headingId));
            section(QStringLiteral("Coordinates — %1 vertices").arg(g->vertices.size()));
            for (std::size_t k = 0; k < g->vertices.size(); ++k) {
                const auto& v = g->vertices[k];
                const QString base = QStringLiteral("v%1.").arg(k);
                row(base + "x", QStringLiteral("V%1  X").arg(k + 1), formatNumber(v.x), Kind::Number);
                row(base + "y", QStringLiteral("V%1  Y").arg(k + 1), formatNumber(v.y), Kind::Number);
                row(base + "z", QStringLiteral("V%1  Z").arg(k + 1), formatNumber(v.z), Kind::Number);
            }
            attributes("GeometryObject", g->attributes);
            meta(g->meta);
        }
    } else if (sel.kind == EntityKind::Development) {
        const auto* d = m.developments.find(sel.id);
        if (d) {
            const auto mt = mine_design::computeMetrics(ctx_.project(), *d);
            section("Development heading");
            readOnly("Object ID", qstr(d->id.toString()));
            row("headingId", "Heading ID", qstr(d->headingId), Kind::Text);
            row("name", "Heading name", qstr(d->name), Kind::Text);
            row("zone", "Zone", qstr(d->zone), Kind::Text);
            row("declineNumber", "Decline number", qstr(d->declineNumber), Kind::Text);
            row("level", "Level", qstr(d->level), Kind::Text);
            row("activityType", "Activity type", qstr(d->activityType), Kind::Choice,
                {"Development", "Decline", "Ramp", "Crosscut", "Ore drive", "Access drive", "Footwall drive", "Raise"});
            row("status", "Status", qstr(d->status), Kind::Choice, {"Planned", "In progress", "Complete", "Suspended"});
            row("designRevision", "Design revision", qstr(d->designRevision), Kind::Text);
            section("Design");
            row("width", "Design width (m)", formatNumber(d->designWidth, 2), Kind::Number);
            row("height", "Design height (m)", formatNumber(d->designHeight, 2), Kind::Number);
            row("chainageFrom", "Chainage from (m)", formatNumber(d->chainageFrom), Kind::Number);
            readOnly("Chainage to (m)", formatNumber(mt.chainageTo), "Derived: chainage from + centreline length");
            readOnly("Length (m)", formatNumber(mt.length3d), "Derived from centreline geometry (slope length)");
            readOnly("Gradient", mt.gradientPercent ? formatNumber(*mt.gradientPercent, 2) + " %" : "n/a");
            readOnly("Nominal volume (m³)", formatNumber(mt.nominalVolume, 1), "width × height × length (rectangular profile)");
#ifdef OPENMINE_HAS_OCCT
            if (const auto* g = m.geometry.find(d->geometryId)) {
                auto solid = occt::sweepRectangularProfile(g->vertices, d->designWidth, d->designHeight);
                readOnly("Swept solid volume (m³)", solid.ok ? formatNumber(solid.volume, 1) + (solid.valid ? "" : "  (solid not valid)") : "unavailable",
                         solid.ok ? "Open CASCADE sweep of the design profile along the centreline" : qstr(solid.error));
            }
#else
            readOnly("Swept solid volume (m³)", "unavailable", "Built without Open CASCADE");
#endif
            readOnly("Approved schedule length (m)", formatNumber(d->approvedDesignLength));
            readOnly("Centreline geometry", qstr(d->geometryId.toString()));
            section("Schedule links");
            const auto acts = m.activitiesForDevelopment(d->id);
            if (acts.empty()) readOnly("Linked activities", "none — use Scheduler › Linked Activity");
            for (const auto& aid : acts)
                if (const auto* a = m.activities.find(aid))
                    readOnly("Activity", qstr(a->name) + (a->designImpactFlag ? "  ⚠ design changed" : ""), qstr(aid.toString()));
            attributes("MineDevelopment", d->attributes);
            meta(d->meta);
        }
    } else if (sel.kind == EntityKind::Activity) {
        const auto* a = m.activities.find(sel.id);
        if (a) {
            section("Activity");
            readOnly("Activity ID", qstr(a->id.toString()));
            row("name", "Name", qstr(a->name), Kind::Text);
            row("wbsCode", "WBS code", qstr(a->wbsCode), Kind::Text);
            row("kind", "Type", toString(a->kind), Kind::Choice, {"Task", "Milestone", "Summary"});
            if (a->kind == ActivityKind::Task) {
                section("Duration and quantities");
                row("durationFromRate", "Duration from rate", yesNo(a->durationFromRate), Kind::Bool,
                    {}, "Duration = remaining quantity / production rate");
                row("durationDays", "Duration (working days)", formatNumber(a->durationDays, 2),
                    a->durationFromRate ? Kind::ReadOnly : Kind::Number);
                row("quantity", "Planned quantity", formatNumber(a->quantity, 2), Kind::Number);
                row("quantityUnit", "Unit", qstr(a->quantityUnit), Kind::Text);
                row("ratePerDay", "Rate (unit / working day)", formatNumber(a->ratePerDay, 2), Kind::Number);
                row("actualQuantity", "Actual quantity", formatNumber(a->actualQuantity, 2), Kind::Number);
            }
            row("snet", "Start no earlier than", a->startNoEarlierThan ? qstr(a->startNoEarlierThan->toString()) : QString(), Kind::Date);
            const auto& sched = ctx_.schedule();
            auto it = sched.timings.find(a->id);
            section("Calculated (CPM)");
            if (!sched.ok) readOnly("Schedule", sched.errors.empty() ? "not calculated" : qstr(sched.errors.front()));
            else if (it != sched.timings.end()) {
                const auto& t = it->second;
                readOnly("Calc. duration (d)", formatNumber(t.duration, 2));
                readOnly("Start", qstr(t.startDate.toString()));
                readOnly("Finish", qstr(t.finishDate.toString()));
                readOnly("Total float (d)", formatNumber(t.totalFloat, 2));
                readOnly("Free float (d)", formatNumber(t.freeFloat, 2));
                readOnly("Critical", t.critical ? "yes" : "no");
                readOnly("Remaining quantity", formatNumber(t.remainingQuantity, 2));
                readOnly("Complete", formatNumber(t.percentComplete, 1) + " %");
            }
            section("Design links");
            const auto devs = m.developmentsForActivity(a->id);
            if (devs.empty()) readOnly("Linked design", "none");
            for (const auto& did : devs)
                if (const auto* d = m.developments.find(did)) readOnly("Heading", qstr(d->headingId), qstr(did.toString()));
            readOnly("Design impact", a->designImpactFlag ? "⚠ " + qstr(a->designImpactNote) : "none");
            meta(a->meta);
        }
    } else if (sel.kind == EntityKind::Layer) {
        const auto* l = m.layers.find(sel.id);
        if (l) {
            section("Layer");
            readOnly("Layer ID", qstr(l->id.toString()));
            row("name", "Name", qstr(l->name), Kind::Text);
            row("color", "Colour (#RRGGBB)", hexColor(l->colorRgb), Kind::Text);
            row("visible", "Visible", yesNo(l->visible), Kind::Bool);
            row("locked", "Locked", yesNo(l->locked), Kind::Bool);
            meta(l->meta);
        }
    }

    // ---- Materialise rows.
    const auto& c = Theme::colors();
    setRowCount(static_cast<int>(rows_.size()));
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const auto& r = rows_[static_cast<std::size_t>(i)];
        if (r.key == kSection) {
            auto* item = new QTableWidgetItem(r.label);
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
            item->setFlags(Qt::ItemIsEnabled);
            item->setBackground(c.panelAlt);
            item->setForeground(c.accent);
            setItem(i, 0, item);
            setSpan(i, 0, 1, 2);
            continue;
        }
        auto* label = new QTableWidgetItem(r.label);
        label->setFlags(Qt::ItemIsEnabled);
        if (!r.tooltip.isEmpty()) label->setToolTip(r.tooltip);
        setItem(i, 0, label);
        if (r.kind == Kind::Bool) {
            auto* box = new QCheckBox(this);
            box->setChecked(toBool(r.value));
            const QString key = r.key;
            connect(box, &QCheckBox::toggled, this, [this, key](bool on) { commit(key, yesNo(on)); });
            setCellWidget(i, 1, box);
        } else if (r.kind == Kind::Choice) {
            auto* combo = new QComboBox(this);
            combo->addItems(r.choices);
            combo->setCurrentText(r.value);
            const QString key = r.key;
            connect(combo, &QComboBox::currentTextChanged, this, [this, key](const QString& t) { commit(key, t); });
            setCellWidget(i, 1, combo);
        } else {
            auto* item = new QTableWidgetItem(r.value);
            if (r.kind == Kind::ReadOnly) {
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                item->setForeground(c.textMuted);
            } else {
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsEditable);
                if (r.kind == Kind::Date) item->setToolTip("YYYY-MM-DD (empty to clear)");
            }
            if (!r.tooltip.isEmpty()) item->setToolTip(r.tooltip);
            setItem(i, 1, item);
        }
    }
    building_ = false;
}

bool PropertiesPanel::setProperty(const QString& key, const QString& value) {
    bool known = false;
    for (const auto& r : rows_)
        if (r.key == key && r.kind != Kind::ReadOnly) known = true;
    if (!known) {
        ctx_.report(QStringLiteral("Property '%1' is not editable for the current selection").arg(key), true);
        return false;
    }
    const bool ok = commit(key, value);
    rebuild();
    return ok;
}

bool PropertiesPanel::commit(const QString& key, const QString& v) {
    if (building_ || !ctx_.hasProject()) return false;
    auto& p = ctx_.project();
    const auto& m = p.model();
    const auto& sel = ctx_.selection();
    ProjectEditor ed(p);
    try {
        auto setAttr = [&](AttributeMap& attrs) {
            const std::string k = sstr(key.mid(5));
            if (v.isEmpty()) attrs.erase(k);
            else attrs[k] = sstr(v);
        };
        if (sel.isEmpty() || sel.kind == EntityKind::ProjectInfo) {
            ProjectInfo i = m.info;
            if (key == "project.name") i.name = sstr(v);
            else if (key == "project.description") i.description = sstr(v);
            else if (key == "project.start") {
                auto d = Date::parse(sstr(v.trimmed()));
                if (!d) throw ValidationError("Schedule start must be a date (YYYY-MM-DD)");
                i.scheduleStart = *d;
            } else if (key == "project.status") {
                if (v.trimmed().isEmpty()) i.statusDate.reset();
                else {
                    auto d = Date::parse(sstr(v.trimmed()));
                    if (!d) throw ValidationError("Status date must be a date (YYYY-MM-DD) or empty");
                    i.statusDate = *d;
                }
            } else if (key == "project.calendar") {
                for (const auto& c : m.calendars.all())
                    if (qstr(c.name) == v) i.defaultCalendarId = c.id;
            } else if (key == "crs.name") i.crs.name = sstr(v);
            else if (key == "crs.code") i.crs.authorityCode = sstr(v.trimmed());
            else if (key == "crs.vertical") i.crs.verticalDatum = sstr(v);
            else if (key == "crs.localGrid") i.crs.isLocalMineGrid = toBool(v);
            else if (key == "crs.southAxes") i.crs.southOrientatedAxes = toBool(v);
            ed.setProjectInfo(i, "Edit project " + sstr(key));
        } else if (sel.kind == EntityKind::Geometry) {
            GeometryObject g = *m.geometry.find(sel.id);
            if (key == "name") g.name = sstr(v);
            else if (key == "layer") {
                const auto* l = ed.findLayerByName(sstr(v));
                if (!l) throw ValidationError("No such layer");
                g.layerId = l->id;
            } else if (key == "closed") g.closed = toBool(v);
            else if (key.startsWith("attr.")) setAttr(g.attributes);
            else if (key.startsWith('v')) {
                const auto idx = key.mid(1).section('.', 0, 0).toULong();
                const QString axis = key.section('.', 1, 1);
                if (idx >= g.vertices.size()) throw ValidationError("No such vertex");
                const double val = requireNumber(v, "Coordinate");
                if (axis == "x") g.vertices[idx].x = val;
                else if (axis == "y") g.vertices[idx].y = val;
                else g.vertices[idx].z = val;
            }
            const int flagged = mine_design::updateGeometryWithImpact(p, g, "Edit " + g.name + " " + sstr(key));
            if (flagged > 0)
                ctx_.report(QStringLiteral("Design change affects %1 schedule activit%2 — flagged for review (quantities NOT changed). "
                                           "Use Scheduler › Apply Design Quantities to recalculate.")
                                .arg(flagged)
                                .arg(flagged == 1 ? "y" : "ies"));
        } else if (sel.kind == EntityKind::Development) {
            MineDevelopment d = *m.developments.find(sel.id);
            if (key == "headingId") d.headingId = sstr(v.trimmed());
            else if (key == "name") d.name = sstr(v);
            else if (key == "zone") d.zone = sstr(v);
            else if (key == "declineNumber") d.declineNumber = sstr(v);
            else if (key == "level") d.level = sstr(v);
            else if (key == "activityType") d.activityType = sstr(v);
            else if (key == "status") d.status = sstr(v);
            else if (key == "designRevision") d.designRevision = sstr(v);
            else if (key == "width") d.designWidth = requireNumber(v, "Width");
            else if (key == "height") d.designHeight = requireNumber(v, "Height");
            else if (key == "chainageFrom") d.chainageFrom = requireNumber(v, "Chainage");
            else if (key.startsWith("attr.")) setAttr(d.attributes);
            ed.updateDevelopment(d);
        } else if (sel.kind == EntityKind::Activity) {
            ScheduleActivity a = *m.activities.find(sel.id);
            if (key == "name") a.name = sstr(v);
            else if (key == "wbsCode") a.wbsCode = sstr(v);
            else if (key == "kind") {
                auto k = activityKindFromString(sstr(v));
                if (!k) throw ValidationError("Unknown activity type");
                a.kind = *k;
            } else if (key == "durationFromRate") a.durationFromRate = toBool(v);
            else if (key == "durationDays") a.durationDays = requireNumber(v, "Duration");
            else if (key == "quantity") a.quantity = requireNumber(v, "Quantity");
            else if (key == "quantityUnit") a.quantityUnit = sstr(v);
            else if (key == "ratePerDay") a.ratePerDay = requireNumber(v, "Rate");
            else if (key == "actualQuantity") a.actualQuantity = requireNumber(v, "Actual quantity");
            else if (key == "snet") {
                if (v.trimmed().isEmpty()) a.startNoEarlierThan.reset();
                else {
                    auto d = Date::parse(sstr(v.trimmed()));
                    if (!d) throw ValidationError("Date must be YYYY-MM-DD");
                    a.startNoEarlierThan = *d;
                }
            }
            ed.updateActivity(a);
        } else if (sel.kind == EntityKind::Layer) {
            CadLayer l = *m.layers.find(sel.id);
            if (key == "name") l.name = sstr(v.trimmed());
            else if (key == "visible") l.visible = toBool(v);
            else if (key == "locked") l.locked = toBool(v);
            else if (key == "color") {
                QString h = v.trimmed();
                if (h.startsWith('#')) h = h.mid(1);
                bool ok = false;
                const auto rgb = h.toUInt(&ok, 16);
                if (!ok || h.size() != 6) throw ValidationError("Colour must be #RRGGBB");
                l.colorRgb = rgb;
            }
            ed.updateLayer(l);
        }
        return true;
    } catch (const std::exception& e) {
        ctx_.report(QStringLiteral("Edit rejected: %1").arg(e.what()), true);
        QTimer::singleShot(0, this, &PropertiesPanel::rebuild); // restore displayed value
        return false;
    }
}

} // namespace openmine::app
