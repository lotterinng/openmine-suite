# modules/planning

4D mine planning (`FourD.h`): the state of every scheduled design object at an instant —
planned, actual and baseline progress, excavated part of each heading, resources at work —
playback frames, excavated envelopes cut at the planned face, colour modes (state, start period,
resource, activity) and per-object comparison of two schedules (base vs scenario).

Tests: `tests/unit/test_planning.cpp` (`FourD.*`).
