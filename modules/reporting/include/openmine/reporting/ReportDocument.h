#pragma once
// SPDX-License-Identifier: Apache-2.0
// Format-independent technical reports, rendered to self-contained HTML (inline CSS and SVG, print
// styles) and to paginated PDF (A4, repeated table headers, vector charts, "page i of n"
// footers). Reports are stored in the project as Report entities with their file as an asset, so
// what was issued is kept with the data it describes.

#include "openmine/project/Project.h"
#include "openmine/reporting/Pdf.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openmine::reporting {

struct ReportTable {
    std::string caption;
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
    std::vector<bool> numeric;           ///< per column: right-aligned (default: detected)
    std::vector<int> highlightRows;      ///< row indices drawn emphasised (e.g. failures)
};

struct ChartSeries {
    std::string name;
    std::vector<std::pair<double, double>> points;
    std::uint32_t color = 0x1F77B4;
    bool dashed = false;
};

struct LineChart {
    std::string title, xLabel, yLabel;
    std::vector<ChartSeries> series;
    std::optional<double> yZeroLine;     ///< draw a reference line at this y
};

/// A cross-section drawing (equal scale): design and as-built outlines in section coordinates.
struct SectionPlot {
    std::string title;
    std::vector<std::pair<double, double>> design, asBuilt, envelope;
};

struct ReportBlock {
    enum class Kind { Heading, Paragraph, KeyValues, Table, Chart, Section, Note, PageBreak } kind = Kind::Paragraph;
    int level = 1;                                        ///< Heading: 1–3
    std::string text;                                     ///< Heading, Paragraph, Note
    std::vector<std::pair<std::string, std::string>> keyValues;
    ReportTable table;
    LineChart chart;
    std::vector<SectionPlot> sections;                    ///< drawn in a grid
};

struct ReportDocument {
    std::string title, subtitle;
    std::vector<std::pair<std::string, std::string>> meta; ///< project, generated, by, software …
    std::vector<ReportBlock> blocks;

    ReportDocument& heading(const std::string& t, int level = 1);
    ReportDocument& paragraph(const std::string& t);
    ReportDocument& note(const std::string& t);
    ReportDocument& keyValues(std::vector<std::pair<std::string, std::string>> kv);
    ReportDocument& table(ReportTable t);
    ReportDocument& chart(LineChart c);
    ReportDocument& sections(std::vector<SectionPlot> s);
    ReportDocument& pageBreak();
};

std::string renderHtml(const ReportDocument& doc);
/// PDF bytes; returns the page count in `pages` when given.
std::string renderPdf(const ReportDocument& doc, std::size_t* pages = nullptr);

/// Standard metadata block (project name and id, generated time UTC, user, software version).
std::vector<std::pair<std::string, std::string>> standardMeta(const Project& project);

/// Stores the rendered report ("html" or "pdf") as an asset and a Report entity (one undo step).
Uuid storeReport(Project& project, const ReportDocument& doc, const std::string& kind, const std::string& format);

/// Number formatting used in reports (fixed decimals, thousands separators optional).
std::string fmtNumber(double v, int decimals, bool thousands = false);

} // namespace openmine::reporting
