# Requirements specification (summary)

Source: the OpenMine Suite master development prompt. This file restates requirements as
testable statements and tracks where each is verified. IDs are stable.

## Functional — foundation and vertical slice (Phase 1 + Step 4)

| ID | Requirement | Verified by |
|---|---|---|
| F-01 | Create, open, save and close a project | `Persistence.*`, GUI smoke test, `test_spec_workflow_round_trip` |
| F-02 | Create a CAD line/polyline interactively and by command | smoke test (viewport clicks + `POLYLINE`) |
| F-03 | Display geometry in the 3D viewport; select by picking | smoke test (pick check), screenshots |
| F-04 | Edit supported properties; reject invalid input without change | smoke test, `Commands.ValidationFailureChangesNothing` |
| F-05 | Persistent unique identifiers for all entities | `Uuid.*`, persistence round-trip |
| F-06 | Create a schedule activity linked to design; show in Gantt | `MineDesign.LinkedActivityIsOneUndoStep`, smoke test |
| F-07 | Geometry + activity links persist across close/reopen | smoke test, `Persistence.RoundTripPreservesEverything` |
| F-08 | Undo/redo for every edit | `Commands.*` |
| F-09 | CPM with FS/SS/FF/SF, lags, floats, critical path | `Cpm.*` |
| F-10 | Design change flags, never silently modifies schedule; baseline preserved | `MineDesign.GeometryChange…`, smoke test |
| F-11 | Schema-driven custom attributes | `Commands.CustomAttributesAreSchemaValidated` |
| F-12 | Python API for the workflow | `scripting/python/tests` |

## Non-functional

| ID | Requirement | Approach |
|---|---|---|
| N-01 | Windows 10/11 x64 target | MSVC + vcpkg preset (CI job defined; not yet executed) |
| N-02 | Offline operation | no network use at runtime |
| N-03 | Data integrity on save | atomic save, integrity + FK checks, `.bak` |
| N-04 | Numerical reliability | double precision, explicit tolerances, reference tests |
| N-05 | Large coordinates | render origin; precision test at Lo magnitudes |
| N-06 | No fake functionality | planned tools disabled/labelled; Python raises `NotImplementedError` |
| N-07 | Licensing | Apache-2.0; LGPL deps dynamically linked; GPL-only deps excluded |
| N-08 | Responsiveness | coalesced recalculation; background workers for heavy jobs (Phase 3) |
