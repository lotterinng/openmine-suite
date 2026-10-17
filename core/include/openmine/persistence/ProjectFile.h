#pragma once
// SPDX-License-Identifier: Apache-2.0
// OpenMine project container (.openmine) — see docs/architecture/project-format.md.
//
//   MyMine.openmine            SQLite database (schema below, PRAGMA application_id = 'OMIN')
//   MyMine.openmine-assets/    optional sidecar folder for large binaries (point clouds,
//                              meshes) referenced from the `asset` table by relative path
//                              and SHA-256. Not embedded in SQL.
//   MyMine.openmine.bak        previous version, kept by every successful save.

#include "openmine/project/Project.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace openmine::persistence {

inline constexpr int kCurrentSchemaVersion = 1;
inline constexpr std::int32_t kApplicationId = 0x4F4D494E; // 'OMIN'
inline constexpr const char* kFileExtension = ".openmine";

class PersistenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Saves the whole model atomically: written to "<file>.saving", verified with
/// PRAGMA integrity_check and foreign_key_check, then renamed over the target.
/// On any failure the existing file is left untouched. Marks the undo stack clean.
void saveProject(Project& project, const std::filesystem::path& file);

/// Loads a project. Files from older schema versions are migrated in a temporary
/// copy (the original is never modified). Files from newer versions are refused.
std::unique_ptr<Project> loadProject(const std::filesystem::path& file);

/// Integrity report for an existing file (does not load it).
struct IntegrityReport {
    bool ok = false;
    int schemaVersion = 0;
    std::vector<std::string> problems;
};
IntegrityReport checkProjectFile(const std::filesystem::path& file);

/// The SQL DDL for the current schema (exposed for documentation and tests).
const char* schemaSql();

} // namespace openmine::persistence
