// SPDX-License-Identifier: Apache-2.0
#include "openmine/geology/Drillholes.h"

#include "openmine/project/ProjectEditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

namespace openmine::geology {

namespace {
using util::CsvTable;
using Sev = DrillholeIssue::Severity;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

const std::map<std::string, std::vector<std::string_view>>& roleNames() {
    static const std::map<std::string, std::vector<std::string_view>> m = {
        {"holeId", {"holeid", "hole_id", "hole id", "hole", "bhid", "dhid", "hole_name", "holename", "id"}},
        {"x", {"x", "east", "easting", "xcollar", "x_collar", "collar_x", "e"}},
        {"y", {"y", "north", "northing", "ycollar", "y_collar", "collar_y", "n"}},
        {"z", {"z", "rl", "elev", "elevation", "zcollar", "z_collar", "collar_z", "collar_rl"}},
        {"length", {"length", "depth", "max_depth", "maxdepth", "eoh", "total_depth", "td"}},
        {"type", {"type", "hole_type", "holetype", "drill_type"}},
        {"date", {"date", "drilled", "end_date", "date_drilled"}},
        {"depth", {"depth", "at", "distance", "survey_depth", "md"}},
        {"azimuth", {"azimuth", "azi", "az", "bearing", "brg"}},
        {"dip", {"dip", "inclination", "incl", "inc"}},
        {"from", {"from", "depth_from", "mfrom", "from_m", "fromdepth"}},
        {"to", {"to", "depth_to", "mto", "to_m", "todepth"}},
    };
    return m;
}

std::optional<std::size_t> findColumn(const CsvTable& t, const std::string& role, const DrillholeImportOptions& o) {
    if (auto it = o.columns.find(role); it != o.columns.end()) return t.column(std::string_view(it->second));
    for (auto n : roleNames().at(role))
        if (auto c = t.column(n)) return c;
    return std::nullopt;
}

struct Parsed {
    enum Kind { Missing, Number, Below, Above, Text } kind = Missing;
    double value = 0;
};

Parsed parseValue(const std::string& raw, const DrillholeImportOptions& o) {
    const std::string s = trim(raw);
    for (const auto& m : o.missingTokens)
        if (lower(s) == lower(m)) return {};
    if (!s.empty() && (s[0] == '<' || s[0] == '>')) {
        if (auto v = util::parseNumber(std::string_view(s).substr(1))) return {s[0] == '<' ? Parsed::Below : Parsed::Above, *v};
    }
    if (auto v = util::parseNumber(s)) return {Parsed::Number, *v};
    return {Parsed::Text, 0};
}

std::optional<double> numericValue(const Parsed& p, const DrillholeImportOptions& o, DrillholeReadResult& r) {
    switch (p.kind) {
    case Parsed::Number: return p.value;
    case Parsed::Above: ++r.aboveDetectionCount; return p.value;
    case Parsed::Below:
        ++r.belowDetectionCount;
        switch (o.belowDetection) {
        case BelowDetection::HalfLimit: return p.value / 2;
        case BelowDetection::Limit: return p.value;
        case BelowDetection::Zero: return 0.0;
        case BelowDetection::Missing: return std::nullopt;
        }
        return std::nullopt;
    case Parsed::Missing: ++r.missingCount; return std::nullopt;
    case Parsed::Text: return std::nullopt;
    }
    return std::nullopt;
}

void issue(DrillholeReadResult& r, Sev s, std::string table, std::size_t row, std::string hole, std::string msg) {
    r.issues.push_back({s, std::move(table), row, std::move(hole), std::move(msg)});
}

std::string fmt(double v) {
    std::ostringstream ss;
    ss << std::setprecision(10) << v;
    return ss.str();
}
} // namespace

std::size_t DrillholeReadResult::errors() const {
    return static_cast<std::size_t>(std::count_if(issues.begin(), issues.end(), [](const auto& i) { return i.severity == Sev::Error; }));
}

std::vector<DrillholeInterval> mergeIntervals(const std::vector<DrillholeInterval>& a, const std::vector<DrillholeInterval>& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::vector<double> cuts;
    for (const auto* v : {&a, &b})
        for (const auto& i : *v) {
            cuts.push_back(i.from);
            cuts.push_back(i.to);
        }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double x, double y) { return std::abs(x - y) < 1e-9; }), cuts.end());
    auto covering = [](const std::vector<DrillholeInterval>& v, double from, double to) -> const DrillholeInterval* {
        const double mid = 0.5 * (from + to);
        for (const auto& i : v)
            if (i.from <= mid && mid < i.to) return &i;
        return nullptr;
    };
    std::vector<DrillholeInterval> out;
    for (std::size_t k = 0; k + 1 < cuts.size(); ++k) {
        const auto* ia = covering(a, cuts[k], cuts[k + 1]);
        const auto* ib = covering(b, cuts[k], cuts[k + 1]);
        if (!ia && !ib) continue;
        DrillholeInterval iv;
        iv.from = cuts[k];
        iv.to = cuts[k + 1];
        for (const auto* src : {ia, ib}) {
            if (!src) continue;
            for (const auto& [kname, v] : src->values) iv.values[kname] = v;
            for (const auto& [kname, v] : src->codes) iv.codes[kname] = v;
        }
        out.push_back(std::move(iv));
    }
    return out;
}

DrillholeReadResult readDrillholes(const DrillholeTables& t, const DrillholeImportOptions& o) {
    DrillholeReadResult r;
    // ---- collars
    const auto& C = t.collars;
    const auto cId = findColumn(C, "holeId", o), cx = findColumn(C, "x", o), cy = findColumn(C, "y", o), cz = findColumn(C, "z", o);
    if (!cId || !cx || !cy || !cz) {
        issue(r, Sev::Error, "collar", 0, {}, "Collar table needs hole ID, X, Y and Z columns");
        return r;
    }
    const auto cLen = findColumn(C, "length", o), cType = findColumn(C, "type", o), cDate = findColumn(C, "date", o);
    std::map<std::string, Drillhole> holes;
    std::set<std::string> rejected;
    std::map<std::string, bool> hasLength;
    for (std::size_t i = 0; i < C.rows.size(); ++i) {
        const auto& row = C.rows[i];
        const std::string id = trim(row[*cId]);
        if (id.empty()) {
            issue(r, Sev::Error, "collar", i + 1, {}, "Empty hole ID");
            ++r.rejectedRows;
            continue;
        }
        if (holes.count(id) || rejected.count(id)) {
            issue(r, Sev::Error, "collar", i + 1, id, "Duplicate collar; the first record is kept");
            ++r.rejectedRows;
            continue;
        }
        const auto x = util::parseNumber(row[*cx]), y = util::parseNumber(row[*cy]), z = util::parseNumber(row[*cz]);
        if (!x || !y || !z) {
            issue(r, Sev::Error, "collar", i + 1, id, "Collar coordinates are missing or not numeric; hole rejected");
            rejected.insert(id);
            ++r.rejectedRows;
            continue;
        }
        Drillhole h;
        h.holeId = id;
        h.collar = {*x, *y, *z};
        if (cLen) {
            if (auto L = util::parseNumber(row[*cLen]); L && *L > 0) {
                h.length = *L;
                hasLength[id] = true;
            } else if (!trim(row[*cLen]).empty()) issue(r, Sev::Warning, "collar", i + 1, id, "Invalid hole length '" + row[*cLen] + "'; derived from the data");
        }
        if (cType && !trim(row[*cType]).empty()) h.holeType = trim(row[*cType]);
        if (cDate && !trim(row[*cDate]).empty()) {
            if (auto d = Date::parse(trim(row[*cDate]))) h.drilledDate = *d;
            else issue(r, Sev::Warning, "collar", i + 1, id, "Unrecognised date '" + row[*cDate] + "' (expected YYYY-MM-DD); not imported");
        }
        holes.emplace(id, std::move(h));
    }

    // ---- surveys
    if (!t.surveys.headers.empty()) {
        const auto& S = t.surveys;
        const auto sId = findColumn(S, "holeId", o), sD = findColumn(S, "depth", o), sA = findColumn(S, "azimuth", o), sI = findColumn(S, "dip", o);
        if (!sId || !sD || !sA || !sI) issue(r, Sev::Error, "survey", 0, {}, "Survey table needs hole ID, depth, azimuth and dip columns");
        else
            for (std::size_t i = 0; i < S.rows.size(); ++i) {
                const auto& row = S.rows[i];
                const std::string id = trim(row[*sId]);
                auto it = holes.find(id);
                if (it == holes.end()) {
                    issue(r, Sev::Error, "survey", i + 1, id, rejected.count(id) ? "Hole was rejected" : "No collar for this hole");
                    ++r.rejectedRows;
                    continue;
                }
                const auto d = util::parseNumber(row[*sD]), a = util::parseNumber(row[*sA]), dip = util::parseNumber(row[*sI]);
                if (!d || !a || !dip || *d < 0) {
                    issue(r, Sev::Error, "survey", i + 1, id, "Survey depth, azimuth or dip missing or invalid");
                    ++r.rejectedRows;
                    continue;
                }
                if (*dip < -90 || *dip > 90) {
                    issue(r, Sev::Error, "survey", i + 1, id, "Dip " + fmt(*dip) + "° outside ±90°");
                    ++r.rejectedRows;
                    continue;
                }
                double az = std::fmod(*a, 360.0);
                if (az < 0) az += 360;
                if (az != *a) issue(r, Sev::Info, "survey", i + 1, id, "Azimuth " + fmt(*a) + "° normalised to " + fmt(az) + "°");
                auto& sv = it->second.surveys;
                if (std::any_of(sv.begin(), sv.end(), [&](const auto& s) { return std::abs(s.depth - *d) < 1e-9; })) {
                    issue(r, Sev::Error, "survey", i + 1, id, "Duplicate survey depth " + fmt(*d) + " m");
                    ++r.rejectedRows;
                    continue;
                }
                sv.push_back({*d, az, *dip});
            }
    }

    // ---- interval tables
    for (const auto& [name, T] : t.intervals) {
        const auto iId = findColumn(T, "holeId", o), iF = findColumn(T, "from", o), iT = findColumn(T, "to", o);
        if (!iId || !iF || !iT) {
            issue(r, Sev::Error, name, 0, {}, "Interval table needs hole ID, from and to columns");
            continue;
        }
        // Classify the data columns.
        std::vector<std::pair<std::size_t, bool>> fields; // column, numeric
        for (std::size_t c = 0; c < T.headers.size(); ++c) {
            if (c == *iId || c == *iF || c == *iT) continue;
            const auto& hname = T.headers[c];
            auto inList = [&](const std::vector<std::string>& l) {
                return std::any_of(l.begin(), l.end(), [&](const std::string& s) { return lower(s) == lower(hname); });
            };
            if (inList(o.ignoreColumns)) continue;
            if (!isValidAttributeKey(hname)) {
                issue(r, Sev::Warning, name, 0, {}, "Column '" + hname + "' is not a valid field name (letters, digits, underscore); skipped");
                continue;
            }
            if (r.fieldTables.count(hname) && r.fieldTables[hname] != name) {
                issue(r, Sev::Error, name, 0, {}, "Field '" + hname + "' also appears in table '" + r.fieldTables[hname] + "'; skipped");
                continue;
            }
            bool numeric = !inList(o.codeColumns);
            std::size_t present = 0;
            if (numeric)
                for (const auto& row : T.rows) {
                    const auto p = parseValue(row[c], o);
                    if (p.kind == Parsed::Text) {
                        numeric = false;
                        break;
                    }
                    if (p.kind != Parsed::Missing) ++present;
                }
            if (numeric && present == 0) issue(r, Sev::Warning, name, 0, {}, "Column '" + hname + "' has no values");
            fields.emplace_back(c, numeric);
            r.fieldTables[hname] = name;
            (numeric ? r.numericFields : r.codeFields).push_back(hname);
        }
        std::map<std::string, std::vector<DrillholeInterval>> perHole;
        for (std::size_t i = 0; i < T.rows.size(); ++i) {
            const auto& row = T.rows[i];
            const std::string id = trim(row[*iId]);
            if (!holes.count(id)) {
                issue(r, Sev::Error, name, i + 1, id, rejected.count(id) ? "Hole was rejected" : "No collar for this hole");
                ++r.rejectedRows;
                continue;
            }
            const auto f = util::parseNumber(row[*iF]), to = util::parseNumber(row[*iT]);
            if (!f || !to || *f < 0 || *f >= *to) {
                issue(r, Sev::Error, name, i + 1, id, "Invalid from/to (" + row[*iF] + ", " + row[*iT] + ")");
                ++r.rejectedRows;
                continue;
            }
            DrillholeInterval iv;
            iv.from = *f;
            iv.to = *to;
            for (const auto& [c, numeric] : fields) {
                const auto p = parseValue(row[c], o);
                if (numeric) {
                    if (auto v = numericValue(p, o, r)) iv.values[T.headers[c]] = *v;
                } else if (p.kind != Parsed::Missing) iv.codes[T.headers[c]] = trim(row[c]);
                else ++r.missingCount;
            }
            perHole[id].push_back(std::move(iv));
            // Remember the source row for messages.
            perHole[id].back().codes.emplace("__row", std::to_string(i + 1));
        }
        for (auto& [id, list] : perHole) {
            std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.from < b.from; });
            std::vector<DrillholeInterval> kept;
            for (auto& iv : list) {
                const std::size_t row = std::stoul(iv.codes["__row"]);
                iv.codes.erase("__row");
                if (!kept.empty() && iv.from < kept.back().to - 1e-9) {
                    issue(r, Sev::Error, name, row, id,
                          "Interval " + fmt(iv.from) + "–" + fmt(iv.to) + " m overlaps " + fmt(kept.back().from) + "–" + fmt(kept.back().to) + " m; rejected");
                    ++r.rejectedRows;
                    continue;
                }
                kept.push_back(std::move(iv));
            }
            auto& h = holes.at(id);
            h.intervals = mergeIntervals(h.intervals, kept);
        }
    }

    // ---- hole lengths and final checks
    for (auto& [id, h] : holes) {
        double maxDepth = 0;
        for (const auto& s : h.surveys) maxDepth = std::max(maxDepth, s.depth);
        for (const auto& iv : h.intervals) maxDepth = std::max(maxDepth, iv.to);
        if (!hasLength[id]) {
            if (maxDepth <= 0) {
                issue(r, Sev::Error, "collar", 0, id, "No hole length and no downhole data; hole rejected");
                ++r.rejectedHoles;
                continue;
            }
            h.length = maxDepth;
            issue(r, Sev::Info, "collar", 0, id, "Hole length taken from the deepest record (" + fmt(maxDepth) + " m)");
        } else {
            auto& iv = h.intervals;
            const auto before = iv.size();
            iv.erase(std::remove_if(iv.begin(), iv.end(),
                                    [&](const DrillholeInterval& x) {
                                        if (x.to <= h.length + 1e-9) return false;
                                        issue(r, Sev::Error, "intervals", 0, id,
                                              "Interval " + fmt(x.from) + "–" + fmt(x.to) + " m extends beyond the hole length " + fmt(h.length) + " m; rejected");
                                        return true;
                                    }),
                     iv.end());
            r.rejectedRows += before - iv.size();
            auto& sv = h.surveys;
            const auto sb = sv.size();
            sv.erase(std::remove_if(sv.begin(), sv.end(),
                                    [&](const DrillholeSurvey& s) {
                                        if (s.depth <= h.length + 1e-9) return false;
                                        issue(r, Sev::Error, "survey", 0, id, "Survey at " + fmt(s.depth) + " m is beyond the hole length; rejected");
                                        return true;
                                    }),
                     sv.end());
            r.rejectedRows += sb - sv.size();
        }
        std::sort(h.surveys.begin(), h.surveys.end(), [](const auto& a, const auto& b) { return a.depth < b.depth; });
        if (h.surveys.empty()) {
            h.surveys.push_back({0, o.defaultAzimuth, o.defaultDip});
            issue(r, Sev::Warning, "survey", 0, id, "No surveys; default orientation " + fmt(o.defaultAzimuth) + "/" + fmt(o.defaultDip) + " used");
        }
        r.holes.push_back(std::move(h));
    }
    r.rejectedHoles += rejected.size();
    return r;
}

DrillholeImportSummary importDrillholes(Project& p, const DrillholeReadResult& read, bool replace) {
    DrillholeImportSummary s;
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Import " + std::to_string(read.holes.size()) + " drillholes");
    for (auto h : read.holes) {
        const Drillhole* existing = nullptr;
        for (const auto& e : p.model().drillholes.all())
            if (e.holeId == h.holeId) existing = &e;
        if (existing) {
            if (!replace) {
                s.skipped.push_back(h.holeId);
                continue;
            }
            h.id = existing->id;
            h.meta = existing->meta;
            ed.update(h);
            s.updated.push_back(h.id);
        } else s.created.push_back(ed.add(h));
    }
    macro.commit();
    return s;
}

std::vector<DrillholeIssue> checkDrillhole(const Drillhole& h, double maxDls) {
    std::vector<DrillholeIssue> out;
    auto add = [&](Sev s, std::string m) { out.push_back({s, "", 0, h.holeId, std::move(m)}); };
    if (h.surveys.empty()) add(Sev::Warning, "No downhole surveys");
    else if (h.surveys.front().depth > 0) add(Sev::Info, "First survey at " + fmt(h.surveys.front().depth) + " m; collar orientation assumed equal");
    auto iv = h.intervals;
    std::sort(iv.begin(), iv.end(), [](const auto& a, const auto& b) { return a.from < b.from; });
    double end = 0, sampled = 0;
    for (const auto& i : iv) {
        if (i.from > end + 1e-6) add(Sev::Info, "Gap " + fmt(end) + "–" + fmt(i.from) + " m");
        end = std::max(end, i.to);
        sampled += i.to - i.from;
    }
    if (h.length > end + 1e-6 && !iv.empty()) add(Sev::Info, "Not logged/sampled below " + fmt(end) + " m");
    if (h.length > 0) {
        std::ostringstream ss;
        ss << "Logged/sampled " << std::fixed << std::setprecision(1) << 100.0 * sampled / h.length << "% of the hole";
        add(Sev::Info, ss.str());
    }
    const DesurveyedHole d(h);
    const auto dls = doglegSeverity(d);
    for (std::size_t i = 0; i < dls.size(); ++i)
        if (dls[i] > maxDls)
            add(Sev::Warning, "Dogleg severity " + fmt(std::round(dls[i] * 100) / 100) + "°/30 m between " + fmt(d.stations()[i].depth) + " and " +
                                  fmt(d.stations()[i + 1].depth) + " m (check survey)");
    return out;
}

DrillholeCsv exportDrillholesCsv(const std::vector<Drillhole>& holes, DesurveyMethod method) {
    DrillholeCsv out;
    std::set<std::string> values, codes;
    for (const auto& h : holes)
        for (const auto& i : h.intervals) {
            for (const auto& [k, v] : i.values) values.insert(k);
            for (const auto& [k, v] : i.codes) codes.insert(k);
        }
    std::ostringstream c, s, iv;
    c << "HOLEID,X,Y,Z,LENGTH,TYPE,DATE\n";
    s << "HOLEID,DEPTH,AZIMUTH,DIP\n";
    iv << "HOLEID,FROM,TO,XS,YS,ZS,XM,YM,ZM,XE,YE,ZE";
    for (const auto& k : values) iv << ',' << util::csvField(k);
    for (const auto& k : codes) iv << ',' << util::csvField(k);
    iv << '\n';
    for (const auto& h : holes) {
        c << util::csvLine({h.holeId, fmt(h.collar.x), fmt(h.collar.y), fmt(h.collar.z), fmt(h.length), h.holeType,
                            h.drilledDate ? h.drilledDate->toString() : std::string{}})
          << '\n';
        for (const auto& sv : h.surveys) s << util::csvLine({h.holeId, fmt(sv.depth), fmt(sv.azimuth), fmt(sv.dip)}) << '\n';
        const DesurveyedHole d(h, method);
        for (const auto& i : h.intervals) {
            const Vec3 a = d.at(i.from), m = d.at(0.5 * (i.from + i.to)), b = d.at(i.to);
            std::vector<std::string> f{h.holeId, fmt(i.from), fmt(i.to), fmt(a.x), fmt(a.y), fmt(a.z), fmt(m.x), fmt(m.y), fmt(m.z), fmt(b.x), fmt(b.y), fmt(b.z)};
            for (const auto& k : values) {
                auto it = i.values.find(k);
                f.push_back(it == i.values.end() ? std::string{} : fmt(it->second));
            }
            for (const auto& k : codes) {
                auto it = i.codes.find(k);
                f.push_back(it == i.codes.end() ? std::string{} : it->second);
            }
            iv << util::csvLine(f) << '\n';
        }
    }
    out.collars = c.str();
    out.surveys = s.str();
    out.intervals = iv.str();
    return out;
}

std::vector<Uuid> createDrillholeTraces(Project& p, const Uuid& layerId, double step, DesurveyMethod method) {
    std::vector<Uuid> ids;
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), "Create drillhole traces");
    for (const auto& h : p.model().drillholes.all()) {
        GeometryObject g;
        g.type = GeometryType::Polyline;
        g.name = h.holeId;
        g.layerId = layerId;
        g.vertices = DesurveyedHole(h, method).trace(step);
        ids.push_back(ed.addGeometry(std::move(g)));
    }
    macro.commit();
    return ids;
}

} // namespace openmine::geology
