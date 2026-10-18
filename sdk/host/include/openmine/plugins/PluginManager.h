#pragma once
// SPDX-License-Identifier: Apache-2.0
// Host side of the native plugin interface (sdk/include/openmine_plugin.h): discovery from plugin
// folders (manifest + shared library), version checks, loading/unloading, the registries of
// plugin commands, importers and exporters, and the ABI-boundary implementation of the host
// functions (validated, undoable model access through entity documents).

#include "openmine/jobs/Job.h"
#include "openmine/project/Project.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openmine::plugins {

struct PluginManifest {
    std::string id, name, version, library, description, author, license;
    int apiVersion = 0;
    std::filesystem::path folder;
    std::filesystem::path libraryPath() const; ///< platform file name (lib<name>.so, <name>.dll, lib<name>.dylib)
};

/// Reads and validates "plugin.json" in a folder. Throws std::runtime_error with the reason.
PluginManifest readManifest(const std::filesystem::path& folder);

struct PluginStatus {
    PluginManifest manifest;
    bool loaded = false;
    std::string error;                       ///< why it is not loaded
    std::vector<std::string> commands, importers, exporters;
};

struct Registration {
    std::string name;        ///< command name, or file extension (lower case, with the dot)
    std::string description;
    std::string pluginId;
};

struct PluginCallResult {
    bool ok = false;
    int code = 0;
    std::string text;                        ///< result / report from the plugin
    std::string error;
    std::vector<std::string> log;            ///< messages logged by the plugin during the call
    bool rolledBack = false;                 ///< an edit was left open and was rolled back
};

class PluginManager {
public:
    PluginManager();
    ~PluginManager(); ///< unloads every plugin
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /// Manifests in the immediate sub-folders of each directory (invalid ones are reported in
    /// `problems`).
    std::vector<PluginManifest> discover(const std::vector<std::filesystem::path>& directories, std::vector<std::string>* problems = nullptr) const;
    /// Loads a plugin; false (with the reason in its status) when it is incompatible or fails.
    bool load(const PluginManifest& manifest);
    /// Discovers and loads all (skipping ids in `disabled`). Returns the number loaded.
    int loadAll(const std::vector<std::filesystem::path>& directories, const std::vector<std::string>& disabled = {});
    void unload(const std::string& pluginId);
    void unloadAll();

    const std::vector<PluginStatus>& plugins() const;
    std::vector<Registration> commands() const;
    std::vector<Registration> importers() const;
    std::vector<Registration> exporters() const;
    std::optional<Registration> importerFor(const std::filesystem::path& file) const;
    std::optional<Registration> exporterFor(const std::filesystem::path& file) const;

    PluginCallResult runCommand(Project& project, const std::string& name, const std::string& argumentsJson = "{}",
                                jobs::Progress& progress = jobs::nullProgress());
    PluginCallResult runImport(Project& project, const std::filesystem::path& file, const std::string& optionsJson = "{}",
                               jobs::Progress& progress = jobs::nullProgress());
    PluginCallResult runExport(Project& project, const std::filesystem::path& file, const std::string& optionsJson = "{}",
                               jobs::Progress& progress = jobs::nullProgress());

    /// Receives every plugin log line (level 0–3, plugin id, message).
    void setLogSink(std::function<void(int, const std::string&, const std::string&)> sink);

    static int apiVersion();

    struct Impl; ///< opaque (used by the C host functions)

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace openmine::plugins
