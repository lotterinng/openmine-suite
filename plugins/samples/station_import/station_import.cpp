// SPDX-License-Identifier: Apache-2.0
// Sample OpenMine native plugin (C ABI v1). It shows the complete plugin pattern:
//  * an importer for total-station coordinate files (.stn): "name,easting,northing,elevation[,code]"
//    with '#' comments; every line is either imported or reported with its reason, and the whole
//    import is one undo step (begin_edit / end_edit);
//  * an exporter of survey control points to GeoJSON (.geojson);
//  * commands "stations.summary" (count and extent) and "stations.traverse" (a polyline through
//    named stations, in the given order).
// The plugin uses only openmine_plugin.h; JSON handling uses nlohmann/json internally (header only),
// nothing C++ crosses the boundary and no exception escapes a callback.
#include "openmine_plugin.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <exception>

using nlohmann::json;

namespace {
const OpenMineHostV1* H = nullptr;

std::string take(OmString& s) {
    std::string r = s.data ? std::string(s.data, s.size) : std::string();
    H->free_string(&s);
    return r;
}

int fail(OmString* out, const std::string& msg) {
    if (out) {
        out->size = msg.size();
        out->data = static_cast<char*>(std::malloc(msg.size() + 1));
        std::memcpy(out->data, msg.c_str(), msg.size() + 1);
    }
    return OM_ERROR;
}
int ok(OmString* out, const std::string& msg) {
    fail(out, msg);
    return OM_OK;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) {
            out.push_back(trim(cur));
            cur.clear();
        } else cur += c;
    }
    out.push_back(trim(cur));
    return out;
}

bool number(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(v);
}

/// All control points: name → (key, document).
bool controlPoints(OmHostContext* ctx, std::map<std::string, std::pair<std::string, json>>& out, std::string& err) {
    OmString s{};
    if (H->list_documents(ctx, "SurveyControlPoint", &s) != OM_OK) {
        err = H->last_error(ctx);
        return false;
    }
    for (const auto& e : json::parse(take(s))) out[e.at("doc").at("columns").at("name").get<std::string>()] = {e.at("key"), e.at("doc")};
    return true;
}

int importStations(OmHostContext* ctx, void*, const char* path, const char* optionsJson, OmString* report) {
    try {
        const json opts = json::parse(optionsJson && *optionsJson ? optionsJson : "{}");
        const bool replace = opts.value("replace_existing", false);
        const double sigmaH = opts.value("horizontal_sigma", 0.005), sigmaV = opts.value("vertical_sigma", 0.005);
        std::ifstream in(path);
        if (!in) return fail(report, std::string("Cannot open ") + path);
        std::map<std::string, std::pair<std::string, json>> existing;
        std::string err;
        if (!controlPoints(ctx, existing, err)) return fail(report, err);
        std::vector<std::string> problems;
        int added = 0, updated = 0, lineNo = 0;
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        if (H->begin_edit(ctx, "Import survey stations") != OM_OK) return fail(report, H->last_error(ctx));
        for (const auto& raw : lines) {
            ++lineNo;
            if (H->report_progress(ctx, static_cast<double>(lineNo) / lines.size(), "Importing stations")) {
                H->end_edit(ctx, 0);
                return OM_CANCELLED;
            }
            const std::string line = trim(raw);
            if (line.empty() || line[0] == '#') continue;
            const auto f = split(line, ',');
            double e, n, z;
            if (f.size() < 4 || f[0].empty() || !number(f[1], e) || !number(f[2], n) || !number(f[3], z)) {
                problems.push_back("line " + std::to_string(lineNo) + ": expected name,easting,northing,elevation[,code] — skipped");
                continue;
            }
            json doc;
            auto it = existing.find(f[0]);
            if (it != existing.end()) {
                if (!replace) {
                    problems.push_back("line " + std::to_string(lineNo) + ": station '" + f[0] + "' already exists — skipped (replace_existing is false)");
                    continue;
                }
                doc = it->second.second;
            } else {
                OmString t{};
                if (H->template_document(ctx, "SurveyControlPoint", &t) != OM_OK) {
                    H->end_edit(ctx, 0);
                    return fail(report, H->last_error(ctx));
                }
                doc = json::parse(take(t));
            }
            auto& c = doc.at("columns");
            c["name"] = f[0];
            c["code"] = f.size() > 4 ? f[4] : "";
            c["position_x"] = e;
            c["position_y"] = n;
            c["position_z"] = z;
            c["horizontal_sigma"] = sigmaH;
            c["vertical_sigma"] = sigmaV;
            c["source"] = std::string("Station file ") + path;
            OmString key{};
            if (H->put_document(ctx, "SurveyControlPoint", doc.dump().c_str(), &key) != OM_OK) {
                problems.push_back("line " + std::to_string(lineNo) + ": " + H->last_error(ctx));
                continue;
            }
            const std::string k = take(key);
            existing[f[0]] = {k, doc};
            (it != existing.end() ? updated : added)++;
        }
        if (H->end_edit(ctx, 1) != OM_OK) return fail(report, H->last_error(ctx));
        json r{{"added", added}, {"updated", updated}, {"problems", problems}};
        H->log(ctx, OM_LOG_INFO, ("Imported " + std::to_string(added) + " stations, updated " + std::to_string(updated)).c_str());
        return ok(report, r.dump());
    } catch (const std::exception& e) {
        H->end_edit(ctx, 0);
        return fail(report, e.what());
    }
}

int exportGeoJson(OmHostContext* ctx, void*, const char* path, const char*, OmString* report) {
    try {
        std::map<std::string, std::pair<std::string, json>> cps;
        std::string err;
        if (!controlPoints(ctx, cps, err)) return fail(report, err);
        OmString info{};
        H->project_info(ctx, &info);
        const json pi = json::parse(take(info));
        json fc{{"type", "FeatureCollection"}, {"name", pi.value("name", "")}, {"features", json::array()}};
        for (const auto& [name, kd] : cps) {
            const auto& c = kd.second.at("columns");
            fc["features"].push_back({{"type", "Feature"},
                                      {"id", kd.first},
                                      {"geometry", {{"type", "Point"}, {"coordinates", {c.at("position_x"), c.at("position_y"), c.at("position_z")}}}},
                                      {"properties", {{"name", name}, {"code", c.at("code")}, {"role", c.at("role")}}}});
        }
        std::ofstream o(path);
        o << fc.dump(2);
        if (!o) return fail(report, std::string("Cannot write ") + path);
        return ok(report, json{{"exported", cps.size()}}.dump());
    } catch (const std::exception& e) {
        return fail(report, e.what());
    }
}

int summary(OmHostContext* ctx, void*, const char*, OmString* out) {
    try {
        std::map<std::string, std::pair<std::string, json>> cps;
        std::string err;
        if (!controlPoints(ctx, cps, err)) return fail(out, err);
        double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
        for (const auto& [n, kd] : cps) {
            const auto& c = kd.second.at("columns");
            const double v[3] = {c.at("position_x"), c.at("position_y"), c.at("position_z")};
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
        }
        json r{{"count", cps.size()}};
        if (!cps.empty()) r["extent"] = {{lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}};
        return ok(out, r.dump());
    } catch (const std::exception& e) {
        return fail(out, e.what());
    }
}

int traverse(OmHostContext* ctx, void*, const char* args, OmString* out) {
    try {
        const json a = json::parse(args && *args ? args : "{}");
        if (!a.contains("stations") || a.at("stations").size() < 2) return fail(out, "Give at least two station names in \"stations\"");
        std::map<std::string, std::pair<std::string, json>> cps;
        std::string err;
        if (!controlPoints(ctx, cps, err)) return fail(out, err);
        json verts = json::array();
        for (const auto& n : a.at("stations")) {
            const auto it = cps.find(n.get<std::string>());
            if (it == cps.end()) return fail(out, "Unknown station '" + n.get<std::string>() + "'");
            const auto& c = it->second.second.at("columns");
            verts.push_back({c.at("position_x"), c.at("position_y"), c.at("position_z")});
        }
        json g{{"type", "Polyline"}, {"name", a.value("name", "Traverse")}, {"layer", a.value("layer", "0")}, {"vertices", verts}, {"closed", a.value("closed", false)}};
        OmString key{};
        if (H->add_geometry(ctx, g.dump().c_str(), &key) != OM_OK) return fail(out, H->last_error(ctx));
        return ok(out, json{{"geometry", take(key)}}.dump());
    } catch (const std::exception& e) {
        return fail(out, e.what());
    }
}
} // namespace

extern "C" OPENMINE_PLUGIN_EXPORT int openmine_plugin_init_v1(const OpenMineHostV1* host, OmHostContext* ctx, OpenMinePluginInfoV1* info) {
    if (!host || host->api_version != OPENMINE_PLUGIN_API_VERSION || host->struct_size < sizeof(OpenMineHostV1)) return OM_INVALID;
    H = host;
    info->id = "org.openmine.samples.stations";
    info->version = "1.0.0";
    info->shutdown = [] { H = nullptr; };
    if (host->register_importer(ctx, ".stn", "Total-station coordinates (name,E,N,Z[,code])", importStations, nullptr) != OM_OK) return OM_ERROR;
    if (host->register_exporter(ctx, ".geojson", "Survey control points as GeoJSON", exportGeoJson, nullptr) != OM_OK) return OM_ERROR;
    if (host->register_command(ctx, "stations.summary", "Number and extent of survey control points", summary, nullptr) != OM_OK) return OM_ERROR;
    if (host->register_command(ctx, "stations.traverse", "Polyline through stations: {\"stations\":[…],\"layer\":\"0\",\"name\":\"…\"}", traverse, nullptr) !=
        OM_OK)
        return OM_ERROR;
    return OM_OK;
}
