# ADR 0004 — Design changes flag schedule activities; quantities change only by explicit recalculation

* Status: accepted (v0.1)

## Context
Spec §6.5 requires detecting geometry changes, identifying affected activities and allowing a
*controlled* recalculation without silently modifying approved schedules.

## Decision
Each `MineDevelopment` stores `approvedDesignLength`, the length the schedule quantity was derived
from. Geometry edits made through `mine_design::updateGeometryWithImpact` set `designImpactFlag`
and a descriptive note on linked activities **in the same undo step**. `applyDesignQuantities`
(explicit user action) updates the quantity, re-approves the length, clears the flag and writes a
`Recalculate` audit entry. Baselines are immutable snapshots and are never touched. Restoring the
geometry to the approved length clears stale flags automatically.

## Consequences
Planners see exactly which activities are out of date and why; nothing changes behind their back.
Future: compare volume/profile changes as well as length; per-revision `DesignRevision` records.
