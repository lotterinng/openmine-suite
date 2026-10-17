// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/PointCloudProject.h"

#include "openmine/commands/CommandStack.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/TextFormats.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

namespace openmine::survey {

namespace fs = std::filesystem;

namespace {
std::string lowerExt(const fs::path& f) {
    std::string e = f.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

struct Registry {
    std::mutex m;
    std::map<std::string, SourceOpener> readers;
    std::map<std::string, FileWriter> writers;
    Registry() {
        for (const char* e : {".xyz", ".csv", ".txt", ".asc"}) readers[e] = [](const fs::path& f) { return openTextSource(f); };
        readers[".pts"] = [](const fs::path& f) { return openPtsSource(f); };
        readers[".ptx"] = [](const fs::path& f) { return openPtxSource(f); };
        auto textWriter = [](char delim) {
            return [delim](PointSource& src, const fs::path& f, jobs::Progress& pr) {
                TextWriteOptions o;
                o.delimiter = delim;
                auto sink = textSink(f, o);
                src.rewind();
                sink->begin(src.schema());
                PointBuffer b(src.schema());
                while (true) {
                    b.clear();
                    if (src.read(b, 1 << 16) == 0) break;
                    sink->write(b);
                    pr.update(src.progress(), "Writing points");
                }
                sink->finish();
            };
        };
        writers[".csv"] = textWriter(',');
        writers[".xyz"] = textWriter(' ');
        writers[".txt"] = textWriter(' ');
        writers[".pts"] = [](PointSource& src, const fs::path& f, jobs::Progress& pr) {
            auto sink = ptsSink(f);
            src.rewind();
            sink->begin(src.schema());
            PointBuffer b(src.schema());
            while (true) {
                b.clear();
                if (src.read(b, 1 << 16) == 0) break;
                sink->write(b);
                pr.update(src.progress(), "Writing points");
            }
            sink->finish();
        };
    }
};
Registry& registry() {
    static Registry r;
    return r;
}
} // namespace

void registerReader(const std::string& ext, SourceOpener opener) {
    std::lock_guard lk(registry().m);
    registry().readers[ext] = std::move(opener);
}
void registerWriter(const std::string& ext, FileWriter writer) {
    std::lock_guard lk(registry().m);
    registry().writers[ext] = std::move(writer);
}
std::vector<std::string> readableExtensions() {
    std::lock_guard lk(registry().m);
    std::vector<std::string> v;
    for (const auto& [k, _] : registry().readers) v.push_back(k);
    return v;
}
std::vector<std::string> writableExtensions() {
    std::lock_guard lk(registry().m);
    std::vector<std::string> v;
    for (const auto& [k, _] : registry().writers) v.push_back(k);
    return v;
}

std::unique_ptr<PointSource> openPointFile(const fs::path& file) {
    SourceOpener op;
    {
        std::lock_guard lk(registry().m);
        const auto it = registry().readers.find(lowerExt(file));
        if (it == registry().readers.end()) throw PointCloudError("Unsupported point-cloud format: " + file.extension().string());
        op = it->second;
    }
    if (!fs::exists(file)) throw PointCloudError("File not found: " + file.string());
    return op(file);
}

std::optional<BoundingBox> TransformSource::boundsHint() const {
    const auto b = inner_->boundsHint();
    if (!b || b->isEmpty()) return b;
    BoundingBox out = BoundingBox::empty();
    for (int k = 0; k < 8; ++k) out.expand(t_.apply({(k & 1) ? b->max.x : b->min.x, (k & 2) ? b->max.y : b->min.y, (k & 4) ? b->max.z : b->min.z}));
    return out;
}

std::size_t TransformSource::read(PointBuffer& out, std::size_t max) {
    const std::size_t before = out.size();
    const std::size_t n = inner_->read(out, max);
    if (t_.isIdentity()) return n;
    for (std::size_t i = before; i < out.size(); ++i) out.xyz[i] = t_.apply(out.xyz[i]);
    if (out.schema().normals)
        for (std::size_t i = before; i < out.size(); ++i) {
            const Vec3 v = t_.applyNormal({out.normal[i][0], out.normal[i][1], out.normal[i][2]});
            out.normal[i] = {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
        }
    return n;
}

namespace {
PreparedDataset prepareFromSource(Project& p, PointSource& src, const std::string& name, const OctreeBuildOptions& oo, jobs::Progress& progress) {
    auto [id, folder] = p.assets().beginFolder(AssetKind::PointCloud);
    try {
        const auto info = buildOctree(src, folder, oo, progress);
        PreparedDataset out;
        out.asset = p.assets().finalizeFolder(id, AssetKind::PointCloud);
        out.dataset.name = name;
        out.dataset.asset.id = out.asset.id;
        out.dataset.pointCount = info.pointCount;
        out.dataset.storedBounds = info.bounds;
        out.dataset.attributes = info.schema;
        out.dataset.colorMode = info.schema.rgb ? "RGB" : (info.schema.intensity ? "Intensity" : "Elevation");
        return out;
    } catch (...) {
        p.assets().abortFolder(id, AssetKind::PointCloud);
        throw;
    }
}
} // namespace

PreparedDataset prepareImport(Project& p, const fs::path& file, const ImportOptions& o, jobs::Progress& progress) {
    auto src = openPointFile(file);
    auto out = prepareFromSource(p, *src, o.name.empty() ? file.stem().string() : o.name, o.octree, progress);
    out.dataset.sourceFile = file.filename().string();
    out.dataset.sourceFormat = src->formatName();
    out.dataset.sourceCrs = o.sourceCrs.empty() ? src->declaredCrs() : o.sourceCrs;
    out.dataset.scanDate = o.scanDate;
    out.dataset.scanner = o.scanner;
    out.dataset.developmentId = o.developmentId;
    out.dataset.transform = o.transform;
    return out;
}

PreparedDataset prepareFromPoints(Project& p, const PointBuffer& points, const std::string& name, const std::string& sourceFile,
                                  const std::string& sourceFormat, jobs::Progress& progress) {
    BufferSource src(points);
    auto out = prepareFromSource(p, src, name, OctreeBuildOptions{}, progress);
    out.dataset.sourceFile = sourceFile;
    out.dataset.sourceFormat = sourceFormat;
    return out;
}

Uuid commitDataset(Project& p, PreparedDataset prepared, const std::string& text) {
    ProjectEditor ed(p);
    CommandStack::MacroScope macro(p.commands(), text);
    ed.add(prepared.asset);
    const Uuid id = ed.add(std::move(prepared.dataset));
    macro.commit();
    return id;
}

Uuid importPointCloud(Project& p, const fs::path& file, const ImportOptions& o, jobs::Progress& progress) {
    return commitDataset(p, prepareImport(p, file, o, progress), "Import " + file.filename().string());
}

const PointCloudDataset& dataset(const Project& p, const Uuid& id) {
    const auto* d = p.model().pointClouds.find(id);
    if (!d) throw PointCloudError("Point-cloud dataset does not exist");
    return *d;
}

OctreeReader openDataset(const Project& p, const PointCloudDataset& d) {
    const auto* a = p.model().assets.find(d.asset.id);
    if (!a) throw PointCloudError("The point-cloud asset of '" + d.name + "' is missing from the project");
    return OctreeReader(p.assets().pathOf(*a));
}

PointBuffer datasetPoints(const Project& p, const Uuid& id, bool projectCoordinates, const std::optional<BoundingBox>& box,
                          jobs::Progress& progress) {
    const auto& d = dataset(p, id);
    const OctreeReader r = openDataset(p, d);
    std::optional<BoundingBox> storedBox;
    if (box) {
        // Conservative stored-coordinate box for node selection, exact test afterwards.
        const auto inv = d.transform.inverse();
        if (!inv) throw PointCloudError("Dataset transform is not invertible");
        BoundingBox sb = BoundingBox::empty();
        for (int k = 0; k < 8; ++k) sb.expand(inv->apply({(k & 1) ? box->max.x : box->min.x, (k & 2) ? box->max.y : box->min.y, (k & 4) ? box->max.z : box->min.z}));
        storedBox = sb;
    }
    PointBuffer b = r.readAll(storedBox, progress);
    if (projectCoordinates || box) {
        if (!d.transform.isIdentity()) b.transform(d.transform);
        if (box) {
            std::vector<char> keep(b.size());
            for (std::size_t i = 0; i < b.size(); ++i) keep[i] = box->contains(b.xyz[i]) ? 1 : 0;
            b = b.filtered(keep);
        }
        if (!projectCoordinates) b.transform(*d.transform.inverse());
    }
    return b;
}

namespace {
/// Owns the reader its source refers to.
class DatasetSource : public PointSource {
public:
    DatasetSource(OctreeReader r) : r_(std::move(r)), inner_(r_.source()) {}
    PointSchema schema() const override { return inner_->schema(); }
    std::string formatName() const override { return inner_->formatName(); }
    std::optional<std::int64_t> countHint() const override { return inner_->countHint(); }
    std::optional<BoundingBox> boundsHint() const override { return inner_->boundsHint(); }
    std::size_t read(PointBuffer& out, std::size_t max) override { return inner_->read(out, max); }
    void rewind() override { inner_->rewind(); }
    double progress() const override { return inner_->progress(); }

private:
    OctreeReader r_;
    std::unique_ptr<PointSource> inner_;
};
} // namespace

std::unique_ptr<PointSource> datasetSource(const Project& p, const Uuid& id) {
    const auto& d = dataset(p, id);
    return std::make_unique<TransformSource>(std::make_unique<DatasetSource>(openDataset(p, d)), d.transform);
}

PreparedDataset prepareDerived(Project& p, const Uuid& parentId, const PointBuffer& points, const std::string& name,
                               const std::string& derivation, jobs::Progress& progress) {
    const auto& parent = dataset(p, parentId);
    BufferSource src(points);
    auto out = prepareFromSource(p, src, name, OctreeBuildOptions{}, progress);
    out.dataset.parentDatasetId = parentId;
    out.dataset.derivation = derivation;
    out.dataset.sourceFile = parent.sourceFile;
    out.dataset.sourceFormat = parent.sourceFormat;
    out.dataset.sourceCrs = parent.sourceCrs;
    out.dataset.scanDate = parent.scanDate;
    out.dataset.scanner = parent.scanner;
    out.dataset.developmentId = parent.developmentId;
    // Points are in project coordinates: identity transform.
    return out;
}

void exportDataset(const Project& p, const Uuid& id, const fs::path& file, jobs::Progress& progress) {
    FileWriter w;
    {
        std::lock_guard lk(registry().m);
        const auto it = registry().writers.find(lowerExt(file));
        if (it == registry().writers.end()) throw PointCloudError("Unsupported export format: " + file.extension().string());
        w = it->second;
    }
    auto src = datasetSource(p, id);
    w(*src, file, progress);
}

} // namespace openmine::survey
