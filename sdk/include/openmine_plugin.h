/* SPDX-License-Identifier: Apache-2.0
 *
 * OpenMine Suite native plugin interface — version 1 (stable C ABI).
 *
 * A plugin is a shared library plus a manifest ("plugin.json") in its own folder:
 *
 *   {
 *     "id": "org.example.stations",   // reverse-DNS, unique
 *     "name": "Station import",
 *     "version": "1.0.0",
 *     "api_version": 1,               // must equal OPENMINE_PLUGIN_API_VERSION of the host
 *     "library": "station_import",    // file name without prefix/suffix (lib….so, ….dll, lib….dylib)
 *     "description": "…", "author": "…", "license": "Apache-2.0"
 *   }
 *
 * The library exports one function, openmine_plugin_init_v1, called once after loading. In it the
 * plugin registers commands, importers and exporters with the host and returns its information.
 *
 * Rules:
 *  - Only C types cross the boundary; no C++ exceptions may escape a plugin function.
 *  - The model is read and changed exclusively through entity documents (JSON) — the lossless
 *    document format of OpenMine (see docs/plugins.md); every change is validated by the same rules
 *    as the user interface, is undoable and is audited. Changes are grouped with begin_edit /
 *    end_edit (one undo step). An edit left open when a callback returns is rolled back.
 *  - Strings returned by the host (OmString) are owned by the plugin after the call and must be
 *    released with host->free_string. Strings passed to the host are copied.
 *  - Return codes: OM_OK (0) on success; OM_NOT_FOUND; OM_ERROR with details from host->last_error.
 *  - Callbacks run on the thread that invoked them; long operations should call report_progress,
 *    which returns 1 when the user cancelled (the callback should then return OM_CANCELLED).
 */
#ifndef OPENMINE_PLUGIN_H
#define OPENMINE_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENMINE_PLUGIN_API_VERSION 1

#if defined(_WIN32)
#define OPENMINE_PLUGIN_EXPORT __declspec(dllexport)
#else
#define OPENMINE_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

enum { OM_OK = 0, OM_ERROR = 1, OM_NOT_FOUND = 2, OM_CANCELLED = 3, OM_INVALID = 4 };
enum { OM_LOG_DEBUG = 0, OM_LOG_INFO = 1, OM_LOG_WARNING = 2, OM_LOG_ERROR = 3 };

typedef struct OmHostContext OmHostContext; /* opaque, valid for the duration of a call */

typedef struct OmString {
    char* data; /* NUL-terminated UTF-8 */
    size_t size;
} OmString;

/* Callbacks implemented by plugins. `user` is the pointer given at registration. */
typedef int (*OmCommandFn)(OmHostContext* ctx, void* user, const char* argumentsJson, OmString* resultText);
typedef int (*OmImportFn)(OmHostContext* ctx, void* user, const char* filePath, const char* optionsJson, OmString* reportText);
typedef int (*OmExportFn)(OmHostContext* ctx, void* user, const char* filePath, const char* optionsJson, OmString* reportText);

typedef struct OpenMineHostV1 {
    uint32_t struct_size;
    uint32_t api_version;
    const char* host_version;

    /* Registration (only during openmine_plugin_init_v1). */
    int (*register_command)(OmHostContext* ctx, const char* name, const char* help, OmCommandFn fn, void* user);
    int (*register_importer)(OmHostContext* ctx, const char* extension, const char* description, OmImportFn fn, void* user);
    int (*register_exporter)(OmHostContext* ctx, const char* extension, const char* description, OmExportFn fn, void* user);

    /* Diagnostics. */
    void (*log)(OmHostContext* ctx, int level, const char* message);
    const char* (*last_error)(OmHostContext* ctx);
    void (*free_string)(OmString* s);
    int (*report_progress)(OmHostContext* ctx, double fraction, const char* stage);

    /* Model access (during command/import/export callbacks). */
    int (*project_info)(OmHostContext* ctx, OmString* infoJson);
    int (*document_types)(OmHostContext* ctx, OmString* typesJsonArray);
    int (*list_documents)(OmHostContext* ctx, const char* type, OmString* documentsJsonArray);
    int (*get_document)(OmHostContext* ctx, const char* type, const char* key, OmString* documentJson);
    int (*template_document)(OmHostContext* ctx, const char* type, OmString* documentJson);
    int (*begin_edit)(OmHostContext* ctx, const char* description);
    int (*put_document)(OmHostContext* ctx, const char* type, const char* documentJson, OmString* key);
    int (*delete_document)(OmHostContext* ctx, const char* type, const char* key);
    int (*end_edit)(OmHostContext* ctx, int commit);
    /* Convenience for geometry (the document form stores vertices as packed binary):
     * {"type":"Point|Line|Polyline","name":…,"layer":"<uuid or layer name>","vertices":[[x,y,z],…],
     *  "closed":false,"attributes":{…}}  →  new GeometryObject key. */
    int (*add_geometry)(OmHostContext* ctx, const char* geometryJson, OmString* key);
    /* {"name","type","layer","layer_name","vertices":[[x,y,z],…],"closed","attributes"} */
    int (*get_geometry)(OmHostContext* ctx, const char* key, OmString* geometryJson);
} OpenMineHostV1;

typedef struct OpenMinePluginInfoV1 {
    uint32_t struct_size;
    const char* id;              /* must match the manifest */
    const char* version;
    void (*shutdown)(void);      /* optional; called before unloading */
} OpenMinePluginInfoV1;

/* Exported by every plugin. Return OM_OK to stay loaded. */
typedef int (*OpenMinePluginInitV1)(const OpenMineHostV1* host, OmHostContext* ctx, OpenMinePluginInfoV1* info);
#define OPENMINE_PLUGIN_INIT_SYMBOL "openmine_plugin_init_v1"

#ifdef __cplusplus
}
#endif

#endif /* OPENMINE_PLUGIN_H */
