#pragma once
// SPDX-License-Identifier: Apache-2.0
// Declarative mapping between entity structs and SQLite tables (internal to openmine_core).
// One `Table<T>` drives CREATE TABLE, INSERT and SELECT so the three can never disagree.

#include "Sqlite.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/project/EntityStore.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace openmine::persistence::detail {

using nlohmann::json;

// ---- JSON helpers for nested value types -------------------------------------------------------
inline json toJson(Vec3 v) { return json::array({v.x, v.y, v.z}); }
inline Vec3 vec3FromJson(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }
inline json toJson(Vec2 v) { return json::array({v.x, v.y}); }
inline Vec2 vec2FromJson(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>()}; }

inline json parseJson(const std::string& s, const char* what) {
    if (s.empty()) return json();
    auto j = json::parse(s, nullptr, false);
    if (j.is_discarded()) throw PersistenceError(std::string("Corrupt JSON in ") + what);
    return j;
}

// ---- Binary arrays (little-endian; see static_assert in ProjectFile.cpp) ------------------------
template <class V> std::vector<unsigned char> packVector(const std::vector<V>& v) {
    std::vector<unsigned char> out(v.size() * sizeof(V));
    if (!v.empty()) std::memcpy(out.data(), v.data(), out.size());
    return out;
}
template <class V> std::vector<V> unpackVector(const std::vector<unsigned char>& b, const char* what) {
    if (b.size() % sizeof(V) != 0) throw PersistenceError(std::string("Corrupt binary data in ") + what);
    std::vector<V> out(b.size() / sizeof(V));
    if (!out.empty()) std::memcpy(out.data(), b.data(), b.size());
    return out;
}

// ---- Column / table ------------------------------------------------------------------------------
template <class T> struct Column {
    std::string name;
    std::string decl;
    std::function<void(Stmt&, int, const T&)> bind;
    std::function<void(const Stmt&, int, T&)> read;
};

template <class T> struct Table {
    std::string name;
    std::vector<Column<T>> columns;
    std::vector<std::string> constraints; ///< table constraints (UNIQUE, CHECK)
    std::vector<std::string> indices;     ///< "CREATE INDEX ..." statements

    std::string ddl() const {
        std::string s = "CREATE TABLE " + name + " (\n    uuid TEXT PRIMARY KEY,\n    sort_index INTEGER NOT NULL";
        for (const auto& c : columns) s += ",\n    " + c.name + " " + c.decl;
        for (const auto& c : constraints) s += ",\n    " + c;
        s += "\n);\n";
        for (const auto& i : indices) s += i + ";\n";
        return s;
    }

    void write(Db& db, const EntityStore<T>& store) const {
        std::string sql = "INSERT INTO " + name + " (uuid, sort_index";
        for (const auto& c : columns) sql += ", " + c.name;
        sql += ") VALUES (?, ?";
        for (std::size_t i = 0; i < columns.size(); ++i) sql += ", ?";
        sql += ")";
        Stmt st(db, sql);
        int idx = 0;
        for (const auto& e : store.all()) {
            st.bindUuid(1, e.id).bind(2, idx++);
            for (std::size_t i = 0; i < columns.size(); ++i) columns[i].bind(st, static_cast<int>(i) + 3, e);
            st.run();
        }
    }

    void read(Db& db, EntityStore<T>& store) const {
        std::string sql = "SELECT uuid";
        for (const auto& c : columns) sql += ", " + c.name;
        sql += " FROM " + name + " ORDER BY sort_index";
        Stmt st(db, sql);
        while (st.step()) {
            T e;
            e.id = st.uuid(0);
            for (std::size_t i = 0; i < columns.size(); ++i) columns[i].read(st, static_cast<int>(i) + 1, e);
            store.upsert(e);
        }
    }
};

// ---- Column builders --------------------------------------------------------------------------------
template <class T> Column<T> text(std::string n, std::string T::*m, std::string decl = "TEXT NOT NULL") {
    return {std::move(n), std::move(decl), [m](Stmt& s, int i, const T& e) { s.bind(i, e.*m); },
            [m](const Stmt& s, int i, T& e) { e.*m = s.text(i); }};
}
template <class T> Column<T> real(std::string n, double T::*m, std::string decl = "REAL NOT NULL") {
    return {std::move(n), std::move(decl), [m](Stmt& s, int i, const T& e) { s.bind(i, e.*m); },
            [m](const Stmt& s, int i, T& e) { e.*m = s.real(i); }};
}
template <class T> Column<T> integer(std::string n, int T::*m, std::string decl = "INTEGER NOT NULL") {
    return {std::move(n), std::move(decl), [m](Stmt& s, int i, const T& e) { s.bind(i, e.*m); },
            [m](const Stmt& s, int i, T& e) { e.*m = static_cast<int>(s.integer(i)); }};
}
template <class T> Column<T> int64(std::string n, std::int64_t T::*m, std::string decl = "INTEGER NOT NULL") {
    return {std::move(n), std::move(decl), [m](Stmt& s, int i, const T& e) { s.bind(i, e.*m); },
            [m](const Stmt& s, int i, T& e) { e.*m = s.integer(i); }};
}
template <class T> Column<T> boolean(std::string n, bool T::*m) {
    return {n, "INTEGER NOT NULL CHECK (" + n + " IN (0,1))", [m](Stmt& s, int i, const T& e) { s.bind(i, e.*m); },
            [m](const Stmt& s, int i, T& e) { e.*m = s.integer(i) != 0; }};
}
/// Reference to another entity table. Nil ids are stored as NULL.
template <class T>
Column<T> ref(std::string n, Uuid T::*m, const std::string& table, const std::string& onDelete = "SET NULL", bool notNull = false) {
    return {std::move(n), std::string("TEXT") + (notNull ? " NOT NULL" : "") + " REFERENCES " + table + "(uuid) ON DELETE " + onDelete,
            [m](Stmt& s, int i, const T& e) { s.bindUuid(i, e.*m); }, [m](const Stmt& s, int i, T& e) { e.*m = s.uuid(i); }};
}
/// Uuid stored without a foreign key (history / polymorphic references).
template <class T> Column<T> uuidCol(std::string n, Uuid T::*m, bool notNull = false) {
    return {std::move(n), notNull ? "TEXT NOT NULL" : "TEXT", [m](Stmt& s, int i, const T& e) { s.bindUuid(i, e.*m); },
            [m](const Stmt& s, int i, T& e) { e.*m = s.uuid(i); }};
}
template <class T> Column<T> assetRef(std::string n, AssetRef T::*m, bool notNull = false) {
    return {std::move(n), std::string("TEXT") + (notNull ? " NOT NULL" : "") + " REFERENCES asset(uuid) ON DELETE RESTRICT",
            [m](Stmt& s, int i, const T& e) { s.bindUuid(i, (e.*m).id); }, [m](const Stmt& s, int i, T& e) { (e.*m).id = s.uuid(i); }};
}
template <class T, NamedEnum E> Column<T> enumCol(std::string n, E T::*m) {
    std::string check = "TEXT NOT NULL CHECK (" + n + " IN (";
    bool first = true;
    for (const auto& name : enumNames<E>()) {
        check += (first ? "'" : ",'") + name + "'";
        first = false;
    }
    check += "))";
    return {std::move(n), check, [m](Stmt& s, int i, const T& e) { s.bind(i, toString(e.*m)); },
            [m, n](const Stmt& s, int i, T& e) {
                auto v = enumFromString<E>(s.text(i));
                if (!v) throw PersistenceError("Unknown value '" + s.text(i) + "' in column " + n);
                e.*m = *v;
            }};
}
template <class T> Column<T> dateCol(std::string n, Date T::*m) {
    return {std::move(n), "TEXT NOT NULL", [m](Stmt& s, int i, const T& e) { s.bind(i, (e.*m).toString()); },
            [m](const Stmt& s, int i, T& e) {
                auto d = Date::parse(s.text(i));
                if (!d) throw PersistenceError("Corrupt date '" + s.text(i) + "'");
                e.*m = *d;
            }};
}
template <class T> Column<T> optDateCol(std::string n, std::optional<Date> T::*m) {
    return {std::move(n), "TEXT", [m](Stmt& s, int i, const T& e) {
                if (e.*m) s.bind(i, (e.*m)->toString());
                else s.bindNull(i);
            },
            [m](const Stmt& s, int i, T& e) {
                if (s.isNull(i)) {
                    e.*m = std::nullopt;
                    return;
                }
                auto d = Date::parse(s.text(i));
                if (!d) throw PersistenceError("Corrupt date '" + s.text(i) + "'");
                e.*m = *d;
            }};
}
template <class T> Column<T> dateTimeCol(std::string n, DateTime T::*m) {
    return {std::move(n), "TEXT NOT NULL", [m](Stmt& s, int i, const T& e) { s.bind(i, (e.*m).toString()); },
            [m](const Stmt& s, int i, T& e) {
                auto d = DateTime::parse(s.text(i));
                if (!d) throw PersistenceError("Corrupt date-time '" + s.text(i) + "'");
                e.*m = *d;
            }};
}
template <class T> Column<T> optDateTimeCol(std::string n, std::optional<DateTime> T::*m) {
    return {std::move(n), "TEXT", [m](Stmt& s, int i, const T& e) {
                if (e.*m) s.bind(i, (e.*m)->toString());
                else s.bindNull(i);
            },
            [m](const Stmt& s, int i, T& e) {
                if (s.isNull(i)) {
                    e.*m = std::nullopt;
                    return;
                }
                auto d = DateTime::parse(s.text(i));
                if (!d) throw PersistenceError("Corrupt date-time '" + s.text(i) + "'");
                e.*m = *d;
            }};
}
/// Appends three REAL columns <prefix>_x, <prefix>_y, <prefix>_z.
template <class T> void addXyz(Table<T>& t, const std::string& prefix, Vec3 T::*m) {
    for (int k = 0; k < 3; ++k) {
        static const char* sfx[] = {"_x", "_y", "_z"};
        t.columns.push_back({prefix + sfx[k], "REAL NOT NULL", [m, k](Stmt& s, int i, const T& e) { s.bind(i, (e.*m)[k]); },
                             [m, k](const Stmt& s, int i, T& e) { (e.*m)[k] = s.real(i); }});
    }
}
template <class T> Column<T> transformCol(std::string n, Transform3 T::*m) {
    return {std::move(n), "TEXT NOT NULL", [m](Stmt& s, int i, const T& e) { s.bind(i, (e.*m).toString()); },
            [m](const Stmt& s, int i, T& e) {
                auto t = Transform3::parse(s.text(i));
                if (!t) throw PersistenceError("Corrupt transform");
                e.*m = *t;
            }};
}
template <class T> Column<T> meta3(int which) {
    static const char* names[] = {"created_at", "modified_at", "revision"};
    if (which == 2)
        return {names[2], "INTEGER NOT NULL", [](Stmt& s, int i, const T& e) { s.bind(i, e.meta.revision); },
                [](const Stmt& s, int i, T& e) { e.meta.revision = s.integer(i); }};
    if (which == 0)
        return {names[0], "TEXT NOT NULL", [](Stmt& s, int i, const T& e) { s.bind(i, e.meta.createdAt); },
                [](const Stmt& s, int i, T& e) { e.meta.createdAt = s.text(i); }};
    return {names[1], "TEXT NOT NULL", [](Stmt& s, int i, const T& e) { s.bind(i, e.meta.modifiedAt); },
            [](const Stmt& s, int i, T& e) { e.meta.modifiedAt = s.text(i); }};
}
template <class T> void addMeta(Table<T>& t) {
    t.columns.push_back(meta3<T>(0));
    t.columns.push_back(meta3<T>(1));
    t.columns.push_back(meta3<T>(2));
}
template <class T>
Column<T> custom(std::string n, std::string decl, std::function<void(Stmt&, int, const T&)> b, std::function<void(const Stmt&, int, T&)> r) {
    return {std::move(n), std::move(decl), std::move(b), std::move(r)};
}

} // namespace openmine::persistence::detail
