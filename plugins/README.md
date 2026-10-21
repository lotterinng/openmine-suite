# Plugins

OpenMine loads **native plugins** through a stable C interface, `sdk/include/openmine_plugin.h`
(API version 1). A plugin is a folder with a manifest (`plugin.json`) and a shared library that
exports `openmine_plugin_init_v1`. During initialisation it registers:

- **commands** (run from the command console, the scripting API or the ribbon),
- **importers** and **exporters** by file extension.

Plugins read and change the project only through *entity documents* — the lossless JSON form of
every entity used by the project file and the shared repository. Every change goes through the same
validation as the user interface, is undoable and is audited; changes are grouped with
`begin_edit`/`end_edit` into one undo step. The host rolls back an edit left open, catches
exceptions at the boundary, refuses registrations outside initialisation and duplicate names, and
rejects manifests whose `api_version` it does not provide. See `docs/plugins.md`.

Build a plugin with CMake using `openmine_add_plugin()` (see `sdk/CMakeLists.txt`) or any build
system: it needs only `openmine_plugin.h`.

## Sample

`samples/station_import` — imports total-station coordinate files (`.stn`) as survey control
points (every rejected line reported), exports control points as GeoJSON, and provides the
commands `stations.summary` and `stations.traverse`. It is built and exercised by
`tests/unit/test_plugins.cpp`, together with a deliberately misbehaving plugin
(`tests/plugins/misbehaving`) that checks the host's protections.

Python plugins are packages using the `openmine` Python API, discovered through the
`openmine.plugins` entry-point group; `samples/python_levels` is an example (see `docs/plugins.md`).

Licensing: plugins may use any licence compatible with dynamic loading by an Apache-2.0 host.
