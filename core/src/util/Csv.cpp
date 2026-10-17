// SPDX-License-Identifier: Apache-2.0
#include "openmine/util/Csv.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace openmine::util {

namespace {
std::string norm(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    std::string r(s.substr(a, b - a));
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}
} // namespace

std::optional<std::size_t> CsvTable::column(std::string_view name) const {
    const auto n = norm(name);
    for (std::size_t i = 0; i < headers.size(); ++i)
        if (norm(headers[i]) == n) return i;
    return std::nullopt;
}

std::optional<std::size_t> CsvTable::column(std::initializer_list<std::string_view> names) const {
    for (auto n : names)
        if (auto c = column(n)) return c;
    return std::nullopt;
}

char detectDelimiter(std::string_view text) {
    const char cands[] = {',', ';', '\t', '|'};
    int best = 0;
    char d = ',';
    for (char c : cands) {
        int n = 0;
        bool q = false;
        for (char x : text) {
            if (x == '"') q = !q;
            else if (!q && (x == '\n' || x == '\r')) break;
            else if (!q && x == c) ++n;
        }
        if (n > best) {
            best = n;
            d = c;
        }
    }
    return d;
}

CsvTable parseCsv(std::string_view text, char delimiter) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    CsvTable t;
    t.delimiter = delimiter ? delimiter : detectDelimiter(text);
    std::vector<std::vector<std::string>> records;
    std::vector<std::string> rec;
    std::string field;
    bool quoted = false, fieldStarted = false, any = false;
    std::size_t line = 1;
    auto endField = [&] {
        rec.push_back(std::move(field));
        field.clear();
        fieldStarted = false;
    };
    auto endRecord = [&] {
        if (any || !rec.empty()) {
            endField();
            const bool blank = std::all_of(rec.begin(), rec.end(), [](const std::string& s) { return s.empty(); });
            if (!blank) records.push_back(std::move(rec));
        }
        rec.clear();
        any = false;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else quoted = false;
            } else {
                if (c == '\n') ++line;
                field += c;
            }
            continue;
        }
        if (c == '"' && !fieldStarted) {
            quoted = true;
            fieldStarted = any = true;
        } else if (c == t.delimiter) {
            any = true;
            endField();
        } else if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            endRecord();
            ++line;
        } else if (c == '\n') {
            endRecord();
            ++line;
        } else {
            field += c;
            fieldStarted = any = true;
        }
    }
    if (quoted) t.warnings.push_back("Unterminated quoted field at end of file (line " + std::to_string(line) + ")");
    endRecord();
    if (records.empty()) return t;
    t.headers = std::move(records.front());
    for (auto& h : t.headers) {
        std::size_t a = 0, b = h.size();
        while (a < b && std::isspace(static_cast<unsigned char>(h[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(h[b - 1]))) --b;
        h = h.substr(a, b - a);
    }
    for (std::size_t r = 1; r < records.size(); ++r) {
        auto& row = records[r];
        if (row.size() != t.headers.size())
            t.warnings.push_back("Row " + std::to_string(r + 1) + " has " + std::to_string(row.size()) + " fields; expected " + std::to_string(t.headers.size()));
        row.resize(t.headers.size());
        t.rows.push_back(std::move(row));
    }
    return t;
}

CsvTable readCsvFile(const std::string& path, char delimiter) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseCsv(ss.str(), delimiter);
}

std::string csvField(std::string_view s, char delimiter) {
    if (s.find_first_of(std::string{delimiter, '"', '\n', '\r'}) == std::string_view::npos) return std::string(s);
    std::string r = "\"";
    for (char c : s) {
        if (c == '"') r += '"';
        r += c;
    }
    return r + "\"";
}

std::string csvLine(const std::vector<std::string>& fields, char delimiter) {
    std::string r;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) r += delimiter;
        r += csvField(fields[i], delimiter);
    }
    return r;
}

std::optional<double> parseNumber(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    double v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return v;
}

} // namespace openmine::util
