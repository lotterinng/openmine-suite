# modules/scheduling

Calendar-time scheduling. Design and validation: [docs/architecture/scheduling.md](../../docs/architecture/scheduling.md).

| Area | Header | Notes |
|---|---|---|
| Working time | `CalendarTime.h` | shift calendars with exceptions, intersections of activity and resource calendars, work arithmetic in minutes |
| Input | `ScheduleInput.h` | rates, cycle rates, resource productivity, dated actuals and scenario overrides resolved into working durations |
| CPM | `Cpm.h` | forward/backward pass for FS/SS/FF/SF with lags, SNET/FNLT/MSO, negative float, reforecast from a status date, WBS roll-up, baselines and variances |
| Levelling | `Levelling.h` | serial schedule-generation scheme with priorities, resource and work-area limits; resource usage profiles |
| Production | `Production.h` | development cycle model (drill, charge, blast, muck, support, other) → advance rates |
| Periods | `Periods.h` | scheduled / actual / target quantities per day, week, month, quarter, year; scenario summaries |
| Scenarios | `ScenarioSchedule.h` | the one function every caller uses to get a scenario's dates (CPM, plus levelling when asked) |
| Tables | `ScheduleTable.h` | export-ready rows shared by CSV, XLSX, PDF and reports |

Optimisation with OR-Tools is a separate layer: `integrations/ortools`. Tests:
`tests/unit/test_scheduling.cpp`, `tests/unit/test_optimiser.cpp`.
