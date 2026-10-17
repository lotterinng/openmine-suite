// SPDX-License-Identifier: Apache-2.0
#include "openmine/reporting/ScheduleExport.h"

#include <xlsxwriter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace openmine::reporting {

namespace {
constexpr double kDay = 1440.0;
const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::optional<double> minutes(const std::string& s) {
    if (s.empty()) return std::nullopt;
    if (auto t = DateTime::parse(s)) return static_cast<double>(t->minutes());
    return std::nullopt;
}
std::string fixed(double v, int d) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}
} // namespace

std::size_t writeGanttPdf(const Project& project, const scheduling::ScheduleInput& in, const scheduling::ScheduleResult& r, const ScheduleBaseline* baseline,
                          const std::filesystem::path& file, const GanttOptions& o) {
    if (!r.ok) throw std::runtime_error("The schedule has not been calculated successfully");
    const auto rows = scheduling::scheduleTable(project, in, r, baseline);
    PdfDocument pdf(o.title.empty() ? project.model().info.name + " schedule" : o.title, project.userName());
    // Timeline range in whole days.
    double t0 = 1e300, t1 = -1e300;
    for (const auto& row : rows) {
        if (auto s = minutes(row.start)) t0 = std::min(t0, *s);
        if (auto f = minutes(row.finish)) t1 = std::max(t1, *f);
        if (auto s = minutes(row.baselineStart)) t0 = std::min(t0, *s);
        if (auto f = minutes(row.baselineFinish)) t1 = std::max(t1, *f);
    }
    if (o.from) t0 = static_cast<double>(o.from->days()) * kDay;
    if (o.to) t1 = static_cast<double>(o.to->days() + 1) * kDay;
    if (!(t1 > t0)) t1 = t0 + kDay;
    t0 = std::floor(t0 / kDay) * kDay;
    t1 = std::ceil(t1 / kDay) * kDay;
    if (!o.to) t1 += std::max(2.0, std::ceil((t1 - t0) / kDay * 0.02)) * kDay; // room for milestone diamonds and labels
    const PageSize ps = o.page;
    const double margin = 28, tableW = std::min(0.42 * (ps.width - 2 * margin), 470.0), headerH = 46, footerH = 20, rh = o.rowHeight;
    const double tlX = margin + tableW, tlW = ps.width - margin - tlX;
    const double scale = tlW / (t1 - t0); // points per minute
    auto X = [&](double t) { return tlX + (std::clamp(t, t0, t1) - t0) * scale; };
    struct Col {
        const char* title;
        double width;
        Align align;
    };
    const std::vector<Col> cols = {{"WBS", 0.12, Align::Left}, {"Activity", 0.38, Align::Left}, {"Start", 0.15, Align::Left},
                                   {"Finish", 0.15, Align::Left}, {"Dur (d)", 0.1, Align::Right}, {"TF (d)", 0.1, Align::Right}};
    const int perPage = std::max(1, static_cast<int>((ps.height - 2 * margin - headerH - footerH) / rh));
    const Rgb grid{0.85, 0.85, 0.85}, text{0.1, 0.1, 0.1}, critical = Rgb::hex(0xD62828), normal = Rgb::hex(0x2F6DB5), done = Rgb::hex(0x264653),
              base = Rgb::hex(0x9E9E9E), summary = Rgb::hex(0x222222), weekend{0.96, 0.96, 0.96}, status = Rgb::hex(0xE76F51), header{0.93, 0.95, 0.98};
    const double span = (t1 - t0) / kDay;
    const std::size_t pages = (rows.size() + static_cast<std::size_t>(perPage) - 1) / static_cast<std::size_t>(perPage);
    for (std::size_t pg = 0; pg < std::max<std::size_t>(1, pages); ++pg) {
        pdf.newPage(ps);
        const double top = margin + headerH;
        // Title and header.
        pdf.text(margin, margin - 8, pdf.pageCount() == 1 ? (o.title.empty() ? project.model().info.name + " — schedule" : o.title) : std::string(), 11, true);
        pdf.rect(margin, margin, ps.width - 2 * margin, headerH, &header, &grid);
        double cx = margin;
        for (const auto& c : cols) {
            const double w = c.width * tableW;
            pdf.text(c.align == Align::Right ? cx + w - 3 : cx + 3, margin + headerH - 6, c.title, 7.5, true, text, c.align);
            cx += w;
        }
        // Timescale: months on the first band, days / weeks on the second.
        {
            int y;
            unsigned m, d;
            Date day(static_cast<std::int64_t>(t0 / kDay));
            const Date last(static_cast<std::int64_t>(t1 / kDay));
            while (day < last) {
                day.toYmd(y, m, d);
                Date next = Date::fromYmd(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1);
                if (next > last) next = last;
                const double xa = X(static_cast<double>(day.days()) * kDay), xb = X(static_cast<double>(next.days()) * kDay);
                pdf.line(xa, margin, xa, margin + headerH, 0.4, grid);
                const std::string label = std::string(kMonths[m - 1]) + " " + std::to_string(y);
                if (xb - xa > PdfDocument::textWidth(label, 7, true) + 4) pdf.text((xa + xb) / 2, margin + 12, label, 7, true, text, Align::Center);
                day = next;
            }
            const double dayW = kDay * scale;
            const int stepDays = dayW >= 12 ? 1 : (dayW * 7 >= 14 ? 7 : 28);
            for (Date dd(static_cast<std::int64_t>(t0 / kDay)); dd < last; dd = dd.addDays(1)) {
                const double xa = X(static_cast<double>(dd.days()) * kDay);
                if (dayW >= 3 && dd.isoWeekday() >= 6)
                    pdf.rect(xa, top, dayW, perPage * rh, &weekend); // weekend shading
                const bool tick = stepDays == 1 || (stepDays == 7 && dd.isoWeekday() == 1) || (stepDays == 28 && dd.isoWeekday() == 1 && (dd.days() / 7) % 4 == 0);
                if (!tick) continue;
                pdf.line(xa, margin + 18, xa, margin + headerH, 0.3, grid);
                unsigned m2, d2;
                int y2;
                dd.toYmd(y2, m2, d2);
                if (stepDays * dayW > 10) pdf.text(xa + 1.5, margin + headerH - 6, std::to_string(d2), 6, false, text);
            }
            (void)span;
        }
        // Rows.
        std::map<Uuid, double> rowY; // centre line of each activity on this page
        const std::size_t first = pg * static_cast<std::size_t>(perPage), lastRow = std::min(rows.size(), first + static_cast<std::size_t>(perPage));
        for (std::size_t i = first; i < lastRow; ++i) {
            const auto& row = rows[i];
            const double y = top + static_cast<double>(i - first) * rh;
            rowY[row.id] = y + rh / 2;
            if ((i - first) % 2 == 1) {
                const Rgb band{0.985, 0.985, 0.985};
                pdf.rect(margin, y, tableW, rh, &band);
            }
            pdf.line(margin, y + rh, ps.width - margin, y + rh, 0.2, grid);
            const bool isSummary = row.kind == "Summary";
            const std::vector<std::string> cells = {row.wbs, std::string(static_cast<std::size_t>(row.depth) * 2, ' ') + row.name, row.startDate, row.finishDate,
                                                    isSummary ? "" : fixed(row.durationDays, 1), isSummary ? "" : fixed(row.totalFloatDays, 1)};
            double x = margin;
            for (std::size_t c = 0; c < cols.size(); ++c) {
                const double w = cols[c].width * tableW;
                const double indent = c == 1 ? row.depth * 8.0 : 0;
                pdf.textFit(cols[c].align == Align::Right ? x + w - 3 : x + 3 + indent, y + rh - 4, c == 1 ? row.name : cells[c], 7, w - 6 - indent,
                            isSummary, row.critical && c == 1 ? critical : text, cols[c].align);
                x += w;
            }
            const auto s = minutes(row.start), f = minutes(row.finish);
            if (!s || !f) continue;
            const double xa = X(*s), xb = X(*f);
            if (row.kind == "Milestone" || *f - *s < 1e-6) {
                const double cy = y + rh / 2, h = rh * 0.35;
                pdf.polygon({{xa, cy - h}, {xa + h, cy}, {xa, cy + h}, {xa - h, cy}}, row.critical ? critical : summary);
                continue;
            }
            if (isSummary) {
                const double by = y + rh * 0.35, bh = rh * 0.22;
                pdf.rect(xa, by, std::max(xb - xa, 0.5), bh, &summary);
                pdf.polygon({{xa, by}, {xa + 3, by}, {xa, by + bh + 3}}, summary);
                pdf.polygon({{xb, by}, {xb - 3, by}, {xb, by + bh + 3}}, summary);
                continue;
            }
            const double by = y + rh * 0.2, bh = rh * (o.showBaseline && row.hasBaseline ? 0.42 : 0.6);
            const Rgb fill = row.critical ? critical : normal;
            pdf.rect(xa, by, std::max(xb - xa, 0.75), bh, &fill);
            if (row.percent > 0) pdf.rect(xa, by + bh * 0.3, std::max((xb - xa) * std::min(row.percent, 100.0) / 100.0, 0.5), bh * 0.4, &done);
            if (o.showBaseline && row.hasBaseline)
                if (auto bs = minutes(row.baselineStart), bf = minutes(row.baselineFinish); bs && bf)
                    pdf.rect(X(*bs), by + bh + 1.5, std::max(X(*bf) - X(*bs), 0.75), rh * 0.16, &base);
            if (xb - xa > 40 && !row.resources.empty()) pdf.textFit(xb + 3, y + rh - 4, row.resources, 6, tlX + tlW - xb - 4, false, text);
        }
        // Dependencies (FS shown as elbow arrows; others as straight connectors).
        if (o.showDependencies)
            for (const auto& d : in.dependencies) {
                const auto py = rowY.find(d.predecessorId), sy = rowY.find(d.successorId);
                if (py == rowY.end() || sy == rowY.end()) continue;
                const auto pt = r.timings.find(d.predecessorId), st = r.timings.find(d.successorId);
                if (pt == r.timings.end() || st == r.timings.end()) continue;
                const bool fromEnd = d.type == DependencyType::FinishToStart || d.type == DependencyType::FinishToFinish;
                const bool toStart = d.type == DependencyType::FinishToStart || d.type == DependencyType::StartToStart;
                const double x1 = X(fromEnd ? pt->second.ef : pt->second.es), x2 = X(toStart ? st->second.es : st->second.ef);
                const double y1 = py->second, y2 = sy->second;
                const Rgb c = pt->second.critical && st->second.critical ? critical : Rgb{0.35, 0.35, 0.35};
                const double elbow = toStart ? std::min(x1 + 4, x2 - 2) : std::max(x1, x2) + 4;
                pdf.line(x1, y1, elbow, y1, 0.4, c);
                pdf.line(elbow, y1, elbow, y2, 0.4, c);
                pdf.line(elbow, y2, x2, y2, 0.4, c);
                const double dir = x2 >= elbow ? 1 : -1;
                pdf.polygon({{x2, y2}, {x2 - 3 * dir, y2 - 1.8}, {x2 - 3 * dir, y2 + 1.8}}, c);
            }
        // Status date and frame.
        if (in.statusDate) {
            const double xs = X(*in.statusDate);
            pdf.line(xs, margin, xs, top + perPage * rh, 0.8, status, 2);
            pdf.text(xs + 2, margin + 26, "Status " + scheduling::toDateTime(*in.statusDate).date().toString(), 6, true, status);
        }
        pdf.line(tlX, margin, tlX, top + perPage * rh, 0.6, Rgb{0.5, 0.5, 0.5});
        pdf.rect(margin, margin, ps.width - 2 * margin, headerH + perPage * rh, nullptr, &grid, 0.6);
        // Footer and legend.
        const double fy = ps.height - margin + 4;
        pdf.text(margin, fy, project.model().info.name + "  ·  printed " + Date::todayUtc().toString() + (baseline ? "  ·  baseline: " + baseline->name : ""), 7, false, text);
        pdf.text(ps.width - margin, fy, "Page " + std::to_string(pg + 1) + " of " + std::to_string(std::max<std::size_t>(1, pages)), 7, false, text, Align::Right);
        double lx = margin + tableW;
        auto legend = [&](const Rgb& c, const std::string& label) {
            pdf.rect(lx, fy - 6, 10, 5, &c);
            pdf.text(lx + 13, fy, label, 6.5, false, text);
            lx += 20 + PdfDocument::textWidth(label, 6.5);
        };
        legend(critical, "Critical");
        legend(normal, "Non-critical");
        legend(done, "Progress");
        if (baseline) legend(base, "Baseline");
    }
    pdf.save(file);
    return pdf.pageCount();
}

namespace {
struct Workbook {
    lxw_workbook* wb;
    explicit Workbook(const std::filesystem::path& f) : wb(workbook_new(f.string().c_str())) {
        if (!wb) throw std::runtime_error("Cannot create " + f.string());
    }
    ~Workbook() {
        if (wb) workbook_close(wb);
    }
    void close() {
        const lxw_error e = workbook_close(wb);
        wb = nullptr;
        if (e != LXW_NO_ERROR) throw std::runtime_error(std::string("Writing the workbook failed: ") + lxw_strerror(e));
    }
};
lxw_datetime toLxw(DateTime t) {
    int y;
    unsigned m, d;
    t.date().toYmd(y, m, d);
    const int mod = static_cast<int>(t.minuteOfDay());
    return {y, static_cast<int>(m), static_cast<int>(d), mod / 60, mod % 60, 0.0};
}
std::string sheetName(std::string s) {
    for (auto& c : s)
        if (std::string("[]:*?/\\").find(c) != std::string::npos) c = '_';
    if (s.size() > 31) s.resize(31);
    return s.empty() ? "Sheet" : s;
}
} // namespace

void writeScheduleXlsx(const Project& project, const std::vector<scheduling::ScheduleRow>& rows, const std::filesystem::path& file) {
    Workbook book(file);
    lxw_workbook* wb = book.wb;
    lxw_format* hdr = workbook_add_format(wb);
    format_set_bold(hdr);
    format_set_bg_color(hdr, 0xDCE6F1);
    format_set_border(hdr, LXW_BORDER_THIN);
    lxw_format* dateFmt = workbook_add_format(wb);
    format_set_num_format(dateFmt, "yyyy-mm-dd hh:mm");
    lxw_format* num1 = workbook_add_format(wb);
    format_set_num_format(num1, "0.0");
    lxw_format* num2 = workbook_add_format(wb);
    format_set_num_format(num2, "0.00");
    lxw_format* critFmt = workbook_add_format(wb);
    format_set_font_color(critFmt, 0xC00000);
    format_set_bold(critFmt);
    std::vector<lxw_format*> indent(8);
    std::vector<lxw_format*> indentBold(8);
    for (int k = 0; k < 8; ++k) {
        indent[static_cast<std::size_t>(k)] = workbook_add_format(wb);
        format_set_indent(indent[static_cast<std::size_t>(k)], static_cast<std::uint8_t>(k));
        indentBold[static_cast<std::size_t>(k)] = workbook_add_format(wb);
        format_set_indent(indentBold[static_cast<std::size_t>(k)], static_cast<std::uint8_t>(k));
        format_set_bold(indentBold[static_cast<std::size_t>(k)]);
    }
    lxw_worksheet* ws = workbook_add_worksheet(wb, "Schedule");
    const std::vector<std::pair<const char*, double>> cols = {
        {"WBS", 12}, {"Activity", 40}, {"Kind", 10}, {"Start", 17}, {"Finish", 17}, {"Duration (d)", 12}, {"Total float (d)", 14}, {"Free float (d)", 13},
        {"Critical", 9}, {"Quantity", 11}, {"Actual", 11}, {"Remaining", 11}, {"% complete", 11}, {"Unit", 7}, {"Resources", 28}, {"Design objects", 24},
        {"Calendar", 18}, {"Baseline start", 17}, {"Baseline finish", 17}, {"Finish variance (d)", 17}, {"Duration basis", 30}, {"Design impact", 30}};
    for (std::size_t c = 0; c < cols.size(); ++c) {
        worksheet_write_string(ws, 0, static_cast<lxw_col_t>(c), cols[c].first, hdr);
        worksheet_set_column(ws, static_cast<lxw_col_t>(c), static_cast<lxw_col_t>(c), cols[c].second, nullptr);
    }
    lxw_row_t r = 1;
    for (const auto& row : rows) {
        const bool sum = row.kind == "Summary";
        const auto depth = static_cast<std::size_t>(std::clamp(row.depth, 0, 7));
        worksheet_write_string(ws, r, 0, row.wbs.c_str(), nullptr);
        worksheet_write_string(ws, r, 1, row.name.c_str(), sum ? indentBold[depth] : indent[depth]);
        worksheet_write_string(ws, r, 2, row.kind.c_str(), nullptr);
        auto writeDate = [&](lxw_col_t c, const std::string& s) {
            if (auto t = DateTime::parse(s)) {
                lxw_datetime d = toLxw(*t);
                worksheet_write_datetime(ws, r, c, &d, dateFmt);
            }
        };
        writeDate(3, row.start);
        writeDate(4, row.finish);
        if (!sum) {
            worksheet_write_number(ws, r, 5, row.durationDays, num1);
            worksheet_write_number(ws, r, 6, row.totalFloatDays, num1);
            worksheet_write_number(ws, r, 7, row.freeFloatDays, num1);
        }
        worksheet_write_boolean(ws, r, 8, row.critical, row.critical ? critFmt : nullptr);
        if (row.quantity > 0) {
            worksheet_write_number(ws, r, 9, row.quantity, num2);
            worksheet_write_number(ws, r, 10, row.actual, num2);
            worksheet_write_number(ws, r, 11, row.remaining, num2);
        }
        worksheet_write_number(ws, r, 12, row.percent / 100.0, nullptr);
        worksheet_write_string(ws, r, 13, row.unit.c_str(), nullptr);
        worksheet_write_string(ws, r, 14, row.resources.c_str(), nullptr);
        worksheet_write_string(ws, r, 15, row.linkedDesign.c_str(), nullptr);
        worksheet_write_string(ws, r, 16, row.calendar.c_str(), nullptr);
        if (row.hasBaseline) {
            writeDate(17, row.baselineStart);
            writeDate(18, row.baselineFinish);
            worksheet_write_number(ws, r, 19, row.finishVarianceDays, num1);
        }
        worksheet_write_string(ws, r, 20, row.durationBasis.c_str(), nullptr);
        worksheet_write_string(ws, r, 21, row.designImpact.c_str(), nullptr);
        ++r;
    }
    lxw_format* pct = workbook_add_format(wb);
    format_set_num_format(pct, "0%");
    worksheet_set_column(ws, 12, 12, 11, pct);
    worksheet_freeze_panes(ws, 1, 2);
    if (r > 1) worksheet_autofilter(ws, 0, 0, r - 1, static_cast<lxw_col_t>(cols.size() - 1));

    // Weekly Gantt sheet.
    double t0 = 1e300, t1 = -1e300;
    for (const auto& row : rows) {
        if (auto s = minutes(row.start)) t0 = std::min(t0, *s);
        if (auto f = minutes(row.finish)) t1 = std::max(t1, *f);
    }
    lxw_worksheet* gs = workbook_add_worksheet(wb, "Gantt");
    worksheet_write_string(gs, 0, 0, "Activity", hdr);
    worksheet_set_column(gs, 0, 0, 40, nullptr);
    if (t1 > t0) {
        Date w0(static_cast<std::int64_t>(std::floor(t0 / kDay)));
        w0 = w0.addDays(1 - w0.isoWeekday()); // Monday
        const auto weeks = static_cast<int>(std::ceil((t1 / kDay - static_cast<double>(w0.days())) / 7.0));
        lxw_format* wk = workbook_add_format(wb);
        format_set_num_format(wk, "dd mmm");
        format_set_rotation(wk, 90);
        format_set_bold(wk);
        lxw_format* bar = workbook_add_format(wb);
        format_set_bg_color(bar, 0x2F6DB5);
        lxw_format* cbar = workbook_add_format(wb);
        format_set_bg_color(cbar, 0xD62828);
        lxw_format* sbar = workbook_add_format(wb);
        format_set_bg_color(sbar, 0x404040);
        for (int k = 0; k < weeks; ++k) {
            lxw_datetime d = toLxw(DateTime::fromDate(w0.addDays(7 * k)));
            worksheet_write_datetime(gs, 0, static_cast<lxw_col_t>(k + 1), &d, wk);
        }
        worksheet_set_column(gs, 1, static_cast<lxw_col_t>(weeks), 3, nullptr);
        worksheet_set_row(gs, 0, 48, nullptr);
        lxw_row_t gr = 1;
        for (const auto& row : rows) {
            const auto depth = static_cast<std::size_t>(std::clamp(row.depth, 0, 7));
            worksheet_write_string(gs, gr, 0, row.name.c_str(), row.kind == "Summary" ? indentBold[depth] : indent[depth]);
            const auto s = minutes(row.start), f = minutes(row.finish);
            if (s && f)
                for (int k = 0; k < weeks; ++k) {
                    const double ws0 = static_cast<double>(w0.addDays(7 * k).days()) * kDay, ws1 = ws0 + 7 * kDay;
                    if (*s < ws1 && (*f > ws0 || (*f == *s && *s >= ws0)))
                        worksheet_write_blank(gs, gr, static_cast<lxw_col_t>(k + 1), row.kind == "Summary" ? sbar : (row.critical ? cbar : bar));
                }
            ++gr;
        }
        worksheet_freeze_panes(gs, 1, 1);
    }
    lxw_doc_properties props{};
    const std::string title = project.model().info.name + " schedule";
    props.title = const_cast<char*>(title.c_str());
    props.author = const_cast<char*>("OpenMine Suite");
    workbook_set_properties(wb, &props);
    book.close();
}

void writeXlsx(const std::filesystem::path& file, const std::vector<Sheet>& sheets) {
    Workbook book(file);
    lxw_workbook* wb = book.wb;
    lxw_format* hdr = workbook_add_format(wb);
    format_set_bold(hdr);
    format_set_bg_color(hdr, 0xDCE6F1);
    format_set_border(hdr, LXW_BORDER_THIN);
    lxw_format* dateFmt = workbook_add_format(wb);
    format_set_num_format(dateFmt, "yyyy-mm-dd hh:mm");
    for (const auto& sh : sheets) {
        lxw_worksheet* ws = workbook_add_worksheet(wb, sheetName(sh.name).c_str());
        if (!ws) throw std::runtime_error("Duplicate or invalid sheet name '" + sh.name + "'");
        for (std::size_t c = 0; c < sh.header.size(); ++c) {
            worksheet_write_string(ws, 0, static_cast<lxw_col_t>(c), sh.header[c].c_str(), hdr);
            const double w = c < sh.columnWidths.size() ? sh.columnWidths[c] : std::max<double>(10, static_cast<double>(sh.header[c].size()) + 2);
            worksheet_set_column(ws, static_cast<lxw_col_t>(c), static_cast<lxw_col_t>(c), w, nullptr);
        }
        lxw_row_t r = 1;
        for (const auto& row : sh.rows) {
            for (std::size_t c = 0; c < row.size(); ++c) {
                const auto col = static_cast<lxw_col_t>(c);
                if (const auto* s = std::get_if<std::string>(&row[c])) worksheet_write_string(ws, r, col, s->c_str(), nullptr);
                else if (const auto* d = std::get_if<double>(&row[c])) {
                    if (std::isfinite(*d)) worksheet_write_number(ws, r, col, *d, nullptr);
                } else if (const auto* t = std::get_if<DateTime>(&row[c])) {
                    lxw_datetime x = toLxw(*t);
                    worksheet_write_datetime(ws, r, col, &x, dateFmt);
                }
            }
            ++r;
        }
        worksheet_freeze_panes(ws, 1, 0);
        if (r > 1 && !sh.header.empty()) worksheet_autofilter(ws, 0, 0, r - 1, static_cast<lxw_col_t>(sh.header.size() - 1));
    }
    book.close();
}

} // namespace openmine::reporting
