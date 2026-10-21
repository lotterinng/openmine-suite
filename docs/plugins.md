# Plugins

OpenMine Suite has two plugin mechanisms. Both change the project only through validated,
undoable, audited operations — a plugin cannot bypass the rules the user interface obeys.

| | Native plugins | Python plugins |
|---|---|---|
| Interface | stable C ABI, `sdk/include/openmine_plugin.h`, API version **1** | `openmine` Python package, entry-point group `openmine.plugins` |
| Packaging | folder with `plugin.json` + shared library | Python distribution (wheel) or package in a `python` subfolder of a plugin folder |
| Provides | commands, importers, exporters (by file extension) | commands |
| Model access | entity documents (JSON) + geometry helpers | the full public Python API |
| Undo | `begin_edit` / `end_edit` = one undo step | each command run = one undo step |
| Host | `sdk/host` — `openmine::plugins::PluginManager` | `openmine.plugins.Registry` in the embedded interpreter |
| Tests | `tests/unit/test_plugins.cpp` | `scripting/python/tests/test_api.py` (`test_python_plugins*`), desktop smoke test |

## Where plugins are found

The desktop application searches, in order (duplicates ignored):

1. `<application folder>/plugins`
2. `<application folder>/../lib/openmine/plugins` (Linux install layout)
3. the per-user data folder: `%APPDATA%/OpenMine/OpenMine Suite/plugins` on Windows,
   `~/.local/share/OpenMine/OpenMine Suite/plugins` on Linux (Qt `AppDataLocation`)
4. every folder in the `OPENMINE_PLUGIN_PATH` environment variable (`;` on Windows, `:` elsewhere)

Each **immediate sub-folder** holding a `plugin.json` is a native plugin. The `python` sub-folder
of each search folder is added to the interpreter's `sys.path` for Python plugins, next to
installed distributions.

Settings (Settings ▸ Preferences, and the Plugins dialog in Tools ▸ Plugins):

| Key | Meaning |
|---|---|
| `plugins/disabled` | list of native plugin ids not to load |
| `plugins/python` | discover Python plugins at start-up (default on) |

Plugins that are not loaded are listed with the reason (bad manifest, wrong API version, missing
library, failed initialisation). Tools ▸ Plugins ▸ *Rescan* re-discovers both kinds.

## Running plugin commands

* **Plugins menu** — native commands, then a "Python plugins" section.
* **Command console** — `PLUGIN` lists the commands; `PLUGIN <name> {"json": "arguments"}` runs one.
* **Python** — `openmine.plugins.NativePlugins(dirs).run(project, name, **arguments)`;
  Python plugin commands through `openmine.plugins.discover().run(project, name, **arguments)`.
* **Import/Export** — File ▸ Import/Export offer every extension registered by a native plugin.

A native command hides a Python command of the same name (a warning names both).

## Native plugins (C ABI v1)

### Manifest

```json
{
  "id": "org.example.stations",
  "name": "Station import",
  "version": "1.0.0",
  "api_version": 1,
  "library": "station_import",
  "description": "…",
  "author": "…",
  "license": "Apache-2.0"
}
```

`library` is the file name without platform prefix/suffix: `libstation_import.so`,
`station_import.dll`, `libstation_import.dylib`. A manifest whose `api_version` differs from the
host's is reported ("requires plugin API version 99") and not loaded.

### Entry point

```c
#include <openmine_plugin.h>

static int summary(OmHostContext* ctx, void* user, const char* argumentsJson, OmString* result);

OPENMINE_PLUGIN_EXPORT int openmine_plugin_init_v1(const OpenMineHostV1* host, OmHostContext* ctx,
                                                   OpenMinePluginInfoV1* info) {
    info->id = "org.example.stations";   /* must match the manifest */
    info->version = "1.0.0";
    info->shutdown = NULL;               /* optional */
    host->register_command(ctx, "stations.summary", "Summarise control points", summary, NULL);
    host->register_importer(ctx, ".stn", "Total-station coordinates", import_stn, NULL);
    return OM_OK;                        /* anything else: the plugin is unloaded */
}
```

Registration is only accepted during `openmine_plugin_init_v1`; names and extensions must be
unique across loaded plugins.

### Host functions

| Group | Function | Notes |
|---|---|---|
| Registration | `register_command`, `register_importer`, `register_exporter` | init only |
| Diagnostics | `log(level, message)`, `last_error()`, `free_string(s)` | log lines go to the application log and the call result |
| Progress | `report_progress(fraction, stage)` | returns 1 when the user cancelled; return `OM_CANCELLED` |
| Reading | `project_info`, `document_types`, `list_documents(type)`, `get_document(type, key)`, `get_geometry(key)` | JSON strings |
| Editing | `begin_edit(text)`, `put_document(type, json)`, `delete_document(type, key)`, `end_edit(commit)` | one undo step |
| Templates | `template_document(type)` | a default entity with a fresh uuid to fill in |
| Geometry | `add_geometry({"type","name","layer","vertices","closed","attributes"})` | vertices as `[[x,y,z],…]` (the document form stores them packed) |

Return codes: `OM_OK` 0, `OM_ERROR` 1, `OM_NOT_FOUND` 2, `OM_CANCELLED` 3, `OM_INVALID` 4.
Strings returned by the host (`OmString`) belong to the plugin and are released with
`free_string`; strings passed to the host are copied.

### Entity documents

Documents are the lossless JSON form of every entity — the same encoding used by the project
file, the PostgreSQL shared repository and the Python `project.documents` API. Every entity
document has:

```json
{
  "uuid": "0fb368cf-f41e-4175-871c-27ba977d15c7",
  "columns": { "name": "Standard 5-day", "kind": "Project", "standard_hours_per_day": 24.0,
               "shifts": "[{\"duration\":1440,\"name\":\"Day\",\"start\":0,\"weekdays\":31}]",
               "revision": 1, "created_at": "…", "modified_at": "…" },
  "children": { }
}
```

`columns` are exactly the columns of the entity's table in the project schema
([reference/project-schema.md](reference/project-schema.md)); `children` holds rows of child
tables (e.g. vertex blocks, profile points). `document_types` lists the types in dependency
order (`CoordinateReferenceSystem` … `GeotechClassification`, plus `ProjectInfo` and
`ActivityDesignLink`, whose key is `<activity uuid>/<design uuid>`).

`put_document` with a nil or unknown uuid creates the entity; otherwise it updates it. Every put
goes through `ProjectEditor` — the same validation, metadata stamping, undo and audit as the user
interface. Removal applies the editor's cascade/restrict policies.

### Protections

The host enforces, and `Plugins.HostProtectsTheModelFromMisbehavingPlugins` verifies with a
deliberately misbehaving plugin (`tests/plugins/misbehaving`):

* an edit left open when a callback returns is **rolled back** (`rolledBack = true`);
* invalid documents are rejected with the validation message and change nothing;
* registrations after initialisation are refused;
* C++ exceptions thrown inside a callback are caught at the boundary and reported;
* an incompatible manifest (`tests/plugins/future`, API version 99) is reported, not loaded.

### Building

With CMake inside this repository:

```cmake
openmine_add_plugin(my_plugin ${CMAKE_CURRENT_SOURCE_DIR}/my_plugin SOURCES my_plugin.cpp)
```

builds a `MODULE` library with hidden visibility into `${CMAKE_BINARY_DIR}/plugins/my_plugin/`
next to a copy of the manifest, ready to be discovered. Outside the repository any build system
works: a plugin needs only `openmine_plugin.h` (C, no OpenMine libraries to link).

### Sample: `plugins/samples/station_import`

* importer `.stn` — total-station coordinate files → survey control points; one undo step; every
  rejected line reported with its line number;
* exporter `.geojson` — control points as GeoJSON;
* commands `stations.summary` (count, extent) and `stations.traverse` (adds a polyline through the
  stations in order).

## Python plugins

A Python plugin is a distribution that declares an entry point in the `openmine.plugins` group
resolving to `register(registry)`:

```toml
# pyproject.toml
[project]
name = "openmine-levels-sample"
version = "1.0.0"
dependencies = ["openmine"]

[project.entry-points."openmine.plugins"]
levels = "openmine_levels:register"
```

```python
def register(registry):
    @registry.command("levels.summary", help="Development count, design metres and volume by level")
    def summary(project, **arguments):
        ...
        return {"675": {"headings": 3, "metres": 412.5, "volume_m3": 12487.1}}
```

* Commands receive an `openmine.Project` and keyword arguments (from the console JSON) and return
  JSON-compatible data or text.
* Each run is wrapped in `project.edit(...)`: **one undo step**, rolled back if the command raises.
* A plugin that fails to import or register is reported; the others still load.
* `openmine_add_python_plugin(<folder> <dist> <version> <package> <entry point>)` stages a package
  with its distribution metadata into `${CMAKE_BINARY_DIR}/plugins/python` for development; an
  installed wheel provides the same metadata.

Sample: `plugins/samples/python_levels` — `levels.summary` (headings, metres and volume by mine
level) and `levels.layers` (creates a CAD layer per level).

## Compatibility and licensing

* The C ABI is versioned. Additions that keep `OpenMineHostV1` binary-compatible append members
  (plugins check `struct_size`); any incompatible change introduces `openmine_plugin_init_v2` and a
  new API version, and the host keeps supporting v1 for at least one major release.
* Plugins may use any licence compatible with being loaded by an Apache-2.0 host. They must not
  contain proprietary Maptek/Deswik code or undocumented formats.
