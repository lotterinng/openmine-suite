#pragma once
// SPDX-License-Identifier: Apache-2.0
// Regular block models: geometry (optionally rotated about Z), attribute storage in the asset
// store, domain coding against closed wireframes and surfaces, grade–tonnage reporting and CSV
// exchange.
//
// Storage: the entity (BlockModel) holds the grid definition and the ordered attribute names; the
// values are an immutable asset of little-endian float64 arrays, one per attribute, i-fastest
// (index = i + nx·(j + ny·k)). NaN means "no value". Every save writes a new asset and updates the
// entity in one undo step, so undo restores the previous values.

#include "openmine/jobs/Job.h"
#include "openmine/project/Project.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openmine::geology {

class BlockGrid {
public:
    explicit BlockGrid(const BlockModel& m);
    const BlockModel& model() const { return m_; }
    std::size_t count() const { return static_cast<std::size_t>(m_.nx) * m_.ny * m_.nz; }
    std::size_t index(int i, int j, int k) const { return static_cast<std::size_t>(i) + static_cast<std::size_t>(m_.nx) * (j + static_cast<std::size_t>(m_.ny) * k); }
    std::array<int, 3> ijk(std::size_t index) const;
    /// Local (unrotated, relative to the origin) ↔ world coordinates.
    Vec3 toWorld(Vec3 local) const;
    Vec3 toLocal(Vec3 world) const;
    Vec3 centroid(int i, int j, int k) const;
    Vec3 centroid(std::size_t index) const;
    std::optional<std::array<int, 3>> cellOf(Vec3 world) const;
    double blockVolume() const { return m_.blockSize.x * m_.blockSize.y * m_.blockSize.z; }
    Vec3 axisU() const { return u_; }
    Vec3 axisV() const { return v_; }
    /// The eight world corners of the model extent.
    std::array<Vec3, 8> corners() const;

private:
    BlockModel m_;
    Vec3 u_, v_;
};

struct BlockModelData {
    BlockModel header;
    std::map<std::string, std::vector<double>> values;
    /// Existing attribute, or a new NaN-filled one (appended to header.attributes).
    std::vector<double>& attribute(const std::string& name);
    const std::vector<double>* find(const std::string& name) const;
    void remove(const std::string& name);
    std::size_t count() const { return static_cast<std::size_t>(header.nx) * header.ny * header.nz; }
};

/// Grid that covers a box (world, axis-aligned in the rotated frame) with whole blocks; the
/// origin is snapped to multiples of the block size relative to `snap` (e.g. a mine grid origin).
BlockModel gridCovering(const std::vector<Vec3>& points, Vec3 blockSize, double rotationDeg = 0, Vec3 snap = {}, double padding = 0);

Uuid createBlockModel(Project& project, BlockModel header);
BlockModelData loadBlockModel(const Project& project, const Uuid& id);
/// Writes the values as a new asset and updates the entity (one undo step).
void saveBlockModel(Project& project, const BlockModelData& data, const std::string& text = "Update block model");

// ---- domains ----------------------------------------------------------------------------------
/// Fraction of each block inside a closed, outward-oriented mesh, by sub×sub×sub point sampling
/// (vertical ray parity per sub-column, so cost is one ray set per column).
std::vector<double> fractionInside(const BlockGrid& grid, const MeshData& closedMesh, int sub = 4, jobs::Progress& progress = jobs::nullProgress());
/// Fraction of each block below a surface (topography or a contact): the first surface crossing
/// on a ray down each sub-column from above the model.
std::vector<double> fractionBelow(const BlockGrid& grid, const MeshData& surface, int sub = 4, jobs::Progress& progress = jobs::nullProgress());
/// Writes `codeValue` into `codeField` for blocks whose centroid is inside the mesh (others keep
/// their value) and returns the number coded.
std::size_t codeBlocksInside(BlockModelData& data, const MeshData& closedMesh, const std::string& codeField, double codeValue);

// ---- reporting --------------------------------------------------------------------------------
struct GradeTonnageOptions {
    std::string gradeField;                 ///< cut-off is applied to this field
    std::vector<std::string> otherFields;   ///< reported (tonnage-weighted) at each cut-off
    std::string densityField;               ///< empty = defaultDensity
    double defaultDensity = 2.7;            ///< t/m³
    std::string proportionField;            ///< 0…1 block proportion in the domain (empty = 1)
    std::string filterField;                ///< blocks with filterField == filterValue only
    std::optional<double> filterValue;
    std::vector<double> cutoffs{0};
};
struct GradeTonnageRow {
    double cutoff = 0;
    std::size_t blocks = 0;
    double volume = 0, tonnes = 0;
    std::map<std::string, double> grades; ///< tonnage-weighted means
    std::map<std::string, double> metal;  ///< tonnes × grade
};
std::vector<GradeTonnageRow> gradeTonnage(const BlockModelData& data, const GradeTonnageOptions& options);
std::string gradeTonnageCsv(const std::vector<GradeTonnageRow>& rows);

// ---- CSV exchange -------------------------------------------------------------------------------
/// X,Y,Z (centroids), I,J,K and attributes; blocks whose attributes are all NaN are skipped when
/// `skipEmpty`.
std::string exportBlockModelCsv(const BlockModelData& data, bool skipEmpty = true);
struct BlockCsvImport {
    BlockModelData data;
    std::size_t rows = 0, placed = 0;
    std::vector<std::string> issues;
};
/// Places CSV rows (centroid columns X/Y/Z or XC/YC/ZC) into the given grid. A row whose
/// centroid is off the block centre by more than 1% of the block size, or outside the grid, is
/// rejected and reported; duplicate cells are reported and the first value kept.
BlockCsvImport importBlockModelCsv(const std::string& text, const BlockModel& grid);
/// Infers an unrotated grid from centroid columns and the given block size (or the smallest
/// spacing of distinct coordinates per axis when blockSize is zero).
BlockModel inferGridFromCsv(const std::string& text, Vec3 blockSize = {});

} // namespace openmine::geology
