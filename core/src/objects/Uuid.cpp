// SPDX-License-Identifier: Apache-2.0
#include "openmine/objects/Uuid.h"

#include <mutex>
#include <random>

namespace openmine {

namespace {
std::mt19937_64& engine() {
    // Seeded once from the OS entropy source. Access is serialised below.
    static std::mt19937_64 eng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    return eng;
}
std::mutex& engineMutex() {
    static std::mutex m;
    return m;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

Uuid Uuid::generate() {
    Uuid id;
    {
        std::lock_guard lock(engineMutex());
        std::uint64_t a = engine()();
        std::uint64_t b = engine()();
        for (int i = 0; i < 8; ++i) {
            id.bytes_[i] = static_cast<std::uint8_t>(a >> (8 * i));
            id.bytes_[8 + i] = static_cast<std::uint8_t>(b >> (8 * i));
        }
    }
    id.bytes_[6] = static_cast<std::uint8_t>((id.bytes_[6] & 0x0F) | 0x40); // version 4
    id.bytes_[8] = static_cast<std::uint8_t>((id.bytes_[8] & 0x3F) | 0x80); // RFC variant
    return id;
}

std::optional<Uuid> Uuid::parse(std::string_view text) {
    if (text.size() != 36) return std::nullopt;
    Uuid id;
    int byteIndex = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return std::nullopt;
            ++i;
            continue;
        }
        int hi = hexValue(text[i]);
        int lo = hexValue(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        id.bytes_[byteIndex++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
    }
    return id;
}

std::string Uuid::toString() const {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[bytes_[i] >> 4]);
        out.push_back(hex[bytes_[i] & 0x0F]);
    }
    return out;
}

bool Uuid::isNil() const {
    for (auto b : bytes_)
        if (b != 0) return false;
    return true;
}

} // namespace openmine
