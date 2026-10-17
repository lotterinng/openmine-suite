// SPDX-License-Identifier: Apache-2.0
#include "openmine/persistence/EntityCodec.h"

#include "SchemaV2.h"
#include "openmine/commands/EntityCommands.h"
#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/util/Sha256.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>

namespace openmine::persistence {

using nlohmann::json;

template <class T> std::string encodeEntity(const T& e) { return detail::encodeEntityV2(e); }
template <class T> T decodeEntity(const std::string& d) { return detail::decodeEntityV2<T>(d); }

#define OPENMINE_INSTANTIATE_PUBLIC_CODEC(T, member, Kind)       \
    template std::string encodeEntity<T>(const T&);              \
    template T decodeEntity<T>(const std::string&);
OPENMINE_ENTITY_TYPES(OPENMINE_INSTANTIATE_PUBLIC_CODEC)
#undef OPENMINE_INSTANTIATE_PUBLIC_CODEC

namespace {
constexpr const char* kInfo = "ProjectInfo";
constexpr const char* kLink = "ActivityDesignLink";

std::string encodeInfo(const ProjectInfo& i) {
    json j{{"uuid", i.id.toString()},
           {"name", i.name},
           {"description", i.description},
           {"schedule_start", i.scheduleStart.toString()},
           {"status_date", i.statusDate ? json(i.statusDate->toString()) : json(nullptr)},
           {"project_crs", i.projectCrsId.isNil() ? json(nullptr) : json(i.projectCrsId.toString())},
           {"default_calendar", i.defaultCalendarId.isNil() ? json(nullptr) : json(i.defaultCalendarId.toString())},
           {"coordinate_display_decimals", i.coordinateDisplayDecimals},
           {"linear_unit", i.linearUnit},
           {"south_orientated_axes", i.southOrientatedAxes},
           {"meta", {i.meta.createdAt, i.meta.modifiedAt, i.meta.revision}}};
    return j.dump();
}

Uuid uuidOf(const json& j) {
    if (j.is_null()) return {};
    auto u = Uuid::parse(j.get<std::string>());
    if (!u) throw PersistenceError("Corrupt identifier in document");
    return *u;
}

ProjectInfo decodeInfo(const std::string& s) {
    const json j = json::parse(s);
    ProjectInfo i;
    i.id = uuidOf(j.at("uuid"));
    i.name = j.at("name");
    i.description = j.at("description");
    auto st = DateTime::parse(j.at("schedule_start").get<std::string>());
    if (!st) throw PersistenceError("Corrupt schedule start in document");
    i.scheduleStart = *st;
    if (!j.at("status_date").is_null()) {
        auto sd = DateTime::parse(j.at("status_date").get<std::string>());
        if (!sd) throw PersistenceError("Corrupt status date in document");
        i.statusDate = *sd;
    }
    i.projectCrsId = uuidOf(j.at("project_crs"));
    i.defaultCalendarId = uuidOf(j.at("default_calendar"));
    i.coordinateDisplayDecimals = j.at("coordinate_display_decimals");
    i.linearUnit = j.at("linear_unit");
    i.southOrientatedAxes = j.at("south_orientated_axes");
    i.meta = {j.at("meta").at(0), j.at("meta").at(1), j.at("meta").at(2)};
    return i;
}

std::string linkKey(const ActivityDesignLink& l) { return l.activityId.toString() + "/" + l.designId.toString(); }
std::string encodeLink(const ActivityDesignLink& l) {
    return json{{"activity", l.activityId.toString()}, {"design", l.designId.toString()}, {"design_type", l.designType}}.dump();
}
ActivityDesignLink decodeLink(const std::string& s) {
    const json j = json::parse(s);
    return {uuidOf(j.at("activity")), uuidOf(j.at("design")), j.at("design_type")};
}

Uuid keyUuid(const std::string& key) {
    auto u = Uuid::parse(key);
    if (!u) throw PersistenceError("Invalid entity key '" + key + "'");
    return *u;
}

/// Per-type dispatch table.
struct TypeOps {
    std::function<void(const ProjectModel&, std::vector<EntityDocument>&)> encodeAll;
    std::function<std::optional<std::string>(const ProjectModel&, const std::string&)> encodeOne;
    std::function<std::unique_ptr<Command>(const ProjectModel&, const std::string&, const std::optional<std::string>&, const std::string&)> set;
    std::function<void(ProjectModel&, const std::string&, const std::string&)> insert;
    std::function<std::vector<std::string>(const ProjectModel&)> order;
    std::function<void(ProjectModel&, const std::vector<std::string>&)> reorder;
};

template <class T> class ReorderCommand : public Command {
public:
    ReorderCommand(std::vector<Uuid> before, std::vector<Uuid> after, std::string text)
        : before_(std::move(before)), after_(std::move(after)), text_(std::move(text)) {}
    void redo(Project& p) override { apply(p, after_); }
    void undo(Project& p) override { apply(p, before_); }
    std::string text() const override { return text_; }

private:
    void apply(Project& p, const std::vector<Uuid>& order) {
        EntityTraits<T>::store(p.mutableModel()).reorder(order);
        for (const auto& id : order) p.events().publish({EntityTraits<T>::kind, ChangeType::Modified, id});
    }
    std::vector<Uuid> before_, after_;
    std::string text_;
};

std::vector<Uuid> toUuids(const std::vector<std::string>& keys) {
    std::vector<Uuid> v;
    for (const auto& k : keys)
        if (auto u = Uuid::parse(k)) v.push_back(*u);
    return v;
}

template <class T> TypeOps opsFor() {
    TypeOps o;
    o.encodeAll = [](const ProjectModel& m, std::vector<EntityDocument>& out) {
        for (const auto& e : EntityTraits<T>::store(m).all()) out.push_back({EntityTraits<T>::name, e.id.toString(), encodeEntity(e)});
    };
    o.encodeOne = [](const ProjectModel& m, const std::string& key) -> std::optional<std::string> {
        const T* e = EntityTraits<T>::store(m).find(keyUuid(key));
        if (!e) return std::nullopt;
        return encodeEntity(*e);
    };
    o.set = [](const ProjectModel& m, const std::string& key, const std::optional<std::string>& doc, const std::string& text) -> std::unique_ptr<Command> {
        const T* cur = EntityTraits<T>::store(m).find(keyUuid(key));
        std::optional<T> before = cur ? std::optional<T>(*cur) : std::nullopt;
        std::optional<T> after;
        if (doc) {
            after = decodeEntity<T>(*doc);
            if (after->id != keyUuid(key)) throw PersistenceError("Document key does not match its uuid");
        }
        if (!before && !after) return nullptr;
        return std::make_unique<SetEntityCommand<T>>(std::move(before), std::move(after), text);
    };
    o.insert = [](ProjectModel& m, const std::string& key, const std::string& doc) {
        T e = decodeEntity<T>(doc);
        if (e.id != keyUuid(key)) throw PersistenceError("Document key does not match its uuid");
        EntityTraits<T>::store(m).upsert(e);
    };
    o.order = [](const ProjectModel& m) {
        std::vector<std::string> v;
        for (const auto& e : EntityTraits<T>::store(m).all()) v.push_back(e.id.toString());
        return v;
    };
    o.reorder = [](ProjectModel& m, const std::vector<std::string>& keys) { EntityTraits<T>::store(m).reorder(toUuids(keys)); };
    return o;
}

template <class T> std::unique_ptr<Command> makeReorder(const ProjectModel& m, const std::vector<std::string>& keys, const std::string& text) {
    std::vector<Uuid> before;
    for (const auto& e : EntityTraits<T>::store(m).all()) before.push_back(e.id);
    return std::make_unique<ReorderCommand<T>>(std::move(before), toUuids(keys), text);
}

const std::map<std::string, TypeOps>& ops() {
    static const std::map<std::string, TypeOps> m = [] {
        std::map<std::string, TypeOps> r;
#define OPENMINE_REGISTER_OPS(T, member, Kind) r.emplace(EntityTraits<T>::name, opsFor<T>());
        OPENMINE_ENTITY_TYPES(OPENMINE_REGISTER_OPS)
#undef OPENMINE_REGISTER_OPS
        return r;
    }();
    return m;
}
} // namespace

const std::vector<std::string>& documentTypes() {
    static const std::vector<std::string> t = [] {
        std::vector<std::string> v;
#define OPENMINE_TYPE_NAME(T, member, Kind) v.push_back(EntityTraits<T>::name);
        OPENMINE_ENTITY_TYPES(OPENMINE_TYPE_NAME)
#undef OPENMINE_TYPE_NAME
        v.push_back(kInfo);
        v.push_back(kLink);
        return v;
    }();
    return t;
}

std::vector<EntityDocument> encodeModel(const ProjectModel& m) {
    std::vector<EntityDocument> out;
    for (const auto& t : documentTypes()) {
        if (t == kInfo) out.push_back({kInfo, "project", encodeInfo(m.info)});
        else if (t == kLink)
            for (const auto& l : m.designLinks) out.push_back({kLink, linkKey(l), encodeLink(l)});
        else ops().at(t).encodeAll(m, out);
    }
    return out;
}

std::optional<std::string> encodeDocument(const ProjectModel& m, const std::string& type, const std::string& key) {
    if (type == kInfo) return encodeInfo(m.info);
    if (type == kLink) {
        for (const auto& l : m.designLinks)
            if (linkKey(l) == key) return encodeLink(l);
        return std::nullopt;
    }
    const auto it = ops().find(type);
    if (it == ops().end()) throw PersistenceError("Unknown document type '" + type + "'");
    return it->second.encodeOne(m, key);
}

std::unique_ptr<Command> setDocumentCommand(const ProjectModel& m, const std::string& type, const std::string& key, const std::optional<std::string>& doc,
                                            const std::string& text) {
    if (type == kInfo) {
        if (!doc) throw PersistenceError("Project information cannot be deleted");
        return std::make_unique<ProjectInfoCommand>(m.info, decodeInfo(*doc), text);
    }
    if (type == kLink) {
        bool present = false;
        for (const auto& l : m.designLinks)
            if (linkKey(l) == key) present = true;
        if (doc) {
            const auto l = decodeLink(*doc);
            if (linkKey(l) != key) throw PersistenceError("Document key does not match the link");
            return present ? nullptr : std::make_unique<DesignLinkCommand>(l, true);
        }
        if (!present) return nullptr;
        for (const auto& l : m.designLinks)
            if (linkKey(l) == key) return std::make_unique<DesignLinkCommand>(l, false);
        return nullptr;
    }
    const auto it = ops().find(type);
    if (it == ops().end()) throw PersistenceError("Unknown document type '" + type + "'");
    return it->second.set(m, key, doc, text);
}

void insertDocument(ProjectModel& m, const std::string& type, const std::string& key, const std::string& doc) {
    if (type == kInfo) {
        m.info = decodeInfo(doc);
        return;
    }
    if (type == kLink) {
        m.designLinks.push_back(decodeLink(doc));
        return;
    }
    const auto it = ops().find(type);
    if (it == ops().end()) throw PersistenceError("Unknown document type '" + type + "'");
    it->second.insert(m, key, doc);
}

std::vector<std::string> entityOrder(const ProjectModel& m, const std::string& type) {
    const auto it = ops().find(type);
    if (it == ops().end()) throw PersistenceError("Unknown entity type '" + type + "'");
    return it->second.order(m);
}

std::unique_ptr<Command> reorderCommand(const ProjectModel& m, const std::string& type, const std::vector<std::string>& order, const std::string& text) {
#define OPENMINE_REORDER(T, member, Kind) \
    if (type == EntityTraits<T>::name) return makeReorder<T>(m, order, text);
    OPENMINE_ENTITY_TYPES(OPENMINE_REORDER)
#undef OPENMINE_REORDER
    throw PersistenceError("Unknown entity type '" + type + "'");
}

void reorderDirect(ProjectModel& m, const std::string& type, const std::vector<std::string>& order) {
    const auto it = ops().find(type);
    if (it == ops().end()) throw PersistenceError("Unknown entity type '" + type + "'");
    it->second.reorder(m, order);
}

std::vector<std::string> checkModel(const Project& p) {
    std::vector<std::string> problems;
    const ProjectEditor ed(const_cast<Project&>(p));
    auto check = [&](const auto& store, const char* type) {
        for (const auto& e : store.all()) {
            try {
                ed.validate(e);
            } catch (const std::exception& ex) {
                problems.push_back(std::string(type) + " " + e.id.toString() + ": " + ex.what());
            }
        }
    };
#define OPENMINE_CHECK_STORE(T, member, Kind) check(p.model().member, #T);
    OPENMINE_ENTITY_TYPES(OPENMINE_CHECK_STORE)
#undef OPENMINE_CHECK_STORE
    const auto& m = p.model();
    for (const auto& l : m.designLinks) {
        if (!m.activities.find(l.activityId)) problems.push_back("Design link to a missing activity " + l.activityId.toString());
        if (!m.developments.find(l.designId) && !m.stopes.find(l.designId) && !m.miningBlocks.find(l.designId))
            problems.push_back("Design link to a missing design object " + l.designId.toString());
    }
    if (!m.info.projectCrsId.isNil() && !m.crs.find(m.info.projectCrsId)) problems.push_back("Project CRS is missing");
    if (!m.info.defaultCalendarId.isNil() && !m.calendars.find(m.info.defaultCalendarId)) problems.push_back("Default calendar is missing");
    return problems;
}

std::string documentHash(const std::string& doc) { return util::sha256Hex(doc); }

} // namespace openmine::persistence
