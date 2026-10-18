#pragma once
// SPDX-License-Identifier: Apache-2.0
// Presentation metadata of entities shared by the explorer, the property editor and the command
// console: names of kinds, document type names (persistence::EntityCodec), display names, icons,
// and per-column field descriptions of entity documents (label, editor kind, choices, units,
// reference targets, read-only rules that protect survey data, approved designs and baselines).

#include "AppContext.h"

#include <QString>
#include <QStringList>

#include <optional>
#include <string>

namespace openmine::app {

/// Document type of an entity kind ("GeometryObject" …; "ProjectInfo" for EntityKind::ProjectInfo).
std::string documentType(EntityKind k);
std::optional<EntityKind> kindForDocument(const std::string& type);
/// Every entity kind that has documents, in dependency order.
const std::vector<EntityKind>& entityKinds();

QString kindLabel(EntityKind k, bool plural = false);
/// Icon name for toolIcon().
QString kindIcon(EntityKind k);

/// The user-facing name of an entity (layer name, heading id, hole id, …).
QString displayName(const ProjectModel& m, const SelectionItem& item);
/// Kind label + name, e.g. "Development LEG2_675_D3".
QString describe(const ProjectModel& m, const SelectionItem& item);
/// Finds an entity by id in any store.
std::optional<EntityKind> findKind(const ProjectModel& m, const Uuid& id);

struct FieldInfo {
    enum class Kind { Text, MultiLine, Real, Integer, Bool, Enum, Reference, Date, DateTime, Color, Json, Binary, ReadOnly };
    Kind kind = Kind::Text;
    QString label;
    QString unit;
    QString tooltip;
    QStringList choices;          ///< Enum (and suggested values for Text)
    bool editableChoices = false; ///< Text with suggestions
    std::string referenceType;    ///< Reference: document type of the target
    bool readOnly = false;
    QString readOnlyReason;
    int group = 0;                ///< 0 identity, 1 main, 2 geometry/quantities, 3 links, 9 metadata
};

/// Description of one column of an entity document. `value` is the current JSON value text and
/// `isBoolInt` tells whether the stored value is a 0/1 integer.
FieldInfo fieldInfo(const std::string& docType, const std::string& column);

/// Human label for a column name ("design_width" → "Design width").
QString humanize(const std::string& column);

} // namespace openmine::app
