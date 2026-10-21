#pragma once
// SPDX-License-Identifier: Apache-2.0
// OpenMine project container (.openmine) — see docs/architecture/project-format.md.
//
//   MyMine.openmine            SQLite database (PRAGMA application_id = 'OMIN', user_version = schema)
//   MyMine.openmine-assets/    sidecar folder for large binaries (point clouds, meshes, B-reps,
//                              block-model arrays, attachments, reports) referenced from the
//                              `asset` table by relative path and SHA-256. Not embedded in SQL.
//   MyMine.openmine.bak        previous version, kept by every successful save.

#include "openmine/Version.h"
#include "openmine/project/Project.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace openmine::persistence {

inline constexpr int kCurrentSchemaVersion = 2;
inline constexpr std::int32_t kApplicationId = 0x4F4D494E; // 'OMIN'
inline constexpr const char* kFileExtension = ".openmine";
inline constexpr const char* kAppVersion = "OpenMine Suite " OPENMINE_VERSION_STRING;

class PersistenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Saves the whole model atomically: assets are copied to the target's sidecar folder, the
/// database is written to "<file>.saving", verified with PRAGMA integrity_check and
/// foreign_key_check, then renamed over the target. On any failure the existing file is left
/// untouched. Marks the undo stack clean.
void saveProject(Project& project, const std::filesystem::path& file);

struct LoadReport {
    int fileSchemaVersion = 0;
    bool migrated = false;             ///< read from an older schema (saved in the new schema on next save)
    std::vector<std::string> warnings; ///< e.g. missing or modified sidecar assets
};

/// Loads a project. Files from older schema versions are read by their version-specific reader
/// (the original is never modified). Files from newer versions are refused.
std::unique_ptr<Project> loadProject(const std::filesystem::path& file, LoadReport* report = nullptr);

/// Integrity report for an existing file (does not load it). `fullAssetCheck` re-hashes every asset.
struct IntegrityReport {
    bool ok = false;
    int schemaVersion = 0;
    std::vector<std::string> problems;
};
IntegrityReport checkProjectFile(const std::filesystem::path& file, bool fullAssetCheck = false);

/// Result of compacting a project's asset store.
struct CompactReport {
    std::vector<std::string> unreferencedAssets; ///< registered assets no project object uses (relative paths)
    std::vector<std::string> orphanFiles;        ///< files/folders in the asset store that are not registered assets
    std::int64_t bytes = 0;                      ///< space they occupy (freed unless dry run)
    bool dryRun = false;
};

/// Removes assets no project object references any more (e.g. point clouds or attachments that
/// were deleted) and files in the asset store that are not registered assets (left by cancelled
/// operations). Needs a saved project. Unless `dryRun`, it clears the undo history — an undo could
/// otherwise restore an object whose stored file is gone — records an audit entry, saves the
/// project and only then deletes the files, so a failure never leaves the project referring to
/// missing files. Files outside the asset kind folders are never touched.
CompactReport compactProject(Project& project, bool dryRun = false);

/// The SQL DDL for the current schema (exposed for documentation and tests).
const std::string& schemaSql();
/// The SQL DDL of schema version 1 (exposed so migration tests can build v1 fixtures).
const char* legacySchemaV1Sql();

} // namespace openmine::persistence
