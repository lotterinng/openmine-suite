# The `.openmine` project container

```
JupiterMine.openmine          SQLite 3 database (the project)
JupiterMine.openmine-assets/  sidecar folder for large binaries (planned use: Phase 3)
   pointclouds/<uuid>/…       e.g. octree tiles
   meshes/<uuid>.ply
JupiterMine.openmine.bak      previous version, written by every successful save
```

* **Identification**: `PRAGMA application_id = 0x4F4D494E` ('OMIN'); files without it are refused.
* **Versioning**: `PRAGMA user_version` = schema version; `schema_migrations` records history.
  Older → migrated in a temp copy on load. Newer → refused with a clear message.
* **Integrity**: on save and load, `PRAGMA integrity_check`, `PRAGMA foreign_key_check` and an
  orphaned-attribute check must pass. `openmine::persistence::checkProjectFile()` reports problems
  without loading.
* **Atomic save**: write `<file>.saving` → verify → copy old file to `.bak` → rename over the
  target. Any failure deletes the temp file and leaves the existing project untouched (tested in
  `Persistence.FailedSaveLeavesExistingFileUntouched`).
* **Precision**: vertices are stored as little-endian IEEE-754 doubles (24 bytes per vertex), so
  coordinates round-trip bit-exactly.
* **Portability**: asset paths are relative to the sidecar folder and carry SHA-256 + size so a
  moved/copied project can be verified. The container never stores absolute paths.
* **Not a proprietary format**: the schema is documented here and in `database-schema.md`; any
  SQLite tool can read it.
