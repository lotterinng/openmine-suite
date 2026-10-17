#pragma once
// SPDX-License-Identifier: Apache-2.0
// PDAL (BSD) adapter: LAS/LAZ/E57/PLY reading and writing in streaming mode, and the PDAL
// filters OpenMine uses (SMRF ground classification, Poisson surface reconstruction). No PDAL
// type appears in this header.

#include "openmine/objects/CadEntities.h"
#include "openmine/survey/PointBuffer.h"

#include <filesystem>
#include <memory>

namespace openmine::pdalio {

std::string pdalVersion();

/// Registers the PDAL readers and writers (.las .laz .e57 .ply) with the survey format registry.
void registerFormats();

/// True for extensions read through PDAL (.las, .laz, .e57, .ply).
bool canRead(const std::filesystem::path& file);
bool canWrite(const std::filesystem::path& file);

/// Streaming source. Reading runs PDAL on a worker thread feeding a bounded queue, so memory use
/// is independent of the file size. Standard dimensions map to the point schema; any other
/// numeric dimension becomes a float scalar field.
std::unique_ptr<survey::PointSource> openPdalSource(const std::filesystem::path& file);

struct PdalWriteOptions {
    double scale = 0.0001;             ///< LAS coordinate resolution (m); LAS stores scaled integers
    std::optional<Vec3> offset;        ///< LAS offset (default: floor of the minimum, from the source bounds)
    int lasMinorVersion = 4;
    std::string crsWkt;                ///< written as the file's spatial reference when not empty
};
/// Streams `source` into a LAS/LAZ/E57/PLY file (format from the extension).
void writePdal(survey::PointSource& source, const std::filesystem::path& file, const PdalWriteOptions& options = {},
               jobs::Progress& progress = jobs::nullProgress());

struct SmrfOptions {
    double cell = 1.0;       ///< m
    double slope = 0.15;     ///< rise/run
    double window = 18.0;    ///< m, maximum window size
    double threshold = 0.5;  ///< m, elevation threshold
    double scalar = 1.25;    ///< elevation scaling factor
};
/// Simple Morphological Filter (Pingel et al. 2013) ground classification via PDAL.
/// Returns ASPRS classes per point: 2 = ground, 1 = unclassified (non-ground).
std::vector<std::uint8_t> smrfGround(const survey::PointBuffer& points, const SmrfOptions& options = {});

struct PoissonOptions {
    int depth = 8;           ///< octree depth of the reconstruction
    bool density = false;
};
/// Poisson surface reconstruction (Kazhdan & Hoppe) via PDAL's filters.poisson. Requires
/// normals in the buffer (estimate them first). Returns a triangle mesh.
MeshData poissonSurface(const survey::PointBuffer& points, const PoissonOptions& options = {});

} // namespace openmine::pdalio
