#pragma once
// SPDX-License-Identifier: Apache-2.0
// Triangle-mesh exchange formats implemented natively: Wavefront OBJ, STL (ASCII and binary) and
// PLY meshes (ASCII, binary little/big endian). Polygons are triangulated as fans (convex) or by
// ear clipping in their best-fit plane (concave). Coordinates are written with full precision.

#include "openmine/objects/CadEntities.h"
#include "openmine/project/Project.h"

#include <filesystem>
#include <string>

namespace openmine::formats {

class MeshFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct MeshFile {
    MeshData mesh;
    std::string name;      ///< object/solid name when the file has one
    std::size_t polygonsTriangulated = 0;
    std::vector<std::string> warnings;
};

/// Reads .obj, .stl or .ply (by extension; STL ASCII/binary detected from content).
MeshFile readMesh(const std::filesystem::path& file);

enum class StlEncoding { Binary, Ascii };
void writeObj(const std::filesystem::path& file, const MeshData& mesh, const std::string& name = {});
/// Binary STL stores single-precision floats; returns a warning when that loses more than 0.1 mm
/// (e.g. mine-grid coordinates in the millions), in which case OBJ, PLY or ASCII STL should be used.
std::vector<std::string> writeStl(const std::filesystem::path& file, const MeshData& mesh, StlEncoding encoding = StlEncoding::Binary,
                                  const std::string& name = {});
void writePly(const std::filesystem::path& file, const MeshData& mesh, bool binary = true);
/// Writes by extension (.obj, .stl binary, .ply binary).
std::vector<std::string> writeMesh(const std::filesystem::path& file, const MeshData& mesh, const std::string& name = {});

/// Adds a read mesh as a mesh object on `layerId`, named after the file's object name (else `name`),
/// with a "source_file" attribute (one undo step).
Uuid addMeshObject(Project& project, const MeshFile& file, const std::string& name, const Uuid& layerId, const std::string& sourceFile);

} // namespace openmine::formats
