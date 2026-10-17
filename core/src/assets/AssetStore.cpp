// SPDX-License-Identifier: Apache-2.0
#include "openmine/assets/AssetStore.h"

#include "openmine/diagnostics/Log.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/util/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace openmine {

namespace {
std::string relString(const fs::path& p) {
    auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
fs::path fromRel(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
} // namespace

AssetStore::AssetStore() = default;
AssetStore::AssetStore(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    fs::create_directories(root_, ec);
}

fs::path AssetStore::sidecarFor(const fs::path& projectFile) {
    auto p = projectFile;
    p += "-assets";
    return p;
}

fs::path AssetStore::temporaryRoot(const Uuid& projectId) {
    return fs::temp_directory_path() / ("openmine-work-" + projectId.toString()) / "assets";
}

std::string AssetStore::kindFolder(AssetKind k) {
    std::string s = toString(k);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Asset AssetStore::importFile(const fs::path& source, AssetKind kind, std::string_view ext, bool move) {
    if (root_.empty()) throw AssetError("Asset store has no root folder");
    Asset a;
    a.id = Uuid::generate();
    a.kind = kind;
    const fs::path rel = fs::path(kindFolder(kind)) / (a.id.toString() + (ext.empty() ? "" : "." + std::string(ext)));
    const fs::path dst = root_ / rel;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (move) {
        fs::rename(source, dst, ec);
        if (ec) { // different volume: copy then delete
            ec.clear();
            fs::copy_file(source, dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) fs::remove(source, ec);
        }
    } else {
        fs::copy_file(source, dst, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) throw AssetError("Cannot store asset from '" + source.string() + "': " + ec.message());
    auto hash = util::sha256File(dst);
    if (!hash) throw AssetError("Cannot read stored asset " + dst.string());
    a.relativePath = relString(rel);
    a.sha256 = *hash;
    a.sizeBytes = static_cast<std::int64_t>(fs::file_size(dst));
    a.createdAt = utcTimestampNow();
    return a;
}

Asset AssetStore::writeBytes(std::string_view bytes, AssetKind kind, std::string_view ext) {
    if (root_.empty()) throw AssetError("Asset store has no root folder");
    Asset a;
    a.id = Uuid::generate();
    a.kind = kind;
    const fs::path rel = fs::path(kindFolder(kind)) / (a.id.toString() + (ext.empty() ? "" : "." + std::string(ext)));
    const fs::path dst = root_ / rel;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    {
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw AssetError("Cannot write asset " + dst.string());
    }
    a.relativePath = relString(rel);
    a.sha256 = util::sha256Hex(bytes);
    a.sizeBytes = static_cast<std::int64_t>(bytes.size());
    a.createdAt = utcTimestampNow();
    return a;
}

std::pair<Uuid, fs::path> AssetStore::beginFolder(AssetKind kind) {
    if (root_.empty()) throw AssetError("Asset store has no root folder");
    const Uuid id = Uuid::generate();
    const fs::path dir = root_ / kindFolder(kind) / id.toString();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw AssetError("Cannot create asset folder " + dir.string() + ": " + ec.message());
    return {id, dir};
}

Asset AssetStore::finalizeFolder(const Uuid& id, AssetKind kind) {
    const fs::path rel = fs::path(kindFolder(kind)) / id.toString();
    const fs::path dir = root_ / rel;
    nlohmann::json files = nlohmann::json::array();
    std::vector<fs::path> paths;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file() && e.path().filename() != "manifest.json") paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());
    std::int64_t total = 0;
    for (const auto& p : paths) {
        auto h = util::sha256File(p);
        if (!h) throw AssetError("Cannot hash " + p.string());
        const auto size = static_cast<std::int64_t>(fs::file_size(p));
        total += size;
        files.push_back({{"path", relString(fs::relative(p, dir))}, {"size", size}, {"sha256", *h}});
    }
    const std::string manifest = nlohmann::json{{"format", "openmine-asset-manifest"}, {"version", 1}, {"files", files}}.dump(1);
    {
        std::ofstream out(dir / "manifest.json", std::ios::binary | std::ios::trunc);
        out << manifest;
        if (!out) throw AssetError("Cannot write manifest in " + dir.string());
    }
    Asset a;
    a.id = id;
    a.kind = kind;
    a.relativePath = relString(rel);
    a.isDirectory = true;
    a.sha256 = util::sha256Hex(manifest);
    a.sizeBytes = total;
    a.createdAt = utcTimestampNow();
    return a;
}

void AssetStore::abortFolder(const Uuid& id, AssetKind kind) {
    std::error_code ec;
    fs::remove_all(root_ / kindFolder(kind) / id.toString(), ec);
}

std::string AssetStore::readBytes(const Asset& a) const {
    std::ifstream in(pathOf(a), std::ios::binary);
    if (!in) throw AssetError("Asset file missing: " + pathOf(a).string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

AssetStore::Verification AssetStore::verify(const Asset& a, bool full) const {
    Verification v;
    const fs::path p = pathOf(a);
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        v.ok = false;
        v.problems.push_back("missing: " + a.relativePath);
        return v;
    }
    if (!a.isDirectory) {
        if (static_cast<std::int64_t>(fs::file_size(p, ec)) != a.sizeBytes) {
            v.ok = false;
            v.problems.push_back("size mismatch: " + a.relativePath);
        }
        if (full && v.ok) {
            auto h = util::sha256File(p);
            if (!h || *h != a.sha256) {
                v.ok = false;
                v.problems.push_back("checksum mismatch: " + a.relativePath);
            }
        }
        return v;
    }
    std::ifstream in(p / "manifest.json", std::ios::binary);
    if (!in) {
        v.ok = false;
        v.problems.push_back("manifest missing: " + a.relativePath);
        return v;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string manifest = ss.str();
    if (util::sha256Hex(manifest) != a.sha256) {
        v.ok = false;
        v.problems.push_back("manifest checksum mismatch: " + a.relativePath);
        return v;
    }
    const auto j = nlohmann::json::parse(manifest, nullptr, false);
    if (j.is_discarded() || !j.contains("files")) {
        v.ok = false;
        v.problems.push_back("manifest unreadable: " + a.relativePath);
        return v;
    }
    for (const auto& f : j["files"]) {
        const fs::path fp = p / fromRel(f["path"].get<std::string>());
        if (!fs::exists(fp, ec) || static_cast<std::int64_t>(fs::file_size(fp, ec)) != f["size"].get<std::int64_t>()) {
            v.ok = false;
            v.problems.push_back("missing or resized: " + a.relativePath + "/" + f["path"].get<std::string>());
            continue;
        }
        if (full) {
            auto h = util::sha256File(fp);
            if (!h || *h != f["sha256"].get<std::string>()) {
                v.ok = false;
                v.problems.push_back("checksum mismatch: " + a.relativePath + "/" + f["path"].get<std::string>());
            }
        }
    }
    return v;
}

void AssetStore::relocate(const fs::path& newRoot, const std::vector<Asset>& assets) {
    std::error_code ec;
    if (!root_.empty() && fs::exists(root_) && fs::exists(newRoot) && fs::equivalent(root_, newRoot, ec)) return;
    fs::create_directories(newRoot, ec);
    for (const auto& a : assets) {
        const fs::path src = root_ / fromRel(a.relativePath), dst = newRoot / fromRel(a.relativePath);
        if (!fs::exists(src)) {
            diag::warn("assets", "cannot relocate missing asset " + a.relativePath);
            continue;
        }
        fs::create_directories(dst.parent_path(), ec);
        if (a.isDirectory) fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        else fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) throw AssetError("Cannot copy asset " + a.relativePath + " to " + newRoot.string() + ": " + ec.message());
    }
    root_ = newRoot;
}

void AssetStore::remove(const Asset& a) {
    std::error_code ec;
    fs::remove_all(pathOf(a), ec);
}

} // namespace openmine
