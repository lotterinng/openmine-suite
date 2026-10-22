// SPDX-License-Identifier: Apache-2.0
#include "openmine/formats/MeshFormats.h"

#include "openmine/project/ProjectEditor.h"

#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Sweep.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace openmine::formats {

namespace fs = std::filesystem;

namespace {
std::string lowerExt(const fs::path& f) {
    std::string e = f.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string slurp(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    if (!in) throw MeshFormatError("Cannot open " + f.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool parseD(std::string_view s, double& v) {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

std::vector<std::string_view> tokens(std::string_view l) {
    std::vector<std::string_view> t;
    std::size_t i = 0;
    while (i < l.size()) {
        while (i < l.size() && std::isspace(static_cast<unsigned char>(l[i]))) ++i;
        const std::size_t b = i;
        while (i < l.size() && !std::isspace(static_cast<unsigned char>(l[i]))) ++i;
        if (i > b) t.push_back(l.substr(b, i - b));
    }
    return t;
}

/// Triangulates one polygon (vertex indices) into `out`; returns false if degenerate.
bool triangulate(const std::vector<Vec3>& V, const std::vector<std::uint32_t>& poly, std::vector<std::array<std::uint32_t, 3>>& out) {
    if (poly.size() < 3) return false;
    if (poly.size() == 3) {
        out.push_back({poly[0], poly[1], poly[2]});
        return true;
    }
    // Newell normal and convexity test.
    Vec3 n;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Vec3 a = V[poly[i]], b = V[poly[(i + 1) % poly.size()]];
        n += Vec3{(a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)};
    }
    if (n.length() < 1e-300) return false;
    n = n.normalized();
    bool convex = true;
    for (std::size_t i = 0; i < poly.size() && convex; ++i) {
        const Vec3 a = V[poly[i]], b = V[poly[(i + 1) % poly.size()]], c = V[poly[(i + 2) % poly.size()]];
        convex = dot(cross(b - a, c - b), n) >= -1e-15;
    }
    if (convex) {
        for (std::size_t i = 1; i + 1 < poly.size(); ++i) out.push_back({poly[0], poly[i], poly[i + 1]});
        return true;
    }
    const Vec3 u = anyPerpendicular(n), w = cross(n, u);
    std::vector<Vec2> ring;
    for (auto i : poly) ring.push_back({dot(V[i], u), dot(V[i], w)});
    const auto tris = geometry::triangulatePolygon(ring);
    if (tris.empty()) return false;
    for (const auto& t : tris) {
        std::array<std::uint32_t, 3> tri{poly[t[0]], poly[t[1]], poly[t[2]]};
        // Keep the polygon's winding.
        if (dot(cross(V[tri[1]] - V[tri[0]], V[tri[2]] - V[tri[0]]), n) < 0) std::swap(tri[1], tri[2]);
        out.push_back(tri);
    }
    return true;
}

MeshFile readObj(const std::string& text) {
    MeshFile f;
    std::size_t pos = 0, lineNo = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        std::string_view line(text.data() + pos, (nl == std::string::npos ? text.size() : nl) - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        ++lineNo;
        if (const auto h = line.find('#'); h != std::string_view::npos) line = line.substr(0, h);
        const auto t = tokens(line);
        if (t.empty()) continue;
        if (t[0] == "v") {
            if (t.size() < 4) throw MeshFormatError("OBJ line " + std::to_string(lineNo) + ": a vertex needs x y z");
            Vec3 v;
            if (!parseD(t[1], v.x) || !parseD(t[2], v.y) || !parseD(t[3], v.z)) throw MeshFormatError("OBJ line " + std::to_string(lineNo) + ": invalid number");
            f.mesh.vertices.push_back(v);
        } else if (t[0] == "f") {
            std::vector<std::uint32_t> poly;
            for (std::size_t k = 1; k < t.size(); ++k) {
                const auto slash = t[k].find('/');
                const auto idx = t[k].substr(0, slash);
                long i = 0;
                const auto r = std::from_chars(idx.data(), idx.data() + idx.size(), i);
                if (r.ec != std::errc{} || i == 0) throw MeshFormatError("OBJ line " + std::to_string(lineNo) + ": invalid face index");
                const long n = static_cast<long>(f.mesh.vertices.size());
                const long z = i < 0 ? n + i : i - 1; // negative = relative
                if (z < 0 || z >= n) throw MeshFormatError("OBJ line " + std::to_string(lineNo) + ": face index out of range");
                poly.push_back(static_cast<std::uint32_t>(z));
            }
            if (poly.size() > 3) ++f.polygonsTriangulated;
            if (!triangulate(f.mesh.vertices, poly, f.mesh.triangles)) f.warnings.push_back("Degenerate face on line " + std::to_string(lineNo) + " skipped");
        } else if ((t[0] == "o" || t[0] == "g") && t.size() > 1 && f.name.empty()) f.name = std::string(t[1]);
    }
    return f;
}

MeshFile readStl(const std::string& data) {
    MeshFile f;
    // Binary if the size matches the triangle count (ASCII files may start with "solid" too).
    bool binary = false;
    if (data.size() >= 84) {
        std::uint32_t n;
        std::memcpy(&n, data.data() + 80, 4);
        binary = 84 + 50ull * n == data.size();
    }
    std::map<std::array<double, 3>, std::uint32_t> index;
    auto vertex = [&](Vec3 v) {
        auto [it, ins] = index.emplace(std::array<double, 3>{v.x, v.y, v.z}, static_cast<std::uint32_t>(f.mesh.vertices.size()));
        if (ins) f.mesh.vertices.push_back(v);
        return it->second;
    };
    if (binary) {
        std::uint32_t n;
        std::memcpy(&n, data.data() + 80, 4);
        f.name = std::string(data.data(), data.data() + 80);
        f.name.erase(std::find(f.name.begin(), f.name.end(), '\0'), f.name.end());
        while (!f.name.empty() && f.name.back() == ' ') f.name.pop_back();
        for (std::uint32_t i = 0; i < n; ++i) {
            const char* p = data.data() + 84 + 50ull * i + 12; // skip the normal
            std::array<std::uint32_t, 3> t{};
            for (int k = 0; k < 3; ++k) {
                float c[3];
                std::memcpy(c, p + 12 * k, 12);
                t[static_cast<std::size_t>(k)] = vertex({c[0], c[1], c[2]});
            }
            if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) f.mesh.triangles.push_back(t);
        }
        f.warnings.push_back("Binary STL stores single-precision coordinates (about 7 significant digits)");
        return f;
    }
    std::istringstream in(data);
    std::string word;
    std::vector<std::uint32_t> facet;
    std::size_t facets = 0;
    while (in >> word) {
        if (word == "solid" && f.name.empty()) std::getline(in, f.name), f.name.erase(0, f.name.find_first_not_of(' '));
        else if (word == "vertex") {
            std::string xs, ys, zs;
            in >> xs >> ys >> zs;
            Vec3 v;
            if (!parseD(xs, v.x) || !parseD(ys, v.y) || !parseD(zs, v.z)) throw MeshFormatError("Invalid STL vertex");
            facet.push_back(vertex(v));
        } else if (word == "endfacet") {
            ++facets;
            if (facet.size() != 3) throw MeshFormatError("STL facet with " + std::to_string(facet.size()) + " vertices");
            if (facet[0] != facet[1] && facet[1] != facet[2] && facet[0] != facet[2]) f.mesh.triangles.push_back({facet[0], facet[1], facet[2]});
            facet.clear();
        }
    }
    if (facets == 0) throw MeshFormatError("The STL file contains no facets");
    return f;
}

// ---- PLY -------------------------------------------------------------------------------------------
struct PlyProperty {
    std::string name, type, countType; // countType non-empty for lists
};
struct PlyElement {
    std::string name;
    std::size_t count = 0;
    std::vector<PlyProperty> props;
};

std::size_t plySize(const std::string& t) {
    if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
    if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
    if (t == "int" || t == "uint" || t == "float" || t == "int32" || t == "uint32" || t == "float32") return 4;
    if (t == "double" || t == "float64") return 8;
    throw MeshFormatError("Unknown PLY type " + t);
}

double plyRead(const char*& p, const std::string& t, bool swap) {
    const std::size_t n = plySize(t);
    unsigned char b[8];
    std::memcpy(b, p, n);
    if (swap) std::reverse(b, b + n);
    p += n;
    if (t == "char" || t == "int8") return static_cast<std::int8_t>(b[0]);
    if (t == "uchar" || t == "uint8") return b[0];
    if (t == "short" || t == "int16") { std::int16_t v; std::memcpy(&v, b, 2); return v; }
    if (t == "ushort" || t == "uint16") { std::uint16_t v; std::memcpy(&v, b, 2); return v; }
    if (t == "int" || t == "int32") { std::int32_t v; std::memcpy(&v, b, 4); return v; }
    if (t == "uint" || t == "uint32") { std::uint32_t v; std::memcpy(&v, b, 4); return v; }
    if (t == "float" || t == "float32") { float v; std::memcpy(&v, b, 4); return v; }
    double v;
    std::memcpy(&v, b, 8);
    return v;
}

MeshFile readPly(const std::string& data) {
    MeshFile f;
    if (data.rfind("ply", 0) != 0) throw MeshFormatError("Not a PLY file");
    const auto endHeader = data.find("end_header");
    if (endHeader == std::string::npos) throw MeshFormatError("PLY header is not terminated");
    std::size_t bodyStart = data.find('\n', endHeader);
    if (bodyStart == std::string::npos) throw MeshFormatError("PLY file has no body");
    ++bodyStart;
    std::istringstream hdr(data.substr(0, endHeader));
    std::string line, format;
    std::vector<PlyElement> elems;
    while (std::getline(hdr, line)) {
        auto t = tokens(line);
        if (t.empty()) continue;
        if (t[0] == "format" && t.size() > 1) format = std::string(t[1]);
        else if (t[0] == "element" && t.size() == 3) elems.push_back({std::string(t[1]), std::stoull(std::string(t[2])), {}});
        else if (t[0] == "property" && !elems.empty()) {
            if (t.size() == 5 && t[1] == "list") elems.back().props.push_back({std::string(t[4]), std::string(t[3]), std::string(t[2])});
            else if (t.size() == 3) elems.back().props.push_back({std::string(t[2]), std::string(t[1]), {}});
        }
    }
    const bool ascii = format == "ascii";
    const bool swap = (format == "binary_big_endian") == (std::endian::native == std::endian::little);
    if (!ascii && format != "binary_little_endian" && format != "binary_big_endian") throw MeshFormatError("Unknown PLY format " + format);
    const char* p = data.data() + bodyStart;
    const char* end = data.data() + data.size();
    std::istringstream body(ascii ? data.substr(bodyStart) : std::string());
    auto value = [&](const std::string& type) -> double {
        if (ascii) {
            std::string s;
            if (!(body >> s)) throw MeshFormatError("PLY body ends early");
            double v;
            if (!parseD(s, v)) throw MeshFormatError("Invalid PLY value " + s);
            return v;
        }
        if (p + plySize(type) > end) throw MeshFormatError("PLY body ends early");
        return plyRead(p, type, swap);
    };
    for (const auto& e : elems) {
        for (std::size_t i = 0; i < e.count; ++i) {
            Vec3 v;
            std::vector<std::uint32_t> poly;
            for (const auto& pr : e.props) {
                if (!pr.countType.empty()) {
                    const auto n = static_cast<std::size_t>(value(pr.countType));
                    for (std::size_t k = 0; k < n; ++k) {
                        const double idx = value(pr.type);
                        if (e.name == "face" && (pr.name == "vertex_indices" || pr.name == "vertex_index")) poly.push_back(static_cast<std::uint32_t>(idx));
                    }
                    continue;
                }
                const double d = value(pr.type);
                if (e.name == "vertex") {
                    if (pr.name == "x") v.x = d;
                    else if (pr.name == "y") v.y = d;
                    else if (pr.name == "z") v.z = d;
                }
            }
            if (e.name == "vertex") f.mesh.vertices.push_back(v);
            else if (e.name == "face") {
                for (auto ix : poly)
                    if (ix >= f.mesh.vertices.size()) throw MeshFormatError("PLY face index out of range");
                if (poly.size() > 3) ++f.polygonsTriangulated;
                triangulate(f.mesh.vertices, poly, f.mesh.triangles);
            }
        }
    }
    if (f.mesh.triangles.empty()) throw MeshFormatError("The PLY file has no faces (point clouds are imported as point clouds)");
    return f;
}

std::string full(double v) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

void commit(const fs::path& file, const std::string& bytes) {
    const auto tmp = file.parent_path() / (file.filename().string() + ".tmp");
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) throw MeshFormatError("Cannot write " + file.string());
        o << bytes;
        if (!o) throw MeshFormatError("Writing " + file.string() + " failed");
    }
    fs::rename(tmp, file);
}
} // namespace

MeshFile readMesh(const fs::path& file) {
    const auto e = lowerExt(file);
    const auto data = slurp(file);
    MeshFile f;
    if (e == ".obj") f = readObj(data);
    else if (e == ".stl") f = readStl(data);
    else if (e == ".ply") f = readPly(data);
    else throw MeshFormatError("Unsupported mesh format " + e);
    if (f.mesh.triangles.empty()) throw MeshFormatError("The file contains no triangles");
    if (f.name.empty()) f.name = file.stem().string();
    return f;
}

void writeObj(const fs::path& file, const MeshData& m, const std::string& name) {
    std::string s = "# OpenMine Suite\n";
    if (!name.empty()) s += "o " + name + "\n";
    for (const auto& v : m.vertices) s += "v " + full(v.x) + " " + full(v.y) + " " + full(v.z) + "\n";
    for (const auto& t : m.triangles) s += "f " + std::to_string(t[0] + 1) + " " + std::to_string(t[1] + 1) + " " + std::to_string(t[2] + 1) + "\n";
    commit(file, s);
}

std::vector<std::string> writeStl(const fs::path& file, const MeshData& m, StlEncoding enc, const std::string& name) {
    const auto normals = geometry::faceNormals(m);
    std::vector<std::string> warnings;
    if (enc == StlEncoding::Ascii) {
        std::string s = "solid " + name + "\n";
        for (std::size_t i = 0; i < m.triangles.size(); ++i) {
            const auto& n = normals[i];
            s += "  facet normal " + full(n.x) + " " + full(n.y) + " " + full(n.z) + "\n    outer loop\n";
            for (auto v : m.triangles[i]) s += "      vertex " + full(m.vertices[v].x) + " " + full(m.vertices[v].y) + " " + full(m.vertices[v].z) + "\n";
            s += "    endloop\n  endfacet\n";
        }
        s += "endsolid " + name + "\n";
        commit(file, s);
        return warnings;
    }
    double maxAbs = 0;
    for (const auto& v : m.vertices) maxAbs = std::max({maxAbs, std::abs(v.x), std::abs(v.y), std::abs(v.z)});
    const double ulp = maxAbs * 5.96e-8; // half a float32 ulp relative
    if (ulp > 1e-4) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "Binary STL stores float32: at coordinates up to %.0f m positions are rounded by up to %.3f m; use OBJ, PLY or ASCII STL", maxAbs, ulp);
        warnings.push_back(buf);
    }
    std::string s(80, ' ');
    const std::string hdr = "OpenMine " + name;
    std::memcpy(s.data(), hdr.data(), std::min<std::size_t>(80, hdr.size()));
    const auto n = static_cast<std::uint32_t>(m.triangles.size());
    s.append(reinterpret_cast<const char*>(&n), 4);
    for (std::size_t i = 0; i < m.triangles.size(); ++i) {
        float rec[12];
        rec[0] = static_cast<float>(normals[i].x);
        rec[1] = static_cast<float>(normals[i].y);
        rec[2] = static_cast<float>(normals[i].z);
        for (int k = 0; k < 3; ++k) {
            const Vec3 v = m.vertices[m.triangles[i][static_cast<std::size_t>(k)]];
            rec[3 + 3 * k] = static_cast<float>(v.x);
            rec[4 + 3 * k] = static_cast<float>(v.y);
            rec[5 + 3 * k] = static_cast<float>(v.z);
        }
        s.append(reinterpret_cast<const char*>(rec), 48);
        s.append(2, '\0');
    }
    commit(file, s);
    return warnings;
}

void writePly(const fs::path& file, const MeshData& m, bool binary) {
    std::string s = "ply\nformat " + std::string(binary ? "binary_little_endian" : "ascii") + " 1.0\ncomment OpenMine Suite\nelement vertex " +
                    std::to_string(m.vertices.size()) + "\nproperty double x\nproperty double y\nproperty double z\nelement face " +
                    std::to_string(m.triangles.size()) + "\nproperty list uchar uint vertex_indices\nend_header\n";
    if (!binary) {
        for (const auto& v : m.vertices) s += full(v.x) + " " + full(v.y) + " " + full(v.z) + "\n";
        for (const auto& t : m.triangles) s += "3 " + std::to_string(t[0]) + " " + std::to_string(t[1]) + " " + std::to_string(t[2]) + "\n";
    } else {
        static_assert(std::endian::native == std::endian::little, "binary PLY writer assumes a little-endian host");
        for (const auto& v : m.vertices) s.append(reinterpret_cast<const char*>(&v), 24);
        for (const auto& t : m.triangles) {
            s.push_back(3);
            s.append(reinterpret_cast<const char*>(t.data()), 12);
        }
    }
    commit(file, s);
}

std::vector<std::string> writeMesh(const fs::path& file, const MeshData& m, const std::string& name) {
    const auto e = lowerExt(file);
    if (e == ".obj") writeObj(file, m, name);
    else if (e == ".stl") return writeStl(file, m, StlEncoding::Binary, name);
    else if (e == ".ply") writePly(file, m, true);
    else throw MeshFormatError("Unsupported mesh format " + e);
    return {};
}

Uuid addMeshObject(Project& p, const MeshFile& mf, const std::string& name, const Uuid& layerId, const std::string& sourceFile) {
    if (mf.mesh.triangles.empty()) throw MeshFormatError("The mesh has no triangles");
    CommandStack::MacroScope macro(p.commands(), "Import " + sourceFile);
    ProjectEditor ed(p);
    ed.ensureAttributeDefinition("GeometryObject", "source_file", "Source file");
    GeometryObject g;
    g.type = GeometryType::Mesh;
    g.name = mf.name.empty() ? name : mf.name;
    g.layerId = layerId;
    g.mesh = std::make_shared<MeshData>(mf.mesh);
    g.attributes["source_file"] = sourceFile;
    const Uuid id = ed.addGeometry(std::move(g));
    macro.commit();
    return id;
}

} // namespace openmine::formats
