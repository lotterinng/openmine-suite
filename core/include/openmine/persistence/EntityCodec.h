#pragma once
// SPDX-License-Identifier: Apache-2.0
// Entity documents: a lossless JSON form of every entity, produced by the same column mapping as
// the project file (so the two can never disagree), plus child rows (attributes, registration
// pairs, baseline rows, scenario overrides, drillhole surveys and intervals). Used by shared
// repositories (PostgreSQL) for storage, diffing and merging. Non-finite reals are encoded as
// {"$f": "nan"|"inf"|"-inf"} and binary columns as {"$b64": …}. Encoding is deterministic, so a
// document's SHA-256 identifies its content.
//
// Besides the entity types, two model-level documents exist: "ProjectInfo" (key "project") and
// "ActivityDesignLink" (key "<activity uuid>/<design uuid>").

#include "openmine/commands/CommandStack.h"
#include "openmine/project/Project.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openmine::persistence {

template <class T> std::string encodeEntity(const T& entity);
template <class T> T decodeEntity(const std::string& document);

struct EntityDocument {
    std::string type;  ///< entity type name (EntityTraits<T>::name), "ProjectInfo" or "ActivityDesignLink"
    std::string key;   ///< uuid, "project", or "<activity>/<design>"
    std::string json;
};

/// Document types in dependency order (referenced types first).
const std::vector<std::string>& documentTypes();
/// Every document of the model, in dependency order.
std::vector<EntityDocument> encodeModel(const ProjectModel& model);
/// One document (nullopt when the entity does not exist).
std::optional<std::string> encodeDocument(const ProjectModel& model, const std::string& type, const std::string& key);
/// Undoable command setting a document to `json` (nullopt = delete). Values are applied verbatim
/// (metadata included); referential integrity is the caller's responsibility (see checkModel).
std::unique_ptr<Command> setDocumentCommand(const ProjectModel& model, const std::string& type, const std::string& key,
                                            const std::optional<std::string>& json, const std::string& text);
/// Inserts documents directly into an empty model (no commands) — for building a checkout.
void insertDocument(ProjectModel& model, const std::string& type, const std::string& key, const std::string& json);
/// Order of the entities of a type (uuid strings) — entity order is meaningful (layer order,
/// activity order) and is exchanged separately from the entity documents.
std::vector<std::string> entityOrder(const ProjectModel& model, const std::string& type);
/// Undoable command reordering a type's entities (listed ids first, others after in their order).
std::unique_ptr<Command> reorderCommand(const ProjectModel& model, const std::string& type, const std::vector<std::string>& order,
                                        const std::string& text);
/// Reorders directly (no command) — for building a checkout.
void reorderDirect(ProjectModel& model, const std::string& type, const std::vector<std::string>& order);
/// Referential-integrity problems of a model (dangling references), as messages.
std::vector<std::string> checkModel(const Project& project);
/// SHA-256 (hex) of a document.
std::string documentHash(const std::string& json);

} // namespace openmine::persistence
