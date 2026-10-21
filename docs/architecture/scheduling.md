# Scheduling engine

| Layer | Location | Responsibility |
|---|---|---|
| Working time | `modules/scheduling` — `CalendarTime` | shift calendars, exceptions, intersections, work arithmetic |
| Input | `modules/scheduling` — `ScheduleInput` | rates, cycles, productivity, dated actuals, scenario overrides → working durations |
| CPM | `modules/scheduling` — `Cpm` | forward/backward pass, floats, constraints, reforecast, summaries, baselines |
| Levelling | `modules/scheduling` — `Levelling` | serial schedule-generation scheme, resource profiles |
| Scenario schedule | `modules/scheduling` — `ScenarioSchedule` | the one function every caller uses to get a scenario's dates |
| Periods / production | `modules/scheduling` — `Periods`, `Production` | metres per period, targets, development-cycle rates |
| Optimiser | `integrations/ortools` — `Optimiser` | OR-Tools CP-SAT resource-constrained sequencing |
| 4D | `modules/planning` — `FourD` | state of every design object at an instant, playback, comparison |
| Exports | `modules/reporting` — `ScheduleExport` | Gantt PDF, XLSX workbook, CSV table |

Tests: `tests/unit/test_scheduling.cpp`, `tests/unit/test_optimiser.cpp`,
`tests/unit/test_planning.cpp`, plus the scheduler sections of `scripting/python/tests/test_api.py`
and the desktop smoke test (`apps/desktop/SmokeTest.cpp`).

## 1. Time model (calendar-time CPM)

* Every instant is a `double` number of **minutes since 1970-01-01 00:00 project time**. Using
  doubles keeps fractional durations exact (162 m ÷ 5 m/d = 32.4 standard days).
* Durations are **working minutes** on the activity's *effective calendar*. Calendar time and
  working time are converted only through `CalendarTime`:
  `addWork(t, d)`, `subtractWork(t, d)`, `workBetween(a, b)`, `shift(t, lag)`,
  `nextWorkStart(t)`, `prevWorkEnd(t)`.
* `CalendarTime` throws `CalendarError` ("Calendar has no working time within 30 years") rather
  than looping forever on a calendar with no working periods; CPM reports it as an error.

### Calendars

A `WorkCalendar` has a weekly pattern of `ShiftDefinition{name, startMinute, lengthMinutes,
weekdayMask}`, dated `CalendarException`s and `standardHoursPerDay`. Rules:

1. A shift **belongs to the weekday it starts on**; a shift crossing midnight continues into the
   next day (Sunday's night shift 18:00–04:00 works Monday 00:00–04:00).
2. An exception **defines the complete working periods of its date**. A holiday has none. Shifts
   that started on an exception date do not spill into the following day.
3. A `CalendarTime` built from several calendars works only when **all of them** are working
   (intersection). An empty list means continuous 24/7 time.

New projects get three calendars: *Standard 5-day* (Mon–Fri 24 h, the default), *Continuous
7-day* and *2 × 10 h shifts, 7 days* (06:00–16:00 and 18:00–04:00, 20 standard hours per day).

### Effective calendar of an activity

```
effective = activity calendar (or project default) ∩ calendar of every assigned resource
```

A jumbo whose resource calendar excludes Sundays therefore stops every heading it is assigned to on
Sundays, whatever the heading's own calendar says (`Levelling.ResourceCalendarRestrictsWork`).

## 2. Durations

`buildScheduleInput(project, scenario)` resolves each non-summary activity:

| Activity | Planned working duration |
|---|---|
| Milestone | 0 |
| Task, fixed | `durationHours` |
| Task, rate-driven | `quantity / ratePerWorkMinute` |
| Task, cycle-driven (`useCycleRate`) | `quantity / metresPerWorkHour` of the linked `DevelopmentCycle` |
| Summary (WBS) | not scheduled; rolled up from its children (min ES … max EF, quantity-weighted progress when units agree, else duration-weighted) |

Rate units convert on the effective calendar: per **hour** → `rate / 60`; per **shift** →
`rate / averageShiftMinutes`; per **day** → `rate / standardDayMinutes`. A resource assignment
with `drivesRate` multiplies the rate by the resource's `productivityFactor`.

The development-cycle model (`Production.h`) derives an advance rate from drilling, charging,
blasting/re-entry, mucking (bucket loads from section area × advance × swell), ground support
(bolts from the support class when given), scaling, survey mark-up and services, with shift
length, shifts per day and utilisation. Sequential tasks are summed — no overlap is assumed, which
is conservative for single-heading cycles (`Production.DevelopmentCycleRate`).

### Progress and reforecasting

* Progress comes from **dated `ProductionActual` records** (quantity on a date; source Manual,
  Survey or Import; optional surveyed face chainage). The actual quantity of an activity is the sum
  of its records up to and including the **status date**.
* With a status date the CPM schedules **remaining** work: `remaining = max(0, quantity − actual)`
  converted at the same rate (fixed-duration tasks use the remaining fraction).
* An activity is *started* if it has an actual start or actual quantity, *completed* if it has an
  actual finish or no remaining quantity.

## 3. Forward pass

For activity *j* with working duration *d_j* on calendar *C_j*, predecessor *i*, lag *L*
(working hours, may be negative = lead; applied on the **successor's** calendar):

| Type | Earliest start of *j* is at least |
|---|---|
| FS | `C_j.shift(EF_i, L)` |
| SS | `C_j.shift(ES_i, L)` |
| FF | `C_j.subtractWork(C_j.shift(EF_i, L), d_j)` |
| SF | `C_j.subtractWork(C_j.shift(ES_i, L), d_j)` |

ES is also bounded below by the first working instant after the project start, by
**Start-No-Earlier-Than** constraints and by a scenario *release time* (the `startNoEarlierThan`
override an optimised scenario writes). **Must-Start-On** fixes ES at the constraint date; when the
logic would start later a warning reports negative float. Then `ES = nextWorkStart(ES)` (for
non-zero durations) and `EF = addWork(ES, d)`.

When reforecasting from a status date *S*:

* **completed** activities keep their actual start/finish (or are placed so that they end by *S*)
  and are not moved;
* **in-progress** activities keep their actual start as ES; their *remaining start* is
  `max(S, logic)` and `EF = addWork(remainingStart, remaining)`;
* **not started** activities start no earlier than *S*.

Logic is ordered by Kahn's topological sort. Circular logic is rejected when a dependency is
added (`ValidationError: Dependency would create a circular logic loop`) and again by the sort,
which names the activities involved. Dependencies to or from summary activities are rejected
("link their child activities"); so are resource assignments and design links to summaries.

## 4. Backward pass, floats and criticality

Project finish *T* = max EF. For each activity in reverse topological order, `LF ≤ T`,
`LF ≤ FNLT date` (**Finish-No-Later-Than**), `LF ≤ addWork(MSO date, d)`, and for each successor
*j* (with *LRS_j* = latest remaining start):

| Type | Latest finish of *i* is at most |
|---|---|
| FS | `C_j.shift(LRS_j, −L)` |
| SS | `C_i.addWork(C_j.shift(LRS_j, −L), d_i)` |
| FF | `C_j.shift(LF_j, −L)` |
| SF | `C_i.addWork(C_j.shift(LF_j, −L), d_i)` |

An activity in progress at the status date has already started, so SS/SF relationships *from* it
cannot move its late finish. `LF = prevWorkEnd(LF)`, `LS = nextWorkStart(subtractWork(LF, d))`
(the first working instant with that much work left, not the end of the previous period).

* **Total float** = `workBetween(remainingStart, latestRemainingStart)` in working hours on the
  activity's calendar; it may be **negative** when deadlines or MSO constraints cannot be met.
* **Free float** = minimum over successors of the working time between the relationship's driving
  date and the successor's date minus lag (FS: EF→RS_j, SS: RS→RS_j, FF: EF→EF_j, SF: RS→EF_j),
  and to *T* for activities without successors.
* **Critical** ⇔ total float ≤ 0.001 working minutes. Completed activities are never critical.
* A **FNLT** deadline that is missed produces a warning as well as negative float.

`recomputeFloats` re-runs the backward pass with given early dates held fixed; levelling uses it so
floats of a levelled schedule are logic floats relative to the levelled dates.

Display dates: start date is the date of ES; finish dates are **inclusive** (an activity ending
exactly at midnight finishes on the previous day).

## 5. Baselines and variances

`makeBaseline` captures an immutable `ScheduleBaseline` (dates, durations, quantities per
activity). Baselines are never rewritten by recalculation or by design changes
(ADR 0004). `compareToBaseline` reports start/finish variance in **calendar hours** (positive =
later than baseline) and quantity differences; the Gantt draws baseline bars and the XLSX/PDF
exports carry the variances.

## 6. Scenarios

A `Scenario` overrides inputs without copying the schedule:

| Override field | Applies to | Effect |
|---|---|---|
| `rate` | activity | new rate (disables the cycle rate) |
| `durationHours` | activity | fixed working duration |
| `priority` | activity | levelling/optimiser priority |
| `excluded` | activity | removed from the scenario |
| `startNoEarlierThan` | activity | release time (written by the optimiser) |
| `calendarId` | activity | different calendar |
| `capacity` | resource | units available |
| `productivityFactor` | resource | rate multiplier for activities it drives |
| `areaLimit` | value `area=n` | maximum concurrent activities in a work area |

`calculateScenario(project, scenario)` returns `{input, cpm, result, levelling}`: the input,
the unlevelled CPM, and — **only when the scenario's `levelling` is `Heuristic`** — the levelled
schedule and its report (`Levelling.ScenarioScheduleLevelsOnlyWhenTheScenarioAsks`). The Scheduler
panel, Reports panel, 4D playback, Python `project.scheduler` and console all call this one
function, so a scenario never shows different dates in different places.

## 7. Resource levelling (heuristic)

Serial schedule-generation scheme (`levelResources`):

1. Order activities by **priority** (highest first), then **least CPM total float**, then
   **earliest CPM start**; an activity is only eligible once all its predecessors are placed.
2. Place each at the earliest time that satisfies its logic and constraints, the **capacity of
   every assigned resource** over the whole span `[start, finish)` (a resource is held for the
   activity's full span, including non-working gaps) and **work-area limits**.
3. Recompute floats against the levelled dates. The `LevellingReport` lists delayed activities,
   total delay and makespan increase.

`resourceProfiles(input, result)` gives piecewise-constant usage per resource with peak and
over-allocated spans for any schedule (CPM, levelled or optimised); the scheduler's resource
histogram draws it. The result is feasible, not necessarily optimal
(`Levelling.SharedJumboSerialisesHeadings`: two headings sharing one jumbo are serialised, the
higher-priority heading first, the unconstrained heading untouched).

## 8. Optimisation (OR-Tools CP-SAT)

`integrations/ortools` is a separate layer on the same `ScheduleInput`; it never modifies the base
schedule. Model (time in slots of `slotHours`, from the status date or project start):

* a start variable per remaining activity; its elapsed duration is a **table over start slots**
  computed on its effective calendar, so night shifts, weekends and holidays are respected at slot
  resolution;
* FS/SS/FF/SF with lags (converted to elapsed slots on the successor calendar at the CPM date);
* resources: cumulative constraints (units × 100 as integers); work-area limits: cumulative of
  ones; user-declared mutually exclusive pairs: no-overlap;
* SNET/MSO hard; FNLT hard, or soft lateness under the `Lateness` objective;
* in-progress activities fixed at the status date; completed activities omitted.

Objectives: `Makespan`, `WeightedCompletion` (Σ (1 + priority) · finish + makespan), `Lateness`.
Statuses: Optimal, Feasible, Infeasible, NoSolutionInTime, InvalidModel. Hard constraints carry
assumption literals; an infeasible model is re-solved to extract a sufficient set of conflicting
constraints, reported in plain language ("Deadline", "Must start on", "Capacity", …).

`storeAsScenario` writes the sequence as a **new scenario** of `startNoEarlierThan` overrides on
top of the base scenario's overrides (one undo step). CPM on that scenario reproduces the
optimised dates exactly and computes floats (`Optimiser.SharedJumboSerialisesAndScenarioReproducesIt`).

## 9. Periods, production and 4D

* `quantitiesByPeriod` spreads each activity's **remaining** quantity uniformly over the working
  time of its remaining work on its effective calendar; actuals come from dated records; targets
  (`ProductionTarget`) are prorated by calendar-day overlap. Periods: day, week, month, quarter,
  year; filters by unit, zone and level. `summarise` produces scenario comparison rows.
* `planning::snapshot` gives the state (not scheduled / future / in progress / complete), planned
  fraction (work-time interpolation on the activity calendar), actual fraction, baseline fraction,
  *behind plan* flag, active resources, planned face chainage and excavated centreline of every
  linked design object at an instant; `excavatedEnvelope` cuts the development's swept envelope at
  that fraction; `compareSnapshots` gives per-object differences between two schedules. Colour
  modes: state, start period (viridis ramp), resource, activity.

## 10. Design ↔ schedule integration

Activities link to design objects (`ActivityDesignLink`). When a linked design object's geometry
changes, the change is applied through `ProjectEditor::updateGeometryWithImpact`, which **flags**
the linked activities with the old and new derived quantities — it never changes their
quantities, durations or baselines. Recalculation of a flagged activity is an explicit, audited,
undoable action (ADR 0004). Survey-derived face positions can be entered as `ProductionActual`
records with source *Survey*, a dataset id and a face chainage.

## 11. Validation

`Cpm.ReferenceNetworkAllRelationshipTypes` — a hand-calculated five-activity network exercising FS,
SS(+2 d), FF(+1 d) and SF(+3 d) on the *Continuous 7-day* calendar (values in days from
2026-01-05):

| Act | d | ES | EF | LS | LF | TF | FF | Critical |
|---|---|---|---|---|---|---|---|---|
| A | 4 | 0 | 4 | 0 | 4 | 0 | 0 | ✓ |
| B | 6 | 4 | 10 | 4 | 10 | 0 | 0 | ✓ |
| C | 3 | 2 | 5 | 3 | 6 | 1 | 1 | |
| D | 5 | 6 | 11 | 6 | 11 | 0 | 0 | ✓ |
| E | 2 | 11 | 13 | 11 | 13 | 0 | 0 | ✓ |

Other tests and what they pin down:

| Test | Expectation |
|---|---|
| `CalendarTime.FiveDayCalendarWithHoliday` | weekend and holiday skipped in work arithmetic |
| `CalendarTime.NightShiftCrossesMidnightAndIntersection` | night shift owned by its start day; intersection of two calendars |
| `Cpm.RateDrivenDurationOnFiveDayCalendar` | 150 m @ 5 m/d → 30 working days, 2026-11-02 → 2026-12-11 |
| `Cpm.ShiftCalendarRates` | 60 m @ 3 m/shift on 2 × 10 h → 200 h, 2026-11-02 00:00 → 2026-11-12 00:00 |
| `Cpm.ReforecastFromDatedActualsAndStatusDate` | ten dated actuals (50 m) → 100 m remaining from 2026-11-16, same finish 2026-12-11, actual start kept |
| `Cpm.ConstraintsMilestonesDeadlinesNegativeFloat` | SNET; FNLT milestone missed by one day → −24 h float, critical, warning |
| `Cpm.SummaryRollUpAndCycleRejection` | WBS roll-up across a weekend (9 days); loop rejected |
| `Cpm.InProgressStartToStartAndWorkingTimeLatestStarts` | SS from an in-progress activity; LS on the first working instant |
| `Production.DevelopmentCycleRate` | cycle breakdown and metres per shift/day/work hour |
| `Levelling.*` (3) | serialisation by priority; levelling only when asked; resource calendar |
| `Periods.ScheduledActualTargetMetresPerWeek` | weekly scheduled, actual and target metres |
| `Scenario.OverridesChangeRatesAndCapacity` | rate and capacity overrides change dates; base unchanged |
| `Optimiser.*` (5) | optimal makespan with a shared jumbo reproduced by CPM; weighted completion; area limits, dependencies, exclusive pairs; infeasible deadlines and capacity explained; shift calendars and status date |
| `FourD.*` (5) | states and excavated centreline; envelope volume ∝ fraction; behind-plan against baseline; playback frames and scenario comparison; colour ramps |

## 12. Known limitations

* Calendars are evaluated at minute resolution; the optimiser at `slotHours` resolution (default
  1 h). The CPM run on the stored scenario gives the exact dates.
* Levelling holds a resource for an activity's whole span; it does not split activities
  (no pre-emption) and does not model partial resource units over time.
* The cycle model assumes sequential cycle tasks (no overlap of support and drilling, no
  multi-heading interleaving of a single jumbo inside the cycle).
* Progress is quantity-based. Out-of-sequence progress is accepted (the activity is scheduled as
  in progress) but there is no "retained logic vs progress override" switch; retained logic is
  always used.
