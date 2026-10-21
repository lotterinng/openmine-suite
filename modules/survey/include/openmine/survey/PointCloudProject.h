#pragma once
// SPDX-License-Identifier: Apache-2.0
// Point-cloud datasets in a project: import (octree asset + PointCloudDataset entity), reading
// in project coordinates, derived datasets from processing, export.
//
// Heavy work is split from model changes: `prepare*` functions run on a worker thread and only
// write new immutable assets; `commit*` functions apply the result through ProjectEditor on the
// UI thread (one undo step). Stored coordinates are never modified: registrations change the
// dataset's transform only.

#include "openmine/survey/Octree.h"
#include "openmine/project/Project.h"

#include <filesystem>
#include <optional>
#include <functional>
#include <memory>

namespace openmine::survey {

using SourceOpener = std::function<std::unique_ptr<PointSource>(const std::filesystem::path&)>;
using FileWriter = std::function<void(PointSource&, const std::filesystem::path&, jobs::Progress&)>;

/// Format registry. Text formats (.xyz .csv .txt .asc .pts .ptx) are built in; integrations
/// (PDAL: .las .laz .e57 .ply) register theirs at application start-up.
void registerReader(const std::string& extension, SourceOpener opener);
void registerWriter(const std::string& extension, FileWriter writer);
std::vector<std::string> readableExtensions();
std::vector<std::string> writableExtensions();
std::unique_ptr<PointSource> openPointFile(const std::filesystem::path& file);

struct ImportOptions {
    std::string name;                 ///< default: file name
    OctreeBuildOptions octree;
    std::string sourceCrs;            ///< overrides the CRS declared by the file
    std::optional<Date> scanDate;
    std::string scanner;
    Uuid developmentId;
    Transform3 transform;             ///< stored → project (e.g. a known local-grid transform)
};

struct PreparedDataset {
    Asset asset;
    PointCloudDataset dataset;
};

/// Converts the file into an octree asset (worker thread). Nothing is added to the model.
PreparedDataset prepareImport(Project& project, const std::filesystem::path& file, const ImportOptions& options,
                              jobs::Progress& progress = jobs::nullProgress());
/// Builds a dataset from points already in project coordinates (e.g. points read from an
/// exchange file) — worker thread. Nothing is added to the model.
PreparedDataset prepareFromPoints(Project& project, const PointBuffer& points, const std::string& name, const std::string& sourceFile,
                                  const std::string& sourceFormat, jobs::Progress& progress = jobs::nullProgress());
/// Adds the asset and dataset (UI thread, one undo step). Returns the dataset id.
Uuid commitDataset(Project& project, PreparedDataset prepared, const std::string& commandText = "Import point cloud");
/// A warning when a dataset's declared CRS (an authority code such as "EPSG:2051", or WKT whose
/// outermost AUTHORITY/ID names one) is not the project CRS, or the project has none. Imports
/// never transform coordinates implicitly, so the user must be told. nullopt when they agree or
/// the dataset declares nothing.
std::optional<std::string> crsMismatch(const Project& project, const PointCloudDataset& dataset);
/// "EPSG:2051" from a code or from the outermost AUTHORITY["EPSG","2051"] / ID["EPSG",2051] of WKT.
std::string authorityOf(const std::string& crsDefinition);

/// prepareImport + commitDataset.
Uuid importPointCloud(Project& project, const std::filesystem::path& file, const ImportOptions& options = {},
                      jobs::Progress& progress = jobs::nullProgress());

const PointCloudDataset& dataset(const Project& project, const Uuid& id);
OctreeReader openDataset(const Project& project, const PointCloudDataset& d);

/// All points (optionally inside a project-coordinate box) in project coordinates (dataset
/// transform applied) or in stored coordinates.
PointBuffer datasetPoints(const Project& project, const Uuid& id, bool projectCoordinates = true,
                          const std::optional<BoundingBox>& projectBox = std::nullopt, jobs::Progress& progress = jobs::nullProgress());

/// A source over a dataset in project coordinates (for streaming export).
std::unique_ptr<PointSource> datasetSource(const Project& project, const Uuid& id);

/// Builds a derived dataset from processed points (given in project coordinates, so its transform
/// is the identity) — worker thread.
PreparedDataset prepareDerived(Project& project, const Uuid& parentId, const PointBuffer& points, const std::string& name,
                               const std::string& derivation, jobs::Progress& progress = jobs::nullProgress());

/// Exports a dataset in project coordinates; the format follows the extension.
void exportDataset(const Project& project, const Uuid& id, const std::filesystem::path& file,
                   jobs::Progress& progress = jobs::nullProgress());

/// Applies a transform to a buffer source on the fly.
class TransformSource : public PointSource {
public:
    TransformSource(std::unique_ptr<PointSource> inner, Transform3 t) : inner_(std::move(inner)), t_(t) {}
    PointSchema schema() const override { return inner_->schema(); }
    std::string formatName() const override { return inner_->formatName(); }
    std::optional<std::int64_t> countHint() const override { return inner_->countHint(); }
    std::optional<BoundingBox> boundsHint() const override;
    std::size_t read(PointBuffer& out, std::size_t max) override;
    void rewind() override { inner_->rewind(); }
    double progress() const override { return inner_->progress(); }

private:
    std::unique_ptr<PointSource> inner_;
    Transform3 t_;
};

} // namespace openmine::survey
