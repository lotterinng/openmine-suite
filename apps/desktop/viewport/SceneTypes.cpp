// SPDX-License-Identifier: Apache-2.0
#include "SceneTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace openmine::app {

namespace {
using Stop = std::array<double, 3>;

std::uint32_t pack(double r, double g, double b) {
    auto c = [](double v) { return static_cast<std::uint32_t>(std::clamp(std::lround(v * 255.0), 0L, 255L)); };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}

std::uint32_t ramp(const std::vector<Stop>& stops, double t) {
    if (!std::isfinite(t)) return 0x707070;
    t = std::clamp(t, 0.0, 1.0);
    const double x = t * static_cast<double>(stops.size() - 1);
    const auto i = static_cast<std::size_t>(std::min(std::floor(x), static_cast<double>(stops.size() - 2)));
    const double f = x - static_cast<double>(i);
    const auto& a = stops[i];
    const auto& b = stops[i + 1];
    return pack(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f);
}

// Matplotlib viridis (CC0) sampled at 9 points.
const std::vector<Stop> kViridis = {{0.267, 0.005, 0.329}, {0.283, 0.141, 0.458}, {0.254, 0.265, 0.530}, {0.207, 0.372, 0.553},
                                    {0.164, 0.471, 0.558}, {0.128, 0.567, 0.551}, {0.135, 0.659, 0.518}, {0.478, 0.821, 0.318},
                                    {0.993, 0.906, 0.144}};
// Perceptual rainbow ("turbo"-like) control points.
const std::vector<Stop> kRainbow = {{0.190, 0.072, 0.232}, {0.275, 0.408, 0.859}, {0.111, 0.721, 0.871}, {0.188, 0.925, 0.522},
                                    {0.640, 0.988, 0.235}, {0.970, 0.776, 0.208}, {0.957, 0.384, 0.110}, {0.729, 0.106, 0.016},
                                    {0.480, 0.016, 0.010}};
// Blue–white–red diverging.
const std::vector<Stop> kDiverging = {{0.019, 0.188, 0.380}, {0.263, 0.576, 0.765}, {0.820, 0.898, 0.941}, {0.969, 0.969, 0.969},
                                      {0.992, 0.859, 0.780}, {0.839, 0.376, 0.302}, {0.404, 0.0, 0.122}};
const std::vector<Stop> kGrey = {{0.08, 0.08, 0.08}, {0.95, 0.95, 0.95}};

const std::array<std::uint32_t, 12> kCategorical = {0x4E79A7, 0xF28E2B, 0xE15759, 0x76B7B2, 0x59A14F, 0xEDC948,
                                                     0xB07AA1, 0xFF9DA7, 0x9C755F, 0xBAB0AC, 0x1F9E89, 0xD4A6C8};
} // namespace

std::uint32_t lutColor(Lut lut, double t) {
    switch (lut) {
    case Lut::Viridis: return ramp(kViridis, t);
    case Lut::Rainbow: return ramp(kRainbow, t);
    case Lut::Diverging: return ramp(kDiverging, t);
    case Lut::Grey: return ramp(kGrey, t);
    case Lut::Categorical: return categoricalColor(static_cast<std::size_t>(std::max(0.0, std::round(t * 11.0))));
    case Lut::Classification: return classificationColor(static_cast<int>(std::round(t)));
    }
    return 0x808080;
}

std::uint32_t categoricalColor(std::size_t index) { return kCategorical[index % kCategorical.size()]; }

std::uint32_t categoricalColor(const std::string& key) {
    // FNV-1a: stable across runs and platforms (std::hash is not).
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : key) {
        h ^= ch;
        h *= 1099511628211ull;
    }
    return categoricalColor(static_cast<std::size_t>(h % kCategorical.size()));
}

std::uint32_t classificationColor(int c) {
    // ASPRS LAS 1.4 standard point classes.
    switch (c) {
    case 0: return 0xB0B0B0;  // created, never classified
    case 1: return 0xC8C8C8;  // unclassified
    case 2: return 0xA0702A;  // ground
    case 3: return 0x9ACD32;  // low vegetation
    case 4: return 0x3CB043;  // medium vegetation
    case 5: return 0x1E7B1E;  // high vegetation
    case 6: return 0xE04040;  // building
    case 7: return 0xFF00FF;  // low point (noise)
    case 9: return 0x3070FF;  // water
    case 10: return 0x8B4513; // rail
    case 11: return 0x505050; // road surface
    case 13: return 0xFFD700; // wire guard
    case 14: return 0xFFA500; // wire conductor
    case 15: return 0x8A2BE2; // transmission tower
    case 17: return 0x708090; // bridge deck
    case 18: return 0xFF1493; // high noise
    default: return categoricalColor(static_cast<std::size_t>(c));
    }
}

std::uint32_t statusColor(const std::string& status, std::uint32_t planned) {
    if (status == "In progress") return 0xF2A93B;
    if (status == "Complete") return 0x4CB050;
    if (status == "Suspended") return 0x8A8A8A;
    return planned;
}

} // namespace openmine::app
