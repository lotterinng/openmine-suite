#pragma once
// SPDX-License-Identifier: Apache-2.0
// Internal: schema version 2 and the legacy version-1 reader.

#include "Sqlite.h"

#include <string>
#include <vector>

namespace openmine {
struct ProjectModel;
}

namespace openmine::persistence::detail {

const std::string& schemaDdlV2();

/// Entity documents for repositories (JSON text): every column of the entity's table plus its
/// child rows, encoded through the same mapping as the project file.
template <class T> std::string encodeEntityV2(const T& e);
template <class T> T decodeEntityV2(const std::string& doc);
void writeModelV2(Db& db, const ProjectModel& m, const std::string& appVersion);
void readModelV2(Db& db, ProjectModel& m);
std::vector<std::string> verifyV2(Db& db);

/// Reads a schema-version-1 file (OpenMine 0.1) into the current model, converting
/// day-based durations to hours and v1 calendars to shift calendars (see ProjectFile.cpp).
void readModelV1(Db& db, ProjectModel& m);
std::vector<std::string> verifyV1(Db& db);
/// The exact v1 DDL (kept for migration tests).
const char* schemaDdlV1();

} // namespace openmine::persistence::detail
