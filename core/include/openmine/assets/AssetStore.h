#pragma once
// SPDX-License-Identifier: Apache-2.0
// Sidecar asset store for large binary data (point-cloud octrees, meshes, B-reps,
// block-model arrays, attachments, reports).
//
//   <project>.openmine-assets/<kind>/<uuid>[.<ext>]       file asset
//   <project>.openmine-assets/<kind>/<uuid>/manifest.json  folder asset (per-file SHA-256)
//
// Assets are immutable once finalised: processing never rewrites an asset, it creates a
// new one. Paths stored in the project are relative to the asset root, so a project
// folder can be moved or copied as a whole.

#include "openmine/objects/ProjectEntities.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace openmine {

class AssetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class AssetStore {
public:
    AssetStore();
    explicit AssetStore(std::filesystem::path root);

    const std::filesystem::path& root() const { return root_; }
    /// The conventional sidecar folder for a project file.
    static std::filesystem::path sidecarFor(const std::filesystem::path& projectFile);
    /// A fresh temporary working root for unsaved projects.
    static std::filesystem::path temporaryRoot(const Uuid& projectId);

    /// Copies (or moves) an existing file into the store.
    Asset importFile(const std::filesystem::path& source, AssetKind kind, std::string_view extension, bool move = false);
    /// Writes bytes as a new file asset.
    Asset writeBytes(std::string_view bytes, AssetKind kind, std::string_view extension);

    /// Starts a folder asset: returns its id and the (empty, created) folder to fill.
    std::pair<Uuid, std::filesystem::path> beginFolder(AssetKind kind);
    /// Writes the manifest and returns the finalised asset (sha256 = hash of manifest).
    Asset finalizeFolder(const Uuid& id, AssetKind kind);
    /// Discards an unfinished folder asset.
    void abortFolder(const Uuid& id, AssetKind kind);

    std::filesystem::path pathOf(const Asset& a) const { return root_ / a.relativePath; }
    std::string readBytes(const Asset& a) const;

    struct Verification {
        bool ok = true;
        std::vector<std::string> problems;
    };
    /// Quick check (existence + size) or full check (re-hash every file).
    Verification verify(const Asset& a, bool full) const;

    /// Copies the given assets to `newRoot` (if different) and switches to it.
    void relocate(const std::filesystem::path& newRoot, const std::vector<Asset>& assets);
    /// Deletes asset files (used by "compact project").
    void remove(const Asset& a);

private:
    static std::string kindFolder(AssetKind k);
    std::filesystem::path root_;
};

} // namespace openmine
