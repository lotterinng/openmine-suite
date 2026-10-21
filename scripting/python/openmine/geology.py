# SPDX-License-Identifier: Apache-2.0
"""Geostatistics helpers that do not need a project (project operations are on
``Project.geology``)."""
from __future__ import annotations

from typing import List, Optional, Sequence

import numpy as np

from . import _core


def statistics(values: Sequence[float], weights: Sequence[float] = ()) -> dict:
    """Count, min/max, mean, median, variance, standard deviation, CV and percentiles (p10–p99) of
    sample values, weighted when ``weights`` are given."""
    return _core.statistics(list(values), list(weights))


def experimental_variogram(points, values: Sequence[float], lag: float, lags: int, azimuth: float = 0.0, dip: float = 0.0,
                           angle_tolerance: float = 90.0, bandwidth: float = 1e300, pairwise_relative: bool = False) -> List[tuple]:
    """``[(mean distance, gamma, pairs), …]`` per lag."""
    return _core.experimental_variogram(np.asarray(points, dtype=float), list(values), lag, lags, azimuth, dip, angle_tolerance, bandwidth,
                                        pairwise_relative)


def fit_variogram(lags, initial: dict, direction=(1.0, 0.0, 0.0), fix_nugget: bool = False) -> dict:
    """Weighted least squares (Cressie) Levenberg–Marquardt fit of nugget, sills and ranges."""
    return _core.fit_variogram([(float(d), float(g), int(n)) for d, g, n in lags], initial, list(direction), fix_nugget)


def composites_to_arrays(composites: List[dict], field: str):
    """(points (n,3), values (n,)) of the composites that have ``field``."""
    rows = [c for c in composites if field in c["values"]]
    pts = np.array([[c["x"], c["y"], c["z"]] for c in rows], dtype=float).reshape(-1, 3)
    vals = np.array([c["values"][field] for c in rows], dtype=float)
    return pts, vals
