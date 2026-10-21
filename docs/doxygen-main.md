# OpenMine Suite C++ API {#mainpage}

The public headers of the toolkit-independent core, the domain modules and the integrations.
Start with:

* `openmine::Project`, `openmine::ProjectModel` and `openmine::ProjectEditor` — the model and the
  only way to change it (validated, undoable, audited);
* `openmine::CommandStack` — undo/redo and macros;
* `openmine::persistence` — the `.openmine` file, entity documents and compaction;
* the domain namespaces `openmine::cad`, `openmine::mine_design`, `openmine::survey`,
  `openmine::scheduling`, `openmine::planning`, `openmine::geology`, `openmine::geotechnical`,
  `openmine::reporting`;
* the integrations `openmine::occt`, `openmine::pdalio`, `openmine::o3d`, `openmine::crs`,
  `openmine::gis`, `openmine::optimisation`, `openmine::repository`, `openmine::formats`,
  `openmine::omf`;
* the plugin C ABI in `openmine_plugin.h`.

Architecture: `ARCHITECTURE.md` and `docs/architecture/` in the repository.
