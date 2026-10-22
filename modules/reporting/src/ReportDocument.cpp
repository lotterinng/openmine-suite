// SPDX-License-Identifier: Apache-2.0
#include "openmine/reporting/ReportDocument.h"

#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <cctype>
#include <cstdio>

namespace openmine::reporting {

// ---- builder ------------------------------------------------------------------------------------
ReportDocument& ReportDocument::heading(const std::string& t, int level) {
    ReportBlock b;
    b.kind = ReportBlock::Kind::Heading;
    b.text = t;
    b.level = std::clamp(level, 1, 3);
    blocks.push_back(std::move(b));
    return *this;
}
ReportDocument& ReportDocument::paragraph(const std::string& t) {
    ReportBlock b;
    b.kind = ReportBlock::Kind::Paragraph;
    b.text = t;
    blocks.push_back(std::move(b));
    return *this;
}
ReportDocument& ReportDocument::note(const std::string& t) {
    ReportBlock b;
    b.kind = ReportBlock::Kind::Note;
    b.text = t;
    blocks.push_back(std::move(b));
    return *this;
}
ReportDocument& ReportDocument::keyValues(std::vector<std::pair<std::string, std::string>> kv) {
    ReportBlock b;
    b.kind = ReportBlock::Kind::KeyValues;
    b.keyValues = std::move(kv);
    blocks.push_back(std::move(b));
    return *this;
}
ReportDocument& ReportDocument::table(ReportTable t) {
    ReportBlock b;
    b.kind = ReportBlock::Kind::Table;
    b.table = std::move(t);
    blocks.push_back(std::move(b));
    return *this;
}
ReportDocument& ReportDocument::chart(LineChart c) {
    ReportBlock b;
    b.kind = ReportBlock::Kind::Chart;
    b.chart = std::move(c);
    blocks.push_back(std::move(b));
    return *this;
}
ReportDocument& ReportDocument::sections(std::vector<SectionPlot> s) {
    ReportBlock b;
    b.kind = ReportBlock::Kind::Section;
    b.sections = std::move(s);
    blocks.push_back(std::move(b));
    return *this;
}
ReportDocument& ReportDocument::pageBreak() {
    ReportBlock b;
    b.kind = ReportBlock::Kind::PageBreak;
    blocks.push_back(std::move(b));
    return *this;
}

std::string fmtNumber(double v, int decimals, bool thousands) {
    if (!std::isfinite(v)) return "–";
    std::ostringstream s;
    s << std::fixed << std::setprecision(decimals) << v;
    std::string r = s.str();
    if (r == "-0" || r.rfind("-0.", 0) == 0) {
        bool allZero = std::all_of(r.begin() + 1, r.end(), [](char c) { return c == '0' || c == '.'; });
        if (allZero) r.erase(0, 1);
    }
    if (!thousands) return r;
    const auto dot = r.find('.');
    std::string intPart = r.substr(0, dot), frac = dot == std::string::npos ? "" : r.substr(dot);
    const bool neg = !intPart.empty() && intPart[0] == '-';
    if (neg) intPart.erase(0, 1);
    std::string out;
    for (std::size_t i = 0; i < intPart.size(); ++i) {
        if (i && (intPart.size() - i) % 3 == 0) out += ',';
        out += intPart[i];
    }
    return (neg ? "-" : "") + out + frac;
}

std::vector<std::pair<std::string, std::string>> standardMeta(const Project& p) {
    return {{"Project", p.model().info.name},
            {"Project id", p.model().info.id.toString()},
            {"Generated (UTC)", utcTimestampNow()},
            {"By", p.userName().empty() ? "—" : p.userName()},
            {"Software", persistence::kAppVersion}};
}

namespace {
bool looksNumeric(const std::string& s) {
    if (s.empty() || s == "–" || s == "—") return true;
    std::size_t digits = 0;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) ++digits;
        else if (!std::strchr("+-.,%eE ", c)) return false;
    }
    return digits > 0;
}
std::vector<bool> numericColumns(const ReportTable& t) {
    if (t.numeric.size() == t.header.size()) return t.numeric;
    std::vector<bool> n(t.header.size(), true);
    for (const auto& r : t.rows)
        for (std::size_t c = 0; c < n.size() && c < r.size(); ++c)
            if (!looksNumeric(r[c])) n[c] = false;
    return n;
}

/// "Nice" axis ticks covering [lo, hi].
std::vector<double> ticks(double lo, double hi, int target = 6) {
    if (!(hi > lo)) {
        lo -= 1;
        hi += 1;
    }
    const double raw = (hi - lo) / target;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double step = mag;
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (m * mag >= raw) {
            step = m * mag;
            break;
        }
    std::vector<double> t;
    for (double v = std::ceil(lo / step - 1e-9) * step; v <= hi + 1e-9 * step; v += step) t.push_back(std::abs(v) < 1e-12 * step ? 0.0 : v);
    return t;
}
std::string tickLabel(double v, const std::vector<double>& all) {
    double step = all.size() > 1 ? all[1] - all[0] : 1;
    int dec = 0;
    while (dec < 6 && std::abs(step * std::pow(10, dec) - std::round(step * std::pow(10, dec))) > 1e-6) ++dec;
    return fmtNumber(v, dec);
}

struct Range {
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    void add(double x, double y) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    bool empty() const { return x0 > x1; }
};

Range chartRange(const LineChart& c) {
    Range r;
    for (const auto& s : c.series)
        for (const auto& [x, y] : s.points)
            if (std::isfinite(x) && std::isfinite(y)) r.add(x, y);
    if (c.yZeroLine && !r.empty()) {
        r.y0 = std::min(r.y0, *c.yZeroLine);
        r.y1 = std::max(r.y1, *c.yZeroLine);
    }
    if (!r.empty()) { // 6 % headroom so lines do not run along the frame
        const double pad = 0.06 * std::max(r.y1 - r.y0, 1e-9);
        r.y0 -= pad;
        r.y1 += pad;
    }
    return r;
}

Range sectionRange(const SectionPlot& s) {
    Range r;
    for (const auto* v : {&s.design, &s.asBuilt, &s.envelope})
        for (const auto& [x, y] : *v) r.add(x, y);
    return r;
}


// ---- HTML ------------------------------------------------------------------------------------------
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}
std::string hexColor(std::uint32_t c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%06X", c & 0xFFFFFF);
    return b;
}
std::string n2(double v) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(2) << v;
    return s.str();
}

std::string svgChart(const LineChart& c) {
    const double W = 720, H = 312, L = 64, R = 16, T = 42, B = 46;
    const Range r = chartRange(c);
    std::ostringstream s;
    s << "<svg class=\"chart\" viewBox=\"0 0 " << W << ' ' << H << "\" role=\"img\" aria-label=\"" << esc(c.title) << "\">";
    if (!c.title.empty()) s << "<text x=\"" << W / 2 << "\" y=\"16\" text-anchor=\"middle\" class=\"ct\">" << esc(c.title) << "</text>";
    if (r.empty()) return s.str() + "<text x=\"" + n2(W / 2) + "\" y=\"" + n2(H / 2) + "\" text-anchor=\"middle\">No data</text></svg>";
    const auto xt = ticks(r.x0, r.x1), yt = ticks(r.y0, r.y1);
    const double x0 = std::min(r.x0, xt.front()), x1 = std::max(r.x1, xt.back()), y0 = std::min(r.y0, yt.front()), y1 = std::max(r.y1, yt.back());
    auto X = [&](double v) { return L + (v - x0) / (x1 - x0 > 0 ? x1 - x0 : 1) * (W - L - R); };
    auto Y = [&](double v) { return H - B - (v - y0) / (y1 - y0 > 0 ? y1 - y0 : 1) * (H - T - B); };
    for (double v : yt)
        s << "<line x1=\"" << n2(L) << "\" x2=\"" << n2(W - R) << "\" y1=\"" << n2(Y(v)) << "\" y2=\"" << n2(Y(v)) << "\" class=\"grid\"/><text x=\"" << n2(L - 6)
          << "\" y=\"" << n2(Y(v) + 4) << "\" text-anchor=\"end\" class=\"tick\">" << tickLabel(v, yt) << "</text>";
    for (double v : xt)
        s << "<line x1=\"" << n2(X(v)) << "\" x2=\"" << n2(X(v)) << "\" y1=\"" << n2(T) << "\" y2=\"" << n2(H - B) << "\" class=\"grid\"/><text x=\"" << n2(X(v))
          << "\" y=\"" << n2(H - B + 16) << "\" text-anchor=\"middle\" class=\"tick\">" << tickLabel(v, xt) << "</text>";
    if (c.yZeroLine) s << "<line x1=\"" << n2(L) << "\" x2=\"" << n2(W - R) << "\" y1=\"" << n2(Y(*c.yZeroLine)) << "\" y2=\"" << n2(Y(*c.yZeroLine)) << "\" class=\"zero\"/>";
    s << "<rect x=\"" << n2(L) << "\" y=\"" << n2(T) << "\" width=\"" << n2(W - L - R) << "\" height=\"" << n2(H - T - B) << "\" class=\"frame\"/>";
    for (const auto& se : c.series) {
        s << "<polyline fill=\"none\" stroke=\"" << hexColor(se.color) << "\" stroke-width=\"1.6\"" << (se.dashed ? " stroke-dasharray=\"5 3\"" : "") << " points=\"";
        for (const auto& [x, y] : se.points)
            if (std::isfinite(y)) s << n2(X(x)) << ',' << n2(Y(y)) << ' ';
        s << "\"/>";
    }
    double lx = L + 8;
    for (const auto& se : c.series) {
        s << "<line x1=\"" << n2(lx) << "\" x2=\"" << n2(lx + 18) << "\" y1=\"" << n2(T - 10) << "\" y2=\"" << n2(T - 10) << "\" stroke=\"" << hexColor(se.color)
          << "\" stroke-width=\"2\"" << (se.dashed ? " stroke-dasharray=\"5 3\"" : "") << "/><text x=\"" << n2(lx + 22) << "\" y=\"" << n2(T - 6) << "\" class=\"tick\">"
          << esc(se.name) << "</text>";
        lx += 30 + 6.5 * se.name.size();
    }
    s << "<text x=\"" << n2(L + (W - L - R) / 2) << "\" y=\"" << n2(H - 8) << "\" text-anchor=\"middle\" class=\"axis\">" << esc(c.xLabel) << "</text>";
    s << "<text transform=\"translate(14 " << n2(T + (H - T - B) / 2) << ") rotate(-90)\" text-anchor=\"middle\" class=\"axis\">" << esc(c.yLabel) << "</text>";
    s << "</svg>";
    return s.str();
}

std::string svgSection(const SectionPlot& sp) {
    const double S = 220, M = 14;
    const Range r = sectionRange(sp);
    std::ostringstream s;
    s << "<figure class=\"section\"><svg viewBox=\"0 0 " << S << ' ' << S << "\" role=\"img\" aria-label=\"" << esc(sp.title) << "\">";
    if (!r.empty()) {
        const double span = std::max({r.x1 - r.x0, r.y1 - r.y0, 1e-6});
        const double sc = (S - 2 * M) / span;
        const double cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
        auto P = [&](const std::vector<std::pair<double, double>>& v, const char* cls, bool close) {
            if (v.empty()) return;
            s << "<" << (close ? "polygon" : "polyline") << " class=\"" << cls << "\" points=\"";
            for (const auto& [x, y] : v) s << n2(S / 2 + (x - cx) * sc) << ',' << n2(S / 2 - (y - cy) * sc) << ' ';
            s << "\"/>";
        };
        P(sp.envelope, "env", true);
        P(sp.design, "design", true);
        P(sp.asBuilt, "asbuilt", true);
    }
    s << "</svg><figcaption>" << esc(sp.title) << "</figcaption></figure>";
    return s.str();
}

constexpr const char* kCss = R"CSS(
:root{--ink:#1d2329;--muted:#5d6874;--line:#d5dbe1;--accent:#1f5f8b;--bad:#b42318;--bg:#ffffff;--band:#f3f6f8}
*{box-sizing:border-box}body{font:14px/1.45 "Helvetica Neue",Helvetica,Arial,sans-serif;color:var(--ink);background:var(--bg);margin:0}
main{max-width:980px;margin:0 auto;padding:32px 28px 64px}
header.r{border-bottom:3px solid var(--accent);margin-bottom:20px;padding-bottom:12px}
header.r h1{font-size:26px;margin:0 0 4px}header.r .sub{color:var(--muted);font-size:15px}
dl.meta{display:grid;grid-template-columns:max-content 1fr;gap:2px 16px;font-size:12px;color:var(--muted);margin:12px 0 0}
dl.meta dt{font-weight:600}dl.meta dd{margin:0}
h2{font-size:19px;margin:28px 0 8px;border-bottom:1px solid var(--line);padding-bottom:4px}h3{font-size:16px;margin:20px 0 6px}h4{font-size:14px;margin:16px 0 4px}
table{border-collapse:collapse;width:100%;font-size:12.5px;margin:8px 0 16px}caption{text-align:left;font-weight:600;padding:4px 0;color:var(--muted)}
th,td{border-bottom:1px solid var(--line);padding:4px 8px;vertical-align:top}th{background:var(--band);text-align:left;position:sticky;top:0}
td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}tr.hl td{color:var(--bad);font-weight:600}
table.kv{width:auto;min-width:50%}table.kv th{background:none;width:40%;font-weight:600}
.note{border-left:4px solid #e0a100;background:#fff8e5;padding:8px 12px;margin:10px 0}
svg.chart{width:100%;height:auto;margin:8px 0 16px}svg text{font:11px Helvetica,Arial,sans-serif;fill:var(--ink)}svg .ct{font-weight:700;font-size:13px}
svg .grid{stroke:#e6eaee;stroke-width:1}svg .frame{fill:none;stroke:#9aa4ae}svg .zero{stroke:#555;stroke-width:1;stroke-dasharray:3 3}svg .tick{fill:var(--muted)}
.sections{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:12px}figure.section{margin:0;border:1px solid var(--line);padding:6px}
figure.section svg{width:100%;height:auto}figcaption{font-size:12px;color:var(--muted);text-align:center}
.design{fill:none;stroke:#1f77b4;stroke-width:1.5}.asbuilt{fill:rgba(214,39,40,.08);stroke:#d62728;stroke-width:1.5}.env{fill:none;stroke:#2ca02c;stroke-dasharray:4 3}
@media print{main{padding:0}h2{break-after:avoid}table,figure,svg{break-inside:avoid}.pb{break-after:page}}
)CSS";
} // namespace

std::string renderHtml(const ReportDocument& d) {
    std::ostringstream o;
    o << "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      << "<meta name=\"generator\" content=\"" << esc(persistence::kAppVersion) << "\"><title>" << esc(d.title) << "</title><style>" << kCss
      << "</style></head><body><main>\n<header class=\"r\"><h1>" << esc(d.title) << "</h1>";
    if (!d.subtitle.empty()) o << "<div class=\"sub\">" << esc(d.subtitle) << "</div>";
    if (!d.meta.empty()) {
        o << "<dl class=\"meta\">";
        for (const auto& [k, v] : d.meta) o << "<dt>" << esc(k) << "</dt><dd>" << esc(v) << "</dd>";
        o << "</dl>";
    }
    o << "</header>\n";
    for (const auto& b : d.blocks) {
        switch (b.kind) {
        case ReportBlock::Kind::Heading: o << "<h" << b.level + 1 << ">" << esc(b.text) << "</h" << b.level + 1 << ">\n"; break;
        case ReportBlock::Kind::Paragraph: o << "<p>" << esc(b.text) << "</p>\n"; break;
        case ReportBlock::Kind::Note: o << "<div class=\"note\">" << esc(b.text) << "</div>\n"; break;
        case ReportBlock::Kind::KeyValues:
            o << "<table class=\"kv\"><tbody>";
            for (const auto& [k, v] : b.keyValues) o << "<tr><th scope=\"row\">" << esc(k) << "</th><td>" << esc(v) << "</td></tr>";
            o << "</tbody></table>\n";
            break;
        case ReportBlock::Kind::Table: {
            const auto num = numericColumns(b.table);
            o << "<table>";
            if (!b.table.caption.empty()) o << "<caption>" << esc(b.table.caption) << "</caption>";
            o << "<thead><tr>";
            for (std::size_t c = 0; c < b.table.header.size(); ++c) o << "<th" << (num[c] ? " class=\"n\"" : "") << ">" << esc(b.table.header[c]) << "</th>";
            o << "</tr></thead><tbody>";
            for (std::size_t r = 0; r < b.table.rows.size(); ++r) {
                const bool hl = std::find(b.table.highlightRows.begin(), b.table.highlightRows.end(), static_cast<int>(r)) != b.table.highlightRows.end();
                o << (hl ? "<tr class=\"hl\">" : "<tr>");
                for (std::size_t c = 0; c < b.table.header.size(); ++c)
                    o << "<td" << (num[c] ? " class=\"n\"" : "") << ">" << esc(c < b.table.rows[r].size() ? b.table.rows[r][c] : "") << "</td>";
                o << "</tr>";
            }
            o << "</tbody></table>\n";
            break;
        }
        case ReportBlock::Kind::Chart: o << svgChart(b.chart) << "\n"; break;
        case ReportBlock::Kind::Section:
            o << "<div class=\"sections\">";
            for (const auto& s : b.sections) o << svgSection(s);
            o << "</div>\n";
            break;
        case ReportBlock::Kind::PageBreak: o << "<div class=\"pb\"></div>\n"; break;
        }
    }
    o << "</main></body></html>\n";
    return o.str();
}

// ---- PDF --------------------------------------------------------------------------------------------------
namespace {
class PdfLayout {
public:
    PdfLayout(PdfDocument& pdf, const ReportDocument& d, std::size_t totalPages) : pdf_(pdf), d_(d), total_(totalPages) {}

    void run() {
        newPage();
        // Title block on the first page.
        for (const auto& line : PdfDocument::wrap(d_.title, 20, contentW(), true)) {
            pdf_.text(kL, y_ + 20, line, 20, true, Rgb::hex(0x1D2329));
            y_ += 26;
        }
        if (!d_.subtitle.empty()) {
            pdf_.text(kL, y_ + 12, d_.subtitle, 12, false, Rgb::hex(0x5D6874));
            y_ += 18;
        }
        const Rgb accent = Rgb::hex(0x1F5F8B);
        pdf_.rect(kL, y_ + 4, contentW(), 2, &accent);
        y_ += 14;
        for (const auto& [k, v] : d_.meta) {
            pdf_.text(kL, y_ + 9, k, 8.5, true, Rgb::hex(0x5D6874));
            pdf_.textFit(kL + 110, y_ + 9, v, 8.5, contentW() - 110, false, Rgb::hex(0x5D6874));
            y_ += 12;
        }
        y_ += 10;
        for (const auto& b : d_.blocks) block(b);
        footer();
    }

    std::size_t pages() const { return page_; }

private:
    static constexpr double kL = 50, kR = 50, kT = 56, kB = 60;
    double contentW() const { return pdf_.pageSize().width - kL - kR; }
    double bottom() const { return pdf_.pageSize().height - kB; }

    void newPage() {
        if (page_ > 0) footer();
        pdf_.newPage(PageSize::a4Portrait());
        ++page_;
        y_ = kT;
        if (page_ > 1) {
            pdf_.textFit(kL, 34, d_.title, 8, contentW(), false, Rgb::hex(0x5D6874));
            pdf_.line(kL, 40, pdf_.pageSize().width - kR, 40, 0.4, Rgb::hex(0xD5DBE1));
        }
    }
    void footer() {
        const double y = pdf_.pageSize().height - 30;
        pdf_.line(kL, y - 10, pdf_.pageSize().width - kR, y - 10, 0.4, Rgb::hex(0xD5DBE1));
        pdf_.text(kL, y, persistence::kAppVersion, 7.5, false, Rgb::hex(0x5D6874));
        const std::string pg = "Page " + std::to_string(page_) + (total_ ? " of " + std::to_string(total_) : "");
        pdf_.text(pdf_.pageSize().width - kR, y, pg, 7.5, false, Rgb::hex(0x5D6874), Align::Right);
    }
    void need(double h) {
        if (y_ + h > bottom()) newPage();
    }

    void block(const ReportBlock& b) {
        switch (b.kind) {
        case ReportBlock::Kind::Heading: {
            const double size = b.level == 1 ? 15 : (b.level == 2 ? 12.5 : 11);
            need(size + 90); // keep headings with what follows
            y_ += b.level == 1 ? 14 : 8;
            for (const auto& l : PdfDocument::wrap(b.text, size, contentW(), true)) {
                pdf_.text(kL, y_ + size, l, size, true);
                y_ += size + 4;
            }
            if (b.level == 1) pdf_.line(kL, y_ + 1, kL + contentW(), y_ + 1, 0.6, Rgb::hex(0xD5DBE1));
            y_ += 6;
            break;
        }
        case ReportBlock::Kind::Paragraph:
        case ReportBlock::Kind::Note: {
            const bool note = b.kind == ReportBlock::Kind::Note;
            const double x = note ? kL + 10 : kL, w = note ? contentW() - 14 : contentW();
            const auto lines = PdfDocument::wrap(b.text, 9.5, w);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                need(13);
                if (note) {
                    const Rgb bar = Rgb::hex(0xE0A100), bg = Rgb::hex(0xFFF8E5);
                    pdf_.rect(kL, y_ - 1, contentW(), 13, &bg);
                    pdf_.rect(kL, y_ - 1, 3, 13, &bar);
                }
                pdf_.text(x, y_ + 9.5, lines[i], 9.5);
                y_ += 13;
            }
            y_ += 6;
            break;
        }
        case ReportBlock::Kind::KeyValues:
            for (const auto& [k, v] : b.keyValues) {
                const auto lines = PdfDocument::wrap(v, 9, contentW() - 190);
                need(12.0 * std::max<std::size_t>(1, lines.size()) + 2);
                pdf_.textFit(kL, y_ + 9, k, 9, 180, true);
                for (std::size_t i = 0; i < std::max<std::size_t>(1, lines.size()); ++i) {
                    if (i < lines.size()) pdf_.text(kL + 190, y_ + 9, lines[i], 9);
                    y_ += 12;
                }
                pdf_.line(kL, y_ - 1, kL + contentW(), y_ - 1, 0.3, Rgb::hex(0xE6EAEE));
            }
            y_ += 8;
            break;
        case ReportBlock::Kind::Table: table(b.table); break;
        case ReportBlock::Kind::Chart: chart(b.chart); break;
        case ReportBlock::Kind::Section: sections(b.sections); break;
        case ReportBlock::Kind::PageBreak: newPage(); break;
        }
    }

    void table(const ReportTable& t) {
        const auto num = numericColumns(t);
        const std::size_t nc = t.header.size();
        if (nc == 0) return;
        const double fs = nc > 8 ? 7 : 8;
        // Column widths: natural widths scaled to the content width.
        std::vector<double> w(nc, 0);
        for (std::size_t c = 0; c < nc; ++c) {
            w[c] = PdfDocument::textWidth(t.header[c], fs, true) + 8;
            for (const auto& r : t.rows)
                if (c < r.size()) w[c] = std::max(w[c], std::min(220.0, PdfDocument::textWidth(r[c], fs) + 8));
        }
        double tot = 0;
        for (double x : w) tot += x;
        for (auto& x : w) x *= contentW() / tot;
        auto header = [&] {
            const Rgb band = Rgb::hex(0xF3F6F8);
            pdf_.rect(kL, y_, contentW(), fs + 6, &band);
            double x = kL;
            for (std::size_t c = 0; c < nc; ++c) {
                if (num[c]) pdf_.textFit(x + w[c] - 3, y_ + fs + 1.5, t.header[c], fs, w[c] - 6, true, {}, Align::Right);
                else pdf_.textFit(x + 3, y_ + fs + 1.5, t.header[c], fs, w[c] - 6, true);
                x += w[c];
            }
            y_ += fs + 6;
        };
        if (!t.caption.empty()) {
            need(3 * (fs + 6) + 14);
            pdf_.text(kL, y_ + 9, t.caption, 9, true, Rgb::hex(0x5D6874));
            y_ += 14;
        } else need(3 * (fs + 6));
        header();
        for (std::size_t r = 0; r < t.rows.size(); ++r) {
            if (y_ + fs + 5 > bottom()) {
                newPage();
                header();
            }
            const bool hl = std::find(t.highlightRows.begin(), t.highlightRows.end(), static_cast<int>(r)) != t.highlightRows.end();
            const Rgb col = hl ? Rgb::hex(0xB42318) : Rgb{};
            double x = kL;
            for (std::size_t c = 0; c < nc; ++c) {
                const std::string& v = c < t.rows[r].size() ? t.rows[r][c] : std::string();
                if (num[c]) pdf_.textFit(x + w[c] - 3, y_ + fs + 1, v, fs, w[c] - 6, hl, col, Align::Right);
                else pdf_.textFit(x + 3, y_ + fs + 1, v, fs, w[c] - 6, hl, col);
                x += w[c];
            }
            y_ += fs + 5;
            pdf_.line(kL, y_, kL + contentW(), y_, 0.25, Rgb::hex(0xE6EAEE));
        }
        y_ += 10;
    }

    void chart(const LineChart& c) {
        const double H = 222, L = 46, B = 30, T = 30;
        need(H + 10);
        const double x0p = kL + L, x1p = kL + contentW() - 6, y0p = y_ + H - B, y1p = y_ + T;
        if (!c.title.empty()) pdf_.text(kL + contentW() / 2, y_ + 11, c.title, 9.5, true, {}, Align::Center);
        const Range r = chartRange(c);
        if (r.empty()) {
            pdf_.text(kL + contentW() / 2, y_ + H / 2, "No data", 9, false, {}, Align::Center);
            y_ += H + 8;
            return;
        }
        const auto xt = ticks(r.x0, r.x1), yt = ticks(r.y0, r.y1);
        const double ax0 = std::min(r.x0, xt.front()), ax1 = std::max(r.x1, xt.back()), ay0 = std::min(r.y0, yt.front()), ay1 = std::max(r.y1, yt.back());
        auto X = [&](double v) { return x0p + (v - ax0) / (ax1 - ax0 > 0 ? ax1 - ax0 : 1) * (x1p - x0p); };
        auto Y = [&](double v) { return y0p - (v - ay0) / (ay1 - ay0 > 0 ? ay1 - ay0 : 1) * (y0p - y1p); };
        const Rgb grid = Rgb::hex(0xE6EAEE), muted = Rgb::hex(0x5D6874);
        for (double v : yt) {
            pdf_.line(x0p, Y(v), x1p, Y(v), 0.4, grid);
            pdf_.text(x0p - 4, Y(v) + 2.5, tickLabel(v, yt), 7, false, muted, Align::Right);
        }
        for (double v : xt) {
            pdf_.line(X(v), y1p, X(v), y0p, 0.4, grid);
            pdf_.text(X(v), y0p + 10, tickLabel(v, xt), 7, false, muted, Align::Center);
        }
        if (c.yZeroLine) pdf_.line(x0p, Y(*c.yZeroLine), x1p, Y(*c.yZeroLine), 0.6, Rgb::hex(0x555555), 3);
        const Rgb frame = Rgb::hex(0x9AA4AE);
        pdf_.rect(x0p, y1p, x1p - x0p, y0p - y1p, nullptr, &frame, 0.6);
        double lx = x0p + 6;
        for (const auto& s : c.series) {
            const Rgb col = Rgb::hex(s.color);
            std::optional<std::pair<double, double>> prev;
            for (const auto& [x, y] : s.points) {
                if (!std::isfinite(y)) {
                    prev.reset();
                    continue;
                }
                if (prev) pdf_.line(X(prev->first), Y(prev->second), X(x), Y(y), 1.1, col, s.dashed ? 3 : 0);
                prev = std::make_pair(x, y);
            }
            pdf_.line(lx, y1p - 7, lx + 14, y1p - 7, 1.6, col, s.dashed ? 3 : 0);
            pdf_.text(lx + 17, y1p - 4.5, s.name, 7, false);
            lx += 26 + PdfDocument::textWidth(s.name, 7);
        }
        pdf_.text((x0p + x1p) / 2, y_ + H - 6, c.xLabel, 7.5, false, muted, Align::Center);
        if (!c.yLabel.empty()) pdf_.textFit(kL, y1p - 4, c.yLabel, 7.5, L - 4, false, muted);
        y_ += H + 8;
    }

    void sections(const std::vector<SectionPlot>& secs) {
        const int cols = 3;
        const double gap = 10, S = (contentW() - gap * (cols - 1)) / cols;
        for (std::size_t i = 0; i < secs.size(); i += cols) {
            need(S + 18);
            for (int k = 0; k < cols && i + k < secs.size(); ++k) {
                const auto& sp = secs[i + static_cast<std::size_t>(k)];
                const double ox = kL + k * (S + gap), oy = y_;
                const Rgb frame = Rgb::hex(0xD5DBE1);
                pdf_.rect(ox, oy, S, S, nullptr, &frame, 0.5);
                const Range r = sectionRange(sp);
                if (!r.empty()) {
                    const double span = std::max({r.x1 - r.x0, r.y1 - r.y0, 1e-6});
                    const double sc = (S - 20) / span, cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
                    auto poly = [&](const std::vector<std::pair<double, double>>& v, std::uint32_t col, double dash) {
                        for (std::size_t j = 0; j < v.size(); ++j) {
                            const auto& a = v[j];
                            const auto& b = v[(j + 1) % v.size()];
                            pdf_.line(ox + S / 2 + (a.first - cx) * sc, oy + S / 2 - (a.second - cy) * sc, ox + S / 2 + (b.first - cx) * sc, oy + S / 2 - (b.second - cy) * sc,
                                      1.0, Rgb::hex(col), dash);
                        }
                    };
                    poly(sp.envelope, 0x2CA02C, 3);
                    poly(sp.design, 0x1F77B4, 0);
                    poly(sp.asBuilt, 0xD62728, 0);
                }
                pdf_.textFit(ox + S / 2, oy + S + 11, sp.title, 7.5, S, false, Rgb::hex(0x5D6874), Align::Center);
            }
            y_ += S + 18;
        }
        y_ += 4;
    }

    PdfDocument& pdf_;
    const ReportDocument& d_;
    std::size_t total_;
    std::size_t page_ = 0;
    double y_ = 0;
};
} // namespace

std::string renderPdf(const ReportDocument& d, std::size_t* pagesOut) {
    // Pass 1 counts pages for the "page i of n" footers; pass 2 renders.
    PdfDocument dry(d.title, persistence::kAppVersion);
    PdfLayout l1(dry, d, 0);
    l1.run();
    PdfDocument pdf(d.title, persistence::kAppVersion);
    PdfLayout l2(pdf, d, l1.pages());
    l2.run();
    if (pagesOut) *pagesOut = l2.pages();
    return pdf.bytes();
}

Uuid storeReport(Project& p, const ReportDocument& d, const std::string& kind, const std::string& format) {
    std::string bytes;
    if (format == "html") bytes = renderHtml(d);
    else if (format == "pdf") bytes = renderPdf(d);
    else throw ValidationError("Report format must be html or pdf");
    return storeReportBytes(p, bytes, d.title, kind, format);
}

Uuid storeReportBytes(Project& p, const std::string& bytes, const std::string& name, const std::string& kind, const std::string& format) {
    if (format != "pdf" && format != "html" && format != "xlsx" && format != "csv")
        throw ValidationError("Unsupported report format '" + format + "' (pdf, html, xlsx or csv)");
    if (bytes.empty()) throw ValidationError("The report file is empty");
    Asset a = p.assets().writeBytes(bytes, AssetKind::Report, format);
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Store report: " + name);
    ed.add(a);
    Report r;
    r.name = name;
    r.kind = kind;
    r.format = format;
    r.asset.id = a.id;
    r.createdAt = utcTimestampNow();
    const Uuid id = ed.add(r);
    macro.commit();
    return id;
}

} // namespace openmine::reporting
