# Plugins — planned (Phase 6)

Two kinds of plugin are planned:

1. **Native plugins**: shared libraries exporting `extern "C" int openmine_plugin_v1(const OpenMineHostV1* host)`.
   The versioned host table will let a plugin register ribbon groups/actions, console commands
   (the console already has a name → handler registry), importers/exporters and analysis tools.
   Plugins may only change the model through the host's command API (undo/audit preserved).
2. **Python plugins**: packages using the public `openmine` API, discovered via the
   `openmine.plugins` entry-point group.

Licensing: plugins may use any licence compatible with dynamic loading by an Apache-2.0 host.
