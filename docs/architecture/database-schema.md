# Database schema

The project file is an SQLite database of **schema version 2** (49 tables). The complete,
column-level reference with every constraint, index and a generated relationship diagram is
[reference/project-schema.md](../reference/project-schema.md); it is produced from a file written
by the build and checked by `ctest -L docs`, so it cannot drift from the code. This page explains
the design rules behind it. DDL: `core/src/persistence/SchemaV2.cpp` (`persistence::schemaSql()`);
the schema-1 reader for migration: `core/src/persistence/LegacyV1.cpp`.

## Tables by domain

| Domain | Tables |
|---|---|
| Project | `project_info`, `schema_migrations`, `audit_log`, `asset`, `attachment`, `analysis_result`, `report`, `material`, `attribute_definition`, `attribute_value` |
| Coordinates | `coordinate_reference_system`, `local_grid_definition`, `transformation_record` |
| CAD | `cad_layer`, `geometry_object` |
| Survey | `survey_control_point`, `point_cloud_dataset`, `registration`, `registration_pair`, `as_built_model`, `design_conformance_result` |
| Mine design | `tunnel_profile`, `mine_level`, `decline`, `ground_support_class`, `mine_development`, `stope`, `mining_block`, `design_revision` |
| Scheduling | `work_calendar`, `schedule_activity`, `activity_dependency`, `activity_design_link`, `resource`, `activity_resource_assignment`, `development_cycle`, `production_actual`, `production_target`, `schedule_baseline`, `baseline_activity`, `scenario`, `scenario_override` |
| Geology / geotechnical | `drillhole`, `drillhole_survey`, `drillhole_interval`, `geological_object`, `block_model`, `structural_measurement`, `geotech_classification` |

## Design rules

* **Identifiers.** Every entity row has a `uuid` (v4, text) primary key that never changes —
  across undo/redo, save/load, repository checkout and plugin edits. Child rows (calendar
  exceptions, drillhole intervals, scenario overrides, baseline rows, registration pairs) are keyed
  by their parent's uuid plus a sequence or natural key.
* **Metadata.** Entity tables carry `created_at`, `modified_at` (UTC ISO-8601) and `revision`
  (incremented on every committed change; the shared repository uses it for optimistic
  concurrency).
* **Order.** Tables whose order is meaningful (layers, activities, dependencies) store `sort_index`.
* **Geometry.** Vertices are packed little-endian float64 triples in a BLOB with a `CHECK` on its
  length; meshes likewise (vertices + uint32 triangles). B-reps, octrees, block-model arrays,
  report files and attachments live in the asset sidecar, referenced by `asset_uuid`.
* **Enumerations** are stored as text and constrained with `CHECK (… IN (…))`.
* **Explicit relationships.** 57 foreign keys with explicit `ON DELETE` actions.
  `Persistence.SchemaHasExplicitRelationships` checks that every `*_uuid` column is a foreign key
  or one of the documented exceptions below (polymorphic references, the baseline snapshot, the
  audit log, and `current_revision_uuid` of headings and stopes, which would otherwise form a
  cycle with `design_revision`).
* **Polymorphic references** (an entity of any type) cannot be SQL foreign keys; they store the
  type next to the id and are checked by `persistence::checkModel` on save and load:
  `attribute_value(entity_uuid, entity_type)`, `activity_design_link(design_uuid, design_type)`,
  `attachment(entity_uuid, entity_type)`, `design_revision(entity_uuid, entity_type)`,
  `scenario_override(entity_uuid)`.
* **Snapshots are not references.** `baseline_activity` has no foreign key to
  `schedule_activity`: a baseline outlives deleted activities. Approved baselines and approved
  design revisions cannot be deleted; transformation records cannot be deleted (audit trail).

## Deletion policies

SQL actions are the last line of defence; deletions are performed by `ProjectEditor`, which
applies the policy as one undoable command and explains refusals.

| Deleting | Effect | Policy |
|---|---|---|
| A layer | refused while it contains objects or is the last layer | RESTRICT |
| A centreline / geometry | refused while a heading, stope, block, geological object or as-built uses it | RESTRICT |
| A heading | design links removed, activities kept; actuals, as-builts, structures, classifications and point clouds lose the link; conformance results deleted; revisions kept | CASCADE / SET NULL |
| An activity | refused with WBS children or recorded actuals; dependencies, resource assignments, design links and scenario overrides of it removed | RESTRICT / CASCADE |
| A calendar | refused while it is the project default or used by an activity or resource | RESTRICT |
| A point-cloud dataset | refused while it is the source of an as-built or an ICP target; its registrations removed | RESTRICT / CASCADE |
| An applied registration | refused (transformation history) | RESTRICT |
| A stope | refused while it has mining blocks | RESTRICT |
| A material, support class, level, decline | references set to null | SET NULL |
| An asset | refused while any object uses it; unused files are removed by *Compact project* | RESTRICT |
| Any entity with attachments | its attachments are deleted with it (restored by undo) | CASCADE |

## Transaction boundaries

* **In memory:** one user action = one `Command` or `MacroCommand` = one undo step. A macro rolls
  back its executed children if any child throws; an exception inside `CommandStack::MacroScope`
  rolls back the whole scope.
* **On disk:** one save = one SQLite transaction writing a fresh database file
  (`defer_foreign_keys = ON`), followed by `integrity_check`, `foreign_key_check` and the model's
  polymorphic checks, then an atomic rename (see [project-format.md](project-format.md)).
* **Shared repository:** one publish = one PostgreSQL transaction creating a revision; a merge
  that would leave dangling references is rolled back (`Fixture.InconsistentMergesAreRolledBack`).

## Audit

`audit_log` records timestamp (UTC), user, action (`Create`, `Modify`, `Delete`, `Link`,
`Unlink`, `Undo`, `Redo`, `Recalculate`, `Migrate`, `Compact`, …), entity type, entity uuid and a
readable detail. It is append-only: undo adds an entry rather than removing one
(`Commands.AuditLogIsAppendOnly`).

## Migrations

A file of an older schema is read by that schema's reader and written in the current schema on
the next save (the original file is never modified on load). This "read old, write current"
approach means migration code cannot diverge from the current writer.
`Persistence.MigratesSchemaVersion1Projects` builds a schema-1 file with the v0.1 DDL and checks
the migrated model. A newer schema is refused with the supported version in the message.

## PostgreSQL / PostGIS

The shared repository (`integrations/postgres`) stores the same entities as **documents** (the
lossless JSON form of each row and its child rows) per project revision, with a PostGIS
`geometry` column (GiST-indexed) holding each spatial entity's footprint for spatial queries, an
asset table keyed by SHA-256, revision history and advisory locks. Its tables are created on first
use (`Repository(…, initialise=True)`) and PostGIS is enabled if the user may do so.
