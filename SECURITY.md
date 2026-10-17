# Security policy

## Supported versions

Only the latest release on `main` receives fixes while the project is pre-1.0.

## Reporting a vulnerability

Please **do not** open a public issue. Use GitHub's private vulnerability reporting
("Security" → "Report a vulnerability") on the project repository. Include affected version, a
description, reproduction steps or a minimal project file, and impact.
We aim to acknowledge reports within 5 working days.

## Scope notes

* Project files (`.openmine`) are SQLite databases. OpenMine verifies `application_id`, schema
  version, integrity and foreign keys before loading, but treat files from untrusted sources with
  care, as you would any document format.
* Python scripts and plugins run with the user's permissions. Only run scripts and plugins you
  trust.
* The enterprise PostgreSQL backend (planned) will require TLS and least-privilege roles.
