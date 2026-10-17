#pragma once
// SPDX-License-Identifier: Apache-2.0
// ASCII point formats implemented natively: delimited text (XYZ / CSV / TXT), Leica PTS and PTX.
// Binary/compressed formats (LAS, LAZ, E57, PLY) are read through integrations/pdal.

#include "openmine/survey/PointBuffer.h"

#include <filesystem>
#include <memory>

namespace openmine::survey {

/// Column roles for delimited text.
enum class TextColumn { Ignore, X, Y, Z, Intensity, Red, Green, Blue, Classification, NormalX, NormalY, NormalZ, GpsTime, Scalar };

struct TextReadOptions {
    char delimiter = 0;           ///< 0 = detect (comma, semicolon, tab, whitespace)
    int skipLines = 0;            ///< lines skipped before the header/data
    int header = -1;              ///< -1 = detect, 0 = no header, 1 = first line is a header
    /// Explicit column roles (overrides detection). Scalar columns take their name from the header
    /// or "field<N>".
    std::vector<TextColumn> columns;
    /// Intensity values are scaled to 0–65535 using this maximum (0 = detect from data: values
    /// ≤ 1 are treated as normalised, ≤ 255 as 8-bit, else 16-bit).
    double intensityMax = 0;
    double colorMax = 0;          ///< same for colours (0 = detect: ≤ 255 → 8-bit)
};

/// Opens a delimited text file (".xyz", ".csv", ".txt", ".asc"). Throws PointCloudError on
/// files whose columns cannot be identified.
std::unique_ptr<PointSource> openTextSource(const std::filesystem::path& file, const TextReadOptions& options = {});
/// Leica PTS (one or more scans, each preceded by its point count; x y z intensity [r g b]).
std::unique_ptr<PointSource> openPtsSource(const std::filesystem::path& file);
/// Leica PTX (gridded scans with scanner pose; points are transformed by each scan's matrix and
/// zero (no-return) points are skipped).
std::unique_ptr<PointSource> openPtxSource(const std::filesystem::path& file);

/// Scanner positions found in a PTX file (one per scan), in output coordinates.
std::vector<Vec3> ptxScannerPositions(const std::filesystem::path& file);

struct TextWriteOptions {
    char delimiter = ',';
    bool header = true;
    int decimals = 4;             ///< position decimals
};
/// Writes delimited text with all attributes of the schema as columns.
std::unique_ptr<PointSink> textSink(const std::filesystem::path& file, const TextWriteOptions& options = {});
/// Writes Leica PTS (x y z intensity [r g b]); intensity mapped to −2048…2047.
std::unique_ptr<PointSink> ptsSink(const std::filesystem::path& file);

} // namespace openmine::survey
