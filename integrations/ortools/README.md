# integrations/ortools

[OR-Tools](https://developers.google.com/optimization) (Apache-2.0) CP-SAT for resource-constrained
schedule optimisation. API: `openmine/optimisation/Optimiser.h`.

The model covers working-calendar time:

- activities have durations that depend on their calendar;
- the four relationship types, with lags;
- renewable resource capacities;
- area limits (concurrent work per zone) and exclusive pairs;
- deadlines, and actuals as of the status date.

Objectives are makespan, weighted completion and lateness. `optimise` explains infeasibility (the
deadlines or capacities that cannot be met). `storeAsScenario` records the optimised sequence as a
scenario, and the CPM engine reproduces it exactly.

Validation: `tests/unit/test_optimiser.cpp` checks that:

- a shared jumbo serialises the work, and the stored scenario reproduces the result;
- weighted completion finishes high-priority work first;
- area limits, dependencies and exclusive pairs are respected;
- infeasible deadlines and capacities are explained;
- shift calendars and the status date are respected.
