#pragma once
// SPDX-License-Identifier: Apache-2.0
// Shared building blocks of the unified OpenMine object model.

#include "openmine/geometry/Transform.h"
#include "openmine/geometry/Vec3.h"
#include "openmine/objects/DateTime.h"
#include "openmine/objects/Uuid.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openmine {

/// Free-form, schema-validated custom attributes (key → canonical text value).
using AttributeMap = std::map<std::string, std::string>;

struct EntityMetadata {
    std::string createdAt;     ///< ISO-8601 UTC
    std::string modifiedAt;    ///< ISO-8601 UTC
    std::int64_t revision = 1; ///< Incremented on every committed change.
    friend bool operator==(const EntityMetadata&, const EntityMetadata&) = default;
};

struct BoundingBox {
    Vec3 min;
    Vec3 max;
    Vec3 center() const { return (min + max) * 0.5; }
    Vec3 size() const { return max - min; }
    bool contains(Vec3 p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
    void expand(Vec3 p) {
        min = componentMin(min, p);
        max = componentMax(max, p);
    }
    void expand(const BoundingBox& b) {
        expand(b.min);
        expand(b.max);
    }
    bool intersects(const BoundingBox& b) const {
        return min.x <= b.max.x && max.x >= b.min.x && min.y <= b.max.y && max.y >= b.min.y && min.z <= b.max.z &&
               max.z >= b.min.z;
    }
    static BoundingBox empty() { return {{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}}; }
    bool isEmpty() const { return min.x > max.x; }
    friend bool operator==(const BoundingBox&, const BoundingBox&) = default;
};

/// A reference to a file in the project's sidecar asset folder (see core/assets).
struct AssetRef {
    Uuid id;
    friend bool operator==(const AssetRef&, const AssetRef&) = default;
    bool isNull() const { return id.isNil(); }
};

} // namespace openmine
