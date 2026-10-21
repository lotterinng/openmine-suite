# The `.openmine` project container

```
JupiterMine.openmine              SQLite 3 database (the project), schema version 2
JupiterMine.openmine-assets/      sidecar folder for large binaries
   pointcloud/<uuid>/             point-cloud octree (folder asset, see formats/octree.md)
       manifest.json  octree.json  nodes/<name>.bin
   mesh/<uuid>.ply                meshes too large to keep inline
   brep/<uuid>.brep               Open CASCADE solids
   blockmodel/<uuid>.bin          block-model value arrays
   attachment/<uuid>.<ext>        attached documents and photos
   report/<uuid>.pdf|.xlsx|.html  generated reports
JupiterMine.openmine.bak          previous version, written by every successful save
```

* **Identification**: `PRAGMA application_id = 0x4F4D494E` ('OMIN'); files without it are refused
  ("Not an OpenMine project file").
* **Versioning**: `PRAGMA user_version` = schema version (current: **2**); `schema_migrations`
  records the history. A file of an older schema is read with that version's reader
  (`core/src/persistence/LegacyV1.cpp` for schema 1, OpenMine 0.1) and written in the current
  schema on the next save — migration cannot drift from the current schema because the writer is
  always the current one. The load report says `migrated`, and an audit entry records it. A file of
  a newer schema is refused with the supported version in the message.
* **Integrity**: on save and load, `PRAGMA integrity_check`, `PRAGMA foreign_key_check` and the
  model's referential checks must pass. Assets are verified on load (existence and size; a full
  SHA-256 check on request); problems are reported as warnings, never ignored.
* **Atomic save**: write `<file>.saving` → verify → copy the old file to `.bak` → rename over the
  target. Any failure deletes the temporary file and leaves the existing project untouched
  (`Persistence.FailedSaveLeavesExistingFileUntouched`). *Save As* copies the sidecar assets
  (`Persistence.SaveAsCopiesSidecarAssets`).
* **Precision**: vertices are stored as little-endian IEEE-754 doubles (24 bytes per vertex), so
  coordinates round-trip bit-exactly (`Persistence.RoundTripPreservesEveryEntityType`, and the
  Python specification test compares vertex arrays exactly after reopening).
* **Portability**: asset paths are relative to the sidecar folder and carry SHA-256 and size, so
  a moved or copied project folder can be verified. The container never stores absolute paths.
  Unsaved projects keep their assets in a temporary root that is relocated on the first save.
* **Immutability of assets**: processing never rewrites an asset; derived data gets a new one.
  *Compact project* removes assets no entity references.
* **Not a proprietary format**: the schema is documented in
  [reference/project-schema.md](../reference/project-schema.md) (generated from the live DDL and
  checked by `ctest -L docs`) and [database-schema.md](database-schema.md); any SQLite tool can
  read it.

## Entity documents

Every entity also has a lossless JSON *document* form (`persistence/EntityCodec`): `{"uuid",
"columns", "children"}` where `columns` are the entity table's columns and `children` the rows of
its child tables. Documents are used by the PostgreSQL shared repository, the plugin C ABI and
`project.documents` in Python. Encoding is deterministic, so a document's SHA-256 identifies its
content (`EntityCodec.DocumentsRoundTripLosslesslyAndDeterministically`).

## Shared repository (PostgreSQL/PostGIS)

`integrations/postgres` stores projects as revisions of entity documents in PostgreSQL 16 with
PostGIS 3.4 geometry columns for spatial queries. Working copies check out a revision into a local
`.openmine` project; publishing is refused when the working copy is behind, pulls apply an
explicit conflict policy (`abort`, `take_remote`, `keep_local`), and advisory locks reserve
entities. Assets are stored once by SHA-256. Tests: `tests/unit/test_pg_repository.cpp` (run when
`OPENMINE_TEST_PG_DSN` points to a server; see DEVELOPMENT.md).
