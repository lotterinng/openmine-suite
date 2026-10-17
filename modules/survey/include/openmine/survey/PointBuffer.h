#pragma once
// SPDX-License-Identifier: Apache-2.0
// In-memory point storage (structure of arrays) and the streaming source/sink interfaces used by
// every reader, writer and processing step. Positions are double precision in the coordinates
// of the source (stored coordinates for datasets, project coordinates after a transform).

#include "openmine/geometry/Transform.h"
#include "openmine/jobs/Job.h"
#include "openmine/objects/SurveyEntities.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openmine::survey {

class PointCloudError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Which attributes a buffer / dataset carries (same structure as the persisted PointAttributes).
using PointSchema = PointAttributes;

struct Rgb16 {
    std::uint16_t r = 0, g = 0, b = 0;
    friend bool operator==(const Rgb16&, const Rgb16&) = default;
};

class PointBuffer {
public:
    PointBuffer() = default;
    explicit PointBuffer(PointSchema schema) { setSchema(std::move(schema)); }

    const PointSchema& schema() const { return schema_; }
    /// Changes the schema: adds empty/zero columns or drops removed ones.
    void setSchema(PointSchema s);

    std::size_t size() const { return xyz.size(); }
    bool empty() const { return xyz.empty(); }
    void reserve(std::size_t n);
    void resize(std::size_t n);
    void clear();

    /// Appends point `i` of `other` (schemas may differ: missing attributes are zero).
    void appendFrom(const PointBuffer& other, std::size_t i);
    /// Appends all points of `other`.
    void append(const PointBuffer& other);
    /// New buffer with the points at `indices` (in order).
    PointBuffer subset(const std::vector<std::size_t>& indices) const;
    /// New buffer with the points where `keep[i]` is non-zero.
    PointBuffer filtered(const std::vector<char>& keep) const;

    BoundingBox bounds() const;
    /// Applies a transform to positions (and rotates normals).
    void transform(const Transform3& t);

    /// Index of a scalar field (adds it if `create`), -1 if absent.
    int scalarIndex(const std::string& name, bool create = false);
    int scalarIndex(const std::string& name) const;

    // Columns (empty when the attribute is not in the schema).
    std::vector<Vec3> xyz;
    std::vector<std::uint16_t> intensity;
    std::vector<Rgb16> rgb;
    std::vector<std::uint8_t> classification;
    std::vector<std::array<float, 3>> normal;
    std::vector<std::uint8_t> returnNumber, numberOfReturns;
    std::vector<double> gpsTime;
    std::vector<std::vector<float>> scalars; ///< parallel to schema().scalarFields

private:
    PointSchema schema_;
};

/// Streaming point reader. `read` appends up to `max` points to `out` (whose schema it sets on
/// the first call) and returns how many were read; 0 means the end.
class PointSource {
public:
    virtual ~PointSource() = default;
    virtual PointSchema schema() const = 0;
    virtual std::string formatName() const = 0;
    virtual std::optional<std::int64_t> countHint() const { return std::nullopt; }
    virtual std::optional<BoundingBox> boundsHint() const { return std::nullopt; }
    /// CRS declared by the file (WKT or EPSG), empty when none.
    virtual std::string declaredCrs() const { return {}; }
    virtual std::size_t read(PointBuffer& out, std::size_t max) = 0;
    /// Restarts from the first point (sources must support at least one rewind).
    virtual void rewind() = 0;
    /// Fraction of the input consumed (for progress), if known.
    virtual double progress() const { return 0.0; }
};

/// A source over an in-memory buffer.
class BufferSource : public PointSource {
public:
    explicit BufferSource(const PointBuffer& b, std::string format = "memory") : b_(b), format_(std::move(format)) {}
    PointSchema schema() const override { return b_.schema(); }
    std::string formatName() const override { return format_; }
    std::optional<std::int64_t> countHint() const override { return static_cast<std::int64_t>(b_.size()); }
    std::optional<BoundingBox> boundsHint() const override { return b_.bounds(); }
    std::size_t read(PointBuffer& out, std::size_t max) override;
    void rewind() override { pos_ = 0; }
    double progress() const override { return b_.empty() ? 1.0 : static_cast<double>(pos_) / static_cast<double>(b_.size()); }

private:
    const PointBuffer& b_;
    std::string format_;
    std::size_t pos_ = 0;
};

/// Streaming point writer.
class PointSink {
public:
    virtual ~PointSink() = default;
    virtual void begin(const PointSchema& schema) = 0;
    virtual void write(const PointBuffer& points) = 0;
    virtual void finish() = 0;
};

/// Reads a whole source into memory (with progress and cancellation).
PointBuffer readAll(PointSource& src, jobs::Progress& progress = jobs::nullProgress());

} // namespace openmine::survey
