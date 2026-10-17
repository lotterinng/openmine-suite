# ADR 0002 — SQLite project container with sidecar assets and whole-file atomic saves

* Status: accepted (v0.1)

## Context
Projects must be portable single files, robust against crashes during save, inspectable with open
tools, and must not embed multi-gigabyte point clouds in SQL.

## Decision
A `.openmine` file is an SQLite database with a documented schema, explicit FKs and CHECKs,
`application_id` and `user_version`. Large binaries live in `<file>-assets/` and are referenced by
relative path + SHA-256. v0.1 saves write a complete new database to a temp file, verify it, and
rename it over the original (keeping `.bak`).

## Consequences
* Crash-safe saves and trivially consistent files.
* Whole-file writes cost O(model size). This is fine for vector design and schedule data
  (megabytes). Point-cloud data never passes through this path. If projects grow large,
  switch to incremental saves in a WAL-mode working copy.
