# Contributing to OpenMine Suite

Thank you for helping. Mining engineers, surveyors and planners are as welcome as developers —
validation datasets and worked examples are some of the most valuable contributions.

## Ground rules

1. **No fake results.** Code must compute real answers or clearly report "not available".
   A feature that depends on an optional component missing from the build is disabled with the
   reason; a format is claimed only when an import or round-trip test passes.
2. **Every model change through `ProjectEditor` / commands** so it is validated, undoable and
   audited. Never write to `Project::mutableModel()` outside commands and persistence.
3. **Never silently alter** survey coordinates, approved designs or baselines.
4. **Numerical code needs reference tests** (hand calculations, published examples or independent
   tools) and documented tolerances/units.
5. **Original work only.** Do not copy code, icons, documentation or formats from proprietary
   products. Clean-room descriptions of *behaviour* are fine; reverse-engineered formats are not.
6. **Licences**: read `docs/licensing.md` before adding a dependency.

## Workflow

* Fork, branch from `main`, keep PRs focused.
* `cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug`
  must pass (see `DEVELOPMENT.md` for dependencies, the PostgreSQL test server and the Xvfb smoke
  test); new behaviour needs tests, and user-visible workflows a check in the GUI smoke test.
* Changed console commands, schema or Python API: regenerate the references (`ctest -L docs`
  says how) — every public Python member needs a docstring.
* Update `IMPLEMENTATION_STATUS.md` when a feature changes state, and add an ADR in `docs/adr/` for
  significant design decisions.
* Commit messages: imperative mood, reference issues (`Fix CPM SF backward pass (#123)`).
* Sign off commits (`git commit -s`) to certify the Developer Certificate of Origin.

## Coding standards

* C++20, `clang-format` (repo `.clang-format`), warnings clean with `-Wall -Wextra -Wpedantic -Wshadow`.
* Public headers in `include/openmine/<area>/`, implementation in `src/`. No Qt/VTK/OCCT types in
  `core` or `modules` public headers.
* Value-type entities; `Uuid` identity; `std::optional` for absent values; exceptions
  (`ValidationError`, `PersistenceError`) at API boundaries; no `using namespace` in headers.
* Files stay focused; module dependencies only point downwards (the graph in `ARCHITECTURE.md`
  has no cycles) and optional integrations are injected as engines rather than linked from modules.
* Python: PEP 8, type hints, `pytest`.

## API versioning

* The C++ API is unstable until 1.0; breaking changes are noted in the changelog.
* The Python package follows semantic versioning from 0.x; a function whose optional component is
  missing from the build raises a clear error (see `openmine.FEATURES`).
* The project file schema is versioned (`PRAGMA user_version`); every schema change needs a
  migration and a test that loads the previous version.
* The plugin C ABI is versioned (`OPENMINE_PLUGIN_API_VERSION`, entry point
  `openmine_plugin_init_v1`); see `docs/plugins.md` for the compatibility rules.
