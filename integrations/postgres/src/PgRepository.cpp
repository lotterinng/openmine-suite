// SPDX-License-Identifier: Apache-2.0
#include "openmine/repository/PgRepository.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/persistence/EntityCodec.h"
#include "openmine/util/Sha256.h"

#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace openmine::repository {

namespace fs = std::filesystem;
using nlohmann::json;
namespace pe = openmine::persistence;

namespace {
constexpr int kSchemaVersion = 1;

constexpr const char* kSchemaSql = R"SQL(
CREATE SCHEMA IF NOT EXISTS openmine;
CREATE TABLE IF NOT EXISTS openmine.repo_meta (key text PRIMARY KEY, value text NOT NULL);
CREATE TABLE IF NOT EXISTS openmine.project (
    id uuid PRIMARY KEY,
    name text NOT NULL,
    srid integer NOT NULL DEFAULT 0,
    head_revision bigint NOT NULL DEFAULT 0,
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS openmine.revision (
    project_id uuid NOT NULL REFERENCES openmine.project(id) ON DELETE CASCADE,
    revision bigint NOT NULL,
    author text NOT NULL,
    message text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    changes integer NOT NULL,
    PRIMARY KEY (project_id, revision)
);
CREATE TABLE IF NOT EXISTS openmine.entity (
    project_id uuid NOT NULL,
    entity_type text NOT NULL,
    entity_key text NOT NULL,
    revision bigint NOT NULL,
    deleted boolean NOT NULL,
    content_sha256 text,
    doc jsonb,
    name text,
    geom geometry,
    PRIMARY KEY (project_id, entity_type, entity_key),
    FOREIGN KEY (project_id, revision) REFERENCES openmine.revision(project_id, revision) ON DELETE CASCADE,
    CHECK (deleted OR (doc IS NOT NULL AND content_sha256 IS NOT NULL))
);
CREATE INDEX IF NOT EXISTS entity_geom_gix ON openmine.entity USING gist (geom);
CREATE INDEX IF NOT EXISTS entity_revision_ix ON openmine.entity (project_id, revision);
CREATE TABLE IF NOT EXISTS openmine.entity_history (
    project_id uuid NOT NULL,
    entity_type text NOT NULL,
    entity_key text NOT NULL,
    revision bigint NOT NULL,
    deleted boolean NOT NULL,
    content_sha256 text,
    doc jsonb,
    PRIMARY KEY (project_id, entity_type, entity_key, revision),
    FOREIGN KEY (project_id, revision) REFERENCES openmine.revision(project_id, revision) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS openmine.blob (
    sha256 text PRIMARY KEY,
    size bigint NOT NULL,
    data bytea NOT NULL
);
CREATE TABLE IF NOT EXISTS openmine.asset_file (
    project_id uuid NOT NULL REFERENCES openmine.project(id) ON DELETE CASCADE,
    asset_key text NOT NULL,
    rel_path text NOT NULL,
    sha256 text NOT NULL REFERENCES openmine.blob(sha256),
    size bigint NOT NULL,
    PRIMARY KEY (project_id, asset_key, rel_path)
);
CREATE TABLE IF NOT EXISTS openmine.entity_lock (
    project_id uuid NOT NULL REFERENCES openmine.project(id) ON DELETE CASCADE,
    entity_type text NOT NULL,
    entity_key text NOT NULL,
    holder text NOT NULL,
    acquired_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL,
    PRIMARY KEY (project_id, entity_type, entity_key)
);
)SQL";

using Params = std::vector<std::optional<std::string>>;

class Result {
public:
    explicit Result(PGresult* r) : r_(r) {}
    ~Result() { PQclear(r_); }
    Result(Result&& o) noexcept : r_(o.r_) { o.r_ = nullptr; }
    Result(const Result&) = delete;
    int rows() const { return PQntuples(r_); }
    bool isNull(int r, int c) const { return PQgetisnull(r_, r, c) != 0; }
    std::string text(int r, int c) const { return std::string(PQgetvalue(r_, r, c), static_cast<std::size_t>(PQgetlength(r_, r, c))); }
    std::int64_t integer(int r, int c) const { return std::stoll(text(r, c)); }
    bool boolean(int r, int c) const { return text(r, c) == "t"; }
    std::string bytes(int r, int c) const { return text(r, c); } // binary result format

private:
    PGresult* r_;
};

std::string key(const std::string& type, const std::string& k) { return type + "|" + k; }

std::string fmt(double v) {
    std::ostringstream s;
    s << std::setprecision(17) << v;
    return s.str();
}
std::string xyz(Vec3 p) { return fmt(p.x) + " " + fmt(p.y) + " " + fmt(p.z); }

std::string pointWkt(Vec3 p) { return "POINT Z (" + xyz(p) + ")"; }
std::string lineWkt(const std::vector<Vec3>& pts) {
    if (pts.empty()) return {};
    if (pts.size() == 1) return pointWkt(pts[0]);
    std::string s = "LINESTRING Z (";
    for (std::size_t i = 0; i < pts.size(); ++i) s += (i ? ", " : "") + xyz(pts[i]);
    return s + ")";
}
std::string envelopeWkt(const std::vector<Vec3>& pts) {
    if (pts.empty()) return {};
    Vec3 lo = pts[0], hi = pts[0];
    for (const auto& p : pts) {
        lo = componentMin(lo, p);
        hi = componentMax(hi, p);
    }
    if (hi.x - lo.x < 1e-12 && hi.y - lo.y < 1e-12) return pointWkt(lo);
    if (hi.x - lo.x < 1e-12 || hi.y - lo.y < 1e-12) return lineWkt({lo, {hi.x, hi.y, lo.z}});
    const double z = lo.z;
    return "POLYGON Z ((" + xyz({lo.x, lo.y, z}) + ", " + xyz({hi.x, lo.y, z}) + ", " + xyz({hi.x, hi.y, z}) + ", " + xyz({lo.x, hi.y, z}) + ", " +
           xyz({lo.x, lo.y, z}) + "))";
}

std::string nameOf(const json& doc) {
    if (!doc.is_object()) return {};
    if (!doc.contains("columns")) return doc.value("name", ""); // ProjectInfo / ActivityDesignLink
    const auto& c = doc.at("columns");
    for (const char* n : {"name", "hole_id", "heading_id", "set_name", "key"})
        if (c.contains(n) && c.at(n).is_string() && !c.at(n).get<std::string>().empty()) return c.at(n).get<std::string>();
    return {};
}

constexpr const char* kOrder = "$order"; // pseudo-document: order of a type's entities

/// Entity documents plus one order document per non-empty entity type.
std::vector<pe::EntityDocument> allDocuments(const ProjectModel& m) {
    auto docs = pe::encodeModel(m);
    for (const auto& t : pe::documentTypes()) {
        if (t == "ProjectInfo" || t == "ActivityDesignLink") continue;
        const auto o = pe::entityOrder(m, t);
        if (!o.empty()) docs.push_back({kOrder, t, json(o).dump()});
    }
    return docs;
}

std::optional<std::string> documentOf(const ProjectModel& m, const std::string& type, const std::string& k) {
    if (type == kOrder) {
        const auto o = pe::entityOrder(m, k);
        if (o.empty()) return std::nullopt;
        return json(o).dump();
    }
    return pe::encodeDocument(m, type, k);
}

/// Remote order merged with local-only entities: each is kept after its local predecessor and
/// after any entities that arrived with this pull at that place (remote additions come first).
std::vector<std::string> mergeOrder(const std::vector<std::string>& remote, const std::vector<std::string>& local,
                                    const std::vector<std::string>& beforePull) {
    std::set<std::string> localSet(local.begin(), local.end()), remoteSet(remote.begin(), remote.end());
    const std::set<std::string> known(beforePull.begin(), beforePull.end());
    std::vector<std::string> out;
    for (const auto& k : remote)
        if (localSet.count(k)) out.push_back(k);
    for (std::size_t i = 0; i < local.size(); ++i) {
        if (remoteSet.count(local[i])) continue;
        // Insert after the nearest preceding local item already placed.
        auto pos = out.begin();
        for (std::size_t j = i; j-- > 0;) {
            auto it = std::find(out.begin(), out.end(), local[j]);
            if (it != out.end()) {
                pos = it + 1;
                break;
            }
        }
        while (pos != out.end() && !known.count(*pos)) ++pos; // skip entities new from the remote
        out.insert(pos, local[i]);
    }
    return out;
}

int sridOf(const ProjectModel& m) {
    const auto* crs = m.projectCrs();
    if (!crs) return 0;
    const std::string& a = crs->authorityCode;
    if (a.rfind("EPSG:", 0) == 0) {
        try {
            return std::stoi(a.substr(5));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

/// Files of an asset (relative to the asset path): one entry "" for a file asset.
std::vector<std::pair<std::string, fs::path>> assetFiles(const AssetStore& store, const Asset& a) {
    std::vector<std::pair<std::string, fs::path>> out;
    const fs::path root = store.pathOf(a);
    if (!a.isDirectory) {
        out.emplace_back("", root);
        return out;
    }
    if (!fs::exists(root)) throw RepositoryError("Asset folder is missing: " + root.string());
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file()) out.emplace_back(fs::relative(e.path(), root).generic_string(), e.path());
    std::sort(out.begin(), out.end());
    return out;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw RepositoryError("Cannot read " + p.string());
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
} // namespace

// ---- documents → geometry -----------------------------------------------------------------------
std::string documentWkt(const std::string& type, const std::string& text) {
    if (type == "GeometryObject") {
        const auto g = pe::decodeEntity<GeometryObject>(text);
        switch (g.type) {
        case GeometryType::Point:
        case GeometryType::Text: return g.vertices.empty() ? std::string{} : pointWkt(g.vertices.front());
        case GeometryType::Line:
        case GeometryType::Spline: {
            auto v = g.vertices;
            if (g.closed && v.size() > 2) v.push_back(v.front());
            return lineWkt(v);
        }
        case GeometryType::Polyline: {
            auto v = g.bulges.empty() ? g.vertices : geometry::expandBulges(g.vertices, g.bulges, g.closed, 0.01);
            if (g.closed && v.size() > 2 && !(v.front() == v.back())) v.push_back(v.front());
            return lineWkt(v);
        }
        case GeometryType::Arc:
        case GeometryType::Circle: return lineWkt(geometry::arcPoints(g.arc, 0.01));
        case GeometryType::Mesh:
        case GeometryType::Solid: return g.mesh ? envelopeWkt(g.mesh->vertices) : std::string{};
        default: return envelopeWkt(g.vertices);
        }
    }
    if (type == "SurveyControlPoint") return pointWkt(pe::decodeEntity<SurveyControlPoint>(text).position);
    if (type == "Drillhole") return pointWkt(pe::decodeEntity<Drillhole>(text).collar);
    if (type == "StructuralMeasurement") return pointWkt(pe::decodeEntity<StructuralMeasurement>(text).location);
    if (type == "GeotechClassification") return pointWkt(pe::decodeEntity<GeotechClassification>(text).location);
    if (type == "PointCloudDataset") {
        const auto d = pe::decodeEntity<PointCloudDataset>(text);
        if (d.pointCount == 0) return {};
        std::vector<Vec3> c;
        for (int n = 0; n < 8; ++n)
            c.push_back(d.transform.apply({(n & 1) ? d.storedBounds.max.x : d.storedBounds.min.x, (n & 2) ? d.storedBounds.max.y : d.storedBounds.min.y,
                                           (n & 4) ? d.storedBounds.max.z : d.storedBounds.min.z}));
        return envelopeWkt(c);
    }
    if (type == "BlockModel") {
        const auto b = pe::decodeEntity<BlockModel>(text);
        const double r = b.rotationDeg * 3.14159265358979323846 / 180;
        const Vec3 u{std::cos(r), std::sin(r), 0}, v{-std::sin(r), std::cos(r), 0};
        const double ex = b.nx * b.blockSize.x, ey = b.ny * b.blockSize.y;
        const Vec3 o = b.origin;
        const Vec3 p1 = o + u * ex, p2 = o + u * ex + v * ey, p3 = o + v * ey;
        return "POLYGON Z ((" + xyz(o) + ", " + xyz(p1) + ", " + xyz(p2) + ", " + xyz(p3) + ", " + xyz(o) + "))";
    }
    return {};
}

// ---- working copy -------------------------------------------------------------------------------
std::string WorkingCopy::toJson() const {
    return json{{"format", "openmine-working-copy"}, {"version", 1}, {"connection", connection}, {"project", projectId.toString()},
                {"base_revision", baseRevision}, {"base_hashes", baseHashes}}
        .dump(1);
}

WorkingCopy WorkingCopy::fromJson(const std::string& s) {
    const json j = json::parse(s);
    if (j.value("format", "") != "openmine-working-copy") throw RepositoryError("Not an OpenMine working-copy file");
    WorkingCopy w;
    w.connection = j.value("connection", "");
    auto id = Uuid::parse(j.at("project").get<std::string>());
    if (!id) throw RepositoryError("Corrupt working-copy project id");
    w.projectId = *id;
    w.baseRevision = j.at("base_revision");
    w.baseHashes = j.at("base_hashes").get<std::map<std::string, std::string>>();
    return w;
}

fs::path WorkingCopy::pathFor(const fs::path& f) { return fs::path(f.string() + "-repo.json"); }

void WorkingCopy::save(const fs::path& f) const {
    const auto p = pathFor(f);
    const auto tmp = fs::path(p.string() + ".saving");
    {
        std::ofstream o(tmp, std::ios::binary);
        o << toJson();
        if (!o) throw RepositoryError("Cannot write " + tmp.string());
    }
    fs::rename(tmp, p);
}

std::optional<WorkingCopy> WorkingCopy::load(const fs::path& f) {
    const auto p = pathFor(f);
    if (!fs::exists(p)) return std::nullopt;
    return fromJson(readFile(p));
}

std::vector<LocalChange> localChanges(const Project& p, const WorkingCopy& wc) {
    std::vector<LocalChange> out;
    std::set<std::string> seen;
    for (const auto& d : allDocuments(p.model())) {
        const auto k = key(d.type, d.key);
        seen.insert(k);
        const auto it = wc.baseHashes.find(k);
        if (it == wc.baseHashes.end()) out.push_back({LocalChange::Kind::Added, d.type, d.key});
        else if (it->second != pe::documentHash(d.json)) out.push_back({LocalChange::Kind::Modified, d.type, d.key});
    }
    for (const auto& [k, h] : wc.baseHashes)
        if (!seen.count(k)) {
            const auto bar = k.find('|');
            out.push_back({LocalChange::Kind::Deleted, k.substr(0, bar), k.substr(bar + 1)});
        }
    return out;
}

// ---- connection -------------------------------------------------------------------------------------
struct PgRepository::Impl {
    PGconn* conn = nullptr;
    std::string connection;

    ~Impl() {
        if (conn) PQfinish(conn);
    }

    /// Several statements without parameters (schema creation).
    void script(const std::string& sql) const {
        PGresult* r = PQexec(conn, sql.c_str());
        const auto st = PQresultStatus(r);
        if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
            const std::string msg = PQresultErrorMessage(r);
            PQclear(r);
            throw RepositoryError("PostgreSQL: " + msg);
        }
        PQclear(r);
    }

    Result exec(const std::string& sql, const Params& params = {}, bool binaryResult = false) const {
        std::vector<const char*> values;
        values.reserve(params.size());
        for (const auto& p : params) values.push_back(p ? p->c_str() : nullptr);
        PGresult* r = PQexecParams(conn, sql.c_str(), static_cast<int>(params.size()), nullptr, values.data(), nullptr, nullptr, binaryResult ? 1 : 0);
        const auto st = PQresultStatus(r);
        if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
            const std::string msg = PQresultErrorMessage(r);
            PQclear(r);
            throw RepositoryError("PostgreSQL: " + msg);
        }
        return Result(r);
    }
    /// One binary (bytea) parameter at index `binaryIndex`; the others are text.
    void execBinary(const std::string& sql, const Params& params, std::size_t binaryIndex, const std::string& bytes) const {
        std::vector<const char*> values;
        std::vector<int> lengths, formats;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i == binaryIndex) {
                values.push_back(bytes.data());
                lengths.push_back(static_cast<int>(bytes.size()));
                formats.push_back(1);
            } else {
                values.push_back(params[i] ? params[i]->c_str() : nullptr);
                lengths.push_back(0);
                formats.push_back(0);
            }
        }
        PGresult* r = PQexecParams(conn, sql.c_str(), static_cast<int>(params.size()), nullptr, values.data(), lengths.data(), formats.data(), 0);
        const auto st = PQresultStatus(r);
        if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
            const std::string msg = PQresultErrorMessage(r);
            PQclear(r);
            throw RepositoryError("PostgreSQL: " + msg);
        }
        PQclear(r);
    }

    struct Tx {
        const Impl& db;
        bool done = false;
        explicit Tx(const Impl& d) : db(d) { db.exec("BEGIN"); }
        void commit() {
            db.exec("COMMIT");
            done = true;
        }
        ~Tx() {
            if (!done) {
                PGresult* r = PQexec(db.conn, "ROLLBACK");
                PQclear(r);
            }
        }
    };

    int srid(const Uuid& project) const {
        const auto r = exec("SELECT srid FROM openmine.project WHERE id = $1::uuid", {project.toString()});
        if (r.rows() == 0) throw RepositoryError("Repository project " + project.toString() + " does not exist");
        return static_cast<int>(r.integer(0, 0));
    }

    void writeDocument(const Uuid& project, std::int64_t rev, int srid, const std::string& type, const std::string& k, const std::optional<std::string>& doc) const {
        if (!doc) {
            exec("INSERT INTO openmine.entity (project_id, entity_type, entity_key, revision, deleted) VALUES ($1::uuid,$2,$3,$4,true) "
                 "ON CONFLICT (project_id, entity_type, entity_key) DO UPDATE SET revision = EXCLUDED.revision, deleted = true, doc = NULL, "
                 "content_sha256 = NULL, name = NULL, geom = NULL",
                 {project.toString(), type, k, std::to_string(rev)});
            exec("INSERT INTO openmine.entity_history (project_id, entity_type, entity_key, revision, deleted) VALUES ($1::uuid,$2,$3,$4,true)",
                 {project.toString(), type, k, std::to_string(rev)});
            return;
        }
        const std::string hash = pe::documentHash(*doc);
        const std::string wkt = documentWkt(type, *doc);
        const std::string name = nameOf(json::parse(*doc));
        exec("INSERT INTO openmine.entity (project_id, entity_type, entity_key, revision, deleted, content_sha256, doc, name, geom) "
             "VALUES ($1::uuid,$2,$3,$4,false,$5,$6::jsonb,$7, CASE WHEN $8::text IS NULL THEN NULL ELSE ST_SetSRID(ST_GeomFromText($8::text), $9::int) END) "
             "ON CONFLICT (project_id, entity_type, entity_key) DO UPDATE SET revision = EXCLUDED.revision, deleted = false, "
             "content_sha256 = EXCLUDED.content_sha256, doc = EXCLUDED.doc, name = EXCLUDED.name, geom = EXCLUDED.geom",
             {project.toString(), type, k, std::to_string(rev), hash, *doc, name, wkt.empty() ? std::nullopt : std::optional<std::string>(wkt),
              std::to_string(srid)});
        exec("INSERT INTO openmine.entity_history (project_id, entity_type, entity_key, revision, deleted, content_sha256, doc) "
             "VALUES ($1::uuid,$2,$3,$4,false,$5,$6::jsonb)",
             {project.toString(), type, k, std::to_string(rev), hash, *doc});
    }

    /// Uploads hold a shared advisory lock so blob garbage collection (exclusive) never runs while
    /// another transaction is about to reference an existing blob.
    std::size_t uploadAsset(const Uuid& project, const AssetStore& store, const Asset& a) const {
        exec("SELECT pg_advisory_xact_lock_shared(hashtext('openmine.repository.blobs'))");
        std::size_t uploaded = 0;
        for (const auto& [rel, path] : assetFiles(store, a)) {
            const std::string bytes = readFile(path);
            const std::string sha = util::sha256Hex(bytes);
            if (exec("SELECT 1 FROM openmine.blob WHERE sha256 = $1", {sha}).rows() == 0) {
                execBinary("INSERT INTO openmine.blob (sha256, size, data) VALUES ($1, $2, $3) ON CONFLICT DO NOTHING", {sha, std::to_string(bytes.size()), std::nullopt}, 2,
                           bytes);
                ++uploaded;
            }
            exec("INSERT INTO openmine.asset_file (project_id, asset_key, rel_path, sha256, size) VALUES ($1::uuid,$2,$3,$4,$5) "
                 "ON CONFLICT (project_id, asset_key, rel_path) DO UPDATE SET sha256 = EXCLUDED.sha256, size = EXCLUDED.size",
                 {project.toString(), a.id.toString(), rel, sha, std::to_string(bytes.size())});
        }
        return uploaded;
    }

    void downloadAsset(const Uuid& project, AssetStore& store, const Asset& a) const {
        const auto r = exec("SELECT f.rel_path, f.sha256, b.data FROM openmine.asset_file f JOIN openmine.blob b ON b.sha256 = f.sha256 "
                            "WHERE f.project_id = $1::uuid AND f.asset_key = $2 ORDER BY f.rel_path",
                            {project.toString(), a.id.toString()}, true);
        if (r.rows() == 0) throw RepositoryError("Asset " + a.id.toString() + " (" + a.relativePath + ") has no stored files");
        const fs::path root = store.pathOf(a);
        for (int i = 0; i < r.rows(); ++i) {
            const std::string rel = r.text(i, 0), sha = r.text(i, 1), bytes = r.bytes(i, 2);
            if (util::sha256Hex(bytes) != sha) throw RepositoryError("Asset file " + rel + " failed its SHA-256 check");
            const fs::path target = a.isDirectory ? root / rel : root;
            fs::create_directories(target.parent_path());
            std::ofstream o(target, std::ios::binary);
            o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!o) throw RepositoryError("Cannot write " + target.string());
        }
        if (!a.isDirectory) {
            const auto h = util::sha256File(root);
            if (!h || *h != a.sha256) throw RepositoryError("Asset " + a.relativePath + " does not match its recorded SHA-256");
        }
    }
};

PgRepository::PgRepository(const std::string& connection) : impl_(std::make_unique<Impl>()) {
    impl_->conn = PQconnectdb(connection.c_str());
    if (PQstatus(impl_->conn) != CONNECTION_OK) {
        const std::string msg = PQerrorMessage(impl_->conn);
        throw RepositoryError("Cannot connect to the repository: " + msg);
    }
    // Remember the connection without any password (key/value form or URI user:password@).
    std::string safe;
    if (connection.rfind("postgres", 0) == 0 && connection.find("://") != std::string::npos) {
        safe = connection;
        const auto start = safe.find("://") + 3;
        const auto at = safe.find('@', start);
        const auto colon = safe.find(':', start);
        if (at != std::string::npos && colon != std::string::npos && colon < at) safe.erase(colon, at - colon);
        const auto q = safe.find("password=");
        if (q != std::string::npos) safe.erase(q, safe.find('&', q) == std::string::npos ? std::string::npos : safe.find('&', q) - q + 1);
    } else {
        std::istringstream ss(connection);
        std::string tok;
        while (ss >> tok)
            if (tok.rfind("password=", 0) != 0) safe += (safe.empty() ? "" : " ") + tok;
    }
    impl_->connection = safe;
    impl_->exec("SET client_min_messages = warning");
}

PgRepository::~PgRepository() = default;

void PgRepository::initialise() {
    auto& db = *impl_;
    Impl::Tx tx(db);
    // Serialise concurrent initialisations (several clients starting at once).
    db.exec("SELECT pg_advisory_xact_lock(hashtext('openmine.repository.schema'))");
    if (db.exec("SELECT 1 FROM pg_extension WHERE extname = 'postgis'").rows() == 0) {
        try {
            db.exec("CREATE EXTENSION IF NOT EXISTS postgis");
        } catch (const RepositoryError& e) {
            throw RepositoryError(std::string("PostGIS is required and could not be enabled (ask the database administrator to run "
                                              "CREATE EXTENSION postgis): ") +
                                  e.what());
        }
    }
    db.script(kSchemaSql);
    const auto r = db.exec("SELECT value FROM openmine.repo_meta WHERE key = 'schema_version'");
    if (r.rows() == 0) db.exec("INSERT INTO openmine.repo_meta VALUES ('schema_version', $1)", {std::to_string(kSchemaVersion)});
    else if (std::stoi(r.text(0, 0)) > kSchemaVersion)
        throw RepositoryError("The repository schema (version " + r.text(0, 0) + ") is newer than this OpenMine build supports (" + std::to_string(kSchemaVersion) + ")");
    tx.commit();
}

int PgRepository::schemaVersion() const {
    const auto r = impl_->exec("SELECT value FROM openmine.repo_meta WHERE key = 'schema_version'");
    return r.rows() ? std::stoi(r.text(0, 0)) : 0;
}
std::string PgRepository::serverVersion() const { return impl_->exec("SHOW server_version").text(0, 0); }
std::string PgRepository::postgisVersion() const { return impl_->exec("SELECT postgis_lib_version()").text(0, 0); }

WorkingCopy PgRepository::createProject(Project& p, const std::string& author, const std::string& message) {
    auto& db = *impl_;
    const auto& m = p.model();
    const Uuid id = m.info.id;
    const int srid = sridOf(m);
    const auto docs = allDocuments(m);
    Impl::Tx tx(db);
    if (db.exec("SELECT 1 FROM openmine.project WHERE id = $1::uuid", {id.toString()}).rows())
        throw RepositoryError("The repository already contains this project (" + id.toString() + "); check it out instead");
    db.exec("INSERT INTO openmine.project (id, name, srid, head_revision) VALUES ($1::uuid, $2, $3, 1)", {id.toString(), m.info.name, std::to_string(srid)});
    db.exec("INSERT INTO openmine.revision (project_id, revision, author, message, changes) VALUES ($1::uuid, 1, $2, $3, $4)",
            {id.toString(), author, message, std::to_string(docs.size())});
    WorkingCopy wc;
    wc.connection = db.connection;
    wc.projectId = id;
    wc.baseRevision = 1;
    for (const auto& d : docs) {
        db.writeDocument(id, 1, srid, d.type, d.key, d.json);
        wc.baseHashes[key(d.type, d.key)] = pe::documentHash(d.json);
    }
    for (const auto& a : m.assets.all()) db.uploadAsset(id, p.assets(), a);
    tx.commit();
    return wc;
}

std::vector<std::pair<Uuid, std::string>> PgRepository::projects() const {
    const auto r = impl_->exec("SELECT id, name FROM openmine.project ORDER BY name");
    std::vector<std::pair<Uuid, std::string>> out;
    for (int i = 0; i < r.rows(); ++i) out.emplace_back(*Uuid::parse(r.text(i, 0)), r.text(i, 1));
    return out;
}

void PgRepository::deleteProject(const Uuid& id) {
    auto& db = *impl_;
    Impl::Tx tx(db);
    db.exec("DELETE FROM openmine.entity WHERE project_id = $1::uuid", {id.toString()});
    db.exec("DELETE FROM openmine.entity_history WHERE project_id = $1::uuid", {id.toString()});
    db.exec("DELETE FROM openmine.project WHERE id = $1::uuid", {id.toString()});
    db.exec("SELECT pg_advisory_xact_lock(hashtext('openmine.repository.blobs'))");
    db.exec("DELETE FROM openmine.blob b WHERE NOT EXISTS (SELECT 1 FROM openmine.asset_file f WHERE f.sha256 = b.sha256)");
    tx.commit();
}

std::int64_t PgRepository::headRevision(const Uuid& id) const {
    const auto r = impl_->exec("SELECT head_revision FROM openmine.project WHERE id = $1::uuid", {id.toString()});
    if (r.rows() == 0) throw RepositoryError("Repository project " + id.toString() + " does not exist");
    return r.integer(0, 0);
}

std::vector<RevisionInfo> PgRepository::revisions(const Uuid& id) const {
    const auto r = impl_->exec("SELECT revision, author, message, to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), changes "
                               "FROM openmine.revision WHERE project_id = $1::uuid ORDER BY revision",
                               {id.toString()});
    std::vector<RevisionInfo> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back({r.integer(i, 0), r.text(i, 1), r.text(i, 2), r.text(i, 3), static_cast<int>(r.integer(i, 4))});
    return out;
}

std::vector<EntityVersion> PgRepository::history(const Uuid& id, const std::string& type, const std::string& k) const {
    const auto r = impl_->exec("SELECT h.revision, h.deleted, r.author, r.message, to_char(r.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                               "coalesce(h.content_sha256, '') FROM openmine.entity_history h JOIN openmine.revision r ON r.project_id = h.project_id AND "
                               "r.revision = h.revision WHERE h.project_id = $1::uuid AND h.entity_type = $2 AND h.entity_key = $3 ORDER BY h.revision",
                               {id.toString(), type, k});
    std::vector<EntityVersion> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back({r.integer(i, 0), r.boolean(i, 1), r.text(i, 2), r.text(i, 3), r.text(i, 4), r.text(i, 5)});
    return out;
}

std::unique_ptr<Project> PgRepository::checkout(const Uuid& id, WorkingCopy& wc, std::int64_t revision) {
    auto& db = *impl_;
    const std::int64_t head = headRevision(id);
    const std::int64_t rev = revision <= 0 ? head : revision;
    if (rev > head) throw RepositoryError("Revision " + std::to_string(rev) + " does not exist (head is " + std::to_string(head) + ")");
    const auto r = db.exec("SELECT entity_type, entity_key, doc::text FROM (SELECT DISTINCT ON (entity_type, entity_key) entity_type, entity_key, deleted, doc "
                           "FROM openmine.entity_history WHERE project_id = $1::uuid AND revision <= $2 ORDER BY entity_type, entity_key, revision DESC) s "
                           "WHERE NOT deleted",
                           {id.toString(), std::to_string(rev)});
    std::map<std::string, int> order;
    for (std::size_t i = 0; i < pe::documentTypes().size(); ++i) order[pe::documentTypes()[i]] = static_cast<int>(i);
    std::vector<pe::EntityDocument> docs;
    std::vector<std::pair<std::string, std::vector<std::string>>> orders;
    for (int i = 0; i < r.rows(); ++i) {
        if (r.text(i, 0) == kOrder) orders.emplace_back(r.text(i, 1), json::parse(r.text(i, 2)).get<std::vector<std::string>>());
        else docs.push_back({r.text(i, 0), r.text(i, 1), r.text(i, 2)});
    }
    std::stable_sort(docs.begin(), docs.end(), [&](const auto& a, const auto& b) { return order.at(a.type) < order.at(b.type); });
    auto p = std::make_unique<Project>();
    for (const auto& d : docs) pe::insertDocument(p->mutableModel(), d.type, d.key, d.json);
    for (const auto& [type, o] : orders) pe::reorderDirect(p->mutableModel(), type, o);
    p->assets() = AssetStore(AssetStore::temporaryRoot(p->model().info.id));
    for (const auto& a : p->model().assets.all()) db.downloadAsset(id, p->assets(), a);
    if (const auto problems = pe::checkModel(*p); !problems.empty()) throw RepositoryError("Checked-out revision is inconsistent: " + problems.front());
    wc = {};
    wc.connection = db.connection;
    wc.projectId = id;
    wc.baseRevision = rev;
    for (const auto& d : allDocuments(p->model())) wc.baseHashes[key(d.type, d.key)] = pe::documentHash(d.json);
    return p;
}

PullResult PgRepository::pull(Project& p, WorkingCopy& wc, ConflictPolicy policy) {
    auto& db = *impl_;
    PullResult res;
    res.fromRevision = wc.baseRevision;
    const std::int64_t head = headRevision(wc.projectId);
    res.toRevision = head;
    if (head == wc.baseRevision) return res;
    if (head < wc.baseRevision) throw RepositoryError("The working copy is ahead of the repository (was the project restored?)");
    const auto r = db.exec("SELECT e.entity_type, e.entity_key, e.deleted, e.doc::text, e.revision, rv.author, coalesce(e.name, '') FROM openmine.entity e "
                           "JOIN openmine.revision rv ON rv.project_id = e.project_id AND rv.revision = e.revision "
                           "WHERE e.project_id = $1::uuid AND e.revision > $2",
                           {wc.projectId.toString(), std::to_string(wc.baseRevision)});
    struct Remote {
        std::string type, k;
        std::optional<std::string> doc;
        std::int64_t revision;
        std::string author, name;
        bool conflict = false;
    };
    std::vector<Remote> remote;
    for (int i = 0; i < r.rows(); ++i) {
        Remote x{r.text(i, 0), r.text(i, 1), std::nullopt, r.integer(i, 4), r.text(i, 5), r.text(i, 6)};
        if (!r.boolean(i, 2)) {
            // Canonical text through the codec (jsonb does not preserve the original formatting).
            const std::string raw = r.text(i, 3);
            if (x.type == kOrder) x.doc = json::parse(raw).dump();
            else {
                Project scratch;
                pe::insertDocument(scratch.mutableModel(), x.type, x.k, raw);
                x.doc = pe::encodeDocument(scratch.model(), x.type, x.k);
            }
        }
        remote.push_back(std::move(x));
    }
    // Local state.
    std::map<std::string, std::string> localHash;
    for (const auto& d : allDocuments(p.model())) localHash[key(d.type, d.key)] = pe::documentHash(d.json);
    for (auto& x : remote) {
        if (x.type == kOrder) continue; // orders are merged, never in conflict
        const auto k = key(x.type, x.k);
        const auto b = wc.baseHashes.find(k);
        const auto l = localHash.find(k);
        const std::optional<std::string> baseH = b == wc.baseHashes.end() ? std::nullopt : std::optional<std::string>(b->second);
        const std::optional<std::string> localH = l == localHash.end() ? std::nullopt : std::optional<std::string>(l->second);
        if (localH == baseH) continue; // not modified locally
        const std::optional<std::string> remoteH = x.doc ? std::optional<std::string>(pe::documentHash(*x.doc)) : std::nullopt;
        if (localH == remoteH) continue; // same outcome on both sides
        x.conflict = true;
        res.conflicts.push_back({x.type, x.k, x.name,
                                 std::string(localH ? "changed" : "deleted") + " locally and " + (x.doc ? "changed" : "deleted") + " in revision " +
                                     std::to_string(x.revision) + " by " + x.author});
    }
    if (!res.conflicts.empty() && policy == ConflictPolicy::Abort) {
        res.aborted = true;
        return res;
    }
    // Assets needed by incoming Asset documents.
    for (const auto& x : remote)
        if (x.type == "Asset" && x.doc && (!x.conflict || policy == ConflictPolicy::TakeRemote)) {
            const auto a = pe::decodeEntity<Asset>(*x.doc);
            db.downloadAsset(wc.projectId, p.assets(), a);
        }
    std::map<std::string, int> order;
    for (std::size_t i = 0; i < pe::documentTypes().size(); ++i) order[pe::documentTypes()[i]] = static_cast<int>(i);
    order[kOrder] = static_cast<int>(order.size());
    std::stable_sort(remote.begin(), remote.end(), [&](const Remote& a, const Remote& b) {
        // Additions/updates in dependency order, then deletions in reverse dependency order.
        if (a.doc.has_value() != b.doc.has_value()) return a.doc.has_value();
        return a.doc ? order.at(a.type) < order.at(b.type) : order.at(a.type) > order.at(b.type);
    });
    std::map<std::string, std::vector<std::string>> beforePull;
    for (const auto& x : remote)
        if (x.type == kOrder) beforePull[x.k] = pe::entityOrder(p.model(), x.k);
    {
        CommandStack::MacroScope macro(p.commands(), "Pull revision " + std::to_string(head));
        for (const auto& x : remote) {
            if (x.type == kOrder) continue;
            if (x.conflict && policy == ConflictPolicy::KeepLocal) continue;
            if (auto cmd = pe::setDocumentCommand(p.model(), x.type, x.k, x.doc, "Pull " + x.type)) {
                p.commands().execute(std::move(cmd));
                ++res.applied;
            }
        }
        for (const auto& x : remote) {
            if (x.type != kOrder || !x.doc) continue;
            const auto local = pe::entityOrder(p.model(), x.k);
            const auto merged = mergeOrder(json::parse(*x.doc).get<std::vector<std::string>>(), local, beforePull[x.k]);
            if (merged != local) p.commands().execute(pe::reorderCommand(p.model(), x.k, merged, "Pull order of " + x.k));
        }
        if (const auto problems = pe::checkModel(p); !problems.empty())
            throw RepositoryError("Pull rolled back — the merged project would be inconsistent: " + problems.front());
        macro.commit();
    }
    for (const auto& x : remote) {
        const auto k = key(x.type, x.k);
        if (x.doc) wc.baseHashes[k] = pe::documentHash(*x.doc);
        else wc.baseHashes.erase(k);
    }
    wc.baseRevision = head;
    return res;
}

PublishResult PgRepository::publish(Project& p, WorkingCopy& wc, const std::string& author, const std::string& message) {
    auto& db = *impl_;
    PublishResult res;
    const auto changes = localChanges(p, wc);
    if (changes.empty()) {
        res.refusal = "Nothing to publish";
        return res;
    }
    if (const auto problems = pe::checkModel(p); !problems.empty()) {
        res.refusal = "The project is inconsistent: " + problems.front();
        return res;
    }
    Impl::Tx tx(db);
    const auto h = db.exec("SELECT head_revision FROM openmine.project WHERE id = $1::uuid FOR UPDATE", {wc.projectId.toString()});
    if (h.rows() == 0) throw RepositoryError("Repository project " + wc.projectId.toString() + " does not exist");
    const std::int64_t head = h.integer(0, 0);
    if (head != wc.baseRevision) {
        res.refusal = "Out of date: the repository is at revision " + std::to_string(head) + " (working copy at " + std::to_string(wc.baseRevision) +
                      "); pull first";
        return res;
    }
    // Locks held by others on changed entities.
    const auto lr = db.exec("SELECT entity_type, entity_key, holder FROM openmine.entity_lock WHERE project_id = $1::uuid AND expires_at > now() AND holder <> $2",
                            {wc.projectId.toString(), author});
    std::map<std::string, std::string> lockedBy;
    for (int i = 0; i < lr.rows(); ++i) lockedBy[key(lr.text(i, 0), lr.text(i, 1))] = lr.text(i, 2);
    for (const auto& c : changes)
        if (auto it = lockedBy.find(key(c.type, c.key)); it != lockedBy.end()) res.conflicts.push_back({c.type, c.key, "", "locked by " + it->second});
    if (!res.conflicts.empty()) return res;

    const std::int64_t rev = head + 1;
    const int srid = sridOf(p.model());
    db.exec("INSERT INTO openmine.revision (project_id, revision, author, message, changes) VALUES ($1::uuid,$2,$3,$4,$5)",
            {wc.projectId.toString(), std::to_string(rev), author, message, std::to_string(changes.size())});
    std::map<std::string, std::string> newHashes;
    for (const auto& c : changes) {
        const auto doc = c.kind == LocalChange::Kind::Deleted ? std::nullopt : documentOf(p.model(), c.type, c.key);
        db.writeDocument(wc.projectId, rev, srid, c.type, c.key, doc);
        if (doc) newHashes[key(c.type, c.key)] = pe::documentHash(*doc);
        if (c.type == "Asset" && doc) res.assetsUploaded += db.uploadAsset(wc.projectId, p.assets(), *p.model().assets.find(*Uuid::parse(c.key)));
    }
    const std::int64_t oldSrid = db.srid(wc.projectId);
    if (oldSrid != srid)
        db.exec("UPDATE openmine.entity SET geom = ST_SetSRID(geom, $2::int) WHERE project_id = $1::uuid AND geom IS NOT NULL",
                {wc.projectId.toString(), std::to_string(srid)});
    db.exec("UPDATE openmine.project SET head_revision = $2, srid = $3, name = $4 WHERE id = $1::uuid",
            {wc.projectId.toString(), std::to_string(rev), std::to_string(srid), p.model().info.name});
    tx.commit();
    for (const auto& c : changes) {
        const auto k = key(c.type, c.key);
        if (c.kind == LocalChange::Kind::Deleted) wc.baseHashes.erase(k);
        else wc.baseHashes[k] = newHashes.at(k);
    }
    wc.baseRevision = rev;
    res.published = true;
    res.revision = rev;
    res.changes = changes.size();
    return res;
}

namespace {
std::string typeFilter(const std::vector<std::string>& types, Params& params) {
    if (types.empty()) return {};
    std::string arr = "{";
    for (std::size_t i = 0; i < types.size(); ++i) arr += (i ? "," : "") + std::string("\"") + types[i] + "\"";
    arr += "}";
    params.push_back(arr);
    return " AND entity_type = ANY($" + std::to_string(params.size()) + "::text[])";
}
} // namespace

std::vector<SpatialHit> PgRepository::intersecting(const Uuid& id, double x0, double y0, double x1, double y1, const std::vector<std::string>& types) const {
    const int srid = impl_->srid(id);
    Params ps{id.toString(), fmt(x0), fmt(y0), fmt(x1), fmt(y1), std::to_string(srid)};
    const std::string tf = typeFilter(types, ps);
    const auto r = impl_->exec("SELECT entity_type, entity_key, coalesce(name,'') FROM openmine.entity WHERE project_id = $1::uuid AND NOT deleted AND geom IS NOT NULL "
                               "AND ST_Intersects(geom, ST_MakeEnvelope($2::float8,$3::float8,$4::float8,$5::float8,$6::int))" + tf +
                                   " ORDER BY entity_type, name, entity_key",
                               ps);
    std::vector<SpatialHit> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back({r.text(i, 0), r.text(i, 1), r.text(i, 2)});
    return out;
}

std::vector<SpatialHit> PgRepository::within(const Uuid& id, double x, double y, double d, const std::vector<std::string>& types) const {
    const int srid = impl_->srid(id);
    Params ps{id.toString(), fmt(x), fmt(y), fmt(d), std::to_string(srid)};
    const std::string tf = typeFilter(types, ps);
    const auto r = impl_->exec("SELECT entity_type, entity_key, coalesce(name,'') FROM openmine.entity WHERE project_id = $1::uuid AND NOT deleted AND geom IS NOT NULL "
                               "AND ST_DWithin(ST_Force2D(geom), ST_SetSRID(ST_MakePoint($2::float8,$3::float8),$5::int), $4::float8)" + tf +
                                   " ORDER BY entity_type, name, entity_key",
                               ps);
    std::vector<SpatialHit> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back({r.text(i, 0), r.text(i, 1), r.text(i, 2)});
    return out;
}

bool PgRepository::lock(const Uuid& id, const std::string& type, const std::string& k, const std::string& holder, std::chrono::seconds ttl) {
    const auto r = impl_->exec("INSERT INTO openmine.entity_lock (project_id, entity_type, entity_key, holder, expires_at) "
                               "VALUES ($1::uuid,$2,$3,$4, now() + make_interval(secs => $5::float8)) "
                               "ON CONFLICT (project_id, entity_type, entity_key) DO UPDATE SET holder = EXCLUDED.holder, acquired_at = now(), "
                               "expires_at = EXCLUDED.expires_at WHERE openmine.entity_lock.holder = EXCLUDED.holder OR openmine.entity_lock.expires_at <= now() "
                               "RETURNING holder",
                               {id.toString(), type, k, holder, std::to_string(ttl.count())});
    return r.rows() == 1;
}

void PgRepository::unlock(const Uuid& id, const std::string& type, const std::string& k, const std::string& holder) {
    impl_->exec("DELETE FROM openmine.entity_lock WHERE project_id = $1::uuid AND entity_type = $2 AND entity_key = $3 AND holder = $4",
                {id.toString(), type, k, holder});
}

std::vector<Lock> PgRepository::locks(const Uuid& id) const {
    const auto r = impl_->exec("SELECT entity_type, entity_key, holder, to_char(acquired_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                               "to_char(expires_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') FROM openmine.entity_lock "
                               "WHERE project_id = $1::uuid AND expires_at > now() ORDER BY entity_type, entity_key",
                               {id.toString()});
    std::vector<Lock> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back({r.text(i, 0), r.text(i, 1), r.text(i, 2), r.text(i, 3), r.text(i, 4)});
    return out;
}

} // namespace openmine::repository
