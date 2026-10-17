// SPDX-License-Identifier: Apache-2.0
#include "openmine/reporting/Pdf.h"

#include "openmine/objects/DateTime.h"

#include <zlib.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace openmine::reporting {

namespace {
// Advance widths (1/1000 em) of WinAnsi characters 32–255 for Helvetica and Helvetica-Bold,
// taken from the metric-compatible Liberation Sans fonts.
constexpr std::array<std::uint16_t, 224> kHelvetica = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
    333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584, 0,
    556, 0, 222, 556, 333, 1000, 556, 556, 333, 1000, 667, 333, 1000, 0, 611, 0,
    0, 222, 222, 333, 333, 350, 556, 1000, 333, 1000, 500, 333, 944, 0, 500, 667,
    278, 333, 556, 556, 556, 556, 260, 556, 333, 737, 370, 556, 584, 333, 737, 552,
    400, 549, 333, 333, 333, 576, 537, 333, 333, 333, 365, 556, 834, 834, 834, 611,
    667, 667, 667, 667, 667, 667, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278,
    722, 722, 778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 667, 667, 611,
    556, 556, 556, 556, 556, 556, 889, 500, 556, 556, 556, 556, 278, 278, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 549, 611, 556, 556, 556, 556, 500, 556, 500};
constexpr std::array<std::uint16_t, 224> kHelveticaBold = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,
    975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,
    333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
    611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584, 0,
    556, 0, 278, 556, 500, 1000, 556, 556, 333, 1000, 667, 333, 1000, 0, 611, 0,
    0, 278, 278, 500, 500, 350, 556, 1000, 333, 1000, 556, 333, 944, 0, 500, 667,
    278, 333, 556, 556, 556, 556, 280, 556, 333, 737, 370, 556, 584, 333, 737, 552,
    400, 549, 333, 333, 333, 576, 556, 333, 333, 333, 365, 556, 834, 834, 834, 611,
    722, 722, 722, 722, 722, 722, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278,
    722, 722, 778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 667, 667, 611,
    556, 556, 556, 556, 556, 556, 889, 556, 556, 556, 556, 556, 278, 278, 278, 278,
    611, 611, 611, 611, 611, 611, 611, 549, 611, 611, 611, 611, 611, 556, 611, 556};

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

std::string color(const Rgb& c, bool stroke) { return num(c.r) + " " + num(c.g) + " " + num(c.b) + (stroke ? " RG\n" : " rg\n"); }

std::string escape(const std::string& winAnsi) {
    std::string o;
    for (char ch : winAnsi) {
        if (ch == '(' || ch == ')' || ch == '\\') o += '\\';
        o += ch;
    }
    return o;
}

std::string deflate(const std::string& in) {
    uLongf n = compressBound(static_cast<uLong>(in.size()));
    std::string out(n, '\0');
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(in.data()), static_cast<uLong>(in.size()), 9) != Z_OK)
        throw std::runtime_error("PDF compression failed");
    out.resize(n);
    return out;
}

constexpr std::array<std::uint16_t, 32> kCp1252High = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
                                                        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
} // namespace

std::string toWinAnsi(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        int n = 0;
        if (c >= 0x80) {
            if ((c >> 5) == 6) { cp = c & 0x1F; n = 1; }
            else if ((c >> 4) == 14) { cp = c & 0x0F; n = 2; }
            else if ((c >> 3) == 30) { cp = c & 0x07; n = 3; }
            for (int k = 1; k <= n && i + static_cast<std::size_t>(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
        }
        i += static_cast<std::size_t>(n) + 1;
        if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
            out += static_cast<char>(cp);
            continue;
        }
        char mapped = '?';
        for (std::size_t k = 0; k < kCp1252High.size(); ++k)
            if (kCp1252High[k] == cp && cp != 0) mapped = static_cast<char>(0x80 + k);
        out += mapped;
    }
    return out;
}

PdfDocument::PdfDocument(std::string title, std::string author) : title_(std::move(title)), author_(std::move(author)) {}

void PdfDocument::newPage(PageSize s) { pages_.push_back({s, {}}); }

PdfDocument::Page& PdfDocument::cur() {
    if (pages_.empty()) newPage();
    return pages_.back();
}
const PageSize& PdfDocument::pageSize() const {
    static const PageSize def;
    return pages_.empty() ? def : pages_.back().size;
}

double PdfDocument::textWidth(const std::string& utf8, double size, bool bold) {
    const auto& w = bold ? kHelveticaBold : kHelvetica;
    double t = 0;
    for (unsigned char c : toWinAnsi(utf8)) t += c >= 32 ? w[c - 32] : 0;
    return t * size / 1000.0;
}

void PdfDocument::text(double x, double y, const std::string& s, double size, bool bold, Rgb c, Align a) {
    auto& p = cur();
    const double w = textWidth(s, size, bold);
    if (a == Align::Center) x -= w / 2;
    else if (a == Align::Right) x -= w;
    p.content += "BT\n" + color(c, false) + std::string(bold ? "/F2 " : "/F1 ") + num(size) + " Tf\n" + num(x) + " " + num(p.size.height - y) + " Td\n(" +
                 escape(toWinAnsi(s)) + ") Tj\nET\n";
}

void PdfDocument::textFit(double x, double y, const std::string& s, double size, double maxW, bool bold, Rgb c, Align a) {
    if (textWidth(s, size, bold) <= maxW) return text(x, y, s, size, bold, c, a);
    std::string t = s;
    while (!t.empty() && textWidth(t + "...", size, bold) > maxW) {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back(); // whole UTF-8 characters
        if (!t.empty() && static_cast<unsigned char>(t.back()) >= 0xC0) t.pop_back();
    }
    text(x, y, t + "...", size, bold, c, a);
}

void PdfDocument::line(double x1, double y1, double x2, double y2, double w, Rgb c, double dash) {
    auto& p = cur();
    p.content += "q\n" + color(c, true) + num(w) + " w\n" + (dash > 0 ? "[" + num(dash) + " " + num(dash) + "] 0 d\n" : "") + num(x1) + " " +
                 num(p.size.height - y1) + " m " + num(x2) + " " + num(p.size.height - y2) + " l S\nQ\n";
}

void PdfDocument::rect(double x, double y, double w, double h, const Rgb* fill, const Rgb* stroke, double width) {
    auto& p = cur();
    std::string s = "q\n";
    if (fill) s += color(*fill, false);
    if (stroke) s += color(*stroke, true) + num(width) + " w\n";
    s += num(x) + " " + num(p.size.height - y - h) + " " + num(w) + " " + num(h) + " re " + (fill && stroke ? "B" : fill ? "f" : "S") + "\nQ\n";
    if (fill || stroke) p.content += s;
}

void PdfDocument::polygon(const std::vector<std::pair<double, double>>& pts, const Rgb& fill) {
    if (pts.size() < 3) return;
    auto& p = cur();
    std::string s = "q\n" + color(fill, false);
    for (std::size_t i = 0; i < pts.size(); ++i) s += num(pts[i].first) + " " + num(p.size.height - pts[i].second) + (i == 0 ? " m\n" : " l\n");
    p.content += s + "h f\nQ\n";
}

std::vector<std::string> PdfDocument::wrap(const std::string& s, double size, double width, bool bold) {
    std::vector<std::string> lines;
    std::istringstream in(s);
    std::string para;
    while (std::getline(in, para)) {
        std::istringstream ws(para);
        std::string word, line;
        while (ws >> word) {
            const std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && textWidth(cand, size, bold) > width) {
                lines.push_back(line);
                line = word;
            } else line = cand;
        }
        lines.push_back(line);
    }
    return lines;
}

std::string PdfDocument::bytes() const {
    std::vector<std::string> objs; // object bodies, numbered from 1
    auto add = [&](std::string body) {
        objs.push_back(std::move(body));
        return objs.size();
    };
    const auto catalog = add(""), pagesObj = add("");
    const auto f1 = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    const auto f2 = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
    std::vector<std::size_t> pageIds;
    const auto& pages = pages_.empty() ? std::vector<Page>{Page{}} : pages_;
    for (const auto& p : pages) {
        const std::string z = deflate(p.content);
        const auto content = add("<< /Length " + std::to_string(z.size()) + " /Filter /FlateDecode >>\nstream\n" + z + "\nendstream");
        pageIds.push_back(add("<< /Type /Page /Parent " + std::to_string(pagesObj) + " 0 R /MediaBox [0 0 " + num(p.size.width) + " " + num(p.size.height) +
                              "] /Resources << /Font << /F1 " + std::to_string(f1) + " 0 R /F2 " + std::to_string(f2) + " 0 R >> >> /Contents " +
                              std::to_string(content) + " 0 R >>"));
    }
    std::string kids;
    for (auto id : pageIds) kids += std::to_string(id) + " 0 R ";
    objs[pagesObj - 1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageIds.size()) + " >>";
    objs[catalog - 1] = "<< /Type /Catalog /Pages " + std::to_string(pagesObj) + " 0 R >>";
    auto pdfString = [](const std::string& s) { return "(" + escape(toWinAnsi(s)) + ")"; };
    std::string stamp = utcTimestampNow(); // YYYY-MM-DDTHH:MM:SSZ
    std::string d = "D:";
    for (char ch : stamp)
        if (std::isdigit(static_cast<unsigned char>(ch))) d += ch;
    const auto info = add("<< /Title " + pdfString(title_) + " /Author " + pdfString(author_) + " /Producer (OpenMine Suite) /CreationDate (" + d + "Z) >>");
    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objs.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (auto off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root " + std::to_string(catalog) + " 0 R /Info " + std::to_string(info) +
           " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

void PdfDocument::save(const std::filesystem::path& f) const {
    const auto tmp = f.parent_path() / (f.filename().string() + ".tmp");
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) throw std::runtime_error("Cannot write " + f.string());
        o << bytes();
        if (!o) throw std::runtime_error("Writing " + f.string() + " failed");
    }
    std::filesystem::rename(tmp, f);
}

} // namespace openmine::reporting
