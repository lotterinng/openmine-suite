#pragma once
// OpenMine Suite — globally unique identifiers for persistent project entities.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace openmine {

/// RFC 9562 version-4 (random) UUID. A default-constructed Uuid is the nil UUID
/// and is used to represent "no reference".
class Uuid {
public:
    Uuid() = default;

    /// Generate a new random (v4) UUID.
    static Uuid generate();

    /// Parse the canonical 8-4-4-4-12 hexadecimal form (case-insensitive).
    static std::optional<Uuid> parse(std::string_view text);

    /// Canonical lowercase 36-character representation.
    std::string toString() const;

    bool isNil() const;
    explicit operator bool() const { return !isNil(); }

    const std::array<std::uint8_t, 16>& bytes() const { return bytes_; }

    friend bool operator==(const Uuid&, const Uuid&) = default;
    friend auto operator<=>(const Uuid&, const Uuid&) = default;

private:
    std::array<std::uint8_t, 16> bytes_{};
};

} // namespace openmine

template <>
struct std::hash<openmine::Uuid> {
    std::size_t operator()(const openmine::Uuid& id) const noexcept {
        std::size_t h = 1469598103934665603ULL;
        for (auto b : id.bytes()) {
            h ^= b;
            h *= 1099511628211ULL;
        }
        return h;
    }
};
