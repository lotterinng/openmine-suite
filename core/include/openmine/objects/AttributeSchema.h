#pragma once
// SPDX-License-Identifier: Apache-2.0
// Schema-driven custom attributes. Definitions are project data (persisted), so mines
// can add their own fields without code changes. Values are stored as canonical text
// and validated against the definition's type.

#include "openmine/objects/Entities.h"

#include <string>
#include <vector>

namespace openmine {

enum class AttributeValueType { Text, Real, Integer, Boolean, Date };

struct AttributeDefinition {
    Uuid id;
    std::string entityType;  ///< "GeometryObject" or "MineDevelopment"
    std::string key;         ///< machine name, [A-Za-z_][A-Za-z0-9_]*
    std::string displayName;
    AttributeValueType valueType = AttributeValueType::Text;
    std::string unit;
    bool required = false;
    std::string defaultValue;
};

const char* toString(AttributeValueType t);
std::optional<AttributeValueType> attributeValueTypeFromString(const std::string& s);

/// Returns an empty string when valid, otherwise a human-readable error.
std::string validateAttributeValue(const AttributeDefinition& def, const std::string& value);
bool isValidAttributeKey(const std::string& key);

} // namespace openmine
