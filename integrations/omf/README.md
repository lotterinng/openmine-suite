# integrations/omf

Our own implementation of [OMF v1](https://github.com/gmggroup/omf) (Open Mining Format):
a JSON header plus zlib-compressed binary arrays. API: `openmine/omf/Omf.h` (document model,
`readOmf` / `writeOmf`) and `openmine/omf/OmfProject.h` (project export and import; the mapping is
documented in the header).

- Every element or data array that has no equivalent is listed in the notes. Nothing is dropped
  silently.
- Imports are one undo step.

Validation:

- `tests/unit/test_omf.cpp`:
  - a byte round trip that preserves everything;
  - refusal of invalid and foreign files;
  - segment chaining;
  - a project export/import round trip.
- `tests/integration/test_omf.py` checks our files with the reference `omf` Python package and
  imports files it writes.
