#pragma once
// SPDX-License-Identifier: Apache-2.0
// OpenMine project container (.openmine) — see docs/architecture/project-format.md.
//
//   MyMine.openmine            SQLite database (PRAGMA application_id = 'OMIN', user_version = schema)
//   MyMine.openmine-assets/    sidecar folder for large binaries (point clouds, meshes, B-reps,
//                              block-model arrays, attachments, reports) referenced from the
//                              `asset` table by relative path and SHA-256. Not embedded in SQL.
//   MyMine.openmine.bak        previous version, kept by every successful save.

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
inline constexpr const char* kAppVersion = "OpenMine Suite 0.2.0";

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

/// The SQL DDL for the current schema (exposed for documentation and tests).
const std::string& schemaSql();
/// The SQL DDL of schema version 1 (exposed so migration tests can build v1 fixtures).
const char* legacySchemaV1Sql();

} // namespace openmine::persistence
