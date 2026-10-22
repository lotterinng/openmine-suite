// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/Octree.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <iterator>
#include <system_error>

namespace openmine::survey {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr char kMagic[8] = {'O', 'M', 'P', 'N', 'O', 'D', 'E', '1'};

// ---- Record (row) encoding for spill files ------------------------------------------------------
std::size_t recordStride(const PointSchema& s) {
    std::size_t n = 24;
    if (s.intensity) n += 2;
    if (s.rgb) n += 6;
    if (s.classification) n += 1;
    if (s.normals) n += 12;
    if (s.returnInfo) n += 2;
    if (s.gpsTime) n += 8;
    n += 4 * s.scalarFields.size();
    return n;
}

template <class T> void put(std::string& out, const T& v) { out.append(reinterpret_cast<const char*>(&v), sizeof(T)); }
template <class T> T get(const char*& p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

void encodeRecord(const PointBuffer& b, std::size_t i, std::string& out) {
    const auto& s = b.schema();
    put(out, b.xyz[i].x);
    put(out, b.xyz[i].y);
    put(out, b.xyz[i].z);
    if (s.intensity) put(out, b.intensity[i]);
    if (s.rgb) put(out, b.rgb[i]);
    if (s.classification) put(out, b.classification[i]);
    if (s.normals) put(out, b.normal[i]);
    if (s.returnInfo) {
        put(out, b.returnNumber[i]);
        put(out, b.numberOfReturns[i]);
    }
    if (s.gpsTime) put(out, b.gpsTime[i]);
    for (const auto& c : b.scalars) put(out, c[i]);
}

void decodeRecords(const std::string& data, PointBuffer& out) {
    const auto& s = out.schema();
    const std::size_t stride = recordStride(s);
    const std::size_t n = data.size() / stride;
    const char* p = data.data();
    out.reserve(out.size() + n);
    for (std::size_t i = 0; i < n; ++i) {
        Vec3 v;
        v.x = get<double>(p);
        v.y = get<double>(p);
        v.z = get<double>(p);
        out.xyz.push_back(v);
        if (s.intensity) out.intensity.push_back(get<std::uint16_t>(p));
        if (s.rgb) out.rgb.push_back(get<Rgb16>(p));
        if (s.classification) out.classification.push_back(get<std::uint8_t>(p));
        if (s.normals) out.normal.push_back(get<std::array<float, 3>>(p));
        if (s.returnInfo) {
            out.returnNumber.push_back(get<std::uint8_t>(p));
            out.numberOfReturns.push_back(get<std::uint8_t>(p));
        }
        if (s.gpsTime) out.gpsTime.push_back(get<double>(p));
        for (auto& c : out.scalars) c.push_back(get<float>(p));
    }
}

// ---- Node files (columnar) ----------------------------------------------------------------------------
template <class T> void putColumn(std::ofstream& o, const std::vector<T>& c) {
    o.write(reinterpret_cast<const char*>(c.data()), static_cast<std::streamsize>(c.size() * sizeof(T)));
}
template <class T> void getColumn(std::ifstream& in, std::vector<T>& c, std::size_t n) {
    c.resize(n);
    in.read(reinterpret_cast<char*>(c.data()), static_cast<std::streamsize>(n * sizeof(T)));
}

void writeNodeFile(const fs::path& file, const PointBuffer& b) {
    std::ofstream o(file, std::ios::binary | std::ios::trunc);
    if (!o) throw PointCloudError("Cannot write " + file.string());
    o.write(kMagic, 8);
    const std::uint64_t n = b.size();
    o.write(reinterpret_cast<const char*>(&n), 8);
    putColumn(o, b.xyz);
    const auto& s = b.schema();
    if (s.intensity) putColumn(o, b.intensity);
    if (s.rgb) putColumn(o, b.rgb);
    if (s.classification) putColumn(o, b.classification);
    if (s.normals) putColumn(o, b.normal);
    if (s.returnInfo) {
        putColumn(o, b.returnNumber);
        putColumn(o, b.numberOfReturns);
    }
    if (s.gpsTime) putColumn(o, b.gpsTime);
    for (const auto& c : b.scalars) putColumn(o, c);
    o.close();
    if (!o) throw PointCloudError("Writing " + file.string() + " failed (disk full?)");
}

PointBuffer readNodeFile(const fs::path& file, const PointSchema& schema) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw PointCloudError("Point-cloud node " + file.filename().string() + " is missing");
    char magic[8];
    in.read(magic, 8);
    if (std::memcmp(magic, kMagic, 8) != 0) throw PointCloudError("Point-cloud node " + file.filename().string() + " is corrupt");
    std::uint64_t n = 0;
    in.read(reinterpret_cast<char*>(&n), 8);
    PointBuffer b(schema);
    getColumn(in, b.xyz, n);
    if (schema.intensity) getColumn(in, b.intensity, n);
    if (schema.rgb) getColumn(in, b.rgb, n);
    if (schema.classification) getColumn(in, b.classification, n);
    if (schema.normals) getColumn(in, b.normal, n);
    if (schema.returnInfo) {
        getColumn(in, b.returnNumber, n);
        getColumn(in, b.numberOfReturns, n);
    }
    if (schema.gpsTime) getColumn(in, b.gpsTime, n);
    for (auto& c : b.scalars) getColumn(in, c, n);
    if (!in) throw PointCloudError("Point-cloud node " + file.filename().string() + " is truncated");
    return b;
}

json schemaJson(const PointSchema& s) {
    return {{"intensity", s.intensity}, {"rgb", s.rgb}, {"classification", s.classification}, {"normals", s.normals},
            {"returnInfo", s.returnInfo}, {"gpsTime", s.gpsTime}, {"scalarFields", s.scalarFields}};
}
PointSchema schemaFrom(const json& j) {
    PointSchema s;
    s.intensity = j.at("intensity");
    s.rgb = j.at("rgb");
    s.classification = j.at("classification");
    s.normals = j.at("normals");
    s.returnInfo = j.at("returnInfo");
    s.gpsTime = j.at("gpsTime");
    s.scalarFields = j.at("scalarFields").get<std::vector<std::string>>();
    return s;
}
json boxJson(const BoundingBox& b) { return json::array({b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z}); }
BoundingBox boxFrom(const json& j) { return {{j[0], j[1], j[2]}, {j[3], j[4], j[5]}}; }

BoundingBox childCube(const BoundingBox& c, int digit) {
    const Vec3 mid = c.center();
    BoundingBox r;
    r.min = {(digit & 4) ? mid.x : c.min.x, (digit & 2) ? mid.y : c.min.y, (digit & 1) ? mid.z : c.min.z};
    r.max = {(digit & 4) ? c.max.x : mid.x, (digit & 2) ? c.max.y : mid.y, (digit & 1) ? c.max.z : mid.z};
    return r;
}

class Builder {
public:
    Builder(PointSource& src, const fs::path& folder, const OctreeBuildOptions& o, jobs::Progress& pr)
        : src_(src), folder_(folder), opt_(o), progress_(pr) {}

    OctreeInfo run() {
        if (fs::exists(folder_) && !fs::is_empty(folder_)) throw PointCloudError("Octree folder is not empty: " + folder_.string());
        fs::create_directories(folder_ / "nodes");
        if (opt_.grid < 2 || opt_.grid > 1024) throw PointCloudError("Octree sampling grid must be 2–1024");
        info_.schema = src_.schema();
        computeBounds();
        if (info_.pointCount == 0) throw PointCloudError("The source contains no points");
        if (static_cast<std::size_t>(info_.pointCount) <= opt_.maxChunkPoints) {
            progress_.update(0.0, "Building octree");
            src_.rewind();
            PointBuffer all = survey::readAll(src_);
            buildSubtree(std::move(all), "r", 0, info_.cube);
        } else {
            buildChunked();
        }
        writeMetadata();
        return info_;
    }

private:
    PointSource& src_;
    fs::path folder_;
    OctreeBuildOptions opt_;
    jobs::Progress& progress_;
    OctreeInfo info_;
    static constexpr int kCountLevel = 7; // 128³ counting grid

    void computeBounds() {
        progress_.update(0.0, "Scanning extents");
        src_.rewind();
        BoundingBox b = BoundingBox::empty();
        std::int64_t n = 0;
        PointBuffer chunk(info_.schema);
        while (true) {
            chunk.clear();
            const auto k = src_.read(chunk, 1 << 18);
            if (k == 0) break;
            for (const auto& p : chunk.xyz) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) throw PointCloudError("The source contains non-finite coordinates");
                b.expand(p);
            }
            n += static_cast<std::int64_t>(k);
            progress_.update(src_.progress());
        }
        info_.pointCount = n;
        info_.bounds = b;
        if (n == 0) return;
        const Vec3 size = b.size();
        const double edge = std::max({size.x, size.y, size.z, 1e-3}) * (1 + 1e-9) + 1e-9;
        info_.cube = {b.min, b.min + Vec3{edge, edge, edge}};
    }

    std::int64_t cellOf(Vec3 p, const BoundingBox& cube, int res) const {
        const double e = cube.max.x - cube.min.x;
        auto ix = [&](double v, double lo) { return std::clamp(static_cast<std::int64_t>((v - lo) / e * res), std::int64_t{0}, static_cast<std::int64_t>(res - 1)); };
        return ix(p.x, cube.min.x) * res * res + ix(p.y, cube.min.y) * res + ix(p.z, cube.min.z);
    }

    void record(const std::string& name, int level, const BoundingBox& cube, const PointBuffer& pts) {
        writeNodeFile(folder_ / "nodes" / (name + ".bin"), pts);
        OctreeNodeInfo n;
        n.name = name;
        n.level = level;
        n.count = static_cast<std::int64_t>(pts.size());
        n.cube = cube;
        n.bounds = pts.bounds();
        n.spacing = (cube.max.x - cube.min.x) / opt_.grid;
        info_.nodes.push_back(std::move(n));
    }

    /// Selects one point per grid cell (nearest to the cell centre) of `cube` at resolution `res`
    /// among the points with `alive`; returns their indices.
    std::vector<std::size_t> sample(const PointBuffer& pts, const std::vector<char>& alive, const BoundingBox& cube, std::int64_t res) const {
        std::unordered_map<std::int64_t, std::pair<std::size_t, double>> best;
        best.reserve(std::min<std::size_t>(pts.size(), 1 << 20));
        const double e = cube.max.x - cube.min.x, cell = e / static_cast<double>(res);
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (!alive[i]) continue;
            const Vec3 p = pts.xyz[i];
            const auto ix = std::clamp(static_cast<std::int64_t>((p.x - cube.min.x) / cell), std::int64_t{0}, res - 1);
            const auto iy = std::clamp(static_cast<std::int64_t>((p.y - cube.min.y) / cell), std::int64_t{0}, res - 1);
            const auto iz = std::clamp(static_cast<std::int64_t>((p.z - cube.min.z) / cell), std::int64_t{0}, res - 1);
            const Vec3 c = cube.min + Vec3{(static_cast<double>(ix) + 0.5) * cell, (static_cast<double>(iy) + 0.5) * cell, (static_cast<double>(iz) + 0.5) * cell};
            const double d = distanceSquared(p, c);
            const std::int64_t key = (ix * res + iy) * res + iz;
            auto [it, inserted] = best.try_emplace(key, i, d);
            if (!inserted && d < it->second.second) it->second = {i, d};
        }
        std::vector<std::size_t> idx;
        idx.reserve(best.size());
        for (const auto& [k, v] : best) idx.push_back(v.first);
        std::sort(idx.begin(), idx.end());
        return idx;
    }

    void buildSubtree(PointBuffer pts, const std::string& name, int level, const BoundingBox& cube) {
        progress_.checkCancelled();
        if (pts.size() <= opt_.maxNodePoints || level >= opt_.maxDepth) {
            record(name, level, cube, pts);
            return;
        }
        std::vector<char> alive(pts.size(), 1);
        const auto own = sample(pts, alive, cube, opt_.grid);
        for (auto i : own) alive[i] = 0;
        record(name, level, cube, pts.subset(own));
        const Vec3 mid = cube.center();
        std::array<std::vector<std::size_t>, 8> parts;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (!alive[i]) continue;
            const Vec3 p = pts.xyz[i];
            const int d = (p.x >= mid.x ? 4 : 0) | (p.y >= mid.y ? 2 : 0) | (p.z >= mid.z ? 1 : 0);
            parts[static_cast<std::size_t>(d)].push_back(i);
        }
        std::array<PointBuffer, 8> children;
        for (int d = 0; d < 8; ++d) children[static_cast<std::size_t>(d)] = pts.subset(parts[static_cast<std::size_t>(d)]);
        pts = PointBuffer{}; // release memory before recursion
        for (int d = 0; d < 8; ++d) {
            auto& c = children[static_cast<std::size_t>(d)];
            if (c.empty()) continue;
            buildSubtree(std::move(c), name + std::to_string(d), level + 1, childCube(cube, d));
        }
    }

    struct Chunk {
        std::string name;
        int level = 0;
        BoundingBox cube;
        std::int64_t count = 0;
        fs::path spill;
        std::string buffer;
    };

    void buildChunked() {
        const int res = 1 << kCountLevel;
        // Pass 2: counts per level-7 cell.
        progress_.update(0.0, "Counting points");
        std::vector<std::uint32_t> counts(static_cast<std::size_t>(res) * res * res, 0);
        src_.rewind();
        PointBuffer chunk(info_.schema);
        while (true) {
            chunk.clear();
            if (src_.read(chunk, 1 << 18) == 0) break;
            for (const auto& p : chunk.xyz) ++counts[static_cast<std::size_t>(cellOf(p, info_.cube, res))];
            progress_.update(src_.progress());
        }
        // Chunks: largest octree nodes with at most maxChunkPoints points.
        std::vector<Chunk> chunks;
        std::vector<std::int32_t> cellChunk(counts.size(), -1);
        std::function<void(const std::string&, int, std::int64_t, std::int64_t, std::int64_t)> split =
            [&](const std::string& name, int level, std::int64_t x0, std::int64_t y0, std::int64_t z0) {
                const std::int64_t span = std::int64_t{1} << (kCountLevel - level); // cells per axis
                std::int64_t total = 0;
                for (std::int64_t x = x0; x < x0 + span; ++x)
                    for (std::int64_t y = y0; y < y0 + span; ++y)
                        for (std::int64_t z = z0; z < z0 + span; ++z) total += counts[static_cast<std::size_t>((x * res + y) * res + z)];
                if (total == 0) return;
                if (static_cast<std::size_t>(total) <= opt_.maxChunkPoints || level == kCountLevel) {
                    const double e = info_.cube.max.x - info_.cube.min.x, cs = e / res;
                    Chunk c;
                    c.name = name;
                    c.level = level;
                    c.count = total;
                    c.cube.min = info_.cube.min + Vec3{static_cast<double>(x0) * cs, static_cast<double>(y0) * cs, static_cast<double>(z0) * cs};
                    c.cube.max = c.cube.min + Vec3{static_cast<double>(span) * cs, static_cast<double>(span) * cs, static_cast<double>(span) * cs};
                    c.spill = folder_ / ("chunk_" + name + ".tmp");
                    const auto id = static_cast<std::int32_t>(chunks.size());
                    for (std::int64_t x = x0; x < x0 + span; ++x)
                        for (std::int64_t y = y0; y < y0 + span; ++y)
                            for (std::int64_t z = z0; z < z0 + span; ++z) cellChunk[static_cast<std::size_t>((x * res + y) * res + z)] = id;
                    chunks.push_back(std::move(c));
                    return;
                }
                const std::int64_t h = span / 2;
                for (int d = 0; d < 8; ++d)
                    split(name + std::to_string(d), level + 1, x0 + ((d & 4) ? h : 0), y0 + ((d & 2) ? h : 0), z0 + ((d & 1) ? h : 0));
            };
        split("r", 0, 0, 0, 0);
        counts = {};
        // Pass 3: distribute to chunk spill files.
        progress_.update(0.0, "Distributing points");
        src_.rewind();
        auto flush = [&](Chunk& c) {
            if (c.buffer.empty()) return;
            std::ofstream o(c.spill, std::ios::binary | std::ios::app);
            o.write(c.buffer.data(), static_cast<std::streamsize>(c.buffer.size()));
            if (!o) throw PointCloudError("Cannot write temporary file (disk full?)");
            c.buffer.clear();
        };
        while (true) {
            chunk.clear();
            if (src_.read(chunk, 1 << 18) == 0) break;
            for (std::size_t i = 0; i < chunk.size(); ++i) {
                auto& c = chunks[static_cast<std::size_t>(cellChunk[static_cast<std::size_t>(cellOf(chunk.xyz[i], info_.cube, res))])];
                encodeRecord(chunk, i, c.buffer);
                if (c.buffer.size() > (1 << 20)) flush(c);
            }
            progress_.update(src_.progress());
        }
        for (auto& c : chunks) flush(c);
        // Chunks → subtrees; ancestors receive the samples of their levels.
        std::map<std::string, PointBuffer> ancestors;
        std::map<std::string, std::pair<int, BoundingBox>> ancestorCubes;
        for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
            auto& c = chunks[ci];
            progress_.update(static_cast<double>(ci) / static_cast<double>(chunks.size()), "Building octree");
            std::string data;
            {
                std::ifstream in(c.spill, std::ios::binary);
                data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            fs::remove(c.spill);
            PointBuffer pts(info_.schema);
            decodeRecords(data, pts);
            data = {};
            std::vector<char> alive(pts.size(), 1);
            BoundingBox acube = info_.cube;
            for (int k = 0; k < c.level; ++k) {
                const std::string an = c.name.substr(0, static_cast<std::size_t>(k) + 1);
                // The ancestor's grid restricted to this chunk: same cells, global resolution grid·2^k.
                const std::int64_t gres = static_cast<std::int64_t>(opt_.grid) << k;
                const auto sel = sample(pts, alive, info_.cube, gres);
                auto& buf = ancestors.try_emplace(an, info_.schema).first->second;
                for (auto i : sel) {
                    buf.appendFrom(pts, i);
                    alive[i] = 0;
                }
                ancestorCubes[an] = {k, acube};
                acube = childCube(acube, c.name[static_cast<std::size_t>(k) + 1] - '0');
            }
            buildSubtree(pts.filtered(alive), c.name, c.level, c.cube);
        }
        for (auto& [name, pts] : ancestors) record(name, ancestorCubes[name].first, ancestorCubes[name].second, pts);
    }

    void writeMetadata() {
        std::sort(info_.nodes.begin(), info_.nodes.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        json j;
        j["format"] = "openmine-octree";
        j["version"] = 1;
        j["pointCount"] = info_.pointCount;
        j["bounds"] = boxJson(info_.bounds);
        j["cube"] = boxJson(info_.cube);
        j["grid"] = opt_.grid;
        j["schema"] = schemaJson(info_.schema);
        json nodes = json::array();
        std::int64_t total = 0;
        for (const auto& n : info_.nodes) {
            nodes.push_back({{"name", n.name}, {"level", n.level}, {"count", n.count}, {"cube", boxJson(n.cube)}, {"bounds", boxJson(n.bounds)}, {"spacing", n.spacing}});
            total += n.count;
        }
        if (total != info_.pointCount) throw PointCloudError("Internal error: octree holds " + std::to_string(total) + " of " + std::to_string(info_.pointCount) + " points");
        j["nodes"] = nodes;
        std::ofstream o(folder_ / "octree.json");
        o << j.dump(1);
        if (!o) throw PointCloudError("Cannot write octree metadata");
    }
};

class OctreeSource : public PointSource {
public:
    explicit OctreeSource(const OctreeReader& r) : r_(r) {}
    PointSchema schema() const override { return r_.info().schema; }
    std::string formatName() const override { return "OpenMine octree"; }
    std::optional<std::int64_t> countHint() const override { return r_.info().pointCount; }
    std::optional<BoundingBox> boundsHint() const override { return r_.info().bounds; }
    void rewind() override {
        node_ = 0;
        pos_ = 0;
        cur_ = PointBuffer{};
        done_ = 0;
    }
    double progress() const override {
        return r_.info().pointCount ? static_cast<double>(done_) / static_cast<double>(r_.info().pointCount) : 1.0;
    }
    std::size_t read(PointBuffer& out, std::size_t max) override {
        if (out.empty()) out.setSchema(schema());
        std::size_t n = 0;
        while (n < max) {
            if (pos_ >= cur_.size()) {
                if (node_ >= r_.info().nodes.size()) break;
                cur_ = r_.readNode(r_.info().nodes[node_++].name);
                pos_ = 0;
                continue;
            }
            const std::size_t k = std::min(max - n, cur_.size() - pos_);
            for (std::size_t i = 0; i < k; ++i) out.appendFrom(cur_, pos_ + i);
            pos_ += k;
            n += k;
        }
        done_ += static_cast<std::int64_t>(n);
        return n;
    }

private:
    const OctreeReader& r_;
    std::size_t node_ = 0, pos_ = 0;
    PointBuffer cur_;
    std::int64_t done_ = 0;
};

} // namespace

OctreeInfo buildOctree(PointSource& source, const fs::path& folder, const OctreeBuildOptions& options, jobs::Progress& progress) {
    Builder b(source, folder, options, progress);
    try {
        return b.run();
    } catch (...) {
        // Leave nothing half-written behind.
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(folder, ec))
            if (e.path().extension() == ".tmp") fs::remove(e.path(), ec);
        throw;
    }
}

OctreeReader::OctreeReader(const fs::path& folder) : folder_(folder) {
    std::ifstream in(folder / "octree.json");
    if (!in) throw PointCloudError("Not an OpenMine point-cloud octree: " + folder.string());
    json j;
    try {
        in >> j;
        if (j.value("format", "") != "openmine-octree") throw PointCloudError("Unknown octree format");
        if (j.value("version", 0) > 1) throw PointCloudError("The octree was written by a newer version of OpenMine");
        info_.pointCount = j.at("pointCount");
        info_.bounds = boxFrom(j.at("bounds"));
        info_.cube = boxFrom(j.at("cube"));
        info_.schema = schemaFrom(j.at("schema"));
        for (const auto& n : j.at("nodes")) {
            OctreeNodeInfo ni;
            ni.name = n.at("name");
            ni.level = n.at("level");
            ni.count = n.at("count");
            ni.cube = boxFrom(n.at("cube"));
            ni.bounds = boxFrom(n.at("bounds"));
            ni.spacing = n.at("spacing");
            index_[ni.name] = info_.nodes.size();
            info_.nodes.push_back(std::move(ni));
        }
    } catch (const json::exception& e) {
        throw PointCloudError(std::string("Corrupt octree metadata: ") + e.what());
    }
}

const OctreeNodeInfo* OctreeReader::node(const std::string& name) const {
    const auto it = index_.find(name);
    return it == index_.end() ? nullptr : &info_.nodes[it->second];
}

std::vector<const OctreeNodeInfo*> OctreeReader::children(const std::string& name) const {
    std::vector<const OctreeNodeInfo*> out;
    for (int d = 0; d < 8; ++d)
        if (const auto* n = node(name + std::to_string(d))) out.push_back(n);
    return out;
}

PointBuffer OctreeReader::readNode(const std::string& name) const {
    if (!node(name)) throw PointCloudError("Unknown octree node " + name);
    return readNodeFile(folder_ / "nodes" / (name + ".bin"), info_.schema);
}

std::vector<const OctreeNodeInfo*> OctreeReader::select(const std::function<bool(const OctreeNodeInfo&)>& descend) const {
    std::vector<const OctreeNodeInfo*> out;
    std::function<void(const OctreeNodeInfo&)> visit = [&](const OctreeNodeInfo& n) {
        out.push_back(&n);
        if (!descend(n)) return;
        for (const auto* c : children(n.name)) visit(*c);
    };
    if (const auto* r = node("r")) visit(*r);
    return out;
}

PointBuffer OctreeReader::readAll(const std::optional<BoundingBox>& box, jobs::Progress& progress) const {
    PointBuffer out(info_.schema);
    if (!box) out.reserve(static_cast<std::size_t>(info_.pointCount));
    std::size_t k = 0;
    for (const auto& n : info_.nodes) {
        progress.update(static_cast<double>(k++) / static_cast<double>(info_.nodes.size()), "Reading points");
        if (box && !n.bounds.intersects(*box)) continue;
        auto b = readNode(n.name);
        if (!box) {
            out.append(b);
            continue;
        }
        for (std::size_t i = 0; i < b.size(); ++i)
            if (box->contains(b.xyz[i])) out.appendFrom(b, i);
    }
    return out;
}

std::unique_ptr<PointSource> OctreeReader::source() const { return std::make_unique<OctreeSource>(*this); }

} // namespace openmine::survey
