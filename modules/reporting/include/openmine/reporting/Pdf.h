#pragma once
// SPDX-License-Identifier: Apache-2.0
// A small, dependency-free PDF 1.7 writer for reports and Gantt charts: pages, text in the
// standard Helvetica fonts (WinAnsi encoding, exact metrics for layout), lines, rectangles and
// polygons. Coordinates are in points with the origin at the top-left of the page, y downwards.
// Content streams are Flate-compressed.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace openmine::reporting {

struct Rgb {
    double r = 0, g = 0, b = 0;
    static Rgb hex(std::uint32_t v) { return {((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0, (v & 0xFF) / 255.0}; }
};

enum class Align { Left, Center, Right };

struct PageSize {
    double width = 595.28, height = 841.89; // A4 portrait
    static PageSize a4Portrait() { return {595.28, 841.89}; }
    static PageSize a4Landscape() { return {841.89, 595.28}; }
    static PageSize a3Landscape() { return {1190.55, 841.89}; }
};

class PdfDocument {
public:
    explicit PdfDocument(std::string title = {}, std::string author = {});

    void newPage(PageSize size = PageSize::a4Portrait());
    std::size_t pageCount() const { return pages_.size(); }
    const PageSize& pageSize() const;

    /// Draws text with its baseline at y.
    void text(double x, double y, const std::string& utf8, double size, bool bold = false, Rgb color = {}, Align align = Align::Left);
    /// As text(), truncated with an ellipsis to fit maxWidth.
    void textFit(double x, double y, const std::string& utf8, double size, double maxWidth, bool bold = false, Rgb color = {}, Align align = Align::Left);
    void line(double x1, double y1, double x2, double y2, double width = 0.5, Rgb color = {}, double dash = 0);
    void rect(double x, double y, double w, double h, const Rgb* fill, const Rgb* stroke = nullptr, double width = 0.5);
    void polygon(const std::vector<std::pair<double, double>>& pts, const Rgb& fill);

    /// Width of text in points (Helvetica / Helvetica-Bold metrics).
    static double textWidth(const std::string& utf8, double size, bool bold = false);
    /// Word-wraps text into lines that fit `width`.
    static std::vector<std::string> wrap(const std::string& utf8, double size, double width, bool bold = false);

    std::string bytes() const;
    void save(const std::filesystem::path& file) const;

private:
    struct Page {
        PageSize size;
        std::string content;
    };
    std::string title_, author_;
    std::vector<Page> pages_;
    Page& cur();
};

/// UTF-8 → Windows-1252 (characters outside it become '?').
std::string toWinAnsi(const std::string& utf8);

} // namespace openmine::reporting
