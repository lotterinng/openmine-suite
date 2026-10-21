// SPDX-License-Identifier: Apache-2.0
// Asset-store compaction (see ProjectFile.h).
#include "openmine/persistence/ProjectFile.h"

#include "openmine/assets/AssetStore.h"
#include "openmine/commands/CommandStack.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/project/ProjectEditor.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace openmine::persistence {

namespace fs = std::filesystem;

namespace {
std::int64_t sizeOf(const fs::path& p) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return static_cast<std::int64_t>(fs::file_size(p, ec));
    std::int64_t n = 0;
    for (const auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec))
        if (e.is_regular_file(ec)) n += static_cast<std::int64_t>(e.file_size(ec));
    return n;
}
std::string generic(const fs::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
} // namespace

CompactReport compactProject(Project& project, bool dryRun) {
    if (project.filePath().empty()) throw PersistenceError("Save the project before compacting it");
    const auto& m = project.model();
    const fs::path root = project.assets().root();
    CompactReport rep;
    rep.dryRun = dryRun;

    // Registered assets nothing refers to.
    const auto refs = m.referencedAssets(); // sorted
    std::vector<Asset> unused;
    for (const auto& a : m.assets.all())
        if (!std::binary_search(refs.begin(), refs.end(), a.id)) {
            unused.push_back(a);
            rep.unreferencedAssets.push_back(a.relativePath);
            rep.bytes += sizeOf(project.assets().pathOf(a));
        }

    // Files in the kind folders that are not registered assets (cancelled imports, aborted builds).
    std::set<std::string> registered;
    for (const auto& a : m.assets.all()) registered.insert(a.id.toString());
    std::vector<fs::path> orphans;
    for (const auto& kind : enumNames<AssetKind>()) {
        std::string folder = kind;
        std::transform(folder.begin(), folder.end(), folder.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::error_code ec;
        const fs::path dir = root / folder;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::string stem = e.path().filename().string();
            if (const auto dot = stem.find('.'); dot != std::string::npos) stem = stem.substr(0, dot);
            if (registered.count(stem)) continue;
            orphans.push_back(e.path());
            rep.orphanFiles.push_back(generic(fs::relative(e.path(), root, ec)));
            rep.bytes += sizeOf(e.path());
        }
    }
    if (dryRun || (unused.empty() && orphans.empty())) return rep;

    // Unregister the unused assets, forget the undo history, save — then delete the files.
    if (!unused.empty()) {
        ProjectEditor ed(project);
        CommandStack::MacroScope macro(project.commands(), "Compact project");
        for (const auto& a : unused) ed.remove<Asset>(a.id);
        macro.commit();
    }
    project.commands().clear();
    project.appendAudit("Compact", "Project", m.info.id,
                        std::to_string(unused.size()) + " unreferenced asset(s) and " + std::to_string(orphans.size()) + " unregistered file(s), " +
                            std::to_string(rep.bytes) + " bytes");
    saveProject(project, project.filePath());
    for (const auto& a : unused) project.assets().remove(a);
    for (const auto& o : orphans) {
        std::error_code ec;
        fs::remove_all(o, ec);
    }
    return rep;
}

} // namespace openmine::persistence
