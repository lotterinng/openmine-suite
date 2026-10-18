# SPDX-License-Identifier: Apache-2.0
"""Rock-mass classification and structural analysis.

Planes are ``(dip, dip_direction)`` pairs in degrees (lower hemisphere conventions, dip direction
clockwise from grid north).
"""
from __future__ import annotations

from typing import Dict, List, Optional, Sequence, Tuple

from . import _core

Plane = Tuple[float, float]


def q_system(rqd: float, jn: float, jr: float, ja: float, jw: float = 1.0, srf: float = 1.0) -> dict:
    """Q = (RQD/Jn)(Jr/Ja)(Jw/SRF) with the published parameter ranges enforced."""
    return _core.q_system(dict(rqd=rqd, jn=jn, jr=jr, ja=ja, jw=jw, srf=srf))


def q_support(span: float, esr: float = 1.6, height: float = 0.0, **q_inputs) -> dict:
    """Equivalent dimension, maximum unsupported span, bolt lengths and support pressures."""
    return _core.q_support(q_inputs, span, height, esr)


def rmr89(**inputs) -> dict:
    """RMR89. Inputs: ``ucs_mpa`` or ``point_load_mpa``, ``rqd``, ``spacing_m``, ``condition``
    (dict with persistence_m, aperture_mm, roughness, infilling, weathering) or ``condition_rating``,
    one of ``inflow_l_min_10m`` / ``water_pressure_ratio`` / ``groundwater``, and optionally
    ``joint_dip``, ``joint_dip_direction``, ``drive_trend`` for the orientation adjustment."""
    return _core.rmr89(inputs)


def hoek_brown(gsi: float, mi: float, sigma_ci: float, disturbance: float = 0.0, intact_modulus: float = 0.0) -> dict:
    return _core.hoek_brown(gsi, mi, sigma_ci, disturbance, intact_modulus)


def fisher(planes: Sequence[Plane]) -> dict:
    return _core.fisher(list(planes))


def joint_sets(planes: Sequence[Plane], k: int, max_angle: float = 0.0, seed: int = 1) -> dict:
    return _core.joint_sets(list(planes), k, max_angle, seed)


def kinematics(planes: Sequence[Plane], face: Plane, friction: float = 30.0, set_planes: Sequence[Plane] = ()) -> dict:
    return _core.kinematics(list(planes), tuple(face), friction, list(set_planes))


def stereonet_svg(planes: Sequence[Plane], sets: int = 0, contours: Sequence[float] = (2, 4, 6, 8), friction: Optional[float] = None,
                  title: str = "") -> str:
    return _core.stereonet_svg(list(planes), sets, list(contours), friction, title)
