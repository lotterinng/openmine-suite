# modules/geotechnical

Structural and rock-mass analysis.

| Area | Header | Notes |
|---|---|---|
| Stereonets | `Stereonet.h` | plane/line algebra, equal-area and equal-angle projections, great and small circles, Fisher statistics, orientation tensor, Terzaghi weighting, density contours, joint-set identification, kinematic analysis (planar, wedge, toppling) for a drive direction, SVG output |
| Rock mass | `RockMass.h` | Q-system (Barton tables, ESR, support chart), RMR89 with orientation adjustment, GSI from RMR, generalised Hoek–Brown and rock-mass modulus, Q ↔ RMR correlations; classifications recorded at a heading chainage with their inputs |

Tests: `tests/unit/test_geotechnical.cpp`; `tests/integration/test_stereonet.py` validates
projections and statistics against mplstereonet.
