// SPDX-License-Identifier: Apache-2.0
#include "openmine/pdalio/Pdal.h"
#include "openmine/survey/PointCloudProject.h"

#include <pdal/Options.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/PointView.hpp>
#include <pdal/QuickInfo.hpp>
#include <pdal/Reader.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/Streamable.hpp>
#include <pdal/io/BufferReader.hpp>
#include <pdal/pdal_config.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace openmine::pdalio {

using survey::PointBuffer;
using survey::PointCloudError;
using survey::PointSchema;
namespace Dim = pdal::Dimension;

namespace {

std::string lowerExt(const std::filesystem::path& f) {
    std::string e = f.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string driver(const std::filesystem::path& f, bool reading) {
    const auto e = lowerExt(f);
    const std::string prefix = reading ? "readers." : "writers.";
    if (e == ".las" || e == ".laz") return prefix + "las";
    if (e == ".e57") return prefix + "e57";
    if (e == ".ply") return prefix + "ply";
    throw PointCloudError("No PDAL driver for " + e);
}

std::string fileFormatName(const std::filesystem::path& f) {
    auto e = lowerExt(f).substr(1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return e;
}

const std::set<std::string>& standardDims() {
    static const std::set<std::string> s = {"X", "Y", "Z", "Intensity", "Red", "Green", "Blue", "Classification", "ReturnNumber",
                                            "NumberOfReturns", "GpsTime", "NormalX", "NormalY", "NormalZ"};
    return s;
}

/// How each layout dimension maps to the point buffer.
struct Mapping {
    struct Field {
        Dim::Id id;
        std::string name;
        Dim::Type type;
        int scalar = -1;
    };
    std::vector<Field> fields;
    PointSchema schema;

    void build(const pdal::PointLayout& layout) {
        fields.clear();
        schema = {};
        for (auto id : layout.dims()) {
            const std::string name = layout.dimName(id);
            Field f{id, name, layout.dimType(id), -1};
            if (name == "Intensity") schema.intensity = true;
            else if (name == "Red" || name == "Green" || name == "Blue") schema.rgb = true;
            else if (name == "Classification") schema.classification = true;
            else if (name == "ReturnNumber" || name == "NumberOfReturns") schema.returnInfo = true;
            else if (name == "GpsTime") schema.gpsTime = true;
            else if (name == "NormalX" || name == "NormalY" || name == "NormalZ") schema.normals = true;
            else if (!standardDims().count(name)) {
                f.scalar = static_cast<int>(schema.scalarFields.size());
                schema.scalarFields.push_back(name);
            }
            fields.push_back(f);
        }
    }

    void extract(pdal::PointRef& p, PointBuffer& out) const {
        Vec3 v;
        std::uint16_t inten = 0;
        survey::Rgb16 c;
        std::uint8_t cls = 0, rn = 1, nr = 1;
        double t = 0;
        std::array<float, 3> n{0, 0, 0};
        auto col = [&](const Field& f) {
            const double d = p.getFieldAs<double>(f.id);
            return Dim::size(f.type) == 1 ? d * 257.0 : d; // 8-bit colours → 16-bit
        };
        for (const auto& f : fields) {
            const auto& nm = f.name;
            if (f.id == Dim::Id::X) v.x = p.getFieldAs<double>(f.id);
            else if (f.id == Dim::Id::Y) v.y = p.getFieldAs<double>(f.id);
            else if (f.id == Dim::Id::Z) v.z = p.getFieldAs<double>(f.id);
            else if (nm == "Intensity") {
                double d = p.getFieldAs<double>(f.id);
                if (Dim::base(f.type) == Dim::BaseType::Floating && d <= 1.0) d *= 65535.0;
                inten = static_cast<std::uint16_t>(std::clamp(d, 0.0, 65535.0));
            } else if (nm == "Red") c.r = static_cast<std::uint16_t>(std::clamp(col(f), 0.0, 65535.0));
            else if (nm == "Green") c.g = static_cast<std::uint16_t>(std::clamp(col(f), 0.0, 65535.0));
            else if (nm == "Blue") c.b = static_cast<std::uint16_t>(std::clamp(col(f), 0.0, 65535.0));
            else if (nm == "Classification") cls = p.getFieldAs<std::uint8_t>(f.id);
            else if (nm == "ReturnNumber") rn = p.getFieldAs<std::uint8_t>(f.id);
            else if (nm == "NumberOfReturns") nr = p.getFieldAs<std::uint8_t>(f.id);
            else if (nm == "GpsTime") t = p.getFieldAs<double>(f.id);
            else if (nm == "NormalX") n[0] = p.getFieldAs<float>(f.id);
            else if (nm == "NormalY") n[1] = p.getFieldAs<float>(f.id);
            else if (nm == "NormalZ") n[2] = p.getFieldAs<float>(f.id);
            else if (f.scalar >= 0) out.scalars[static_cast<std::size_t>(f.scalar)].push_back(p.getFieldAs<float>(f.id));
        }
        out.xyz.push_back(v);
        if (schema.intensity) out.intensity.push_back(inten);
        if (schema.rgb) out.rgb.push_back(c);
        if (schema.classification) out.classification.push_back(cls);
        if (schema.normals) out.normal.push_back(n);
        if (schema.returnInfo) {
            out.returnNumber.push_back(rn);
            out.numberOfReturns.push_back(nr);
        }
        if (schema.gpsTime) out.gpsTime.push_back(t);
    }
};

struct Aborted {};

/// Stream table that hands every full batch to a callback before it is overwritten.
class BatchTable : public pdal::FixedPointTable {
public:
    BatchTable(pdal::point_count_t cap, std::function<void(BatchTable&)> cb) : FixedPointTable(cap), cb_(std::move(cb)) {}

protected:
    void reset() override {
        cb_(*this);
        FixedPointTable::reset();
    }

private:
    std::function<void(BatchTable&)> cb_;
};

class PdalSource : public survey::PointSource {
public:
    explicit PdalSource(std::filesystem::path f) : file_(std::move(f)), driver_(driver(file_, true)) {
        if (!std::filesystem::exists(file_)) throw PointCloudError("File not found: " + file_.string());
        try {
            pdal::StageFactory factory;
            pdal::Stage* r = factory.createStage(driver_);
            if (!r) throw PointCloudError("PDAL driver " + driver_ + " is not available");
            pdal::Options o;
            o.add("filename", file_.string());
            r->setOptions(o);
            const pdal::QuickInfo qi = r->preview();
            if (qi.valid()) {
                count_ = static_cast<std::int64_t>(qi.m_pointCount);
                if (qi.m_bounds.valid())
                    bounds_ = BoundingBox{{qi.m_bounds.minx, qi.m_bounds.miny, qi.m_bounds.minz}, {qi.m_bounds.maxx, qi.m_bounds.maxy, qi.m_bounds.maxz}};
                if (!qi.m_srs.empty()) crs_ = qi.m_srs.getWKT();
            }
            pdal::FixedPointTable t(16);
            r->prepare(t);
            mapping_.build(*t.layout());
        } catch (const pdal::pdal_error& e) {
            throw PointCloudError("Cannot open " + file_.filename().string() + ": " + e.what());
        }
    }
    ~PdalSource() override { stop(); }

    PointSchema schema() const override { return mapping_.schema; }
    std::string formatName() const override { return formatName_impl(); }
    std::optional<std::int64_t> countHint() const override { return count_ > 0 ? std::optional<std::int64_t>(count_) : std::nullopt; }
    std::optional<BoundingBox> boundsHint() const override { return bounds_; }
    std::string declaredCrs() const override { return crs_; }
    double progress() const override { return count_ > 0 ? std::min(1.0, static_cast<double>(delivered_) / static_cast<double>(count_)) : 0.0; }

    void rewind() override {
        stop();
        leftover_ = PointBuffer{};
        leftPos_ = 0;
        delivered_ = 0;
    }

    std::size_t read(PointBuffer& out, std::size_t max) override {
        if (out.empty()) out.setSchema(mapping_.schema);
        if (!started_) start();
        std::size_t n = 0;
        while (n < max) {
            if (leftPos_ < leftover_.size()) {
                const std::size_t k = std::min(max - n, leftover_.size() - leftPos_);
                for (std::size_t i = 0; i < k; ++i) out.appendFrom(leftover_, leftPos_ + i);
                leftPos_ += k;
                n += k;
                continue;
            }
            std::unique_lock lk(m_);
            cv_.wait(lk, [&] { return !queue_.empty() || finished_; });
            if (queue_.empty()) {
                if (!error_.empty()) throw PointCloudError("Reading " + file_.filename().string() + " failed: " + error_);
                break;
            }
            leftover_ = std::move(queue_.front());
            queue_.pop_front();
            leftPos_ = 0;
            cv_.notify_all();
        }
        delivered_ += static_cast<std::int64_t>(n);
        return n;
    }

private:
    std::filesystem::path file_;
    std::string driver_;
    Mapping mapping_;
    std::int64_t count_ = 0, delivered_ = 0;
    std::optional<BoundingBox> bounds_;
    std::string crs_;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<PointBuffer> queue_;
    bool started_ = false, finished_ = false, abort_ = false;
    std::string error_;
    PointBuffer leftover_;
    std::size_t leftPos_ = 0;

    std::string formatName_impl() const { return fileFormatName(file_); }

    void start() {
        started_ = true;
        finished_ = false;
        abort_ = false;
        error_.clear();
        worker_ = std::thread([this] {
            try {
                pdal::StageFactory factory;
                pdal::Stage* r = factory.createStage(driver_);
                pdal::Options o;
                o.add("filename", file_.string());
                r->setOptions(o);
                BatchTable table(65536, [this](BatchTable& t) {
                    PointBuffer b(mapping_.schema);
                    b.reserve(t.numPoints());
                    for (pdal::PointId i = 0; i < t.numPoints(); ++i) {
                        if (t.skip(i)) continue;
                        pdal::PointRef p(t, i);
                        mapping_.extract(p, b);
                    }
                    std::unique_lock lk(m_);
                    cv_.wait(lk, [&] { return queue_.size() < 4 || abort_; });
                    if (abort_) throw Aborted{};
                    queue_.push_back(std::move(b));
                    cv_.notify_all();
                });
                r->prepare(table);
                r->execute(table);
            } catch (const Aborted&) {
            } catch (const std::exception& e) {
                std::lock_guard lk(m_);
                error_ = e.what();
            }
            std::lock_guard lk(m_);
            finished_ = true;
            cv_.notify_all();
        });
    }

    void stop() {
        if (!started_) return;
        {
            std::lock_guard lk(m_);
            abort_ = true;
            cv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        queue_.clear();
        started_ = false;
        finished_ = false;
    }
};

/// PDAL reader stage that pulls points from an OpenMine PointSource (streaming).
class SourceStage : public pdal::Reader, public pdal::Streamable {
public:
    SourceStage(survey::PointSource& s, jobs::Progress& p) : src_(s), progress_(p) {}
    std::string getName() const override { return "readers.openmine"; }

private:
    survey::PointSource& src_;
    jobs::Progress& progress_;
    PointBuffer buf_;
    std::size_t pos_ = 0;
    std::vector<Dim::Id> scalarIds_;

    void addDimensions(pdal::PointLayoutPtr layout) override {
        const auto s = src_.schema();
        layout->registerDims({Dim::Id::X, Dim::Id::Y, Dim::Id::Z});
        if (s.intensity) layout->registerDim(Dim::Id::Intensity);
        if (s.rgb) layout->registerDims({Dim::Id::Red, Dim::Id::Green, Dim::Id::Blue});
        if (s.classification) layout->registerDim(Dim::Id::Classification);
        if (s.returnInfo) layout->registerDims({Dim::Id::ReturnNumber, Dim::Id::NumberOfReturns});
        if (s.gpsTime) layout->registerDim(Dim::Id::GpsTime);
        if (s.normals) layout->registerDims({Dim::Id::NormalX, Dim::Id::NormalY, Dim::Id::NormalZ});
        scalarIds_.clear();
        for (const auto& n : s.scalarFields) scalarIds_.push_back(layout->registerOrAssignDim(n, Dim::Type::Float));
    }
    void ready(pdal::PointTableRef) override {
        src_.rewind();
        buf_ = PointBuffer(src_.schema());
        pos_ = 0;
    }
    bool processOne(pdal::PointRef& p) override {
        if (pos_ >= buf_.size()) {
            buf_.clear();
            pos_ = 0;
            if (src_.read(buf_, 65536) == 0) return false;
            progress_.update(src_.progress(), "Writing points");
        }
        const auto& s = buf_.schema();
        const std::size_t i = pos_++;
        p.setField(Dim::Id::X, buf_.xyz[i].x);
        p.setField(Dim::Id::Y, buf_.xyz[i].y);
        p.setField(Dim::Id::Z, buf_.xyz[i].z);
        if (s.intensity) p.setField(Dim::Id::Intensity, buf_.intensity[i]);
        if (s.rgb) {
            p.setField(Dim::Id::Red, buf_.rgb[i].r);
            p.setField(Dim::Id::Green, buf_.rgb[i].g);
            p.setField(Dim::Id::Blue, buf_.rgb[i].b);
        }
        if (s.classification) p.setField(Dim::Id::Classification, buf_.classification[i]);
        if (s.returnInfo) {
            p.setField(Dim::Id::ReturnNumber, buf_.returnNumber[i]);
            p.setField(Dim::Id::NumberOfReturns, buf_.numberOfReturns[i]);
        }
        if (s.gpsTime) p.setField(Dim::Id::GpsTime, buf_.gpsTime[i]);
        if (s.normals) {
            p.setField(Dim::Id::NormalX, buf_.normal[i][0]);
            p.setField(Dim::Id::NormalY, buf_.normal[i][1]);
            p.setField(Dim::Id::NormalZ, buf_.normal[i][2]);
        }
        for (std::size_t k = 0; k < scalarIds_.size(); ++k) p.setField(scalarIds_[k], buf_.scalars[k][i]);
        return true;
    }
    pdal::point_count_t read(pdal::PointViewPtr view, pdal::point_count_t count) override {
        pdal::point_count_t n = 0;
        pdal::PointId idx = view->size();
        pdal::PointRef point(*view, idx);
        while (n < count) {
            point.setPointId(idx);
            if (!processOne(point)) break;
            ++n;
            ++idx;
        }
        return n;
    }
};

void registerViewDims(pdal::PointTable& table, bool withNormals) {
    table.layout()->registerDims({Dim::Id::X, Dim::Id::Y, Dim::Id::Z});
    if (withNormals) table.layout()->registerDims({Dim::Id::NormalX, Dim::Id::NormalY, Dim::Id::NormalZ});
    table.layout()->registerDim(Dim::Id::Classification);
}

/// Fills a view after the pipeline was prepared (the layout is final by then).
pdal::PointViewPtr fillView(pdal::PointTable& table, const PointBuffer& b, bool withNormals) {
    auto view = std::make_shared<pdal::PointView>(table);
    for (std::size_t i = 0; i < b.size(); ++i) {
        const auto id = static_cast<pdal::PointId>(i);
        view->setField(Dim::Id::X, id, b.xyz[i].x);
        view->setField(Dim::Id::Y, id, b.xyz[i].y);
        view->setField(Dim::Id::Z, id, b.xyz[i].z);
        view->setField(Dim::Id::Classification, id, std::uint8_t{1});
        if (withNormals) {
            view->setField(Dim::Id::NormalX, id, b.normal[i][0]);
            view->setField(Dim::Id::NormalY, id, b.normal[i][1]);
            view->setField(Dim::Id::NormalZ, id, b.normal[i][2]);
        }
    }
    return view;
}

} // namespace

std::string pdalVersion() { return pdal::Config::fullVersionString(); }

void registerFormats() {
    for (const char* e : {".las", ".laz", ".e57", ".ply"}) {
        survey::registerReader(e, [](const std::filesystem::path& f) { return openPdalSource(f); });
        survey::registerWriter(e, [](survey::PointSource& s, const std::filesystem::path& f, jobs::Progress& p) { writePdal(s, f, {}, p); });
    }
}

bool canRead(const std::filesystem::path& f) {
    const auto e = lowerExt(f);
    return e == ".las" || e == ".laz" || e == ".e57" || e == ".ply";
}
bool canWrite(const std::filesystem::path& f) { return canRead(f); }

std::unique_ptr<survey::PointSource> openPdalSource(const std::filesystem::path& file) { return std::make_unique<PdalSource>(file); }

void writePdal(survey::PointSource& source, const std::filesystem::path& file, const PdalWriteOptions& opt, jobs::Progress& progress) {
    try {
        pdal::StageFactory factory;
        SourceStage reader(source, progress);
        const std::string drv = driver(file, false);
        pdal::Stage* w = factory.createStage(drv);
        if (!w) throw PointCloudError("PDAL driver " + drv + " is not available");
        pdal::Options o;
        o.add("filename", file.string());
        const auto ext = lowerExt(file);
        if (ext == ".las" || ext == ".laz") {
            Vec3 off;
            if (opt.offset) off = *opt.offset;
            else if (auto b = source.boundsHint(); b && !b->isEmpty()) off = {std::floor(b->min.x), std::floor(b->min.y), std::floor(b->min.z)};
            else throw PointCloudError("LAS export needs an offset or a source with known bounds");
            for (const char* a : {"x", "y", "z"}) o.add(std::string("scale_") + a, opt.scale);
            o.add("offset_x", off.x);
            o.add("offset_y", off.y);
            o.add("offset_z", off.z);
            o.add("minor_version", opt.lasMinorVersion);
            o.add("dataformat_id", source.schema().rgb ? 7 : 6);
            o.add("extra_dims", "all");
            if (ext == ".laz") o.add("compression", true);
        }
        if (ext == ".ply") o.add("storage_mode", "little endian");
        if (ext == ".e57") o.add("double_precision", true); // never round survey coordinates to float32
        if (!opt.crsWkt.empty() && (ext == ".las" || ext == ".laz")) o.add("a_srs", opt.crsWkt);
        w->setOptions(o);
        w->setInput(reader);
        if (w->pipelineStreamable()) {
            pdal::FixedPointTable table(65536);
            w->prepare(table);
            w->execute(table);
        } else { // e.g. writers.ply: the whole cloud passes through memory
            pdal::PointTable table;
            w->prepare(table);
            w->execute(table);
        }
    } catch (const pdal::pdal_error& e) {
        throw PointCloudError("Writing " + file.filename().string() + " failed: " + e.what());
    }
}

std::vector<std::uint8_t> smrfGround(const PointBuffer& points, const SmrfOptions& so) {
    if (points.empty()) return {};
    try {
        pdal::PointTable table;
        registerViewDims(table, false);
        pdal::BufferReader reader;
        pdal::StageFactory factory;
        pdal::Stage* f = factory.createStage("filters.smrf");
        pdal::Options o;
        o.add("cell", so.cell);
        o.add("slope", so.slope);
        o.add("window", so.window);
        o.add("threshold", so.threshold);
        o.add("scalar", so.scalar);
        f->setOptions(o);
        f->setInput(reader);
        f->prepare(table);
        reader.addView(fillView(table, points, false));
        const auto views = f->execute(table);
        std::vector<std::uint8_t> cls(points.size(), 1);
        for (const auto& v : views)
            for (pdal::PointId i = 0; i < v->size(); ++i) {
                // Views keep the original point ids (the buffer reader's view is filtered in place).
                cls[i] = v->getFieldAs<std::uint8_t>(Dim::Id::Classification, i) == 2 ? 2 : 1;
            }
        return cls;
    } catch (const pdal::pdal_error& e) {
        throw PointCloudError(std::string("SMRF ground filter failed: ") + e.what());
    }
}

MeshData poissonSurface(const PointBuffer& points, const PoissonOptions& po) {
    if (!points.schema().normals) throw PointCloudError("Poisson reconstruction needs point normals: estimate normals first");
    if (points.size() < 10) throw PointCloudError("Too few points for a surface");
    try {
        pdal::PointTable table;
        registerViewDims(table, true);
        pdal::BufferReader reader;
        pdal::StageFactory factory;
        pdal::Stage* f = factory.createStage("filters.poisson");
        pdal::Options o;
        o.add("depth", po.depth);
        o.add("density", po.density);
        f->setOptions(o);
        f->setInput(reader);
        f->prepare(table);
        reader.addView(fillView(table, points, true));
        const auto views = f->execute(table);
        if (views.empty()) throw PointCloudError("Poisson reconstruction produced no output");
        const auto out = *views.begin();
        const pdal::TriangularMesh* mesh = out->mesh();
        if (!mesh || mesh->size() == 0) throw PointCloudError("Poisson reconstruction produced no triangles");
        MeshData m;
        m.vertices.reserve(out->size());
        for (pdal::PointId i = 0; i < out->size(); ++i)
            m.vertices.push_back({out->getFieldAs<double>(Dim::Id::X, i), out->getFieldAs<double>(Dim::Id::Y, i), out->getFieldAs<double>(Dim::Id::Z, i)});
        for (std::size_t t = 0; t < mesh->size(); ++t) {
            const auto& tri = (*mesh)[t];
            m.triangles.push_back({static_cast<std::uint32_t>(tri.m_a), static_cast<std::uint32_t>(tri.m_b), static_cast<std::uint32_t>(tri.m_c)});
        }
        return m;
    } catch (const pdal::pdal_error& e) {
        throw PointCloudError(std::string("Poisson reconstruction failed: ") + e.what());
    }
}

} // namespace openmine::pdalio
