# modules/mine_design

Underground design as domain objects linked to the schedule.

| Area | Header | Notes |
|---|---|---|
| Developments | `Development.h` | headings on centrelines with derived length, gradient, chainage, section area, volume, tonnes; profiles; watertight excavation envelopes; linked activities; design ↔ schedule impact detection, flagging and controlled recalculation |
| Layouts | `Layout.h` | horizontal alignments with circular curves; switchback and spiral declines and ramps to a target elevation at constant grade; access drives; ore drives offset from contacts; crosscuts at intervals or chainages |
| Heading tools | `Headings.h` | gradient profiles, design rules (grade, grade on curves, radius, grade change), regrading, vertical curves, levels and level assignment, crosscuts in the project |
| Stopes | `Stopes.h` | stopes from outlines or wireframes, tonnage and grade, subdivision into mining blocks |
| Revisions | `Revisions.h` | immutable design snapshots with approval, comparison and restore |
| Geometry edits | `GeometryEdits.h` | project-level CAD edits (transform, offset, trim, extend, split, join, explode, fillet, chamfer, grips, mesh sections) shared by desktop, console and Python — one undo step each, centreline changes flagged |

Tests: `tests/unit/test_mine_design.cpp` (`MineDesign.*`, `Headings.*`, `Profiles.*`). Design rule
for the schedule link: [ADR 0004](../../docs/adr/0004-design-change-flags-not-overwrites.md).
