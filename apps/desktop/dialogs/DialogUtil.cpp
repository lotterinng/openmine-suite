// SPDX-License-Identifier: Apache-2.0
#include "DialogUtil.h"

#include "../app/EntityInfo.h"

#include "openmine/project/ProjectEditor.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>

namespace openmine::app {

void fillEntityCombo(QComboBox* combo, const ProjectModel& m, EntityKind kind, const Uuid& select, bool allowNone,
                     const std::function<bool(const Uuid&)>& filter) {
    combo->clear();
    if (allowNone) combo->addItem(QObject::tr("(none)"), QString());
    auto add = [&](const Uuid& id) {
        if (filter && !filter(id)) return;
        combo->addItem(displayName(m, {kind, id}), uuidText(id));
        if (id == select) combo->setCurrentIndex(combo->count() - 1);
    };
#define OPENMINE_FILL(T, member, Kind) \
    if (kind == EntityKind::Kind)      \
        for (const auto& e : m.member.all()) add(e.id);
    OPENMINE_ENTITY_TYPES(OPENMINE_FILL)
#undef OPENMINE_FILL
}

Uuid comboUuid(const QComboBox* combo) { return uuidFrom(combo->currentData().toString()); }

QDoubleSpinBox* spin(double min, double max, double value, int decimals, const QString& suffix, QWidget* parent) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setValue(value);
    if (!suffix.isEmpty()) s->setSuffix(suffix);
    s->setMinimumWidth(110);
    return s;
}

QSpinBox* ispin(int min, int max, int value, QWidget* parent) {
    auto* s = new QSpinBox(parent);
    s->setRange(min, max);
    s->setValue(value);
    return s;
}

QDate toQDate(Date d) {
    int y;
    unsigned m, dd;
    d.toYmd(y, m, dd);
    return QDate(y, static_cast<int>(m), static_cast<int>(dd));
}

Date fromQDate(const QDate& d) { return Date::fromYmd(d.year(), static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day())); }

Uuid storeAnalysis(Project& p, AnalysisKind kind, const std::string& name, const nlohmann::json& inputs, const nlohmann::json& parameters,
                   const nlohmann::json& summary, const nlohmann::json& payload) {
    AnalysisResult r;
    r.kind = kind;
    r.name = name;
    r.inputsJson = inputs.dump();
    r.parametersJson = parameters.dump();
    r.summaryJson = summary.dump();
    if (!payload.is_null()) r.payloadJson = payload.dump();
    r.createdAt = utcTimestampNow();
    r.user = p.userName();
    return ProjectEditor(p).add(r, "Store analysis: " + name);
}

Uuid workflowLayer(AppContext& ctx, const std::string& fallback, std::uint32_t color) {
    auto& p = ctx.project();
    ProjectEditor ed(p);
    (void)ctx;
    if (const auto* l = ed.findLayerByName(fallback); l && !l->locked) return l->id;
    return ed.addLayer(fallback, color);
}

void ensureAttribute(Project& p, const std::string& entityType, const std::string& key, const std::string& displayName, AttributeValueType type,
                     const std::string& unit) {
    for (const auto& d : p.model().attributeDefinitions.all())
        if (d.entityType == entityType && d.key == key) return;
    AttributeDefinition def;
    def.entityType = entityType;
    def.key = key;
    def.displayName = displayName;
    def.valueType = type;
    def.unit = unit;
    ProjectEditor(p).addAttributeDefinition(def);
}

} // namespace openmine::app
