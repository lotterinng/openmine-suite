# SPDX-License-Identifier: Apache-2.0
"""OpenMine Suite Python API.

The public API mirrors the intended design in the master specification. Functions that
exist in the native core are implemented; functions that depend on modules not yet built
(point-cloud survey, design conformance) raise ``NotImplementedError`` with the planned
phase rather than returning simulated results.

Example::

    from openmine import Project
    project = Project.new("Jupiter Mine", start="2026-11-02")
    design = project.cad.create_development(name="LEG2_675_D3_DEC", length=150, width=5.5, height=5.5,
                                            start=(-25000, 2900000, 1200), azimuth_deg=270, gradient_percent=-12.5)
    activity = project.scheduler.create_activity(name="Develop LEG2_675_D3_DEC", geometry=design, production_rate=5.0)
    print(project.scheduler.calculate().activity(activity.id)["finish"])
    project.save("JUP_Mine.openmine")
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from os import PathLike
from typing import Iterable, Optional, Sequence, Tuple, Union

from . import _core
from ._core import PersistenceError, ValidationError

__all__ = ["Project", "Development", "Activity", "ScheduleResult", "ValidationError", "PersistenceError", "__version__"]
__version__ = _core.__version__

Point = Tuple[float, float, float]


@dataclass(frozen=True)
class Development:
    """A development heading (domain object referencing centreline geometry)."""
    project: "Project"
    id: str

    @property
    def info(self) -> dict:
        return self.project._native.development(self.id)

    @property
    def heading_id(self) -> str:
        return self.info["heading_id"]

    @property
    def length(self) -> float:
        return self.info["length"]

    @property
    def geometry_id(self) -> str:
        return self.info["geometry_id"]


@dataclass(frozen=True)
class Activity:
    project: "Project"
    id: str

    @property
    def info(self) -> dict:
        return self.project._native.activity(self.id)

    def record_actual(self, quantity: float) -> None:
        self.project._native.set_actual(self.id, float(quantity))

    def apply_design_quantities(self) -> None:
        """Controlled, audited recalculation of the quantity from the linked design."""
        self.project._native.apply_design_quantities(self.id)


class ScheduleResult:
    def __init__(self, raw: dict):
        self.raw = raw
        self.ok: bool = raw["ok"]
        self.errors: list = raw["errors"]
        self.warnings: list = raw["warnings"]
        self.project_start: str = raw["project_start"]
        self.project_finish: str = raw["project_finish"]

    def activity(self, activity_id: str) -> dict:
        return self.raw["activities"][activity_id]


class _Cad:
    def __init__(self, project: "Project"):
        self._p = project

    def create_polyline(self, points: Iterable[Sequence[float]], name: str = "", layer: str = "0", closed: bool = False) -> str:
        pts = [tuple(float(c) for c in p) for p in points]
        if any(len(p) != 3 for p in pts):
            raise ValueError("points must be (x, y, z) triples")
        return self._p._native.add_polyline(pts, name, layer, closed)

    def geometry(self, geometry_id: str) -> dict:
        return self._p._native.geometry(geometry_id)

    def update_vertices(self, geometry_id: str, points: Iterable[Sequence[float]]) -> int:
        """Edit geometry. Linked schedule activities are flagged (never silently changed).
        Returns the number of activities flagged."""
        return self._p._native.update_vertices(geometry_id, [tuple(float(c) for c in p) for p in points])

    def create_development(self, name: str, length: Optional[float] = None, width: float = 5.0, height: float = 5.0, *,
                           points: Optional[Iterable[Sequence[float]]] = None, start: Point = (0.0, 0.0, 0.0),
                           azimuth_deg: float = 0.0, gradient_percent: float = 0.0, level: str = "",
                           layer: str = "Development Centrelines") -> Development:
        """Create a development heading.

        Either give ``points`` (a centreline) or ``length`` for a straight heading from
        ``start`` on bearing ``azimuth_deg`` (clockwise from +Y) at ``gradient_percent``.
        ``length`` is the 3D (slope) length in metres.
        """
        if points is None:
            if length is None or length <= 0:
                raise ValueError("give either points or a positive length")
            g = gradient_percent / 100.0
            plan = length / math.sqrt(1.0 + g * g)
            az = math.radians(azimuth_deg)
            x0, y0, z0 = (float(c) for c in start)
            points = [(x0, y0, z0), (x0 + plan * math.sin(az), y0 + plan * math.cos(az), z0 + plan * g)]
        gid = self.create_polyline(points, name=f"{name} centreline", layer=layer)
        return Development(self._p, self._p._native.create_development(gid, name, width, height, level))


class _Scheduler:
    def __init__(self, project: "Project"):
        self._p = project

    def create_activity(self, name: str, geometry: Optional[Development] = None, production_rate: Optional[float] = None,
                        duration_days: float = 1.0) -> Activity:
        """Create an activity. With ``geometry`` and ``production_rate`` it is a rate-driven
        activity linked to the development (quantity = centreline length)."""
        if geometry is not None:
            if production_rate is None:
                raise ValueError("a geometry-linked activity needs production_rate (m per working day)")
            return Activity(self._p, self._p._native.create_linked_activity(geometry.id, float(production_rate), name))
        return Activity(self._p, self._p._native.add_activity(name, float(duration_days), "Task"))

    def add_dependency(self, predecessor: Activity, successor: Activity, type: str = "FS", lag_days: float = 0.0) -> str:
        return self._p._native.add_dependency(predecessor.id, successor.id, type, float(lag_days))

    def calculate(self) -> ScheduleResult:
        return ScheduleResult(self._p._native.calculate())

    def capture_baseline(self, name: str) -> None:
        self._p._native.capture_baseline(name)

    def set_status_date(self, date: Optional[str]) -> None:
        self._p._native.set_status_date(date)

    def activities(self) -> list:
        return self._p._native.activities()


class _Survey:
    def __init__(self, project: "Project"):
        self._p = project

    def import_pointcloud(self, path: Union[str, PathLike]):
        raise NotImplementedError("Point-cloud import (LAS/LAZ/E57) is planned for Phase 3 and is not available in this build")

    def compare_design(self, design: Development, asbuilt):
        raise NotImplementedError("Design-vs-as-built conformance is planned for Phase 6 and is not available in this build")


class Project:
    """An OpenMine project (.openmine). Every edit is validated, undoable and audited."""

    def __init__(self, native: "_core.NativeProject"):
        self._native = native
        self.cad = _Cad(self)
        self.scheduler = _Scheduler(self)
        self.survey = _Survey(self)

    @classmethod
    def new(cls, name: str, start: str) -> "Project":
        return cls(_core.NativeProject.create(name, start))

    @classmethod
    def open(cls, path: Union[str, PathLike]) -> "Project":
        return cls(_core.NativeProject.load(path))

    def save(self, path: Optional[Union[str, PathLike]] = None) -> None:
        target = path if path is not None else self._native.file_path
        if not str(target):
            raise ValueError("first save needs a path")
        self._native.save(target)

    @property
    def name(self) -> str:
        return self._native.name

    @property
    def is_modified(self) -> bool:
        return self._native.is_modified

    def undo(self) -> None:
        self._native.undo()

    def redo(self) -> None:
        self._native.redo()

    def set_crs(self, name: str, authority_code: str = "") -> None:
        """Declare the project CRS (it is never inferred from coordinates)."""
        self._native.set_crs(name, authority_code)

    def audit_log(self) -> list:
        return self._native.audit_log()
