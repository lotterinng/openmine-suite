// SPDX-License-Identifier: Apache-2.0
#include "openmine/geology/BlockModel.h"

#include "openmine/geometry/MeshOps.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/util/Csv.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
#include <iterator>
#include <cstdio>

namespace openmine::geology {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double swapBytes(double x) {
    auto u = std::bit_cast<std::uint64_t>(x);
    std::uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r = (r << 8) | ((u >> (8 * i)) & 0xFF);
    return std::bit_cast<double>(r);
}

std::string fmt(double v) {
    if (std::isnan(v)) return {};
    std::ostringstream ss;
    ss << std::setprecision(12) << v;
    return ss.str();
}

/// All crossings of the vertical line through (x, y) with the mesh, sorted by z.
std::vector<double> verticalCrossings(const geometry::MeshBvh& bvh, double x, double y, double z0, double z1) {
    std::vector<double> zs;
    const double len = z1 - z0;
    double t0 = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        const auto hit = bvh.raycast({x, y, z0 + t0}, {0, 0, 1}, len - t0);
        if (!hit) break;
        zs.push_back(hit->point.z);
        t0 += hit->distance + 1e-9 * std::max(1.0, len);
        if (t0 >= len) break;
    }
    return zs;
}

// Column jitter (fraction of the sub-cell) so sampling lines avoid lying exactly on mesh edges of
// grid-aligned wireframes.
constexpr double kJitter = 1.2345678e-6, kJitterV = 2.7182818e-6;
} // namespace

// ---- BlockGrid ----------------------------------------------------------------------------------
BlockGrid::BlockGrid(const BlockModel& m) : m_(m) {
    const double r = m.rotationDeg * std::numbers::pi / 180.0;
    u_ = {std::cos(r), std::sin(r), 0};
    v_ = {-std::sin(r), std::cos(r), 0};
}

std::array<int, 3> BlockGrid::ijk(std::size_t idx) const {
    const auto nx = static_cast<std::size_t>(m_.nx), ny = static_cast<std::size_t>(m_.ny);
    return {static_cast<int>(idx % nx), static_cast<int>((idx / nx) % ny), static_cast<int>(idx / (nx * ny))};
}

Vec3 BlockGrid::toWorld(Vec3 l) const { return m_.origin + u_ * l.x + v_ * l.y + Vec3{0, 0, l.z}; }

Vec3 BlockGrid::toLocal(Vec3 w) const {
    const Vec3 d = w - m_.origin;
    return {dot(d, u_), dot(d, v_), d.z};
}

Vec3 BlockGrid::centroid(int i, int j, int k) const {
    return toWorld({(i + 0.5) * m_.blockSize.x, (j + 0.5) * m_.blockSize.y, (k + 0.5) * m_.blockSize.z});
}

Vec3 BlockGrid::centroid(std::size_t idx) const {
    const auto c = ijk(idx);
    return centroid(c[0], c[1], c[2]);
}

std::optional<std::array<int, 3>> BlockGrid::cellOf(Vec3 w) const {
    const Vec3 l = toLocal(w);
    const auto i = static_cast<long>(std::floor(l.x / m_.blockSize.x)), j = static_cast<long>(std::floor(l.y / m_.blockSize.y)),
               k = static_cast<long>(std::floor(l.z / m_.blockSize.z));
    if (i < 0 || j < 0 || k < 0 || i >= m_.nx || j >= m_.ny || k >= m_.nz) return std::nullopt;
    return std::array<int, 3>{static_cast<int>(i), static_cast<int>(j), static_cast<int>(k)};
}

std::array<Vec3, 8> BlockGrid::corners() const {
    std::array<Vec3, 8> c;
    const Vec3 e{m_.nx * m_.blockSize.x, m_.ny * m_.blockSize.y, m_.nz * m_.blockSize.z};
    for (int n = 0; n < 8; ++n) c[static_cast<std::size_t>(n)] = toWorld({(n & 1) ? e.x : 0, (n & 2) ? e.y : 0, (n & 4) ? e.z : 0});
    return c;
}

// ---- data -----------------------------------------------------------------------------------------
std::vector<double>& BlockModelData::attribute(const std::string& name) {
    auto it = values.find(name);
    if (it != values.end()) return it->second;
    if (!isValidAttributeKey(name)) throw ValidationError("Invalid block attribute name '" + name + "'");
    header.attributes.push_back(name);
    return values.emplace(name, std::vector<double>(count(), kNaN)).first->second;
}

const std::vector<double>* BlockModelData::find(const std::string& name) const {
    auto it = values.find(name);
    return it == values.end() ? nullptr : &it->second;
}

void BlockModelData::remove(const std::string& name) {
    values.erase(name);
    header.attributes.erase(std::remove(header.attributes.begin(), header.attributes.end(), name), header.attributes.end());
}

BlockModel gridCovering(const std::vector<Vec3>& pts, Vec3 size, double rotationDeg, Vec3 snap, double padding) {
    if (pts.empty()) throw ValidationError("No points to cover");
    if (!(size.x > 0 && size.y > 0 && size.z > 0)) throw ValidationError("Block size must be positive");
    BlockModel frame;
    frame.origin = snap;
    frame.rotationDeg = rotationDeg;
    const BlockGrid g(frame);
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (const auto& p : pts) {
        const Vec3 l = g.toLocal(p);
        lo = componentMin(lo, l);
        hi = componentMax(hi, l);
    }
    lo -= Vec3{padding, padding, padding};
    hi += Vec3{padding, padding, padding};
    BlockModel m;
    m.rotationDeg = rotationDeg;
    m.blockSize = size;
    const Vec3 lo2{std::floor(lo.x / size.x) * size.x, std::floor(lo.y / size.y) * size.y, std::floor(lo.z / size.z) * size.z};
    m.nx = std::max(1, static_cast<int>(std::ceil((hi.x - lo2.x) / size.x - 1e-9)));
    m.ny = std::max(1, static_cast<int>(std::ceil((hi.y - lo2.y) / size.y - 1e-9)));
    m.nz = std::max(1, static_cast<int>(std::ceil((hi.z - lo2.z) / size.z - 1e-9)));
    m.origin = g.toWorld(lo2);
    return m;
}

Uuid createBlockModel(Project& p, BlockModel header) {
    header.dataAsset = {};
    header.attributes.clear();
    return ProjectEditor(p).add(std::move(header), "Create block model");
}

BlockModelData loadBlockModel(const Project& p, const Uuid& id) {
    const auto* bm = p.model().blockModels.find(id);
    if (!bm) throw ValidationError("Block model does not exist");
    BlockModelData d;
    d.header = *bm;
    const std::size_t n = d.count();
    if (bm->attributes.empty()) return d;
    const auto* asset = p.model().assets.find(bm->dataAsset.id);
    if (!asset) throw ValidationError("The data asset of block model '" + bm->name + "' is missing");
    const std::string bytes = p.assets().readBytes(*asset);
    if (bytes.size() != n * bm->attributes.size() * sizeof(double))
        throw ValidationError("Block model data size does not match its definition (" + std::to_string(bytes.size()) + " bytes)");
    for (std::size_t a = 0; a < bm->attributes.size(); ++a) {
        std::vector<double> v(n);
        std::memcpy(v.data(), bytes.data() + a * n * sizeof(double), n * sizeof(double));
        if constexpr (std::endian::native == std::endian::big)
            for (auto& x : v) x = swapBytes(x);
        d.values.emplace(bm->attributes[a], std::move(v));
    }
    return d;
}

void saveBlockModel(Project& p, const BlockModelData& d, const std::string& text) {
    const auto* bm = p.model().blockModels.find(d.header.id);
    if (!bm) throw ValidationError("Block model does not exist");
    if (bm->nx != d.header.nx || bm->ny != d.header.ny || bm->nz != d.header.nz)
        throw ValidationError("The block model grid changed since it was loaded; reload before saving");
    const std::size_t n = d.count();
    std::string bytes;
    bytes.resize(n * d.header.attributes.size() * sizeof(double));
    for (std::size_t a = 0; a < d.header.attributes.size(); ++a) {
        const auto* v = d.find(d.header.attributes[a]);
        if (!v || v->size() != n) throw ValidationError("Attribute '" + d.header.attributes[a] + "' has the wrong number of values");
        std::vector<double> le = *v;
        if constexpr (std::endian::native == std::endian::big)
            for (auto& x : le) x = swapBytes(x);
        std::memcpy(bytes.data() + a * n * sizeof(double), le.data(), n * sizeof(double));
    }
    BlockModel h = *bm;
    h.attributes = d.header.attributes;
    h.name = d.header.name;
    h.description = d.header.description;
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), text);
    if (h.attributes.empty()) h.dataAsset = {};
    else {
        Asset asset = p.assets().writeBytes(bytes, AssetKind::BlockModel, "f64");
        h.dataAsset.id = ed.add(asset);
    }
    ed.update(h);
    macro.commit();
}

// ---- domains ------------------------------------------------------------------------------------
std::vector<double> fractionInside(const BlockGrid& g, const MeshData& mesh, int sub, jobs::Progress& progress) {
    const auto& m = g.model();
    sub = std::max(1, sub);
    std::vector<double> frac(g.count(), 0.0);
    const geometry::MeshBvh bvh(mesh);
    const double z0 = m.origin.z - 1.0, z1 = m.origin.z + m.nz * m.blockSize.z + 1.0;
    const double w = 1.0 / (static_cast<double>(sub) * sub * sub);
    const int cols = m.nx * sub;
    for (int ci = 0; ci < cols; ++ci) {
        progress.checkCancelled();
        progress.update(static_cast<double>(ci) / cols, "Domain proportions");
        for (int cj = 0; cj < m.ny * sub; ++cj) {
            const Vec3 xy = g.toWorld({(ci + 0.5 + kJitter) / sub * m.blockSize.x, (cj + 0.5 + kJitterV) / sub * m.blockSize.y, 0});
            const auto zs = verticalCrossings(bvh, xy.x, xy.y, z0, z1);
            if (zs.empty()) continue;
            for (int kk = 0; kk < m.nz * sub; ++kk) {
                const double z = m.origin.z + (kk + 0.5) / sub * m.blockSize.z;
                const auto below = std::lower_bound(zs.begin(), zs.end(), z) - zs.begin();
                if (below % 2 == 1) frac[g.index(ci / sub, cj / sub, kk / sub)] += w;
            }
        }
    }
    progress.update(1.0);
    return frac;
}

std::vector<double> fractionBelow(const BlockGrid& g, const MeshData& surface, int sub, jobs::Progress& progress) {
    const auto& m = g.model();
    sub = std::max(1, sub);
    std::vector<double> frac(g.count(), 0.0);
    std::vector<int> missing(g.count(), 0);
    const geometry::MeshBvh bvh(surface);
    const double top = m.origin.z + m.nz * m.blockSize.z + 1.0;
    BoundingBox bb;
    bb.min = bb.max = surface.vertices.empty() ? Vec3{} : surface.vertices.front();
    for (const auto& v : surface.vertices) bb.expand(v);
    const double start = std::max(top, bb.max.z + 1.0), depth = start - std::min(m.origin.z - 1.0, bb.min.z - 1.0);
    const double w = 1.0 / (static_cast<double>(sub) * sub * sub);
    for (int ci = 0; ci < m.nx * sub; ++ci) {
        progress.checkCancelled();
        progress.update(static_cast<double>(ci) / (m.nx * sub), "Proportion below surface");
        for (int cj = 0; cj < m.ny * sub; ++cj) {
            const Vec3 xy = g.toWorld({(ci + 0.5 + kJitter) / sub * m.blockSize.x, (cj + 0.5 + kJitterV) / sub * m.blockSize.y, 0});
            const auto hit = bvh.raycast({xy.x, xy.y, start}, {0, 0, -1}, depth);
            for (int kk = 0; kk < m.nz * sub; ++kk) {
                const auto idx = g.index(ci / sub, cj / sub, kk / sub);
                if (!hit) {
                    ++missing[idx];
                    continue;
                }
                const double z = m.origin.z + (kk + 0.5) / sub * m.blockSize.z;
                if (z < hit->point.z) frac[idx] += w;
            }
        }
    }
    for (std::size_t i = 0; i < frac.size(); ++i)
        if (missing[i]) frac[i] = kNaN; // the surface does not cover the whole block
    progress.update(1.0);
    return frac;
}

std::size_t codeBlocksInside(BlockModelData& d, const MeshData& mesh, const std::string& field, double code) {
    const BlockGrid g(d.header);
    const geometry::MeshBvh bvh(mesh);
    auto& v = d.attribute(field);
    std::size_t n = 0;
    for (std::size_t i = 0; i < d.count(); ++i)
        if (bvh.contains(g.centroid(i))) {
            v[i] = code;
            ++n;
        }
    return n;
}

// ---- reporting ----------------------------------------------------------------------------------
std::vector<GradeTonnageRow> gradeTonnage(const BlockModelData& d, const GradeTonnageOptions& o) {
    const auto* grade = d.find(o.gradeField);
    if (!grade) throw ValidationError("Block model has no attribute '" + o.gradeField + "'");
    const auto* dens = o.densityField.empty() ? nullptr : d.find(o.densityField);
    if (!o.densityField.empty() && !dens) throw ValidationError("Block model has no attribute '" + o.densityField + "'");
    const auto* prop = o.proportionField.empty() ? nullptr : d.find(o.proportionField);
    if (!o.proportionField.empty() && !prop) throw ValidationError("Block model has no attribute '" + o.proportionField + "'");
    const auto* filt = o.filterField.empty() ? nullptr : d.find(o.filterField);
    if (!o.filterField.empty() && !filt) throw ValidationError("Block model has no attribute '" + o.filterField + "'");
    std::vector<std::string> fields{o.gradeField};
    for (const auto& f : o.otherFields)
        if (f != o.gradeField) {
            if (!d.find(f)) throw ValidationError("Block model has no attribute '" + f + "'");
            fields.push_back(f);
        }
    const double vol = BlockGrid(d.header).blockVolume();
    std::vector<GradeTonnageRow> rows;
    for (double c : o.cutoffs) {
        GradeTonnageRow r;
        r.cutoff = c;
        std::map<std::string, double> tw, gw;
        for (std::size_t i = 0; i < d.count(); ++i) {
            const double g = (*grade)[i];
            if (std::isnan(g) || g < c) continue;
            if (filt && (!o.filterValue || (*filt)[i] != *o.filterValue)) continue;
            const double p = prop ? (*prop)[i] : 1.0;
            if (!(p > 0)) continue;
            const double rho = dens ? (*dens)[i] : o.defaultDensity;
            if (!(rho > 0)) continue; // no density: not reportable
            const double v = vol * p, t = v * rho;
            ++r.blocks;
            r.volume += v;
            r.tonnes += t;
            for (const auto& f : fields) {
                const double x = (*d.find(f))[i];
                if (std::isnan(x)) continue;
                tw[f] += t;
                gw[f] += t * x;
            }
        }
        for (const auto& f : fields)
            if (tw[f] > 0) {
                r.grades[f] = gw[f] / tw[f];
                r.metal[f] = gw[f];
            }
        rows.push_back(std::move(r));
    }
    return rows;
}

std::string gradeTonnageCsv(const std::vector<GradeTonnageRow>& rows) {
    std::set<std::string> fields;
    for (const auto& r : rows)
        for (const auto& [k, v] : r.grades) fields.insert(k);
    std::vector<std::string> head{"CUTOFF", "BLOCKS", "VOLUME", "TONNES"};
    for (const auto& f : fields) {
        head.push_back(f);
        head.push_back(f + "_METAL");
    }
    std::ostringstream out;
    out << util::csvLine(head) << '\n';
    for (const auto& r : rows) {
        std::vector<std::string> l{fmt(r.cutoff), std::to_string(r.blocks), fmt(r.volume), fmt(r.tonnes)};
        for (const auto& f : fields) {
            auto g = r.grades.find(f);
            l.push_back(g == r.grades.end() ? std::string{} : fmt(g->second));
            auto m = r.metal.find(f);
            l.push_back(m == r.metal.end() ? std::string{} : fmt(m->second));
        }
        out << util::csvLine(l) << '\n';
    }
    return out.str();
}

// ---- CSV ------------------------------------------------------------------------------------------
std::string exportBlockModelCsv(const BlockModelData& d, bool skipEmpty) {
    const BlockGrid g(d.header);
    std::ostringstream out;
    std::vector<std::string> head{"X", "Y", "Z", "I", "J", "K"};
    for (const auto& a : d.header.attributes) head.push_back(a);
    out << util::csvLine(head) << '\n';
    for (std::size_t i = 0; i < d.count(); ++i) {
        bool any = false;
        for (const auto& a : d.header.attributes)
            if (!std::isnan(d.values.at(a)[i])) any = true;
        if (skipEmpty && !any && !d.header.attributes.empty()) continue;
        const auto c = g.centroid(i);
        const auto ijk = g.ijk(i);
        std::vector<std::string> l{fmt(c.x), fmt(c.y), fmt(c.z), std::to_string(ijk[0]), std::to_string(ijk[1]), std::to_string(ijk[2])};
        for (const auto& a : d.header.attributes) l.push_back(fmt(d.values.at(a)[i]));
        out << util::csvLine(l) << '\n';
    }
    return out.str();
}

namespace {
struct CentroidCols {
    std::size_t x, y, z;
};
std::optional<CentroidCols> centroidColumns(const util::CsvTable& t) {
    const auto x = t.column({"x", "xc", "xcentre", "xcenter", "x_centre", "x_center", "east"});
    const auto y = t.column({"y", "yc", "ycentre", "ycenter", "y_centre", "y_center", "north"});
    const auto z = t.column({"z", "zc", "zcentre", "zcenter", "z_centre", "z_center", "rl", "elev"});
    if (!x || !y || !z) return std::nullopt;
    return CentroidCols{*x, *y, *z};
}
} // namespace

BlockCsvImport importBlockModelCsv(const std::string& text, const BlockModel& grid) {
    BlockCsvImport r;
    r.data.header = grid;
    r.data.header.attributes.clear();
    const auto t = util::parseCsv(text);
    for (const auto& w : t.warnings) r.issues.push_back(w);
    const auto cc = centroidColumns(t);
    if (!cc) {
        r.issues.push_back("No centroid columns (X/Y/Z or XC/YC/ZC)");
        return r;
    }
    const auto ic = t.column({"i", "ix"}), jc = t.column({"j", "iy"}), kc = t.column({"k", "iz"});
    std::vector<std::size_t> attrCols;
    for (std::size_t c = 0; c < t.headers.size(); ++c) {
        if (c == cc->x || c == cc->y || c == cc->z || c == ic || c == jc || c == kc) continue;
        if (!isValidAttributeKey(t.headers[c])) {
            r.issues.push_back("Column '" + t.headers[c] + "' is not a valid attribute name; skipped");
            continue;
        }
        bool numeric = true;
        for (const auto& row : t.rows)
            if (!row[c].empty() && !util::parseNumber(row[c])) {
                numeric = false;
                break;
            }
        if (!numeric) {
            r.issues.push_back("Column '" + t.headers[c] + "' has non-numeric values; block attributes are numeric, so it was skipped");
            continue;
        }
        attrCols.push_back(c);
        r.data.attribute(t.headers[c]);
    }
    const BlockGrid g(grid);
    std::vector<char> filled(r.data.count(), 0);
    std::size_t rejected = 0, dup = 0;
    for (std::size_t i = 0; i < t.rows.size(); ++i) {
        ++r.rows;
        const auto& row = t.rows[i];
        const auto x = util::parseNumber(row[cc->x]), y = util::parseNumber(row[cc->y]), z = util::parseNumber(row[cc->z]);
        if (!x || !y || !z) {
            if (++rejected <= 20) r.issues.push_back("Row " + std::to_string(i + 1) + ": centroid not numeric; rejected");
            continue;
        }
        const Vec3 p{*x, *y, *z};
        const auto cell = g.cellOf(p);
        if (!cell) {
            if (++rejected <= 20) r.issues.push_back("Row " + std::to_string(i + 1) + ": outside the grid; rejected");
            continue;
        }
        const Vec3 c = g.centroid((*cell)[0], (*cell)[1], (*cell)[2]);
        const Vec3 off = g.toLocal(p) - g.toLocal(c);
        if (std::abs(off.x) > 0.01 * grid.blockSize.x || std::abs(off.y) > 0.01 * grid.blockSize.y || std::abs(off.z) > 0.01 * grid.blockSize.z) {
            if (++rejected <= 20) r.issues.push_back("Row " + std::to_string(i + 1) + ": not at a block centre of this grid (sub-blocked or different grid); rejected");
            continue;
        }
        const auto idx = g.index((*cell)[0], (*cell)[1], (*cell)[2]);
        if (filled[idx]) {
            if (++dup <= 20) r.issues.push_back("Row " + std::to_string(i + 1) + ": duplicate block; first value kept");
            continue;
        }
        filled[idx] = 1;
        ++r.placed;
        for (auto c2 : attrCols)
            if (auto v = util::parseNumber(row[c2])) r.data.values.at(t.headers[c2])[idx] = *v;
    }
    if (rejected > 20) r.issues.push_back(std::to_string(rejected) + " rows rejected in total");
    if (dup > 20) r.issues.push_back(std::to_string(dup) + " duplicate rows in total");
    return r;
}

BlockModel inferGridFromCsv(const std::string& text, Vec3 size) {
    const auto t = util::parseCsv(text);
    const auto cc = centroidColumns(t);
    if (!cc) throw ValidationError("No centroid columns (X/Y/Z or XC/YC/ZC)");
    std::array<std::set<double>, 3> u;
    for (const auto& row : t.rows) {
        const std::size_t cols[3] = {cc->x, cc->y, cc->z};
        for (int a = 0; a < 3; ++a)
            if (auto v = util::parseNumber(row[cols[a]])) u[static_cast<std::size_t>(a)].insert(*v);
    }
    BlockModel m;
    m.name = "Imported block model";
    for (int a = 0; a < 3; ++a) {
        const auto& s = u[static_cast<std::size_t>(a)];
        if (s.empty()) throw ValidationError("No numeric centroids");
        double step = size[a];
        if (!(step > 0)) {
            step = 1e300;
            for (auto it = std::next(s.begin()); it != s.end(); ++it) step = std::min(step, *it - *std::prev(it));
            if (step == 1e300) throw ValidationError("Cannot infer the block size from a single centroid; give the block size");
        }
        const double lo = *s.begin(), hi = *s.rbegin();
        const int n = static_cast<int>(std::lround((hi - lo) / step)) + 1;
        if (a == 0) {
            m.blockSize.x = step;
            m.nx = n;
            m.origin.x = lo - step / 2;
        } else if (a == 1) {
            m.blockSize.y = step;
            m.ny = n;
            m.origin.y = lo - step / 2;
        } else {
            m.blockSize.z = step;
            m.nz = n;
            m.origin.z = lo - step / 2;
        }
    }
    return m;
}

} // namespace openmine::geology
