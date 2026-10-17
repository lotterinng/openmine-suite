# Scheduling engine

Implementation: `modules/scheduling` (`WorkingTime`, `Cpm`). Tests: `tests/unit/test_scheduling.cpp`.

## Time model

* The CPM runs in **working-day offsets** on the project's default calendar.
* Offset `0.0` = start of the first working day on/after the project schedule start.
* An activity occupying `[s, f)` starts on working day `floor(s)` and finishes on working day
  `ceil(f) − 1` (finish dates are inclusive, as in common scheduling practice).
* Milestones (duration 0) are dated at their start.
* Fractional durations are allowed (e.g. 162 m ÷ 5 m/d = 32.4 d); dates round as above.
* Lags are in working days and may be negative (leads).

## Durations

| Activity | Duration used |
|---|---|
| Task, fixed | `durationDays` (× remaining fraction by quantity when a status date is set) |
| Task, rate-driven | `quantity / ratePerDay`, or `max(0, quantity − actual) / ratePerDay` when a status date is set |
| Milestone | 0 |
| Summary (WBS) | roll-up: min ES … max EF of children |

## Forward / backward pass

For successor *j* of predecessor *i* with lag *L*:

| Type | Forward (ES_j ≥) | Backward (LF_i ≤) | Free-float slack |
|---|---|---|---|
| FS | EF_i + L | LS_j − L | ES_j − L − EF_i |
| SS | ES_i + L | LS_j − L + d_i | ES_j − L − ES_i |
| FF | EF_i + L − d_j | LF_j − L | EF_j − L − EF_i |
| SF | ES_i + L − d_j | LF_j − L + d_i | EF_j − L − ES_i |

ES is also bounded below by 0, by start-no-earlier-than constraints and (when reforecasting) by
the status date. Project finish *T* = max EF; LF of activities without successors = *T*.
Total float = LS − ES; free float = min slack (or *T* − EF); critical ⇔ TF ≤ 1e-6 d.
Circular logic is rejected when the dependency is added and again detected by the topological
sort (Kahn) with the activities involved named.

## Validation

`Cpm.ReferenceNetworkAllRelationshipTypes` checks a hand-calculated five-activity network that
exercises FS, SS(+2), FF(+1) and SF(+3) on a 7-day calendar:

| Act | d | ES | EF | LS | LF | TF | FF | Critical |
|---|---|---|---|---|---|---|---|---|
| A | 4 | 0 | 4 | 0 | 4 | 0 | 0 | ✓ |
| B | 6 | 4 | 10 | 4 | 10 | 0 | 0 | ✓ |
| C | 3 | 2 | 5 | 3 | 6 | 1 | 1 | |
| D | 5 | 6 | 11 | 6 | 11 | 0 | 0 | ✓ |
| E | 2 | 11 | 13 | 11 | 13 | 0 | 0 | ✓ |

Other tests: 5-day calendar with holiday; rate-driven 150 m @ 5 m/d → 30 working days,
2026-11-02 → 2026-12-11; reforecast with 50 m actual from status date 2026-11-16 → same finish;
SNET + milestone; WBS roll-up.

## Known limitations (v0.1)

* All activities use the project default calendar (a warning is raised if an activity names a
  different calendar). Per-activity/resource calendars need calendar-time CPM — planned.
* Dependencies to/from summary activities are rejected.
* Progress is a single actual quantity per activity; dated `ProductionActual` records, actual
  start/finish dates and out-of-sequence progress handling are planned (Phase 5).
* No resource constraints; OR-Tools CP-SAT RCPSP layer planned (Phase 6) as a separate engine
  whose output is a scenario, never an overwrite of the CPM results or baselines.
