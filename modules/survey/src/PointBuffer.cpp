// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/PointBuffer.h"

#include <algorithm>
#include <cmath>

namespace openmine::survey {

void PointBuffer::setSchema(PointSchema s) {
    const std::size_t n = size();
    auto fit = [n](auto& col, bool present) {
        if (present) col.resize(n);
        else {
            col.clear();
            col.shrink_to_fit();
        }
    };
    fit(intensity, s.intensity);
    fit(rgb, s.rgb);
    fit(classification, s.classification);
    fit(normal, s.normals);
    fit(returnNumber, s.returnInfo);
    fit(numberOfReturns, s.returnInfo);
    fit(gpsTime, s.gpsTime);
    std::vector<std::vector<float>> ns;
    for (const auto& name : s.scalarFields) {
        const auto it = std::find(schema_.scalarFields.begin(), schema_.scalarFields.end(), name);
        if (it != schema_.scalarFields.end()) ns.push_back(std::move(scalars[static_cast<std::size_t>(it - schema_.scalarFields.begin())]));
        else ns.emplace_back(n, 0.0f);
    }
    scalars = std::move(ns);
    schema_ = std::move(s);
}

void PointBuffer::reserve(std::size_t n) {
    xyz.reserve(n);
    if (schema_.intensity) intensity.reserve(n);
    if (schema_.rgb) rgb.reserve(n);
    if (schema_.classification) classification.reserve(n);
    if (schema_.normals) normal.reserve(n);
    if (schema_.returnInfo) {
        returnNumber.reserve(n);
        numberOfReturns.reserve(n);
    }
    if (schema_.gpsTime) gpsTime.reserve(n);
    for (auto& s : scalars) s.reserve(n);
}

void PointBuffer::resize(std::size_t n) {
    xyz.resize(n);
    if (schema_.intensity) intensity.resize(n);
    if (schema_.rgb) rgb.resize(n);
    if (schema_.classification) classification.resize(n);
    if (schema_.normals) normal.resize(n);
    if (schema_.returnInfo) {
        returnNumber.resize(n);
        numberOfReturns.resize(n);
    }
    if (schema_.gpsTime) gpsTime.resize(n);
    for (auto& s : scalars) s.resize(n);
}

void PointBuffer::clear() { resize(0); }

void PointBuffer::appendFrom(const PointBuffer& o, std::size_t i) {
    xyz.push_back(o.xyz[i]);
    if (schema_.intensity) intensity.push_back(o.schema_.intensity ? o.intensity[i] : 0);
    if (schema_.rgb) rgb.push_back(o.schema_.rgb ? o.rgb[i] : Rgb16{});
    if (schema_.classification) classification.push_back(o.schema_.classification ? o.classification[i] : 0);
    if (schema_.normals) normal.push_back(o.schema_.normals ? o.normal[i] : std::array<float, 3>{0, 0, 0});
    if (schema_.returnInfo) {
        returnNumber.push_back(o.schema_.returnInfo ? o.returnNumber[i] : 1);
        numberOfReturns.push_back(o.schema_.returnInfo ? o.numberOfReturns[i] : 1);
    }
    if (schema_.gpsTime) gpsTime.push_back(o.schema_.gpsTime ? o.gpsTime[i] : 0.0);
    for (std::size_t k = 0; k < schema_.scalarFields.size(); ++k) {
        const int j = o.scalarIndex(schema_.scalarFields[k]);
        scalars[k].push_back(j >= 0 ? o.scalars[static_cast<std::size_t>(j)][i] : 0.0f);
    }
}

void PointBuffer::append(const PointBuffer& o) {
    if (o.schema_ == schema_) {
        xyz.insert(xyz.end(), o.xyz.begin(), o.xyz.end());
        intensity.insert(intensity.end(), o.intensity.begin(), o.intensity.end());
        rgb.insert(rgb.end(), o.rgb.begin(), o.rgb.end());
        classification.insert(classification.end(), o.classification.begin(), o.classification.end());
        normal.insert(normal.end(), o.normal.begin(), o.normal.end());
        returnNumber.insert(returnNumber.end(), o.returnNumber.begin(), o.returnNumber.end());
        numberOfReturns.insert(numberOfReturns.end(), o.numberOfReturns.begin(), o.numberOfReturns.end());
        gpsTime.insert(gpsTime.end(), o.gpsTime.begin(), o.gpsTime.end());
        for (std::size_t k = 0; k < scalars.size(); ++k) scalars[k].insert(scalars[k].end(), o.scalars[k].begin(), o.scalars[k].end());
        return;
    }
    reserve(size() + o.size());
    for (std::size_t i = 0; i < o.size(); ++i) appendFrom(o, i);
}

PointBuffer PointBuffer::subset(const std::vector<std::size_t>& idx) const {
    PointBuffer out(schema_);
    out.reserve(idx.size());
    for (auto i : idx) out.appendFrom(*this, i);
    return out;
}

PointBuffer PointBuffer::filtered(const std::vector<char>& keep) const {
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < size() && i < keep.size(); ++i)
        if (keep[i]) idx.push_back(i);
    return subset(idx);
}

BoundingBox PointBuffer::bounds() const {
    BoundingBox b = BoundingBox::empty();
    for (const auto& p : xyz) b.expand(p);
    return b;
}

void PointBuffer::transform(const Transform3& t) {
    for (auto& p : xyz) p = t.apply(p);
    if (schema_.normals)
        for (auto& n : normal) {
            const Vec3 v = t.applyNormal({n[0], n[1], n[2]});
            n = {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
        }
}

int PointBuffer::scalarIndex(const std::string& name, bool create) {
    const auto it = std::find(schema_.scalarFields.begin(), schema_.scalarFields.end(), name);
    if (it != schema_.scalarFields.end()) return static_cast<int>(it - schema_.scalarFields.begin());
    if (!create) return -1;
    schema_.scalarFields.push_back(name);
    scalars.emplace_back(size(), 0.0f);
    return static_cast<int>(scalars.size() - 1);
}

int PointBuffer::scalarIndex(const std::string& name) const {
    const auto it = std::find(schema_.scalarFields.begin(), schema_.scalarFields.end(), name);
    return it == schema_.scalarFields.end() ? -1 : static_cast<int>(it - schema_.scalarFields.begin());
}

std::size_t BufferSource::read(PointBuffer& out, std::size_t max) {
    if (out.empty() && !(out.schema() == b_.schema())) out.setSchema(b_.schema());
    const std::size_t n = std::min(max, b_.size() - pos_);
    out.reserve(out.size() + n);
    for (std::size_t i = 0; i < n; ++i) out.appendFrom(b_, pos_ + i);
    pos_ += n;
    return n;
}

PointBuffer readAll(PointSource& src, jobs::Progress& progress) {
    PointBuffer all(src.schema());
    if (auto c = src.countHint(); c && *c > 0) all.reserve(static_cast<std::size_t>(*c));
    PointBuffer chunk(src.schema());
    while (true) {
        chunk.clear();
        const std::size_t n = src.read(chunk, 1 << 18);
        if (n == 0) break;
        all.append(chunk);
        progress.update(src.progress(), "Reading points");
    }
    return all;
}

} // namespace openmine::survey
