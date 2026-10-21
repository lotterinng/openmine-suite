// SPDX-License-Identifier: Apache-2.0
#include "openmine/project/Attachments.h"

#include "openmine/assets/AssetStore.h"
#include "openmine/commands/CommandStack.h"
#include "openmine/project/EntityTypes.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/util/Sha256.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace openmine {

namespace fs = std::filesystem;

namespace {
std::string lowerExtension(const fs::path& f) {
    std::string e = f.extension().string();
    if (!e.empty() && e.front() == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}
} // namespace

bool entityExists(const ProjectModel& m, const std::string& type, const Uuid& id) {
#define OPENMINE_ENTITY_EXISTS(T, member, Kind) \
    if (type == #T) return m.member.contains(id);
    OPENMINE_ENTITY_TYPES(OPENMINE_ENTITY_EXISTS)
#undef OPENMINE_ENTITY_EXISTS
    return false;
}

std::string mimeTypeFor(const fs::path& file) {
    static const std::map<std::string, std::string> types = {
        {"pdf", "application/pdf"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"tif", "image/tiff"},
        {"tiff", "image/tiff"},
        {"bmp", "image/bmp"},
        {"webp", "image/webp"},
        {"svg", "image/svg+xml"},
        {"txt", "text/plain"},
        {"csv", "text/csv"},
        {"md", "text/markdown"},
        {"html", "text/html"},
        {"htm", "text/html"},
        {"json", "application/json"},
        {"xml", "application/xml"},
        {"zip", "application/zip"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"odt", "application/vnd.oasis.opendocument.text"},
        {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
        {"dxf", "image/vnd.dxf"},
        {"las", "application/vnd.las"},
        {"laz", "application/vnd.laszip"},
        {"e57", "model/e57"},
        {"mp4", "video/mp4"},
        {"mov", "video/quicktime"},
    };
    const auto it = types.find(lowerExtension(file));
    return it == types.end() ? "application/octet-stream" : it->second;
}

Uuid attachFile(Project& project, const std::string& entityType, const Uuid& entityId, const fs::path& file, const std::string& description) {
    if (!entityId.isNil() && !entityExists(project.model(), entityType, entityId))
        throw ValidationError("Cannot attach to " + (entityType.empty() ? std::string("an entity") : entityType) + " " + entityId.toString() +
                              ": it does not exist");
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) throw AssetError("Not a file: " + file.string());
    Asset asset = project.assets().importFile(file, AssetKind::Attachment, lowerExtension(file));
    try {
        ProjectEditor ed(project);
        CommandStack::MacroScope macro(project.commands(), "Attach " + file.filename().string());
        ed.add(asset);
        Attachment a;
        a.entityId = entityId;
        a.entityType = entityId.isNil() ? std::string("Project") : entityType;
        a.fileName = file.filename().string();
        a.mimeType = mimeTypeFor(file);
        a.description = description;
        a.asset.id = asset.id;
        const Uuid id = ed.add(std::move(a));
        macro.commit();
        return id;
    } catch (...) {
        project.assets().remove(asset); // nothing refers to the copy
        throw;
    }
}

void saveAttachment(const Project& project, const Uuid& attachmentId, const fs::path& destination) {
    const auto* a = project.model().attachments.find(attachmentId);
    if (!a) throw ValidationError("Attachment does not exist");
    const auto* asset = project.model().assets.find(a->asset.id);
    if (!asset) throw AssetError("The attachment's stored file is not registered in the project");
    const fs::path src = project.assets().pathOf(*asset);
    const auto hash = util::sha256File(src);
    if (!hash) throw AssetError("The stored file of " + a->fileName + " is missing: " + src.string());
    if (*hash != asset->sha256) throw AssetError("The stored file of " + a->fileName + " was modified outside OpenMine (SHA-256 mismatch)");
    std::error_code ec;
    if (destination.has_parent_path()) fs::create_directories(destination.parent_path(), ec);
    fs::copy_file(src, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) throw AssetError("Cannot write " + destination.string() + ": " + ec.message());
}

std::vector<const Attachment*> attachmentsOf(const Project& project, const Uuid& entityId) {
    std::vector<const Attachment*> out;
    for (const auto& a : project.model().attachments.all())
        if (a.entityId == entityId) out.push_back(&a);
    return out;
}

} // namespace openmine
