#pragma once
// SPDX-License-Identifier: Apache-2.0
// SHA-256 (FIPS 180-4) for asset integrity checks.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace openmine::util {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::array<std::uint8_t, 32> digest();
    std::string hexDigest();

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::size_t bufLen_ = 0;
    std::uint64_t totalBits_ = 0;
    bool finished_ = false;
};

std::string sha256Hex(std::string_view data);
/// Streams a file; nullopt if it cannot be read.
std::optional<std::string> sha256File(const std::filesystem::path& file);

} // namespace openmine::util
