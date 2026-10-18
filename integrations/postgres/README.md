# integrations/postgres

The shared project repository on PostgreSQL with PostGIS, through libpq (PostgreSQL licence). API:
`openmine/repository/PgRepository.h`.

- Entities are stored as the same lossless documents as the project file mapping, with a full
  revision history.
- Checkout works at any revision. The working-copy state is kept next to the project file.
- `pull` takes an explicit conflict policy (abort, keep mine, take theirs). An inconsistent merge
  rolls back.
- `publish` is serialised. Entity order is merged.
- Assets are content-addressed and verified on checkout.
- Geometry gets PostGIS footprints for spatial queries (`within`).
- Entity locks have a time-to-live.

Validation: `tests/unit/test_pg_repository.cpp` runs against a scratch database. Set
`-DOPENMINE_TEST_PG_DSN="host=… port=… dbname=… user=…"`; the tests are skipped without it. They
cover:

- checkout reproduces the project and its assets;
- two users publish and pull without conflicts;
- conflicts are reported and resolved explicitly;
- deletions, locks and spatial queries;
- inconsistent merges roll back;
- entity order is preserved and merged.

The Python API test runs a repository round trip as well.
