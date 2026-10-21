# modules/geology

Drillholes, compositing, block models and grade estimation.

| Area | Header | Notes |
|---|---|---|
| Drillholes | `Drillholes.h` | collar / survey / any number of interval tables from delimited text; every rejected record reported; QA (overlaps, gaps, depth beyond hole); CSV export |
| Desurvey | `Desurvey.h` | minimum curvature, balanced tangent, tangent; vertical default, extrapolation, solid/surface crossings |
| Compositing | `Compositing.h` | length composites with minimum coverage and residual handling, domain and bench composites, density weighting, statistics, quantiles, capping |
| Block models | `BlockModel.h` | regular, optionally rotated about Z; values stored in an asset; domain proportions from solids; grade–tonnage; CSV |
| Estimation | `Estimation.h` | experimental and fitted variograms (anisotropic, nested), cell declustering, nearest neighbour, inverse distance, ordinary and simple kriging with search passes, diagnostics and cross-validation |

Tests: `tests/unit/test_geology.cpp`.
