#pragma once
// SPDX-License-Identifier: Apache-2.0
// Shared multi-user project repository on PostgreSQL + PostGIS.
//
// Model: a repository holds projects; each publish creates a numbered revision (author, message,
// time) and stores the changed entity documents (see persistence/EntityCodec.h) as JSONB, with a
// PostGIS geometry per spatial entity for server-side spatial queries. Every version of every
// entity is kept (entity_history), so any revision can be checked out and each entity's history
// listed. Assets (point-cloud octrees, B-reps, block-model arrays, attachments, reports) are stored
// content-addressed by SHA-256 and verified on download.
//
// Workflow (like a version-control client, with the local .openmine file as the working copy):
//   checkout → edit locally (undo/redo as usual) → pull (merge others' revisions) → publish.
//  * Publishing requires the working copy to be up to date with the head revision (pull first);
//    it is one transaction serialised per project (SELECT … FOR UPDATE on the project row), so
//    two publishers can never interleave.
//  * Pull applies remote changes as one undoable step. An entity changed both locally and
//    remotely since the base revision is a conflict; the policy decides: Abort (default — nothing
//    is applied, conflicts are reported), TakeRemote, or KeepLocal (the local version is kept and
//    will overwrite the remote one at the next publish). The merged model is checked for
//    referential integrity before it is accepted; otherwise the pull is rolled back.
//  * Entities can be locked (with expiry) to prevent concurrent edits; publishing a change to an
//    entity locked by someone else is refused.
// Nothing is merged field-by-field and nothing is overwritten silently.

#include "openmine/project/Project.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace openmine::repository {

class RepositoryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Binding of a local working copy to a repository project (kept next to the project file as
/// "<file>.openmine-repo.json"; it never contains passwords).
struct WorkingCopy {
    std::string connection;            ///< libpq connection string without password (informational)
    Uuid projectId;
    std::int64_t baseRevision = 0;
    std::map<std::string, std::string> baseHashes; ///< "<type>|<key>" → SHA-256 of the document at the base revision
    std::string toJson() const;
    static WorkingCopy fromJson(const std::string& json);
    void save(const std::filesystem::path& projectFile) const;
    static std::optional<WorkingCopy> load(const std::filesystem::path& projectFile);
    static std::filesystem::path pathFor(const std::filesystem::path& projectFile);
};

struct LocalChange {
    enum class Kind { Added, Modified, Deleted } kind = Kind::Modified;
    std::string type, key;
};
/// Local changes of a working copy relative to its base revision.
std::vector<LocalChange> localChanges(const Project& project, const WorkingCopy& wc);

struct RevisionInfo {
    std::int64_t revision = 0;
    std::string author, message, createdAt;
    int changes = 0;
};

struct EntityVersion {
    std::int64_t revision = 0;
    bool deleted = false;
    std::string author, message, createdAt, hash;
};

struct Conflict {
    std::string type, key, name;
    std::string reason;                ///< "changed locally and in revision N by X", "locked by X"
};

enum class ConflictPolicy { Abort, TakeRemote, KeepLocal };

struct PullResult {
    std::int64_t fromRevision = 0, toRevision = 0;
    std::size_t applied = 0;
    std::vector<Conflict> conflicts;
    bool aborted = false;
};

struct PublishResult {
    bool published = false;
    std::int64_t revision = 0;
    std::size_t changes = 0, assetsUploaded = 0;
    std::vector<Conflict> conflicts;   ///< non-empty ⇒ nothing was published
    std::string refusal;               ///< e.g. "out of date: pull revision 7 first"
};

struct SpatialHit {
    std::string type, key, name;
};

struct Lock {
    std::string type, key, holder, acquiredAt, expiresAt;
};

class PgRepository {
public:
    /// Connects with a libpq connection string ("host=… port=… dbname=… user=…" or a URI).
    explicit PgRepository(const std::string& connection);
    ~PgRepository();
    PgRepository(const PgRepository&) = delete;
    PgRepository& operator=(const PgRepository&) = delete;

    /// Creates the schema (and the PostGIS extension when permitted). Idempotent; refuses a
    /// database whose repository schema is newer than this build.
    void initialise();
    int schemaVersion() const;
    std::string serverVersion() const;
    std::string postgisVersion() const;

    /// Publishes a local project as a new repository project (revision 1). Returns the working copy.
    WorkingCopy createProject(Project& project, const std::string& author, const std::string& message = "Initial import");
    std::vector<std::pair<Uuid, std::string>> projects() const;
    void deleteProject(const Uuid& projectId);

    /// A full working copy at `revision` (0 = head). Assets are downloaded into the project's
    /// asset store and verified.
    std::unique_ptr<Project> checkout(const Uuid& projectId, WorkingCopy& wc, std::int64_t revision = 0);

    std::int64_t headRevision(const Uuid& projectId) const;
    std::vector<RevisionInfo> revisions(const Uuid& projectId) const;
    std::vector<EntityVersion> history(const Uuid& projectId, const std::string& type, const std::string& key) const;

    PullResult pull(Project& project, WorkingCopy& wc, ConflictPolicy policy = ConflictPolicy::Abort);
    PublishResult publish(Project& project, WorkingCopy& wc, const std::string& author, const std::string& message);

    /// Entities whose geometry intersects the 2D box (project coordinates), optionally of the given types.
    std::vector<SpatialHit> intersecting(const Uuid& projectId, double minX, double minY, double maxX, double maxY,
                                         const std::vector<std::string>& types = {}) const;
    /// Entities within `distance` (2D) of a point.
    std::vector<SpatialHit> within(const Uuid& projectId, double x, double y, double distance, const std::vector<std::string>& types = {}) const;

    bool lock(const Uuid& projectId, const std::string& type, const std::string& key, const std::string& holder, std::chrono::seconds ttl);
    void unlock(const Uuid& projectId, const std::string& type, const std::string& key, const std::string& holder);
    std::vector<Lock> locks(const Uuid& projectId) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Representative geometry of a document as EWKT-less WKT with Z (empty when the entity has none).
std::string documentWkt(const std::string& type, const std::string& json);

} // namespace openmine::repository
