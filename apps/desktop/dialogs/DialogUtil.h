#pragma once
// SPDX-License-Identifier: Apache-2.0
// Small helpers shared by the workflow dialogs.

#include "../app/AppContext.h"

#include <QDate>
#include <QString>

#include <nlohmann/json.hpp>

#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QSpinBox;
class QWidget;

namespace openmine::app {

/// Fills a combo with the entities of a kind (data = uuid text). `filter` restricts the list.
void fillEntityCombo(QComboBox* combo, const ProjectModel& m, EntityKind kind, const Uuid& select = {}, bool allowNone = false,
                     const std::function<bool(const Uuid&)>& filter = {});
Uuid comboUuid(const QComboBox* combo);
QDoubleSpinBox* spin(double min, double max, double value, int decimals = 3, const QString& suffix = {}, QWidget* parent = nullptr);
QSpinBox* ispin(int min, int max, int value, QWidget* parent = nullptr);
QDate toQDate(Date d);
Date fromQDate(const QDate& d);

/// Stores a reproducible analysis result (inputs, parameters, summary, optional payload) — one undo step.
Uuid storeAnalysis(Project& p, AnalysisKind kind, const std::string& name, const nlohmann::json& inputs, const nlohmann::json& parameters,
                   const nlohmann::json& summary, const nlohmann::json& payload = nlohmann::json());

/// The layer that receives the objects a workflow creates (e.g. "As-built", "Surfaces"): an unlocked
/// layer of that name, created when missing. Call inside the workflow's undo step.
Uuid workflowLayer(AppContext& ctx, const std::string& fallback, std::uint32_t color);

/// Makes sure the project schema defines a custom attribute (adds the definition when missing).
/// Call inside the workflow's undo step, before setting the attribute on an entity.
void ensureAttribute(Project& p, const std::string& entityType, const std::string& key, const std::string& displayName,
                     AttributeValueType type = AttributeValueType::Text, const std::string& unit = {});

} // namespace openmine::app
