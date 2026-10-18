// SPDX-License-Identifier: Apache-2.0
#include "openmine/plugins/PluginManager.h"

#include "openmine/persistence/EntityCodec.h"
#include "openmine/persistence/ProjectFile.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine_plugin.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using nlohmann::json;
namespace fs = std::filesystem;
namespace pe = openmine::persistence;

namespace openmine::plugins {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

#if defined(_WIN32)
using Handle = HMODULE;
Handle openLibrary(const fs::path& p, std::string& err) {
    Handle h = LoadLibraryW(p.wstring().c_str());
    if (!h) err = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
    return h;
}
void* symbol(Handle h, const char* name) { return reinterpret_cast<void*>(GetProcAddress(h, name)); }
void closeLibrary(Handle h) { FreeLibrary(h); }
#else
using Handle = void*;
Handle openLibrary(const fs::path& p, std::string& err) {
    Handle h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) err = dlerror();
    return h;
}
void* symbol(Handle h, const char* name) { return dlsym(h, name); }
void closeLibrary(Handle h) { dlclose(h); }
#endif

OmString makeString(const std::string& s) {
    OmString o;
    o.size = s.size();
    o.data = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(o.data, s.c_str(), s.size() + 1);
    return o;
}
} // namespace

fs::path PluginManifest::libraryPath() const {
#if defined(_WIN32)
    return folder / (library + ".dll");
#elif defined(__APPLE__)
    return folder / ("lib" + library + ".dylib");
#else
    return folder / ("lib" + library + ".so");
#endif
}

PluginManifest readManifest(const fs::path& folder) {
    const fs::path f = folder / "plugin.json";
    std::ifstream in(f);
    if (!in) throw std::runtime_error("No plugin.json in " + folder.string());
    json j;
    try {
        j = json::parse(in);
    } catch (const json::exception& e) {
        throw std::runtime_error("Invalid plugin.json in " + folder.string() + ": " + e.what());
    }
    PluginManifest m;
    m.folder = folder;
    auto req = [&](const char* k) {
        if (!j.contains(k) || !j.at(k).is_string() || j.at(k).get<std::string>().empty())
            throw std::runtime_error(std::string("plugin.json in ") + folder.string() + " lacks \"" + k + "\"");
        return j.at(k).get<std::string>();
    };
    m.id = req("id");
    m.name = req("name");
    m.version = req("version");
    m.library = req("library");
    if (!j.contains("api_version") || !j.at("api_version").is_number_integer()) throw std::runtime_error("plugin.json lacks an integer \"api_version\"");
    m.apiVersion = j.at("api_version");
    m.description = j.value("description", "");
    m.author = j.value("author", "");
    m.license = j.value("license", "");
    if (!std::regex_match(m.id, std::regex(R"([a-z0-9]+(\.[a-z0-9_-]+)+)")))
        throw std::runtime_error("Plugin id '" + m.id + "' must be reverse-DNS (e.g. org.example.tool)");
    if (m.library.find('/') != std::string::npos || m.library.find('\\') != std::string::npos || m.library.find("..") != std::string::npos)
        throw std::runtime_error("Plugin library must be a plain file name");
    return m;
}

// ---- host context -----------------------------------------------------------------------------
struct Loaded;
} // namespace openmine::plugins

struct OmHostContext {
    openmine::plugins::PluginManager::Impl* mgr = nullptr;
    openmine::plugins::Loaded* plugin = nullptr;
    openmine::Project* project = nullptr;
    openmine::jobs::Progress* progress = nullptr;
    bool registering = false;
    std::string lastError;
    std::unique_ptr<openmine::CommandStack::MacroScope> edit;
    std::vector<std::string> log;
};

namespace openmine::plugins {

struct Command {
    std::string name, help;
    OmCommandFn fn;
    void* user;
};
struct FileHandler {
    std::string ext, description;
    void* fn; // OmImportFn / OmExportFn
    void* user;
};

struct Loaded {
    PluginStatus status;
    Handle handle{};
    OpenMinePluginInfoV1 info{};
    std::vector<Command> commands;
    std::vector<FileHandler> importers, exporters;
};

struct PluginManager::Impl {
    std::vector<std::unique_ptr<Loaded>> loaded;
    mutable std::vector<PluginStatus> statusCache;
    std::function<void(int, const std::string&, const std::string&)> sink;
    OpenMineHostV1 host{};

    Loaded* findPlugin(const std::string& id) {
        for (auto& l : loaded)
            if (l->status.manifest.id == id) return l.get();
        return nullptr;
    }
    bool nameTaken(const std::string& n, bool command, bool importer) const {
        for (const auto& l : loaded) {
            if (!l->status.loaded) continue;
            if (command)
                for (const auto& c : l->commands)
                    if (c.name == n) return true;
            if (!command)
                for (const auto& h : (importer ? l->importers : l->exporters))
                    if (h.ext == n) return true;
        }
        return false;
    }
};

namespace {
#define OM_GUARD_BEGIN try {
#define OM_GUARD_END                                                                                                                             \
    }                                                                                                                                            \
    catch (const std::exception& e) {                                                                                                           \
        ctx->lastError = e.what();                                                                                                               \
        return OM_ERROR;                                                                                                                         \
    }                                                                                                                                            \
    catch (...) {                                                                                                                                \
        ctx->lastError = "unknown error";                                                                                                        \
        return OM_ERROR;                                                                                                                         \
    }

int needProject(OmHostContext* ctx) {
    if (!ctx->project) {
        ctx->lastError = "No project is available in this context (model access is only allowed inside command, import and export callbacks)";
        return OM_INVALID;
    }
    return OM_OK;
}

int hRegisterCommand(OmHostContext* ctx, const char* name, const char* help, OmCommandFn fn, void* user) {
    OM_GUARD_BEGIN
    if (!ctx->registering) throw std::runtime_error("Registration is only allowed during plugin initialisation");
    if (!name || !fn) throw std::runtime_error("Command name and function are required");
    const std::string n = name;
    if (!std::regex_match(n, std::regex(R"([a-z][a-z0-9_-]*(\.[a-z0-9_-]+)*)"))) throw std::runtime_error("Invalid command name '" + n + "'");
    if (ctx->mgr->nameTaken(n, true, false) || std::any_of(ctx->plugin->commands.begin(), ctx->plugin->commands.end(), [&](const auto& c) { return c.name == n; }))
        throw std::runtime_error("Command '" + n + "' is already registered");
    ctx->plugin->commands.push_back({n, help ? help : "", fn, user});
    ctx->plugin->status.commands.push_back(n);
    return OM_OK;
    OM_GUARD_END
}

std::string normExt(const char* e) {
    std::string x = lower(e ? e : "");
    if (!x.empty() && x[0] != '.') x = "." + x;
    if (x.size() < 2) throw std::runtime_error("Invalid file extension");
    return x;
}

int hRegisterImporter(OmHostContext* ctx, const char* ext, const char* desc, OmImportFn fn, void* user) {
    OM_GUARD_BEGIN
    if (!ctx->registering) throw std::runtime_error("Registration is only allowed during plugin initialisation");
    if (!fn) throw std::runtime_error("Importer function is required");
    const auto x = normExt(ext);
    if (ctx->mgr->nameTaken(x, false, true) || std::any_of(ctx->plugin->importers.begin(), ctx->plugin->importers.end(), [&](const auto& c) { return c.ext == x; }))
        throw std::runtime_error("An importer for '" + x + "' is already registered");
    ctx->plugin->importers.push_back({x, desc ? desc : "", reinterpret_cast<void*>(fn), user});
    ctx->plugin->status.importers.push_back(x);
    return OM_OK;
    OM_GUARD_END
}

int hRegisterExporter(OmHostContext* ctx, const char* ext, const char* desc, OmExportFn fn, void* user) {
    OM_GUARD_BEGIN
    if (!ctx->registering) throw std::runtime_error("Registration is only allowed during plugin initialisation");
    if (!fn) throw std::runtime_error("Exporter function is required");
    const auto x = normExt(ext);
    if (ctx->mgr->nameTaken(x, false, false) || std::any_of(ctx->plugin->exporters.begin(), ctx->plugin->exporters.end(), [&](const auto& c) { return c.ext == x; }))
        throw std::runtime_error("An exporter for '" + x + "' is already registered");
    ctx->plugin->exporters.push_back({x, desc ? desc : "", reinterpret_cast<void*>(fn), user});
    ctx->plugin->status.exporters.push_back(x);
    return OM_OK;
    OM_GUARD_END
}

void hLog(OmHostContext* ctx, int level, const char* msg) {
    try {
        const std::string m = msg ? msg : "";
        static const char* names[] = {"debug", "info", "warning", "error"};
        ctx->log.push_back(std::string(names[std::clamp(level, 0, 3)]) + ": " + m);
        if (ctx->mgr->sink) ctx->mgr->sink(std::clamp(level, 0, 3), ctx->plugin ? ctx->plugin->status.manifest.id : "", m);
    } catch (...) {
    }
}

const char* hLastError(OmHostContext* ctx) { return ctx->lastError.c_str(); }

void hFreeString(OmString* s) {
    if (s && s->data) {
        std::free(s->data);
        s->data = nullptr;
        s->size = 0;
    }
}

int hProgress(OmHostContext* ctx, double f, const char* stage) {
    if (!ctx->progress) return 0;
    try {
        ctx->progress->update(std::clamp(f, 0.0, 1.0), stage ? stage : "");
    } catch (...) {
    }
    return ctx->progress->isCancelled() ? 1 : 0;
}

int hProjectInfo(OmHostContext* ctx, OmString* out) {
    if (int rc = needProject(ctx)) return rc;
    OM_GUARD_BEGIN
    *out = makeString(*pe::encodeDocument(ctx->project->model(), "ProjectInfo", "project"));
    return OM_OK;
    OM_GUARD_END
}

int hDocumentTypes(OmHostContext* ctx, OmString* out) {
    OM_GUARD_BEGIN
    json a = json::array();
    for (const auto& t : pe::documentTypes())
        if (t != "ProjectInfo" && t != "ActivityDesignLink") a.push_back(t);
    *out = makeString(a.dump());
    return OM_OK;
    OM_GUARD_END
}

int hListDocuments(OmHostContext* ctx, const char* type, OmString* out) {
    if (int rc = needProject(ctx)) return rc;
    OM_GUARD_BEGIN
    const std::string t = type ? type : "";
    if (std::find(pe::documentTypes().begin(), pe::documentTypes().end(), t) == pe::documentTypes().end())
        throw std::runtime_error("Unknown document type '" + t + "'");
    json a = json::array();
    for (const auto& d : pe::encodeModel(ctx->project->model()))
        if (d.type == t) a.push_back({{"key", d.key}, {"doc", json::parse(d.json)}});
    *out = makeString(a.dump());
    return OM_OK;
    OM_GUARD_END
}

int hGetDocument(OmHostContext* ctx, const char* type, const char* key, OmString* out) {
    if (int rc = needProject(ctx)) return rc;
    OM_GUARD_BEGIN
    const auto d = pe::encodeDocument(ctx->project->model(), type ? type : "", key ? key : "");
    if (!d) {
        ctx->lastError = "not found";
        return OM_NOT_FOUND;
    }
    *out = makeString(*d);
    return OM_OK;
    OM_GUARD_END
}

int hTemplate(OmHostContext* ctx, const char* type, OmString* out) {
    OM_GUARD_BEGIN
    *out = makeString(pe::templateDocument(type ? type : ""));
    return OM_OK;
    OM_GUARD_END
}

int hBeginEdit(OmHostContext* ctx, const char* desc) {
    if (int rc = needProject(ctx)) return rc;
    OM_GUARD_BEGIN
    if (ctx->edit) throw std::runtime_error("An edit is already open");
    const std::string pid = ctx->plugin ? ctx->plugin->status.manifest.name : "plugin";
    ctx->edit = std::make_unique<CommandStack::MacroScope>(ctx->project->commands(), (desc && *desc ? std::string(desc) : "Plugin edit") + " (" + pid + ")");
    return OM_OK;
    OM_GUARD_END
}

int hEndEdit(OmHostContext* ctx, int commit) {
    if (int rc = needProject(ctx)) return rc;
    OM_GUARD_BEGIN
    if (!ctx->edit) throw std::runtime_error("No edit is open");
    if (commit) ctx->edit->commit();
    ctx->edit.reset(); // not committed ⇒ rolled back
    return OM_OK;
    OM_GUARD_END
}

int hPut(OmHostContext* ctx, const char* type, const char* doc, OmString* key) {
    if (int rc = needProject(ctx)) return rc;
    OM_GUARD_BEGIN
    const Uuid id = pe::putDocument(*ctx->project, type ? type : "", doc ? doc : "", std::string("Plugin: ") + (type ? type : ""));
    if (key) *key = makeString(id.toString());
    return OM_OK;
    OM_GUARD_END
}

int hDelete(OmHostContext* ctx, const char* type, const char* key) {
    if (int rc = needProject(ctx)) return rc;
    OM_GUARD_BEGIN
    pe::removeDocument(*ctx->project, type ? type : "", key ? key : "");
    return OM_OK;
    OM_GUARD_END
}

int hAddGeometry(OmHostContext* ctx, const char* gj, OmString* key) {
    if (int rc = needProject(ctx)) return rc;
    OM_GUARD_BEGIN
    const json j = json::parse(gj ? gj : "");
    GeometryObject g;
    const std::string type = j.value("type", "Polyline");
    if (type == "Point") g.type = GeometryType::Point;
    else if (type == "Line") g.type = GeometryType::Line;
    else if (type == "Polyline") g.type = GeometryType::Polyline;
    else throw std::runtime_error("add_geometry supports Point, Line and Polyline (got '" + type + "')");
    g.name = j.value("name", "");
    const std::string layer = j.value("layer", "");
    const auto& m = ctx->project->model();
    if (auto u = Uuid::parse(layer); u && m.layers.find(*u)) g.layerId = *u;
    else {
        for (const auto& l : m.layers.all())
            if (l.name == layer) g.layerId = l.id;
        if (g.layerId.isNil()) throw std::runtime_error("Layer '" + layer + "' does not exist");
    }
    for (const auto& v : j.at("vertices")) g.vertices.push_back({v.at(0).get<double>(), v.at(1).get<double>(), v.size() > 2 ? v.at(2).get<double>() : 0.0});
    g.closed = j.value("closed", false);
    if (j.contains("attributes"))
        for (const auto& [k, v] : j.at("attributes").items()) g.attributes[k] = v.is_string() ? v.get<std::string>() : v.dump();
    const Uuid id = ProjectEditor(*ctx->project).addGeometry(std::move(g), "Plugin: add " + type);
    if (key) *key = makeString(id.toString());
    return OM_OK;
    OM_GUARD_END
}

int hGetGeometry(OmHostContext* ctx, const char* key, OmString* out) {
    if (int rc = needProject(ctx)) return rc;
    OM_GUARD_BEGIN
    const auto id = Uuid::parse(key ? key : "");
    const GeometryObject* g = id ? ctx->project->model().geometry.find(*id) : nullptr;
    if (!g) {
        ctx->lastError = "not found";
        return OM_NOT_FOUND;
    }
    json v = json::array();
    for (const auto& p : g->vertices) v.push_back({p.x, p.y, p.z});
    const auto* l = ctx->project->model().layers.find(g->layerId);
    *out = makeString(json{{"name", g->name},
                           {"type", toString(g->type)},
                           {"layer", g->layerId.toString()},
                           {"layer_name", l ? l->name : ""},
                           {"vertices", v},
                           {"closed", g->closed},
                           {"attributes", g->attributes}}
                          .dump());
    return OM_OK;
    OM_GUARD_END
}
} // namespace

// ---- manager ------------------------------------------------------------------------------------
PluginManager::PluginManager() : impl_(std::make_unique<Impl>()) {
    auto& h = impl_->host;
    h.struct_size = sizeof(OpenMineHostV1);
    h.api_version = OPENMINE_PLUGIN_API_VERSION;
    h.host_version = persistence::kAppVersion;
    h.register_command = hRegisterCommand;
    h.register_importer = hRegisterImporter;
    h.register_exporter = hRegisterExporter;
    h.log = hLog;
    h.last_error = hLastError;
    h.free_string = hFreeString;
    h.report_progress = hProgress;
    h.project_info = hProjectInfo;
    h.document_types = hDocumentTypes;
    h.list_documents = hListDocuments;
    h.get_document = hGetDocument;
    h.template_document = hTemplate;
    h.begin_edit = hBeginEdit;
    h.put_document = hPut;
    h.delete_document = hDelete;
    h.end_edit = hEndEdit;
    h.add_geometry = hAddGeometry;
    h.get_geometry = hGetGeometry;
}

PluginManager::~PluginManager() { unloadAll(); }

int PluginManager::apiVersion() { return OPENMINE_PLUGIN_API_VERSION; }

void PluginManager::setLogSink(std::function<void(int, const std::string&, const std::string&)> sink) { impl_->sink = std::move(sink); }

std::vector<PluginManifest> PluginManager::discover(const std::vector<fs::path>& dirs, std::vector<std::string>* problems) const {
    std::vector<PluginManifest> out;
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        std::vector<fs::path> sub;
        for (const auto& e : fs::directory_iterator(d, ec))
            if (e.is_directory() && fs::exists(e.path() / "plugin.json")) sub.push_back(e.path());
        std::sort(sub.begin(), sub.end());
        for (const auto& s : sub) {
            try {
                out.push_back(readManifest(s));
            } catch (const std::exception& e) {
                if (problems) problems->push_back(e.what());
            }
        }
    }
    return out;
}

bool PluginManager::load(const PluginManifest& m) {
    if (impl_->findPlugin(m.id) && impl_->findPlugin(m.id)->status.loaded) return true;
    auto l = std::make_unique<Loaded>();
    l->status.manifest = m;
    auto fail = [&](std::string why) {
        l->status.error = std::move(why);
        if (l->handle) closeLibrary(l->handle);
        l->handle = {};
        l->commands.clear();
        l->importers.clear();
        l->exporters.clear();
        l->status.commands.clear();
        l->status.importers.clear();
        l->status.exporters.clear();
        impl_->loaded.push_back(std::move(l));
        return false;
    };
    if (m.apiVersion != OPENMINE_PLUGIN_API_VERSION)
        return fail("Plugin API version " + std::to_string(m.apiVersion) + " is not supported (host provides " + std::to_string(OPENMINE_PLUGIN_API_VERSION) + ")");
    if (impl_->findPlugin(m.id)) return fail("Another plugin with id '" + m.id + "' is already present");
    const fs::path lib = m.libraryPath();
    if (!fs::exists(lib)) return fail("Library not found: " + lib.string());
    std::string err;
    l->handle = openLibrary(lib, err);
    if (!l->handle) return fail("Cannot load " + lib.string() + ": " + err);
    auto init = reinterpret_cast<OpenMinePluginInitV1>(symbol(l->handle, OPENMINE_PLUGIN_INIT_SYMBOL));
    if (!init) return fail(std::string("The library does not export ") + OPENMINE_PLUGIN_INIT_SYMBOL);
    OmHostContext ctx;
    ctx.mgr = impl_.get();
    ctx.plugin = l.get();
    ctx.registering = true;
    l->info.struct_size = sizeof(OpenMinePluginInfoV1);
    int rc = OM_ERROR;
    try {
        rc = init(&impl_->host, &ctx, &l->info);
    } catch (...) {
        return fail("The plugin threw an exception during initialisation (exceptions must not cross the C interface)");
    }
    if (rc != OM_OK) return fail("Initialisation failed: " + (ctx.lastError.empty() ? "code " + std::to_string(rc) : ctx.lastError));
    if (!l->info.id || m.id != l->info.id) return fail("The library reports a different id than its manifest");
    l->status.loaded = true;
    impl_->loaded.push_back(std::move(l));
    return true;
}

int PluginManager::loadAll(const std::vector<fs::path>& dirs, const std::vector<std::string>& disabled) {
    int n = 0;
    std::vector<std::string> problems;
    for (const auto& m : discover(dirs, &problems)) {
        if (std::find(disabled.begin(), disabled.end(), m.id) != disabled.end()) {
            auto l = std::make_unique<Loaded>();
            l->status.manifest = m;
            l->status.error = "Disabled";
            impl_->loaded.push_back(std::move(l));
            continue;
        }
        if (load(m)) ++n;
    }
    for (const auto& p : problems) {
        auto l = std::make_unique<Loaded>();
        l->status.error = p;
        impl_->loaded.push_back(std::move(l));
    }
    return n;
}

void PluginManager::unload(const std::string& id) {
    auto& v = impl_->loaded;
    for (auto it = v.begin(); it != v.end(); ++it)
        if ((*it)->status.manifest.id == id) {
            auto& l = **it;
            if (l.status.loaded && l.info.shutdown) {
                try {
                    l.info.shutdown();
                } catch (...) {
                }
            }
            if (l.handle) closeLibrary(l.handle);
            v.erase(it);
            return;
        }
}

void PluginManager::unloadAll() {
    while (!impl_->loaded.empty()) {
        auto& l = *impl_->loaded.back();
        if (l.status.loaded && l.info.shutdown) {
            try {
                l.info.shutdown();
            } catch (...) {
            }
        }
        if (l.handle) closeLibrary(l.handle);
        impl_->loaded.pop_back();
    }
}

const std::vector<PluginStatus>& PluginManager::plugins() const {
    impl_->statusCache.clear();
    for (const auto& l : impl_->loaded) impl_->statusCache.push_back(l->status);
    return impl_->statusCache;
}

std::vector<Registration> PluginManager::commands() const {
    std::vector<Registration> r;
    for (const auto& l : impl_->loaded)
        if (l->status.loaded)
            for (const auto& c : l->commands) r.push_back({c.name, c.help, l->status.manifest.id});
    std::sort(r.begin(), r.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return r;
}
std::vector<Registration> PluginManager::importers() const {
    std::vector<Registration> r;
    for (const auto& l : impl_->loaded)
        if (l->status.loaded)
            for (const auto& c : l->importers) r.push_back({c.ext, c.description, l->status.manifest.id});
    return r;
}
std::vector<Registration> PluginManager::exporters() const {
    std::vector<Registration> r;
    for (const auto& l : impl_->loaded)
        if (l->status.loaded)
            for (const auto& c : l->exporters) r.push_back({c.ext, c.description, l->status.manifest.id});
    return r;
}

std::optional<Registration> PluginManager::importerFor(const fs::path& f) const {
    const auto x = lower(f.extension().string());
    for (const auto& r : importers())
        if (r.name == x) return r;
    return std::nullopt;
}
std::optional<Registration> PluginManager::exporterFor(const fs::path& f) const {
    const auto x = lower(f.extension().string());
    for (const auto& r : exporters())
        if (r.name == x) return r;
    return std::nullopt;
}

namespace {
template <class F> PluginCallResult invoke(PluginManager::Impl& impl, Loaded& l, Project& p, jobs::Progress& progress, F&& call) {
    PluginCallResult res;
    OmHostContext ctx;
    ctx.mgr = &impl;
    ctx.plugin = &l;
    ctx.project = &p;
    ctx.progress = &progress;
    OmString out{nullptr, 0};
    try {
        res.code = call(&ctx, &out);
    } catch (...) {
        res.code = OM_ERROR;
        ctx.lastError = "The plugin threw an exception (exceptions must not cross the C interface)";
    }
    if (ctx.edit) {
        ctx.edit.reset(); // rolled back
        res.rolledBack = true;
        if (res.code == OM_OK) {
            res.code = OM_ERROR;
            ctx.lastError = "The plugin left an edit open; its changes were rolled back";
        }
    }
    if (out.data) {
        res.text.assign(out.data, out.size);
        std::free(out.data);
    }
    res.ok = res.code == OM_OK;
    if (!res.ok) res.error = ctx.lastError.empty() ? (res.code == OM_CANCELLED ? "Cancelled" : "Plugin error code " + std::to_string(res.code)) : ctx.lastError;
    res.log = std::move(ctx.log);
    return res;
}
} // namespace

PluginCallResult PluginManager::runCommand(Project& p, const std::string& name, const std::string& args, jobs::Progress& progress) {
    for (auto& l : impl_->loaded)
        if (l->status.loaded)
            for (auto& c : l->commands)
                if (c.name == name)
                    return invoke(*impl_, *l, p, progress, [&](OmHostContext* ctx, OmString* out) { return c.fn(ctx, c.user, args.c_str(), out); });
    PluginCallResult r;
    r.error = "Unknown plugin command '" + name + "'";
    return r;
}

PluginCallResult PluginManager::runImport(Project& p, const fs::path& file, const std::string& opts, jobs::Progress& progress) {
    const auto x = lower(file.extension().string());
    const std::string path = file.string();
    for (auto& l : impl_->loaded)
        if (l->status.loaded)
            for (auto& h : l->importers)
                if (h.ext == x)
                    return invoke(*impl_, *l, p, progress, [&](OmHostContext* ctx, OmString* out) {
                        return reinterpret_cast<OmImportFn>(h.fn)(ctx, h.user, path.c_str(), opts.c_str(), out);
                    });
    PluginCallResult r;
    r.error = "No plugin imports '" + x + "' files";
    return r;
}

PluginCallResult PluginManager::runExport(Project& p, const fs::path& file, const std::string& opts, jobs::Progress& progress) {
    const auto x = lower(file.extension().string());
    const std::string path = file.string();
    for (auto& l : impl_->loaded)
        if (l->status.loaded)
            for (auto& h : l->exporters)
                if (h.ext == x)
                    return invoke(*impl_, *l, p, progress, [&](OmHostContext* ctx, OmString* out) {
                        return reinterpret_cast<OmExportFn>(h.fn)(ctx, h.user, path.c_str(), opts.c_str(), out);
                    });
    PluginCallResult r;
    r.error = "No plugin exports '" + x + "' files";
    return r;
}

} // namespace openmine::plugins
