# ADR 0001 — Toolkit-independent core with command-based editing

* Status: accepted (v0.1)

## Context
The suite must be scriptable from Python, testable in CI without a GPU, and later support an
enterprise database. UI frameworks (Qt), renderers (VTK) and kernels (OCCT) change on their own
schedules.

## Decision
`core` and `modules/*` are plain C++20 with SQLite as the only external dependency. All edits go
through `ProjectEditor`, which validates and executes snapshot-based `Command`s on a
`CommandStack`. Commands publish events and append audit entries. Qt, VTK and OCCT are confined to
`apps/` and `integrations/`.

## Consequences
* Undo/redo, auditing and validation are uniform across UI, console and Python.
* Headless tests cover the engine (34 C++ tests + Python tests run without a display).
* Snapshot commands copy whole entities; for very large point clouds, commands will reference
  immutable assets instead of copying data (Phase 3).
