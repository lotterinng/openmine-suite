#pragma once
// SPDX-License-Identifier: Apache-2.0
// Open Mining Format (OMF) v1 reader/writer — the GMG open exchange format (file version
// "OMF-v0.9.0", as written by the reference Python library omf 1.0.x). Implemented from the
// published format: a 60-byte header (magic 0x84838281, version string, project UID, JSON offset),
// zlib-compressed little-endian int64/float64 arrays, then a UTF-8 JSON registry of all objects
// keyed by UID.
//
// Supported: PointSet, LineSet, Surface (triangulated and grid), Volume (tensor grid) elements;
// ScalarData, MappedData (string/colour/scalar legends), StringData, ColorData, Vector3Data,
// Vector2Data and DateTimeData. Image textures are read as notes only (not imported).
// Data order on grids follows OMF: u fastest, then v, then w.

#include "openmine/objects/Entities.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace openmine::omf {

class OmfError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Legend {
    std::string name, description;
    enum class Kind { String, Color, Scalar, DateTime } kind = Kind::String;
    std::vector<std::string> strings;            ///< String and DateTime
    std::vector<std::array<int, 3>> colors;
    std::vector<double> scalars;
};

struct Data {
    enum class Kind { Scalar, Mapped, String, Color, Vector2, Vector3, DateTime } kind = Kind::Scalar;
    std::string name, description;
    std::string location;                         ///< "vertices", "segments", "faces", "cells"
    std::vector<double> scalars;                  ///< Scalar
    std::vector<std::int64_t> indices;            ///< Mapped (−1 = no data)
    std::vector<Legend> legends;                  ///< Mapped
    std::vector<std::string> strings;             ///< String, DateTime
    std::vector<std::array<int, 3>> colors;       ///< Color
    std::vector<Vec3> vectors;                    ///< Vector3 (z = 0 for Vector2)
    std::size_t size() const;
};

struct Element {
    enum class Kind { PointSet, LineSet, Surface, SurfaceGrid, Volume } kind = Kind::PointSet;
    std::string name, description, subtype;
    std::array<int, 3> color{128, 128, 128};
    Vec3 origin;                                  ///< geometry origin (relative to the project origin)
    std::vector<Vec3> vertices;                   ///< PointSet, LineSet, Surface (relative to origin)
    std::vector<std::array<std::int64_t, 2>> segments;
    std::vector<std::array<std::int64_t, 3>> triangles;
    std::vector<double> tensorU, tensorV, tensorW; ///< SurfaceGrid (u, v), Volume (u, v, w)
    Vec3 axisU{1, 0, 0}, axisV{0, 1, 0}, axisW{0, 0, 1};
    std::vector<double> offsetW;                  ///< SurfaceGrid (optional): per-vertex offsets along axisU×axisV
    std::vector<Data> data;
    std::vector<std::string> notes;               ///< unsupported content found while reading
};

struct Document {
    std::string name, description, author, revision, units = "m";
    Vec3 origin;
    std::vector<Element> elements;
    std::string uid;                              ///< read: file UID; write: generated when empty
};

Document readOmf(const std::filesystem::path& file);
Document parseOmf(const std::string& bytes);
void writeOmf(const Document& doc, const std::filesystem::path& file);
std::string serializeOmf(const Document& doc);

/// Throws OmfError if counts are inconsistent (data length vs location, indices in range).
void validate(const Document& doc);

/// Number of locations of the given kind for an element ("vertices", "segments", "faces", "cells").
std::size_t locationCount(const Element& e, const std::string& location);

} // namespace openmine::omf
