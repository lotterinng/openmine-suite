#pragma once
// SPDX-License-Identifier: Apache-2.0
// Minimal RAII wrapper over the SQLite C API (internal to openmine_core).

#include "openmine/persistence/ProjectFile.h"

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openmine::persistence::detail {

class Db {
public:
    Db(const std::filesystem::path& file, int flags) {
        const auto u8 = file.u8string();
        const std::string path(u8.begin(), u8.end());
        if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
            sqlite3_close(db_);
            db_ = nullptr;
            throw PersistenceError("Cannot open '" + path + "': " + msg);
        }
        sqlite3_extended_result_codes(db_, 1);
    }
    ~Db() { sqlite3_close(db_); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    sqlite3* get() const { return db_; }

    void exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw PersistenceError("SQL error: " + msg);
        }
    }

private:
    sqlite3* db_ = nullptr;
};

class Stmt {
public:
    Stmt(Db& db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
            throw PersistenceError(std::string("SQL prepare failed: ") + sqlite3_errmsg(db.get()) + " in: " + sql);
    }
    ~Stmt() { sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, const std::string& v) { check(sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT)); return *this; }
    Stmt& bind(int i, const char* v) { return bind(i, std::string(v)); }
    Stmt& bind(int i, double v) { check(sqlite3_bind_double(stmt_, i, v)); return *this; }
    Stmt& bind(int i, std::int64_t v) { check(sqlite3_bind_int64(stmt_, i, v)); return *this; }
    Stmt& bind(int i, int v) { return bind(i, static_cast<std::int64_t>(v)); }
    Stmt& bind(int i, bool v) { return bind(i, static_cast<std::int64_t>(v ? 1 : 0)); }
    Stmt& bindNull(int i) { check(sqlite3_bind_null(stmt_, i)); return *this; }
    Stmt& bindUuid(int i, const Uuid& id) { return id.isNil() ? bindNull(i) : bind(i, id.toString()); }
    Stmt& bindBlob(int i, const void* data, int size) { check(sqlite3_bind_blob(stmt_, i, data, size, SQLITE_TRANSIENT)); return *this; }

    /// Returns true when a row is available.
    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw PersistenceError(std::string("SQL step failed: ") + sqlite3_errmsg(db_.get()));
    }
    void run() {
        step();
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }

    bool isNull(int c) const { return sqlite3_column_type(stmt_, c) == SQLITE_NULL; }
    std::string text(int c) const {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, c));
        return p ? std::string(p, static_cast<std::size_t>(sqlite3_column_bytes(stmt_, c))) : std::string();
    }
    double real(int c) const { return sqlite3_column_double(stmt_, c); }
    std::int64_t integer(int c) const { return sqlite3_column_int64(stmt_, c); }
    Uuid uuid(int c) const {
        if (isNull(c)) return {};
        auto id = Uuid::parse(text(c));
        if (!id) throw PersistenceError("Corrupt identifier in project file: '" + text(c) + "'");
        return *id;
    }
    std::vector<unsigned char> blob(int c) const {
        auto p = static_cast<const unsigned char*>(sqlite3_column_blob(stmt_, c));
        const int n = sqlite3_column_bytes(stmt_, c);
        return p ? std::vector<unsigned char>(p, p + n) : std::vector<unsigned char>{};
    }

private:
    void check(int rc) {
        if (rc != SQLITE_OK) throw PersistenceError(std::string("SQL bind failed: ") + sqlite3_errmsg(db_.get()));
    }
    Db& db_;
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace openmine::persistence::detail
