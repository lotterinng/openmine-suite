// SPDX-License-Identifier: Apache-2.0
#include "openmine/objects/AttributeSchema.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <system_error>

namespace openmine {

const char* toString(AttributeValueType t) {
    switch (t) {
    case AttributeValueType::Text: return "Text";
    case AttributeValueType::Real: return "Real";
    case AttributeValueType::Integer: return "Integer";
    case AttributeValueType::Boolean: return "Boolean";
    case AttributeValueType::Date: return "Date";
    }
    return "Text";
}

std::optional<AttributeValueType> attributeValueTypeFromString(const std::string& s) {
    if (s == "Text") return AttributeValueType::Text;
    if (s == "Real") return AttributeValueType::Real;
    if (s == "Integer") return AttributeValueType::Integer;
    if (s == "Boolean") return AttributeValueType::Boolean;
    if (s == "Date") return AttributeValueType::Date;
    return std::nullopt;
}

bool isValidAttributeKey(const std::string& key) {
    if (key.empty() || key.size() > 64) return false;
    if (!(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_')) return false;
    for (char c : key)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

std::string validateAttributeValue(const AttributeDefinition& def, const std::string& v) {
    if (v.empty()) return def.required ? "'" + def.displayName + "' is required" : "";
    switch (def.valueType) {
    case AttributeValueType::Text: return "";
    case AttributeValueType::Real: {
        double d = 0;
        auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), d);
        if (ec != std::errc() || p != v.data() + v.size() || !std::isfinite(d))
            return "'" + def.displayName + "' must be a number";
        return "";
    }
    case AttributeValueType::Integer: {
        long long i = 0;
        auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), i);
        if (ec != std::errc() || p != v.data() + v.size()) return "'" + def.displayName + "' must be an integer";
        return "";
    }
    case AttributeValueType::Boolean:
        return (v == "true" || v == "false") ? "" : "'" + def.displayName + "' must be true or false";
    case AttributeValueType::Date:
        return Date::parse(v) ? "" : "'" + def.displayName + "' must be a date (YYYY-MM-DD)";
    }
    return "";
}

} // namespace openmine
