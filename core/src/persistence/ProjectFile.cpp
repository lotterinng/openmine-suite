// SPDX-License-Identifier: Apache-2.0
#include "openmine/persistence/ProjectFile.h"

#include "SchemaV2.h"
#include "Sqlite.h"
#include "openmine/diagnostics/Log.h"

#include <bit>

namespace openmine::persistence {

using detail::Db;
using detail::Stmt;

static_assert(std::endian::native == std::endian::little,
              "Binary vertex/triangle encoding assumes a little-endian host (x86-64 / ARM64)");

namespace {
std::filesystem::path withSuffix(const std::filesystem::path& p, const char* suffix) {
    auto s = p;
    s += suffix;
    return s;
}

void initialiseSchema(Db& db) {
    db.exec("PRAGMA application_id = " + std::to_string(kApplicationId));
    db.exec("BEGIN");
    try {
        db.exec(detail::schemaDdlV2());
        Stmt s(db, "INSERT INTO schema_migrations(version, applied_at, description) VALUES (?,?,?)");
        s.bind(1, 1).bind(2, utcTimestampNow()).bind(3, std::string("Initial OpenMine schema (0.1)")).run();
        s.bind(1, 2).bind(2, utcTimestampNow()).bind(3, std::string("Unified model: survey, design, scheduling, geology (0.2)")).run();
        db.exec("PRAGMA user_version = " + std::to_string(kCurrentSchemaVersion));
        db.exec("COMMIT");
    } catch (...) {
        db.exec("ROLLBACK");
        throw;
    }
}

std::vector<Asset> allAssets(const ProjectModel& m) { return m.assets.all(); }
} // namespace

const std::string& schemaSql() { return detail::schemaDdlV2(); }
const char* legacySchemaV1Sql() { return detail::schemaDdlV1(); }

void saveProject(Project& project, const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    if (file.empty()) throw PersistenceError("No file name given");
    const fs::path tmp = withSuffix(file, ".saving");
    std::error_code ec;
    fs::remove(tmp, ec);
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);

    try {
        // 1. Sidecar assets live next to the project file.
        project.assets().relocate(AssetStore::sidecarFor(file), allAssets(project.model()));
        for (const auto& a : project.model().assets.all()) {
            auto v = project.assets().verify(a, false);
            if (!v.ok)
                for (const auto& pbl : v.problems) diag::warn("persistence", "asset problem while saving: " + pbl);
        }
        // 2. Database.
        {
            Db db(tmp, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
            db.exec("PRAGMA journal_mode = DELETE");
            db.exec("PRAGMA foreign_keys = ON");
            initialiseSchema(db);
            db.exec("BEGIN");
            try {
                db.exec("PRAGMA defer_foreign_keys = ON");
                detail::writeModelV2(db, project.model(), kAppVersion);
                db.exec("COMMIT");
            } catch (...) {
                db.exec("ROLLBACK");
                throw;
            }
            auto problems = detail::verifyV2(db);
            if (!problems.empty()) throw PersistenceError("Save verification failed: " + problems.front());
        }
        if (fs::exists(file)) fs::copy_file(file, withSuffix(file, ".bak"), fs::copy_options::overwrite_existing);
        fs::rename(tmp, file); // atomic replace on the same volume
    } catch (const std::exception& e) {
        fs::remove(tmp, ec);
        throw PersistenceError(std::string("Project was NOT saved (existing file unchanged): ") + e.what());
    }
    project.setFilePath(file);
    project.commands().setClean();
    diag::info("persistence", "saved " + file.string());
}

std::unique_ptr<Project> loadProject(const std::filesystem::path& file, LoadReport* report) {
    namespace fs = std::filesystem;
    if (!fs::exists(file)) throw PersistenceError("File not found: " + file.string());

    int version = 0;
    try {
        Db db(file, SQLITE_OPEN_READONLY);
        Stmt app(db, "PRAGMA application_id");
        app.step();
        if (app.integer(0) != kApplicationId) throw PersistenceError("Not an OpenMine project file: " + file.string());
        Stmt uv(db, "PRAGMA user_version");
        uv.step();
        version = static_cast<int>(uv.integer(0));
    } catch (const PersistenceError&) {
        throw;
    } catch (const std::exception& e) {
        throw PersistenceError(std::string("Not a readable OpenMine project file: ") + e.what());
    }
    if (version > kCurrentSchemaVersion)
        throw PersistenceError("Project was written by a newer OpenMine version (schema " + std::to_string(version) +
                               "); this build supports up to schema " + std::to_string(kCurrentSchemaVersion));
    if (version < 1) throw PersistenceError("Project file has an invalid schema version");

    LoadReport local;
    LoadReport& rep = report ? *report : local;
    rep.fileSchemaVersion = version;
    rep.migrated = version < kCurrentSchemaVersion;

    auto project = std::make_unique<Project>();
    {
        Db db(file, SQLITE_OPEN_READONLY);
        auto problems = version == 1 ? detail::verifyV1(db) : detail::verifyV2(db);
        if (!problems.empty()) throw PersistenceError("Project file failed integrity check: " + problems.front());
        EventBus::Mute mute(project->events());
        if (version == 1) detail::readModelV1(db, project->mutableModel());
        else detail::readModelV2(db, project->mutableModel());
    }
    project->assets() = AssetStore(AssetStore::sidecarFor(file));
    for (const auto& a : project->model().assets.all()) {
        auto v = project->assets().verify(a, false);
        for (const auto& p : v.problems) rep.warnings.push_back("Asset " + p);
    }
    if (rep.migrated)
        project->appendAudit("Migrate", "Project", project->model().info.id,
                             "Read schema v" + std::to_string(version) + " file; will be written as v" +
                                 std::to_string(kCurrentSchemaVersion) + " on next save");
    project->setFilePath(file);
    project->commands().setClean();
    for (const auto& w : rep.warnings) diag::warn("persistence", w);
    diag::info("persistence", "opened " + file.string() + " (schema v" + std::to_string(version) + ")");
    return project;
}

IntegrityReport checkProjectFile(const std::filesystem::path& file, bool fullAssetCheck) {
    IntegrityReport r;
    try {
        {
            Db db(file, SQLITE_OPEN_READONLY);
            Stmt uv(db, "PRAGMA user_version");
            uv.step();
            r.schemaVersion = static_cast<int>(uv.integer(0));
            r.problems = r.schemaVersion == 1 ? detail::verifyV1(db) : detail::verifyV2(db);
        }
        if (r.schemaVersion >= 2 && r.problems.empty()) {
            auto p = loadProject(file);
            for (const auto& a : p->model().assets.all()) {
                auto v = p->assets().verify(a, fullAssetCheck);
                r.problems.insert(r.problems.end(), v.problems.begin(), v.problems.end());
            }
        }
        r.ok = r.problems.empty();
    } catch (const std::exception& e) {
        r.problems.push_back(e.what());
    }
    return r;
}

} // namespace openmine::persistence
