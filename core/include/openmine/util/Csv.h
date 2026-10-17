#pragma once
// SPDX-License-Identifier: Apache-2.0
// Delimited text tables (RFC 4180): quoted fields with embedded delimiters, quotes and line
// breaks, CRLF/LF, UTF-8 BOM, delimiter detection (comma, semicolon, tab, pipe).

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmine::util {

struct CsvTable {
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows; ///< each row padded/truncated to headers.size()
    char delimiter = ',';
    std::vector<std::string> warnings;

    /// Column index by header name (case- and surrounding-space-insensitive).
    std::optional<std::size_t> column(std::string_view name) const;
    /// First matching column among alternative names.
    std::optional<std::size_t> column(std::initializer_list<std::string_view> names) const;
};

/// Detects the delimiter from the first line (the candidate occurring most often outside quotes).
char detectDelimiter(std::string_view text);
/// Parses text with a header row. delimiter 0 = detect. Empty lines are skipped.
CsvTable parseCsv(std::string_view text, char delimiter = 0);
CsvTable readCsvFile(const std::string& path, char delimiter = 0);

/// Quotes a field when it contains the delimiter, a quote or a line break.
std::string csvField(std::string_view s, char delimiter = ',');
std::string csvLine(const std::vector<std::string>& fields, char delimiter = ',');

/// Parses a number in C locale; nullopt for empty/invalid text.
std::optional<double> parseNumber(std::string_view s);

} // namespace openmine::util
