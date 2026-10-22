// SPDX-License-Identifier: Apache-2.0
#include "openmine/omf/Omf.h"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <type_traits>

namespace openmine::omf {

using nlohmann::json;

namespace {
constexpr unsigned char kMagic[4] = {0x84, 0x83, 0x82, 0x81};
constexpr char kVersion[] = "OMF-v0.9.0";
constexpr std::size_t kHeader = 60;

/// Content that is valid OMF but not supported here (reported as a note, unlike corrupt data).
struct Unsupported : OmfError {
    using OmfError::OmfError;
};

// ---- little-endian helpers --------------------------------------------------------------------
template <class T> void putLE(std::string& out, T v) {
    unsigned char b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) std::reverse(b, b + sizeof(T));
    out.append(reinterpret_cast<const char*>(b), sizeof(T));
}
template <class T> T getLE(const char* p) {
    unsigned char b[sizeof(T)];
    std::memcpy(b, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) std::reverse(b, b + sizeof(T));
    T v;
    std::memcpy(&v, b, sizeof(T));
    return v;
}

std::string deflate(const std::string& raw) {
    uLongf n = compressBound(static_cast<uLong>(raw.size()));
    std::string out(n, '\0');
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()), 6) != Z_OK)
        throw OmfError("zlib compression failed");
    out.resize(n);
    return out;
}

std::string inflate(const char* data, std::size_t len) {
    z_stream s{};
    if (inflateInit(&s) != Z_OK) throw OmfError("zlib initialisation failed");
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    s.avail_in = static_cast<uInt>(len);
    std::string out;
    char buf[1 << 16];
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        s.next_out = reinterpret_cast<Bytef*>(buf);
        s.avail_out = sizeof(buf);
        rc = ::inflate(&s, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&s);
            throw OmfError("Corrupt compressed array in OMF file");
        }
        out.append(buf, sizeof(buf) - s.avail_out);
        if (rc == Z_OK && s.avail_in == 0 && s.avail_out != 0) {
            inflateEnd(&s);
            throw OmfError("Truncated compressed array in OMF file");
        }
    }
    inflateEnd(&s);
    return out;
}

std::string nowIso() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

json vec(Vec3 v) { return json::array({v.x, v.y, v.z}); }
Vec3 toVec(const json& j) {
    if (!j.is_array() || j.size() != 3) throw OmfError("Expected a 3-vector in OMF JSON");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

// ---- writer ---------------------------------------------------------------------------------------
class Writer {
public:
    explicit Writer(std::string date) : date_(std::move(date)) { bin_.reserve(1 << 20); }

    std::string record(json body, const std::string& cls) {
        const std::string uid = Uuid::generate().toString();
        body["date_created"] = date_;
        body["date_modified"] = date_;
        body["__class__"] = cls;
        reg_[uid] = std::move(body);
        return uid;
    }
    std::string binary(const std::string& raw, const std::string& dtype, const std::string& cls) {
        const std::string z = deflate(raw);
        json idx{{"start", static_cast<std::uint64_t>(kHeader + bin_.size())}, {"dtype", dtype}, {"length", static_cast<std::uint64_t>(z.size())}};
        bin_ += z;
        return record({{"array", idx}}, cls);
    }
    std::string floats(const std::vector<double>& v, const std::string& cls = "ScalarArray") {
        std::string raw;
        raw.reserve(v.size() * 8);
        for (double x : v) putLE(raw, x);
        return binary(raw, "<f8", cls);
    }
    std::string vectors(const std::vector<Vec3>& v, bool two = false) {
        std::string raw;
        for (const auto& p : v) {
            putLE(raw, p.x);
            putLE(raw, p.y);
            if (!two) putLE(raw, p.z);
        }
        return binary(raw, "<f8", two ? "Vector2Array" : "Vector3Array");
    }
    template <class T> std::string ints(const std::vector<T>& v, const std::string& cls) {
        std::string raw;
        for (const auto& x : v) {
            if constexpr (std::is_arithmetic_v<T>) putLE<std::int64_t>(raw, static_cast<std::int64_t>(x));
            else
                for (auto c : x) putLE<std::int64_t>(raw, static_cast<std::int64_t>(c));
        }
        return binary(raw, "<i8", cls);
    }

    json& at(const std::string& uid) { return reg_.at(uid); }
    std::string finish(const std::string& projectUid) const {
        std::string out;
        out.append(reinterpret_cast<const char*>(kMagic), 4);
        char ver[32] = {};
        std::memcpy(ver, kVersion, sizeof(kVersion) - 1);
        out.append(ver, 32);
        const auto u = Uuid::parse(projectUid);
        if (!u) throw OmfError("Invalid project UID");
        out.append(reinterpret_cast<const char*>(u->bytes().data()), 16);
        putLE<std::uint64_t>(out, static_cast<std::uint64_t>(kHeader + bin_.size()));
        out += bin_;
        json all = json::object();
        for (const auto& [k, v] : reg_) all[k] = v;
        out += all.dump();
        return out;
    }
    std::map<std::string, json> reg_;

private:
    std::string date_;
    std::string bin_;
};

std::string writeData(Writer& w, const Data& d) {
    json b{{"name", d.name}, {"description", d.description}, {"location", d.location}};
    switch (d.kind) {
    case Data::Kind::Scalar:
        b["array"] = w.floats(d.scalars);
        return w.record(b, "ScalarData");
    case Data::Kind::Mapped: {
        b["array"] = w.ints(d.indices, "ScalarArray");
        json legends = json::array();
        for (const auto& l : d.legends) {
            json lb{{"name", l.name}, {"description", l.description}};
            switch (l.kind) {
            case Legend::Kind::String: lb["values"] = w.record({{"array", l.strings}}, "StringArray"); break;
            case Legend::Kind::DateTime: lb["values"] = w.record({{"array", l.strings}}, "DateTimeArray"); break;
            case Legend::Kind::Color: {
                json c = json::array();
                for (const auto& x : l.colors) c.push_back({x[0], x[1], x[2]});
                lb["values"] = w.record({{"array", c}}, "ColorArray");
                break;
            }
            case Legend::Kind::Scalar: lb["values"] = w.floats(l.scalars); break;
            }
            legends.push_back(w.record(lb, "Legend"));
        }
        b["legends"] = legends;
        return w.record(b, "MappedData");
    }
    case Data::Kind::String:
        b["array"] = w.record({{"array", d.strings}}, "StringArray");
        return w.record(b, "StringData");
    case Data::Kind::DateTime:
        b["array"] = w.record({{"array", d.strings}}, "DateTimeArray");
        return w.record(b, "DateTimeData");
    case Data::Kind::Color:
        b["array"] = w.ints(d.colors, "Int3Array");
        return w.record(b, "ColorData");
    case Data::Kind::Vector3:
        b["array"] = w.vectors(d.vectors);
        return w.record(b, "Vector3Data");
    case Data::Kind::Vector2:
        b["array"] = w.vectors(d.vectors, true);
        return w.record(b, "Vector2Data");
    }
    throw OmfError("Unknown data kind");
}

// ---- reader ---------------------------------------------------------------------------------------
class Reader {
public:
    Reader(const std::string& bytes, const json& reg) : bytes_(bytes), reg_(reg) {}

    const json& obj(const json& uidRef) const {
        if (!uidRef.is_string()) throw OmfError("Expected a UID reference in OMF JSON");
        const auto it = reg_.find(uidRef.get<std::string>());
        if (it == reg_.end()) throw OmfError("Missing OMF object " + uidRef.get<std::string>());
        return *it;
    }
    std::string raw(const json& arrayRecord, const char* dtype) const {
        const auto& idx = arrayRecord.at("array");
        if (!idx.is_object()) throw OmfError("Expected a binary array index");
        const auto start = idx.at("start").get<std::uint64_t>(), len = idx.at("length").get<std::uint64_t>();
        if (idx.at("dtype").get<std::string>() != dtype)
            throw OmfError("Unexpected array type " + idx.at("dtype").get<std::string>() + " (expected " + dtype + ")");
        if (start + len > bytes_.size() || start < kHeader) throw OmfError("Array outside the OMF file");
        return inflate(bytes_.data() + start, static_cast<std::size_t>(len));
    }
    std::vector<double> doubles(const json& ref) const {
        const auto& r = obj(ref);
        const auto& idx = r.at("array");
        if (idx.at("dtype").get<std::string>() == "<i8") {
            auto v = int64s(ref);
            return {v.begin(), v.end()};
        }
        const auto s = raw(r, "<f8");
        std::vector<double> v(s.size() / 8);
        for (std::size_t i = 0; i < v.size(); ++i) v[i] = getLE<double>(s.data() + 8 * i);
        return v;
    }
    std::vector<std::int64_t> int64s(const json& ref) const {
        const auto s = raw(obj(ref), "<i8");
        std::vector<std::int64_t> v(s.size() / 8);
        for (std::size_t i = 0; i < v.size(); ++i) v[i] = getLE<std::int64_t>(s.data() + 8 * i);
        return v;
    }
    std::vector<Vec3> vec3s(const json& ref, int dim = 3) const {
        const auto d = doubles(ref);
        if (d.size() % static_cast<std::size_t>(dim)) throw OmfError("Vector array length is not a multiple of " + std::to_string(dim));
        std::vector<Vec3> v(d.size() / static_cast<std::size_t>(dim));
        for (std::size_t i = 0; i < v.size(); ++i) v[i] = {d[dim * i], d[dim * i + 1], dim == 3 ? d[dim * i + 2] : 0.0};
        return v;
    }
    template <std::size_t N> std::vector<std::array<std::int64_t, N>> tuples(const json& ref) const {
        const auto d = int64s(ref);
        if (d.size() % N) throw OmfError("Index array length is not a multiple of " + std::to_string(N));
        std::vector<std::array<std::int64_t, N>> v(d.size() / N);
        for (std::size_t i = 0; i < v.size(); ++i)
            for (std::size_t k = 0; k < N; ++k) v[i][k] = d[N * i + k];
        return v;
    }
    std::vector<std::string> strings(const json& ref) const {
        const auto& a = obj(ref).at("array");
        std::vector<std::string> s;
        for (const auto& x : a) s.push_back(x.is_null() ? std::string{} : x.get<std::string>());
        return s;
    }

    Data data(const json& ref) const {
        const auto& r = obj(ref);
        const std::string cls = r.at("__class__");
        Data d;
        d.name = r.value("name", "");
        d.description = r.value("description", "");
        d.location = r.value("location", "");
        if (cls == "ScalarData") {
            d.kind = Data::Kind::Scalar;
            d.scalars = doubles(r.at("array"));
        } else if (cls == "MappedData") {
            d.kind = Data::Kind::Mapped;
            d.indices = int64s(r.at("array"));
            for (const auto& lref : r.at("legends")) {
                const auto& l = obj(lref);
                Legend lg;
                lg.name = l.value("name", "");
                lg.description = l.value("description", "");
                const auto& v = obj(l.at("values"));
                const std::string vc = v.at("__class__");
                if (vc == "StringArray") {
                    lg.kind = Legend::Kind::String;
                    lg.strings = strings(l.at("values"));
                } else if (vc == "DateTimeArray") {
                    lg.kind = Legend::Kind::DateTime;
                    lg.strings = strings(l.at("values"));
                } else if (vc == "ColorArray") {
                    lg.kind = Legend::Kind::Color;
                    for (const auto& c : v.at("array")) lg.colors.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>()});
                } else if (vc == "ScalarArray") {
                    lg.kind = Legend::Kind::Scalar;
                    lg.scalars = doubles(l.at("values"));
                } else throw Unsupported("Unsupported legend array " + vc);
                d.legends.push_back(std::move(lg));
            }
        } else if (cls == "StringData") {
            d.kind = Data::Kind::String;
            d.strings = strings(r.at("array"));
        } else if (cls == "DateTimeData") {
            d.kind = Data::Kind::DateTime;
            d.strings = strings(r.at("array"));
        } else if (cls == "ColorData") {
            d.kind = Data::Kind::Color;
            const auto& a = obj(r.at("array"));
            if (a.at("__class__") == "ColorArray")
                for (const auto& c : a.at("array")) d.colors.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>()});
            else
                for (const auto& t : tuples<3>(r.at("array"))) d.colors.push_back({static_cast<int>(t[0]), static_cast<int>(t[1]), static_cast<int>(t[2])});
        } else if (cls == "Vector3Data") {
            d.kind = Data::Kind::Vector3;
            d.vectors = vec3s(r.at("array"));
        } else if (cls == "Vector2Data") {
            d.kind = Data::Kind::Vector2;
            d.vectors = vec3s(r.at("array"), 2);
        } else throw Unsupported("Unsupported data class " + cls);
        return d;
    }

private:
    const std::string& bytes_;
    const json& reg_;
};

std::array<int, 3> colorOf(const json& j) {
    if (j.is_array() && j.size() == 3) return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
    return {128, 128, 128};
}
} // namespace

std::size_t Data::size() const {
    switch (kind) {
    case Kind::Scalar: return scalars.size();
    case Kind::Mapped: return indices.size();
    case Kind::String:
    case Kind::DateTime: return strings.size();
    case Kind::Color: return colors.size();
    case Kind::Vector2:
    case Kind::Vector3: return vectors.size();
    }
    return 0;
}

std::size_t locationCount(const Element& e, const std::string& loc) {
    switch (e.kind) {
    case Element::Kind::PointSet: return loc == "vertices" ? e.vertices.size() : 0;
    case Element::Kind::LineSet: return loc == "vertices" ? e.vertices.size() : (loc == "segments" ? e.segments.size() : 0);
    case Element::Kind::Surface: return loc == "vertices" ? e.vertices.size() : (loc == "faces" ? e.triangles.size() : 0);
    case Element::Kind::SurfaceGrid:
        return loc == "vertices" ? (e.tensorU.size() + 1) * (e.tensorV.size() + 1) : (loc == "faces" ? e.tensorU.size() * e.tensorV.size() : 0);
    case Element::Kind::Volume:
        return loc == "vertices" ? (e.tensorU.size() + 1) * (e.tensorV.size() + 1) * (e.tensorW.size() + 1)
                                 : (loc == "cells" ? e.tensorU.size() * e.tensorV.size() * e.tensorW.size() : 0);
    }
    return 0;
}

void validate(const Document& doc) {
    for (const auto& e : doc.elements) {
        const std::string where = "Element '" + e.name + "': ";
        const auto nv = static_cast<std::int64_t>(e.vertices.size());
        for (const auto& s : e.segments)
            if (s[0] < 0 || s[1] < 0 || s[0] >= nv || s[1] >= nv) throw OmfError(where + "segment index out of range");
        for (const auto& t : e.triangles)
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv) throw OmfError(where + "triangle index out of range");
        if (e.kind == Element::Kind::SurfaceGrid && !e.offsetW.empty() && e.offsetW.size() != locationCount(e, "vertices"))
            throw OmfError(where + "offset_w length does not match the grid vertices");
        for (const auto* t : {&e.tensorU, &e.tensorV, &e.tensorW})
            for (double x : *t)
                if (!(x > 0)) throw OmfError(where + "tensor spacings must be positive");
        for (const auto& d : e.data) {
            const auto n = locationCount(e, d.location);
            if (n == 0 && d.size() != 0) throw OmfError(where + "data '" + d.name + "' has an invalid location '" + d.location + "'");
            if (d.size() != n) throw OmfError(where + "data '" + d.name + "' has " + std::to_string(d.size()) + " values for " + std::to_string(n) + " " + d.location);
            if (d.kind == Data::Kind::Mapped)
                for (const auto& l : d.legends) {
                    const std::size_t ln = l.kind == Legend::Kind::Color ? l.colors.size() : (l.kind == Legend::Kind::Scalar ? l.scalars.size() : l.strings.size());
                    for (auto i : d.indices)
                        if (i < -1 || i >= static_cast<std::int64_t>(ln)) throw OmfError(where + "mapped index out of the legend range in '" + d.name + "'");
                }
        }
    }
}

std::string serializeOmf(const Document& doc) {
    validate(doc);
    Writer w(nowIso());
    json elements = json::array();
    for (const auto& e : doc.elements) {
        json data = json::array();
        for (const auto& d : e.data) data.push_back(writeData(w, d));
        json g{{"origin", vec(e.origin)}};
        json body{{"name", e.name}, {"description", e.description}, {"data", data}, {"color", {e.color[0], e.color[1], e.color[2]}}};
        std::string cls;
        switch (e.kind) {
        case Element::Kind::PointSet:
            g["vertices"] = w.vectors(e.vertices);
            body["geometry"] = w.record(g, "PointSetGeometry");
            body["textures"] = json::array();
            body["subtype"] = e.subtype.empty() ? "point" : e.subtype;
            cls = "PointSetElement";
            break;
        case Element::Kind::LineSet:
            g["vertices"] = w.vectors(e.vertices);
            g["segments"] = w.ints(e.segments, "Int2Array");
            body["geometry"] = w.record(g, "LineSetGeometry");
            body["subtype"] = e.subtype.empty() ? "line" : e.subtype;
            cls = "LineSetElement";
            break;
        case Element::Kind::Surface:
            g["vertices"] = w.vectors(e.vertices);
            g["triangles"] = w.ints(e.triangles, "Int3Array");
            body["geometry"] = w.record(g, "SurfaceGeometry");
            body["textures"] = json::array();
            body["subtype"] = "surface";
            cls = "SurfaceElement";
            break;
        case Element::Kind::SurfaceGrid:
            g["axis_u"] = vec(e.axisU);
            g["axis_v"] = vec(e.axisV);
            g["tensor_u"] = e.tensorU;
            g["tensor_v"] = e.tensorV;
            if (!e.offsetW.empty()) g["offset_w"] = w.floats(e.offsetW);
            body["geometry"] = w.record(g, "SurfaceGridGeometry");
            body["textures"] = json::array();
            body["subtype"] = "surface";
            cls = "SurfaceElement";
            break;
        case Element::Kind::Volume:
            g["axis_u"] = vec(e.axisU);
            g["axis_v"] = vec(e.axisV);
            g["axis_w"] = vec(e.axisW);
            g["tensor_u"] = e.tensorU;
            g["tensor_v"] = e.tensorV;
            g["tensor_w"] = e.tensorW;
            body["geometry"] = w.record(g, "VolumeGridGeometry");
            body["subtype"] = "volume";
            cls = "VolumeElement";
            break;
        }
        elements.push_back(w.record(body, cls));
    }
    const std::string puid = doc.uid.empty() ? Uuid::generate().toString() : doc.uid;
    json p{{"name", doc.name},       {"description", doc.description}, {"author", doc.author},     {"revision", doc.revision},
           {"units", doc.units},     {"elements", elements},           {"origin", vec(doc.origin)}, {"date_created", nowIso()},
           {"date_modified", nowIso()}, {"__class__", "Project"}};
    w.reg_[puid] = p;
    return w.finish(puid);
}

void writeOmf(const Document& doc, const std::filesystem::path& file) {
    const std::string bytes = serializeOmf(doc);
    std::ofstream out(file, std::ios::binary);
    if (!out) throw OmfError("Cannot write " + file.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw OmfError("Write failed: " + file.string());
}

Document parseOmf(const std::string& bytes) {
    if (bytes.size() < kHeader || std::memcmp(bytes.data(), kMagic, 4) != 0) throw OmfError("Not an OMF file (bad magic number)");
    const std::string ver(bytes.data() + 4, strnlen(bytes.data() + 4, 32));
    if (ver != kVersion)
        throw OmfError("Unsupported OMF version '" + ver + "' (this reader supports " + kVersion +
                       ", written by omf 1.0.x; OMF 2 files use a different container)");
    Uuid u;
    {
        char hex[37];
        const auto* b = reinterpret_cast<const unsigned char*>(bytes.data() + 36);
        std::snprintf(hex, sizeof hex, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        u = *Uuid::parse(hex);
    }
    const auto jsonStart = getLE<std::uint64_t>(bytes.data() + 52);
    if (jsonStart < kHeader || jsonStart > bytes.size()) throw OmfError("Invalid JSON offset in OMF header");
    json reg;
    try {
        reg = json::parse(bytes.begin() + static_cast<std::ptrdiff_t>(jsonStart), bytes.end());
    } catch (const json::exception& e) {
        throw OmfError(std::string("Invalid OMF JSON: ") + e.what());
    }
    const Reader r(bytes, reg);
    Document doc;
    doc.uid = u.toString();
    const auto it = reg.find(doc.uid);
    if (it == reg.end()) throw OmfError("Project record missing from the OMF registry");
    const auto& p = *it;
    doc.name = p.value("name", "");
    doc.description = p.value("description", "");
    doc.author = p.value("author", "");
    doc.revision = p.value("revision", "");
    doc.units = p.value("units", "");
    if (p.contains("origin")) doc.origin = toVec(p.at("origin"));
    try {
        for (const auto& eref : p.at("elements")) {
            const auto& e = r.obj(eref);
            const std::string cls = e.at("__class__");
            Element el;
            el.name = e.value("name", "");
            el.description = e.value("description", "");
            el.subtype = e.value("subtype", "");
            if (e.contains("color")) el.color = colorOf(e.at("color"));
            const auto& g = r.obj(e.at("geometry"));
            const std::string gc = g.at("__class__");
            if (g.contains("origin")) el.origin = toVec(g.at("origin"));
            if (cls == "PointSetElement") {
                el.kind = Element::Kind::PointSet;
                el.vertices = r.vec3s(g.at("vertices"));
            } else if (cls == "LineSetElement") {
                el.kind = Element::Kind::LineSet;
                el.vertices = r.vec3s(g.at("vertices"));
                el.segments = r.tuples<2>(g.at("segments"));
            } else if (cls == "SurfaceElement" && gc == "SurfaceGeometry") {
                el.kind = Element::Kind::Surface;
                el.vertices = r.vec3s(g.at("vertices"));
                el.triangles = r.tuples<3>(g.at("triangles"));
            } else if (cls == "SurfaceElement" && gc == "SurfaceGridGeometry") {
                el.kind = Element::Kind::SurfaceGrid;
                el.axisU = toVec(g.at("axis_u"));
                el.axisV = toVec(g.at("axis_v"));
                el.tensorU = g.at("tensor_u").get<std::vector<double>>();
                el.tensorV = g.at("tensor_v").get<std::vector<double>>();
                if (g.contains("offset_w") && !g.at("offset_w").is_null()) el.offsetW = r.doubles(g.at("offset_w"));
            } else if (cls == "VolumeElement") {
                el.kind = Element::Kind::Volume;
                el.axisU = toVec(g.at("axis_u"));
                el.axisV = toVec(g.at("axis_v"));
                el.axisW = toVec(g.at("axis_w"));
                el.tensorU = g.at("tensor_u").get<std::vector<double>>();
                el.tensorV = g.at("tensor_v").get<std::vector<double>>();
                el.tensorW = g.at("tensor_w").get<std::vector<double>>();
            } else throw OmfError("Unsupported element class " + cls + " / " + gc);
            if (e.contains("data"))
                for (const auto& dref : e.at("data")) {
                    try {
                        el.data.push_back(r.data(dref));
                    } catch (const Unsupported& ex) {
                        el.notes.push_back(std::string("Data skipped: ") + ex.what());
                    }
                }
            if (e.contains("textures") && !e.at("textures").empty())
                el.notes.push_back(std::to_string(e.at("textures").size()) + " image texture(s) not imported");
            doc.elements.push_back(std::move(el));
        }
    } catch (const json::exception& ex) {
        throw OmfError(std::string("Malformed OMF content: ") + ex.what());
    }
    validate(doc);
    return doc;
}

Document readOmf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw OmfError("Cannot open " + file.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseOmf(ss.str());
}

} // namespace openmine::omf
