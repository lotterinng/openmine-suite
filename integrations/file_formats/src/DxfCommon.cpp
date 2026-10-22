// SPDX-License-Identifier: Apache-2.0
#include "DxfCommon.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <system_error>

namespace openmine::formats {

namespace {
// The AutoCAD Color Index palette (standard 256-entry table; index 0 is unused).
constexpr std::array<std::uint32_t, 256> kAci = {
    0x000000, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF, 0xFFFFFF,
    0x808080, 0xC0C0C0, 0xFF0000, 0xFF7F7F, 0xA50000, 0xA55252, 0x7F0000, 0x7F3F3F,
    0x4C0000, 0x4C2626, 0x260000, 0x261313, 0xFF3F00, 0xFF9F7F, 0xA52900, 0xA56752,
    0x7F1F00, 0x7F4F3F, 0x4C1300, 0x4C2F26, 0x260900, 0x261713, 0xFF7F00, 0xFFBF7F,
    0xA55200, 0xA57C52, 0x7F3F00, 0x7F5F3F, 0x4C2600, 0x4C3926, 0x261300, 0x261C13,
    0xFFBF00, 0xFFDF7F, 0xA57C00, 0xA59152, 0x7F5F00, 0x7F6F3F, 0x4C3900, 0x4C4226,
    0x261C00, 0x262113, 0xFFFF00, 0xFFFF7F, 0xA5A500, 0xA5A552, 0x7F7F00, 0x7F7F3F,
    0x4C4C00, 0x4C4C26, 0x262600, 0x262613, 0xBFFF00, 0xDFFF7F, 0x7CA500, 0x91A552,
    0x5F7F00, 0x6F7F3F, 0x394C00, 0x424C26, 0x1C2600, 0x212613, 0x7FFF00, 0xBFFF7F,
    0x52A500, 0x7CA552, 0x3F7F00, 0x5F7F3F, 0x264C00, 0x394C26, 0x132600, 0x1C2613,
    0x3FFF00, 0x9FFF7F, 0x29A500, 0x67A552, 0x1F7F00, 0x4F7F3F, 0x134C00, 0x2F4C26,
    0x092600, 0x172613, 0x00FF00, 0x7FFF7F, 0x00A500, 0x52A552, 0x007F00, 0x3F7F3F,
    0x004C00, 0x264C26, 0x002600, 0x132613, 0x00FF3F, 0x7FFF9F, 0x00A529, 0x52A567,
    0x007F1F, 0x3F7F4F, 0x004C13, 0x264C2F, 0x002609, 0x135817, 0x00FF7F, 0x7FFFBF,
    0x00A552, 0x52A57C, 0x007F3F, 0x3F7F5F, 0x004C26, 0x264C39, 0x002613, 0x13581C,
    0x00FFBF, 0x7FFFDF, 0x00A57C, 0x52A591, 0x007F5F, 0x3F7F6F, 0x004C39, 0x264C42,
    0x00261C, 0x135858, 0x00FFFF, 0x7FFFFF, 0x00A5A5, 0x52A5A5, 0x007F7F, 0x3F7F7F,
    0x004C4C, 0x264C4C, 0x002626, 0x135858, 0x00BFFF, 0x7FDFFF, 0x007CA5, 0x5291A5,
    0x005F7F, 0x3F6F7F, 0x00394C, 0x26427E, 0x001C26, 0x135858, 0x007FFF, 0x7FBFFF,
    0x0052A5, 0x527CA5, 0x003F7F, 0x3F5F7F, 0x00264C, 0x26397E, 0x001326, 0x131C58,
    0x003FFF, 0x7F9FFF, 0x0029A5, 0x5267A5, 0x001F7F, 0x3F4F7F, 0x00134C, 0x262F7E,
    0x000926, 0x131758, 0x0000FF, 0x7F7FFF, 0x0000A5, 0x5252A5, 0x00007F, 0x3F3F7F,
    0x00004C, 0x26267E, 0x000026, 0x131358, 0x3F00FF, 0x9F7FFF, 0x2900A5, 0x6752A5,
    0x1F007F, 0x4F3F7F, 0x13004C, 0x2F267E, 0x090026, 0x171358, 0x7F00FF, 0xBF7FFF,
    0x5200A5, 0x7C52A5, 0x3F007F, 0x5F3F7F, 0x26004C, 0x39267E, 0x130026, 0x1C1358,
    0xBF00FF, 0xDF7FFF, 0x7C00A5, 0x9152A5, 0x5F007F, 0x6F3F7F, 0x39004C, 0x42264C,
    0x1C0026, 0x581358, 0xFF00FF, 0xFF7FFF, 0xA500A5, 0xA552A5, 0x7F007F, 0x7F3F7F,
    0x4C004C, 0x4C264C, 0x260026, 0x581358, 0xFF00BF, 0xFF7FDF, 0xA5007C, 0xA55291,
    0x7F005F, 0x7F3F6F, 0x4C0039, 0x4C2642, 0x26001C, 0x581358, 0xFF007F, 0xFF7FBF,
    0xA50052, 0xA5527C, 0x7F003F, 0x7F3F5F, 0x4C0026, 0x4C2639, 0x260013, 0x58131C,
    0xFF003F, 0xFF7F9F, 0xA50029, 0xA55267, 0x7F001F, 0x7F3F4F, 0x4C0013, 0x4C262F,
    0x260009, 0x581317, 0x000000, 0x656565, 0x666666, 0x999999, 0xCCCCCC, 0xFFFFFF
};
} // namespace

std::uint32_t aciToRgb(int aci) {
    if (aci < 1 || aci > 255) return 0xFFFFFF;
    return kAci[static_cast<std::size_t>(aci)];
}

int rgbToAci(std::uint32_t rgb) {
    const int r = static_cast<int>((rgb >> 16) & 0xFF), g = static_cast<int>((rgb >> 8) & 0xFF), b = static_cast<int>(rgb & 0xFF);
    int best = 7;
    long bestD = std::numeric_limits<long>::max();
    for (int i = 1; i < 256; ++i) {
        const auto c = kAci[static_cast<std::size_t>(i)];
        const long dr = r - static_cast<int>((c >> 16) & 0xFF), dg = g - static_cast<int>((c >> 8) & 0xFF), db = b - static_cast<int>(c & 0xFF);
        const long d = dr * dr + dg * dg + db * db;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

double metresPerInsUnit(int u) {
    switch (u) {
    case 1: return 0.0254;          // inches
    case 2: return 0.3048;          // feet
    case 3: return 1609.344;        // miles
    case 4: return 0.001;           // millimetres
    case 5: return 0.01;            // centimetres
    case 6: return 1.0;             // metres
    case 7: return 1000.0;          // kilometres
    case 8: return 0.0000254;       // microinches
    case 9: return 0.0000254;       // mils
    case 10: return 0.9144;         // yards
    case 11: return 1e-10;          // angstroms
    case 12: return 1e-9;           // nanometres
    case 13: return 1e-6;           // microns
    case 14: return 0.1;            // decimetres
    case 15: return 10.0;           // decametres
    case 16: return 100.0;          // hectometres
    case 17: return 1e9;            // gigametres
    case 18: return 1.495978707e11; // astronomical units
    case 19: return 9.4607304725808e15; // light years
    case 20: return 3.0856775814913673e16; // parsecs
    case 21: return 1200.0 / 3937.0; // US survey feet
    default: return 0.0;
    }
}

} // namespace openmine::formats

namespace openmine::formats::dxf {

double Group::num() const {
    std::size_t b = 0, e = value.size();
    while (b < e && (value[b] == ' ' || value[b] == '\t')) ++b;
    while (e > b && (value[e - 1] == ' ' || value[e - 1] == '\t' || value[e - 1] == '\r')) --e;
    double v = 0;
    if (b < e && value[b] == '+') ++b;
    const auto r = std::from_chars(value.data() + b, value.data() + e, v);
    if (r.ec != std::errc{}) throw DxfError("Invalid number '" + value + "' for group code " + std::to_string(code));
    return v;
}

int Group::integer() const {
    const double v = num();
    if (!std::isfinite(v) || std::abs(v) > 2147483647.0) throw DxfError("Invalid integer '" + value + "'");
    return static_cast<int>(std::lround(v));
}

std::vector<Group> tokenize(std::string_view text) {
    std::vector<Group> out;
    if (text.substr(0, 22) == "AutoCAD Binary DXF\r\n\x1a") throw DxfError("Binary DXF is not supported; save the drawing as ASCII DXF");
    // Skip UTF-8 BOM.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    std::size_t pos = 0, lineNo = 0;
    auto nextLine = [&](std::string_view& line) -> bool {
        if (pos >= text.size()) return false;
        const auto nl = text.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? text.size() : nl;
        line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        ++lineNo;
        return true;
    };
    std::string_view codeLine, valueLine;
    while (nextLine(codeLine)) {
        std::size_t b = 0, e = codeLine.size();
        while (b < e && (codeLine[b] == ' ' || codeLine[b] == '\t')) ++b;
        while (e > b && (codeLine[e - 1] == ' ' || codeLine[e - 1] == '\t')) --e;
        if (b == e && pos >= text.size()) break; // trailing empty line
        int code = 0;
        const auto r = std::from_chars(codeLine.data() + b, codeLine.data() + e, code);
        if (r.ec != std::errc{} || r.ptr != codeLine.data() + e)
            throw DxfError("Malformed DXF: expected a group code on line " + std::to_string(lineNo));
        if (!nextLine(valueLine)) throw DxfError("Malformed DXF: missing value after group code on line " + std::to_string(lineNo));
        Group g;
        g.code = code;
        g.value = std::string(valueLine);
        // Strings keep leading blanks (text content) but lose trailing blanks for codes 0–9 names.
        if (code == 0 || code == 2 || code == 8 || code == 9 || code == 6 || code == 7 || code == 5 || code == 3 || code == 1001)
            while (!g.value.empty() && (g.value.back() == ' ' || g.value.back() == '\t')) g.value.pop_back();
        if (code == 0 || code == 2 || code == 9)
            while (!g.value.empty() && (g.value.front() == ' ')) g.value.erase(g.value.begin());
        out.push_back(std::move(g));
        if (out.back().code == 0 && out.back().value == "EOF") break;
    }
    return out;
}

Vec3 ocsXAxis(Vec3 n) {
    n = n.normalized();
    constexpr double lim = 1.0 / 64.0;
    const Vec3 ax = (std::abs(n.x) < lim && std::abs(n.y) < lim) ? cross(Vec3{0, 1, 0}, n) : cross(Vec3{0, 0, 1}, n);
    return ax.normalized();
}

Transform3 ocsToWcs(Vec3 n) {
    n = n.normalized();
    const Vec3 ax = ocsXAxis(n), ay = cross(n, ax).normalized();
    return Transform3({ax.x, ay.x, n.x, 0, ax.y, ay.y, n.y, 0, ax.z, ay.z, n.z, 0, 0, 0, 0, 1});
}

Transform3 wcsToOcs(Vec3 n) {
    n = n.normalized();
    const Vec3 ax = ocsXAxis(n), ay = cross(n, ax).normalized();
    return Transform3({ax.x, ax.y, ax.z, 0, ay.x, ay.y, ay.z, 0, n.x, n.y, n.z, 0, 0, 0, 0, 1});
}

namespace {
void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool validUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0) return false;
        for (int k = 1; k <= n; ++k)
            if (i + static_cast<std::size_t>(k) >= s.size() || (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) >> 6) != 2) return false;
        i += static_cast<std::size_t>(n) + 1;
    }
    return true;
}

constexpr std::array<std::uint16_t, 32> kCp1252High = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                                        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                                        0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                                        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
} // namespace

std::string decodeText(std::string_view s, bool cp1252) {
    std::string in;
    if (cp1252 && !validUtf8(s)) {
        for (char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x80) in += ch;
            else if (c < 0xA0) appendUtf8(in, kCp1252High[c - 0x80]);
            else appendUtf8(in, c);
        }
    } else in = std::string(s);
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 6 < in.size() + 1 && (in.compare(i, 3, "\\U+") == 0 || in.compare(i, 3, "\\u+") == 0) && i + 7 <= in.size()) {
            std::uint32_t cp = 0;
            const auto r = std::from_chars(in.data() + i + 3, in.data() + i + 7, cp, 16);
            if (r.ec == std::errc{} && r.ptr == in.data() + i + 7) {
                appendUtf8(out, cp);
                i += 6;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

std::string encodeText(std::string_view s) {
    // R2004 files use the Windows-1252 code page ($DWGCODEPAGE ANSI_1252): characters in that code
    // page are written as single bytes, everything else as \U+XXXX escapes.
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += (c == '\n') ? std::string("\\P") : std::string(1, static_cast<char>(c));
            ++i;
            continue;
        }
        std::uint32_t cp = 0;
        int n = 0;
        if ((c >> 5) == 6) { cp = c & 0x1F; n = 1; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 2; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 3; }
        else { ++i; continue; }
        for (int k = 1; k <= n && i + static_cast<std::size_t>(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
        i += static_cast<std::size_t>(n) + 1;
        if (cp >= 0xA0 && cp <= 0xFF) {
            out += static_cast<char>(cp);
            continue;
        }
        bool done = false;
        for (std::size_t k = 0; k < kCp1252High.size(); ++k)
            if (kCp1252High[k] == cp && cp > 0xFF) {
                out += static_cast<char>(0x80 + k);
                done = true;
                break;
            }
        if (done) continue;
        char buf[16];
        if (cp > 0xFFFF) cp = 0xFFFD; // outside the BMP: not representable in \U+XXXX
        std::snprintf(buf, sizeof buf, "\\U+%04X", cp);
        out += buf;
    }
    return out;
}

std::string stripMText(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '{' || c == '}') continue;
        if (c != '\\' || i + 1 >= s.size()) {
            out += c;
            continue;
        }
        const char k = s[i + 1];
        switch (k) {
        case 'P': out += '\n'; ++i; break;
        case 'N': out += '\n'; ++i; break; // column break
        case '~': out += ' '; ++i; break;  // non-breaking space
        case '\\': case '{': case '}': out += k; ++i; break;
        case 'L': case 'l': case 'O': case 'o': case 'K': case 'k': ++i; break; // underline/overline/strike toggles
        case 'S': { // stacked fraction \Snum^den;  → num/den
            const auto end = s.find(';', i + 2);
            std::string frac(s.substr(i + 2, (end == std::string_view::npos ? s.size() : end) - (i + 2)));
            for (auto& ch : frac)
                if (ch == '^' || ch == '#') ch = '/';
            out += frac;
            i = end == std::string_view::npos ? s.size() : end;
            break;
        }
        case 'U': case 'u': out += c; break; // left for decodeText
        default: { // \f..; \H..; \W..; \Q..; \T..; \A..; \C..; \c..; \p..;
            const auto end = s.find(';', i + 2);
            i = end == std::string_view::npos ? s.size() : end;
            break;
        }
        }
    }
    return out;
}

std::string formatReal(double v) {
    if (!std::isfinite(v)) v = 0;
    if (v == 0) return "0.0";
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    std::string s(buf, r.ptr);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

std::string hex(unsigned h) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%X", h);
    return buf;
}

void Out::g(int code, std::string_view v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%3d\n", code);
    buf_ += buf;
    buf_ += v;
    buf_ += '\n';
}
void Out::g(int code, double v) { g(code, std::string_view(formatReal(v))); }
void Out::g(int code, int v) { g(code, std::string_view(std::to_string(v))); }
void Out::point(int code, Vec3 p, bool with3d) {
    g(code, p.x);
    g(code + 10, p.y);
    if (with3d) g(code + 20, p.z);
}
void Out::handle(int code, unsigned h) { g(code, std::string_view(hex(h))); }

} // namespace openmine::formats::dxf
