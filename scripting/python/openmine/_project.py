# SPDX-License-Identifier: Apache-2.0
"""Project facade: ``Project`` with the ``cad``, ``design``, ``scheduler``, ``survey``, ``geology``,
``geotech``, ``formats`` and ``documents`` namespaces."""
from __future__ import annotations

import contextlib
import math
from dataclasses import dataclass
from os import PathLike
from typing import Any, Dict, Iterable, Iterator, List, Optional, Sequence, Tuple, Union

import numpy as np

from . import _core

PointLike = Sequence[float]
PathType = Union[str, "PathLike[str]"]


def _points(pts) -> np.ndarray:
    a = np.asarray(pts, dtype=float)
    if a.ndim == 1:
        a = a.reshape(1, -1)
    if a.shape[1] == 2:
        a = np.column_stack([a, np.zeros(len(a))])
    return np.ascontiguousarray(a)


def _id(obj) -> str:
    return obj if isinstance(obj, str) else obj.id


def _geometry_id(obj) -> str:
    """A geometry id, or the centreline of a Development."""
    return obj.geometry_id if isinstance(obj, Development) else _id(obj)


def _as_list(ids) -> list:
    return [ids] if isinstance(ids, str) or hasattr(ids, "id") else list(ids)


def _vec3(p) -> np.ndarray:
    a = np.asarray(p, dtype=float).reshape(-1)
    if a.size == 2:
        return np.array([a[0], a[1], 0.0])
    if a.size != 3:
        raise ValueError("A point needs 2 or 3 coordinates")
    return a


def _translation(d) -> np.ndarray:
    m = np.eye(4)
    m[:3, 3] = _vec3(d)
    return m


def _rotation(axis, radians: float, about) -> np.ndarray:
    a = np.asarray(axis, dtype=float)
    if np.linalg.norm(a) < 1e-15:
        raise ValueError("The rotation axis has no length")
    x, y, z = a / np.linalg.norm(a)
    c, s, t = math.cos(radians), math.sin(radians), 1 - math.cos(radians)
    r = np.array([[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
                  [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
                  [t * x * z - s * y, t * y * z + s * x, t * z * z + c]])
    p = _vec3(about)
    m = np.eye(4)
    m[:3, :3] = r
    m[:3, 3] = p - r @ p
    return m


def _scaling(f: float, about) -> np.ndarray:
    p = _vec3(about)
    m = np.eye(4)
    m[:3, :3] *= f
    m[:3, 3] = p - f * p
    return m


def _mirror(p, n) -> np.ndarray:
    n = np.asarray(n, dtype=float)
    r = np.eye(3) - 2 * np.outer(n, n)
    p = _vec3(p)
    m = np.eye(4)
    m[:3, :3] = r
    m[:3, 3] = p - r @ p
    return m


# ---- domain handles -----------------------------------------------------------------------------------
@dataclass(frozen=True)
class Development:
    """A development heading (references its centreline geometry)."""
    project: "Project"
    id: str

    @property
    def document(self) -> dict:
        """The heading's entity document."""
        return self.project.documents.get("MineDevelopment", self.id)

    @property
    def heading_id(self) -> str:
        """The heading identifier (e.g. "LEG2_675_D3_DEC")."""
        return self.document["columns"]["heading_id"]

    @property
    def geometry_id(self) -> str:
        """Id of the centreline geometry."""
        return self.document["columns"]["geometry_uuid"]

    @property
    def metrics(self) -> dict:
        """Derived design quantities: length, horizontal length, gradient, chainage, section area, volume,
        tonnes and loose volume."""
        return self.project._native.development_metrics(self.id)

    @property
    def length(self) -> float:
        """3D centreline length (m)."""
        return self.metrics["length"]

    @property
    def info(self) -> dict:
        """Heading fields with its metrics and linked activities."""
        d = dict(self.document["columns"])
        d.update(self.metrics)
        return d

    def centreline(self) -> np.ndarray:
        """Centreline vertices ``(n, 3)``."""
        return self.project._native.development_centreline(self.id)

    def stations(self, interval: float = 10.0) -> List[dict]:
        """Chainage stations every ``interval`` m with position and formatted chainage."""
        return self.project._native.development_stations(self.id, interval)

    def envelope(self) -> Tuple[np.ndarray, np.ndarray]:
        """Excavation solid as a closed mesh (vertices, triangles)."""
        return self.project._native.development_envelope(self.id)


@dataclass(frozen=True)
class Activity:
    project: "Project"
    id: str

    @property
    def document(self) -> dict:
        """The activity's entity document."""
        return self.project.documents.get("ScheduleActivity", self.id)

    @property
    def info(self) -> dict:
        """Activity fields with its linked designs."""
        d = dict(self.document["columns"])
        d["linked_designs"] = self.project.scheduler.linked_designs(self.id)
        return d

    def record_actual(self, quantity: float, date: str, note: str = "") -> str:
        """Records dated actual progress (an incremental quantity on ``date``); returns its id."""
        return self.project._native.record_actual(self.id, date, float(quantity), note)


class ScheduleResult:
    def __init__(self, raw: dict):
        self.raw = raw
        self.ok: bool = raw["ok"]
        self.errors: List[str] = raw["errors"]
        self.warnings: List[str] = raw["warnings"]
        self.start: Optional[str] = raw.get("start")
        self.finish: Optional[str] = raw.get("finish")
        #: Present when the scenario levels resources: delayed activities, total delay, makespan increase.
        self.levelling: Optional[dict] = raw.get("levelling")

    def activity(self, activity_id) -> dict:
        """Timing of one activity: start, finish, late dates, floats, duration, critical flag."""
        return self.raw["activities"][_id(activity_id)]

    @property
    def activities(self) -> Dict[str, dict]:
        """Timing of every activity by id."""
        return self.raw["activities"]

    @property
    def critical_path(self) -> List[str]:
        """Ids of the critical activities in start order."""
        return sorted((k for k, v in self.activities.items() if v["critical"]), key=lambda k: self.activities[k]["start"])

    def __repr__(self) -> str:
        return f"ScheduleResult(ok={self.ok}, start={self.start!r}, finish={self.finish!r}, activities={len(self.activities)})"


# ---- namespaces --------------------------------------------------------------------------------------------
class _Namespace:
    def __init__(self, project: "Project"):
        self._p = project
        self._n = project._native


class Documents(_Namespace):
    """Generic, lossless access to every entity type (the format used by the project file and the
    shared repository). ``put`` validates exactly like the user interface."""

    types = staticmethod(_core.NativeProject.document_types)

    def list(self, type: str) -> List[dict]:
        """Every entity of a type as documents ``{"uuid", "columns", "children"}``."""
        return self._n.documents(type)

    def get(self, type: str, key: str) -> Optional[dict]:
        """One entity document, or ``None``."""
        return self._n.document(type, key)

    def template(self, type: str) -> dict:
        """A document with the type's default values (for :meth:`put`)."""
        return _core.NativeProject.template(type)

    def put(self, type: str, document: dict, text: str = "") -> str:
        """Adds or replaces an entity from a document (validated like the user interface; one undo
        step). Returns its key."""
        return self._n.put(type, document, text)

    def delete(self, type: str, key: str) -> None:
        """Deletes an entity applying the documented deletion policy (restrict, cascade or set null)."""
        self._n.delete(type, key)

    def check(self) -> List[str]:
        """Model integrity problems; empty when consistent."""
        return self._n.check()


class Cad(_Namespace):
    def layers(self) -> List[dict]:
        """Layers: ``{"id", "name", "color", "visible", "locked"}``."""
        return self._n.layers()

    def add_layer(self, name: str, color: int = 0xD0D0D0) -> str:
        """A new layer (``color`` 0xRRGGBB); returns its id."""
        return self._n.add_layer(name, color)

    def add_point(self, point: PointLike, name: str = "", layer: str = "0", attributes: Optional[dict] = None) -> str:
        """A point object; returns its id."""
        return self._n.add_geometry("Point", _points([point]), name, layer, False, attributes or {}, [])

    def add_polyline(self, points, name: str = "", layer: str = "0", closed: bool = False, attributes: Optional[dict] = None,
                     bulges: Optional[Sequence[float]] = None) -> str:
        """A polyline (optionally closed; ``bulges`` give plan arcs in the DXF convention); returns its id."""
        return self._n.add_geometry("Polyline", _points(points), name, layer, closed, attributes or {}, list(bulges or []))

    def add_line(self, a: PointLike, b: PointLike, name: str = "", layer: str = "0") -> str:
        """A line between two points; returns its id."""
        return self._n.add_geometry("Line", _points([a, b]), name, layer, False, {}, [])

    def add_mesh(self, vertices, triangles, name: str = "", layer: str = "0") -> str:
        """A triangle mesh (vertices ``(n, 3)``, triangles ``(m, 3)``); returns its id."""
        return self._n.add_mesh(_points(vertices), np.ascontiguousarray(triangles, dtype=np.uint32), name, layer)

    def geometry(self, id: str) -> dict:
        """An object's data: type, layer, vertices, closure, bulges, attributes, and as applicable
        ``length``, ``arc``, ``text``, ``dimension_value``, ``hatch_area``, ``mesh``, ``area``, ``volume``."""
        return self._n.geometry(_id(id))

    def ids(self, type: Optional[str] = None, layer: Optional[str] = None) -> List[str]:
        """Object ids, optionally of one type ("Polyline", "Mesh", "Solid", …) and/or on one layer."""
        return self._n.geometry_ids(type, layer)

    def remove(self, id: str) -> None:
        """Deletes an object (refused while a heading uses it as its centreline)."""
        self._n.remove_geometry(_id(id))

    # ---- drawing ------------------------------------------------------------------------------------
    def _add(self, kind: str, params: dict, name: str, layer: str, attributes: Optional[dict]) -> str:
        return self._n.add_cad_object(kind, params, name, layer, attributes or {})

    def add_spline(self, fit_points, name: str = "", layer: str = "0", closed: bool = False, attributes: Optional[dict] = None) -> str:
        """Interpolating spline through the fit points."""
        return self._add("spline", {"points": _points(fit_points), "closed": closed}, name, layer, attributes)

    def add_arc(self, centre: PointLike, radius: float, start_deg: float, end_deg: float, name: str = "", layer: str = "0",
                attributes: Optional[dict] = None) -> str:
        """Plan arc, counter-clockwise from ``start_deg`` to ``end_deg`` (degrees from +X)."""
        return self._add("arc", {"centre": list(centre), "radius": radius, "start_deg": start_deg, "end_deg": end_deg}, name, layer, attributes)

    def add_arc_through(self, p1: PointLike, p2: PointLike, p3: PointLike, name: str = "", layer: str = "0",
                        attributes: Optional[dict] = None) -> str:
        """Arc through three points."""
        return self._add("arc3", {"p1": list(p1), "p2": list(p2), "p3": list(p3)}, name, layer, attributes)

    def add_circle(self, centre: PointLike, radius: float, name: str = "", layer: str = "0", attributes: Optional[dict] = None) -> str:
        """A circle in plan; returns its id."""
        return self._add("circle", {"centre": list(centre), "radius": radius}, name, layer, attributes)

    def add_rectangle(self, corner1: PointLike, corner2: PointLike, name: str = "", layer: str = "0", attributes: Optional[dict] = None) -> str:
        """An axis-aligned rectangle (closed polyline) from two corners; returns its id."""
        return self._add("rectangle", {"corner1": list(corner1), "corner2": list(corner2)}, name, layer, attributes)

    def add_polygon(self, centre: PointLike, radius: float, sides: int, rotation_deg: float = 0.0, inscribed: bool = True, name: str = "",
                    layer: str = "0", attributes: Optional[dict] = None) -> str:
        """Regular polygon (inscribed in, or circumscribed about, the circle of ``radius``)."""
        return self._add("polygon", {"centre": list(centre), "radius": radius, "sides": sides, "rotation_deg": rotation_deg, "inscribed": inscribed},
                         name, layer, attributes)

    def add_text(self, insertion: PointLike, text: str, height: float = 1.0, rotation_deg: float = 0.0, align: str = "left", name: str = "",
                 layer: str = "0") -> str:
        """A text annotation (``align`` "left", "centre" or "right"); returns its id."""
        return self._add("text", {"insertion": list(insertion), "text": text, "height": height, "rotation_deg": rotation_deg, "align": align},
                         name, layer, None)

    def add_dimension(self, kind: str, points, text_height: float = 1.0, precision: int = 3, name: str = "", layer: str = "0") -> str:
        """Dimension (``Aligned``, ``Horizontal``, ``Vertical``: p1, p2, dimension-line point;
        ``Angular``: vertex, p1, p2, arc point; ``Radius``/``Diameter``: centre, point on circle;
        ``Elevation``: point). ``geometry(id)["dimension_value"]`` is the measured value."""
        return self._add("dimension", {"kind": kind, "points": _points(points), "text_height": text_height, "precision": precision}, name, layer, None)

    def add_hatch(self, loops, pattern: str = "ANSI31", spacing: float = 1.0, angle_deg: float = 0.0, name: str = "", layer: str = "0") -> str:
        """Hatch bounded by closed loops (the first is the outer boundary, the others holes);
        patterns SOLID, ANSI31, LINES, CROSS, DOTS."""
        return self._add("hatch", {"loops": [_points(l) for l in loops], "pattern": pattern, "spacing": spacing, "angle_deg": angle_deg}, name, layer, None)

    # ---- editing (each call is one undo step; heading centrelines flag their activities) --------------
    def offset(self, id: str, distance: float, side_point: PointLike, round_joins: bool = False) -> str:
        """Parallel copy of a curve at ``distance`` towards ``side_point`` (plan)."""
        return self._n.offset_curve(_id(id), distance, list(side_point), round_joins)

    def transform(self, ids, matrix, copy: bool = False, text: str = "Transform") -> dict:
        """Applies a 4×4 row-major transform to objects (solids exactly, through the solid
        modeller). Returns ``{"changed", "created", "removed", "notes", "flagged_activities"}``."""
        m = np.asarray(matrix, dtype=float).reshape(16)
        return self._n.transform_objects([_id(i) for i in _as_list(ids)], list(m), copy, text)

    def move(self, ids, delta: PointLike, copy: bool = False) -> dict:
        """Moves objects by ``delta`` (with ``copy``, moves copies)."""
        return self.transform(ids, _translation(delta), copy, "Copy" if copy else "Move")

    def copy(self, ids, delta: PointLike) -> dict:
        """Copies objects displaced by ``delta``; the result lists them in ``created``."""
        return self.move(ids, delta, copy=True)

    def rotate(self, ids, angle_deg: float, about: PointLike = (0.0, 0.0, 0.0), axis: PointLike = (0.0, 0.0, 1.0), copy: bool = False) -> dict:
        """Right-handed rotation about ``axis`` through ``about`` (plan rotation by default)."""
        return self.transform(ids, _rotation(axis, math.radians(angle_deg), about), copy, "Rotate")

    def scale(self, ids, factor: float, about: PointLike = (0.0, 0.0, 0.0), copy: bool = False) -> dict:
        """Scales objects by ``factor`` about ``about``."""
        if not factor > 0:
            raise ValueError("The scale factor must be positive")
        return self.transform(ids, _scaling(factor, about), copy, "Scale")

    def mirror(self, ids, p1: PointLike, p2: PointLike, copy: bool = False) -> dict:
        """Mirror across the vertical plane through the plan line p1–p2."""
        d = (_vec3(p2) - _vec3(p1))[:2]
        n = np.array([-d[1], d[0], 0.0])
        if np.linalg.norm(n) < 1e-12:
            raise ValueError("The mirror line has no length")
        return self.transform(ids, _mirror(p1, n / np.linalg.norm(n)), copy, "Mirror")

    def trim(self, id: str, pick: PointLike, cutting_edges: Sequence[str] = ()) -> dict:
        """Removes the part of a curve containing ``pick`` between its intersections with the
        cutting edges (default: every curve on a visible layer)."""
        return self._n.trim_curve(_id(id), [_id(e) for e in cutting_edges], list(pick))

    def extend(self, id: str, pick: PointLike, boundaries: Sequence[str] = ()) -> dict:
        """Extends the end nearest ``pick`` to the nearest boundary."""
        return self._n.extend_curve(_id(id), [_id(e) for e in boundaries], list(pick))

    def split(self, id: str, at: PointLike) -> dict:
        """Splits a curve at the point nearest ``at``; the first piece keeps the object's identity."""
        return self._n.split_curve(_id(id), list(at))

    def join(self, ids: Sequence[str], tolerance: float = 1e-4) -> dict:
        """Joins curves whose ends meet within ``tolerance`` into polylines (heading centrelines are
        kept and noted); raises ``ValueError`` when nothing joins."""
        return self._n.join_curves([_id(i) for i in ids], tolerance)

    def explode(self, ids) -> dict:
        """Explodes polylines, hatches, dimensions and splines into their parts."""
        return self._n.explode_objects([_id(i) for i in _as_list(ids)])

    def fillet(self, first: str, pick_first: PointLike, second: str, pick_second: PointLike, radius: float) -> dict:
        """Tangent arc between two curves picked near the corner (the same polyline twice: the
        vertex nearest ``pick_second``)."""
        return self._n.fillet_curves(_id(first), list(pick_first), _id(second), list(pick_second), radius)

    def chamfer(self, first: str, pick_first: PointLike, second: str, pick_second: PointLike, distance1: float,
                distance2: Optional[float] = None) -> dict:
        """A straight chamfer between two curves picked near their corner, ``distance1`` along the first
        and ``distance2`` (default: the same) along the second."""
        return self._n.chamfer_curves(_id(first), list(pick_first), _id(second), list(pick_second), distance1,
                                      distance1 if distance2 is None else distance2)

    def create_development(self, name: str, width: float = 5.0, height: float = 5.0, points=None, start: Optional[PointLike] = None,
                           length: Optional[float] = None, azimuth_deg: float = 0.0, gradient_percent: float = 0.0,
                           level: str = "", layer: str = "Development Centrelines", **spec) -> Development:
        """Creates a centreline (from ``points``, or a straight line from ``start`` with ``length``,
        ``azimuth_deg`` and ``gradient_percent``) and the development heading on it."""
        if points is None:
            if start is None or length is None:
                raise ValueError("Give either points or start + length (+ azimuth_deg, gradient_percent)")
            g = gradient_percent / 100.0
            horizontal = length / math.sqrt(1 + g * g)
            az = math.radians(azimuth_deg)
            x0, y0, z0 = start
            points = [(x0, y0, z0), (x0 + horizontal * math.sin(az), y0 + horizontal * math.cos(az), z0 + horizontal * g)]
        gid = self.add_polyline(points, name=name, layer=layer)
        spec.update({"heading_id": name, "width": width, "height": height, "level": level})
        return Development(self._p, self._n.create_development(gid, spec))


class Design(_Namespace):
    """Mine design: headings and their layouts, levels, stopes, design rules, revisions and the
    design ↔ schedule link (changes flag linked activities; quantities change only on request)."""

    def developments(self) -> List[Development]:
        """Every development heading."""
        return [Development(self._p, d["uuid"]) for d in self._n.documents("MineDevelopment")]

    def development(self, id: str) -> Development:
        """The heading with this id."""
        return Development(self._p, _id(id))

    def create_heading(self, geometry: str, heading_id: str, width: float = 5.0, height: float = 5.0, **spec) -> Development:
        """Makes an existing centreline a development heading (``level`` may be a MineLevel code,
        name or id — else it is kept as text — plus zone, profile, material, support_class, …)."""
        return Development(self._p, self._n.create_development(_id(geometry), dict(spec, heading_id=heading_id, width=width, height=height)))

    # layouts: previews ({"vertices", "bulges", "plan_length", "length_3d", "notes"}); nothing is added
    def layout(self, kind: str, **parameters) -> List[dict]:
        """Centreline layouts: ``horizontal`` (points, radius, grade_percent), ``decline`` (start,
        azimuth_deg, grade_percent, straight_length, radius, turn, target_elevation), ``spiral``,
        ``ramp``, ``access`` (start, azimuth_deg, target, radius, max_grade_percent),
        ``ore_drive`` (contact, offset, side, grade_percent), ``crosscuts`` (parent, interval or
        chainages, length, side, angle_deg, grade_percent) and ``vertical`` (points, bulges,
        vpis=[(chainage, elevation, curve_length)])."""
        p = dict(parameters)
        for k in ("points", "contact"):
            if k in p:
                p[k] = _points(p[k])
        for k in ("start", "target"):
            if k in p:
                p[k] = list(_vec3(p[k]))
        if "parent" in p:
            p["parent"] = _id(p["parent"])
        return self._n.layout(kind, p)

    def create_headings(self, layouts: Sequence[dict], prefix: str = "", layer: str = "Development Centrelines", **template) -> List[Development]:
        """Adds a centreline and a heading for each layout (one undo step); ids are ``prefix`` +
        a two-digit sequence when several are created."""
        if layer not in {l["name"] for l in self._p.cad.layers()}:
            self._p.cad.add_layer(layer, 0xFFC107)
        return [Development(self._p, i) for i in self._n.create_headings(list(layouts), template, layer, prefix)]

    def create_crosscuts(self, parent, layer: str = "Development Centrelines", **spec) -> List[Development]:
        """Crosscuts off a heading (activity type "Crosscut"; level, zone and profile inherited)."""
        if layer not in {l["name"] for l in self._p.cad.layers()}:
            self._p.cad.add_layer(layer, 0xFFC107)
        return [Development(self._p, i) for i in self._n.create_crosscuts(_id(parent), spec, layer)]

    # gradients and rules
    def gradient_profile(self, geometry) -> List[dict]:
        """Segments of a centreline (or a heading's) with chainages, plan length, rise, grade, radius and "1 in n"."""
        return self._n.gradient_profile(_geometry_id(geometry))

    def check_design_rules(self, geometry, **rules) -> List[dict]:
        """Violations of max_grade_percent, max_grade_on_curves_percent, min_radius and
        max_grade_change_percent along a centreline."""
        return self._n.check_design_rules(_geometry_id(geometry), rules)

    def grade(self, geometry, grade_percent: Optional[float] = None, start_elevation: Optional[float] = None) -> int:
        """Regrades a centreline (constant grade from the start, or between its end elevations);
        returns the number of linked activities flagged for review."""
        return self._n.grade_curve(_geometry_id(geometry), start_elevation, grade_percent)

    # design ↔ schedule
    def impacts(self) -> List[dict]:
        """Headings whose centreline length differs from the length the schedule was approved against."""
        return self._n.design_impacts()

    def apply_design_quantities(self, activity) -> None:
        """Controlled, audited recalculation of a flagged activity's quantity from the design."""
        self._n.apply_design_quantities(_id(activity))

    def quantities(self) -> List[dict]:
        """Design quantities per heading (length, gradient, section area, volume, tonnes, loose volume)
        with the scheduled quantity and whether it matches the design."""
        return self._n.development_quantities()

    # levels and stopes
    def add_level(self, code: str = "", elevation: float = 0.0, name: str = "", zone: str = "") -> str:
        """A mine level (code, nominal floor elevation); returns its id."""
        return self._n.add_level(name, code, elevation, zone)

    def assign_levels(self, tolerance: float = 5.0) -> int:
        """Links headings to the level whose elevation is within ``tolerance`` of their start."""
        return self._n.assign_developments_to_levels(tolerance)

    def create_stope(self, name: str, outline=None, floor: Optional[float] = None, back: Optional[float] = None, geometry: Optional[str] = None,
                     **spec) -> str:
        """A prism stope from a plan outline between floor and back elevations, or a stope from a
        closed mesh/solid ``geometry``; spec: code, level, material, method, zone, grade,
        grade_unit, dilution_percent, recovery_percent, layer."""
        return self._n.create_stope(dict(spec, name=name), _points(outline) if outline is not None else None, floor, back,
                                    _id(geometry) if geometry is not None else None)

    def stope_metrics(self, stope: str) -> dict:
        """Volume, tonnes, diluted and recovered tonnes, contained metal, mined grade and closure."""
        return self._n.stope_metrics(_id(stope))

    def subdivide_stope(self, stope: str, outline, floor: float, back: float, strike_azimuth_deg: float, panel_length: float,
                        lift_height: float = 0.0) -> List[str]:
        """Mining blocks (in sequence order) of panels along strike, optionally in lifts."""
        return self._n.subdivide_stope(_id(stope), _points(outline), floor, back, strike_azimuth_deg, panel_length, lift_height)

    # revisions
    def capture_revision(self, entity, code: str, comment: str = "") -> str:
        """Captures the current state of a heading or stope as revision ``code`` (one undo step)."""
        return self._n.capture_revision(_id(entity), code, comment)

    def approve_revision(self, revision: str) -> None:
        """Approves a revision (irreversible; audited)."""
        self._n.approve_revision(revision)

    def compare_revisions(self, from_revision: str, to_revision: Optional[str] = None) -> dict:
        """Two revisions of an object, or a revision against the current state."""
        return self._n.compare_revisions(from_revision, to_revision)

    def restore_revision(self, revision: str) -> None:
        """Restores a revision as a new edit (history is kept)."""
        self._n.restore_revision(revision)


class Scheduler(_Namespace):
    def create_activity(self, name: str, geometry: Optional[Development] = None, production_rate: Optional[float] = None,
                        duration_days: Optional[float] = None, **fields) -> Activity:
        """A rate-driven activity linked to a development (``geometry`` + ``production_rate`` per day),
        or a fixed-duration activity (``duration_days``)."""
        if geometry is not None:
            if production_rate is None:
                raise ValueError("A linked activity needs production_rate (quantity per day)")
            return Activity(self._p, self._n.create_linked_activity(_id(geometry), float(production_rate), name))
        f = dict(fields, name=name)
        if duration_days is not None:
            f["duration_days"] = duration_days
        if production_rate is not None:
            f["rate_per_day"] = production_rate
        return Activity(self._p, self._n.add_activity(f))

    def activities(self) -> List[dict]:
        """Activities in project order: ``{"id", "name", "wbs", …}`` (all document columns)."""
        out = []
        for d in self._n.documents("ScheduleActivity"):
            row = dict(d["columns"])
            row["id"] = d["uuid"]
            row["wbs"] = row.get("wbs_code", "")
            out.append(row)
        return out

    def add_milestone(self, name: str, **fields) -> Activity:
        """A zero-duration milestone activity."""
        return Activity(self._p, self._n.add_activity(dict(fields, name=name, milestone=True)))

    def link(self, predecessor, successor, type: str = "FinishToStart", lag_hours: float = 0.0) -> str:
        """A dependency (``type`` FinishToStart, StartToStart, FinishToFinish or StartToFinish; ``lag_hours``
        in working hours of the successor's calendar, negative = lead); returns its id."""
        return self._n.add_dependency(_id(predecessor), _id(successor), type, lag_hours)

    def add_resource(self, name: str, capacity: float = 1.0) -> str:
        """A resource (equipment or crew) available ``capacity`` units at once; returns its id."""
        return self._n.add_resource(name, capacity)

    def assign(self, activity, resource: str, units: float = 1.0) -> str:
        """Assigns a resource (by id or name) to an activity with ``units``; returns the assignment id."""
        return self._n.assign_resource(_id(activity), resource, units)

    def calculate(self, scenario: Optional[str] = None) -> ScheduleResult:
        """The CPM schedule of the base plan or a scenario (levelled when the scenario asks for it)."""
        return ScheduleResult(self._n.calculate_schedule(scenario))

    def table(self, scenario: Optional[str] = None, baseline: Optional[str] = None) -> List[dict]:
        """The calculated schedule as rows (dates, durations, floats, critical flag, quantities, progress,
        resources, linked design and, with ``baseline``, baseline dates and variances)."""
        return self._n.schedule_table(scenario, baseline)

    def create_baseline(self, name: str) -> str:
        """Stores the current schedule as an immutable baseline; returns its id."""
        return self._n.create_baseline(name)

    def compare_to_baseline(self, baseline: str) -> List[dict]:
        """Start and finish variances (hours) of every activity against a baseline."""
        return self._n.compare_to_baseline(baseline)

    def add_scenario(self, name: str, overrides: Iterable[Tuple[str, str, str]] = (), levelling: str = "None") -> str:
        """A scenario: field overrides ``(entity, field, value)`` applied to copies, and the
        resource treatment (``levelling`` "None", "Heuristic" or "Optimised")."""
        return self._n.add_scenario(name, [(_id(e) if e else "", f, str(v)) for e, f, v in overrides], levelling)

    # calendars
    def add_calendar(self, name: str, shifts: Sequence[dict] = (), exceptions: Sequence[dict] = (), kind: str = "Shift",
                     hours_per_day: float = 24.0, make_default: bool = False) -> str:
        """A working calendar. ``shifts``: ``{"name", "start": "HH:MM", "hours", "days": ["Mon", …]}``
        (default every day); ``exceptions``: ``{"date": "YYYY-MM-DD", "reason", "periods":
        [("HH:MM", "HH:MM"), …]}`` (no periods = a non-working day). ``hours_per_day`` defines
        a "day" for rates and durations shown in days."""
        return self._n.add_calendar(name, kind, [dict(x) for x in shifts], [dict(x) for x in exceptions], hours_per_day, make_default)

    def set_calendar(self, activity, calendar: Optional[str]) -> None:
        """The activity's calendar (``None``: the project default)."""
        self._n.set_activity_calendar(_id(activity), calendar)

    # production models
    def add_cycle(self, name: str, **fields) -> str:
        """A development cycle (drill–charge–blast–muck–support) from which advance rates derive:
        round_length, advance_efficiency, holes_per_round, drill_penetration_rate, drill_booms,
        drill_setup_minutes, collar_minutes_per_hole, charge_minutes_per_hole,
        blast_and_reentry_minutes, swell_factor, bucket_capacity, bucket_fill_factor,
        muck_cycle_minutes, bolts_per_round, minutes_per_bolt, mesh_minutes, shotcrete_minutes,
        scaling_minutes, survey_markup_minutes, service_minutes, utilisation, shift_hours,
        shifts_per_day, notes."""
        return self._n.add_cycle(name, fields)

    def evaluate_cycle(self, cycle: str, width: float, height: float, support_class: Optional[str] = None) -> dict:
        """Cycle time breakdown and advance rates for a heading of the given size."""
        return self._n.evaluate_cycle(cycle, width, height, support_class)

    def use_cycle(self, activity, cycle: Optional[str]) -> None:
        """Drives the activity's rate from a development cycle (``None`` stops it)."""
        self._n.use_cycle(_id(activity), cycle)

    # targets and quantities
    def add_target(self, name: str, start: str, end: str, quantity: float, unit: str = "m", zone: str = "", level: str = "") -> str:
        """A production target for a period (optionally for one zone or level); returns its id."""
        return self._n.add_target(name, start, end, quantity, unit, zone, level)

    def production(self, period: str = "month", scenario: Optional[str] = None, unit: str = "m", zone: str = "", level: str = "") -> List[dict]:
        """Scheduled, actual and target quantities by day, week, month, quarter or year."""
        return self._n.production_by_period(period, scenario, unit, zone, level)

    def resource_usage(self, scenario: Optional[str] = None) -> List[dict]:
        """Usage profile of every resource, with over-allocated spans."""
        return self._n.resource_profiles(scenario)

    def snapshot(self, at: str, baseline: Optional[str] = None, scenario: Optional[str] = None) -> dict:
        """4D state of every scheduled design object at a date-time."""
        return self._n.plan_snapshot(at, baseline, scenario)

    def export_gantt_pdf(self, file: PathType, scenario: Optional[str] = None, title: str = "") -> int:
        """A Gantt chart PDF (table, bars, progress, baseline, critical path, dependencies); returns the page count."""
        return self._n.export_gantt_pdf(str(file), scenario, title, None)

    def export_xlsx(self, file: PathType, scenario: Optional[str] = None) -> None:
        """The schedule table as an XLSX workbook."""
        self._n.export_schedule_xlsx(str(file), scenario)

    def optimise(self, scenario: Optional[str] = None, store_as: Optional[str] = None, progress: Optional[_core.Progress] = None,
                 **options) -> dict:
        """Resource-constrained optimisation (OR-Tools CP-SAT). With ``store_as`` a feasible
        sequence is stored as a new scenario; the base schedule is never changed."""
        if not hasattr(self._n, "optimise_schedule"):
            raise NotImplementedError("This build has no schedule optimiser (OR-Tools was not available)")
        return self._n.optimise_schedule(options, scenario, store_as, progress)

    def linked_designs(self, activity) -> List[str]:
        """Design objects (developments, stopes, mining blocks) linked to an activity."""
        aid = _id(activity)
        return [d["design"] for d in self._n.documents("ActivityDesignLink") if d["activity"] == aid]


class PointCloud(str):
    """A point-cloud dataset of the project. The object *is* the dataset id (a ``str``), so it can be
    passed wherever an id is expected; its methods run on the stored dataset.

    Processing never modifies a dataset: every operation creates a **derived** dataset (a new
    octree recording its parent and the derivation, e.g. ``"statistical outlier removal k=16
    n=2.0"``) and returns it, so the original scan stays untouched and auditable. Each derivation is
    one undo step.
    """

    def __new__(cls, project: "Project", id: str):
        obj = super().__new__(cls, id)
        obj._project = project
        return obj

    def __repr__(self) -> str:
        return f"PointCloud({self.name!r}, {self.point_count} points, id={str(self)!r})"

    def __reduce__(self):  # pickles as its id (a project handle is not picklable)
        return (str, (str(self),))

    # ---- description -------------------------------------------------------------------------------
    @property
    def id(self) -> str:
        """The dataset id."""
        return str(self)

    @property
    def project(self) -> "Project":
        """The project the dataset belongs to."""
        return self._project

    @property
    def document(self) -> dict:
        """The dataset's entity document."""
        d = self._project.documents.get("PointCloudDataset", str(self))
        if d is None:
            raise KeyError(f"The point cloud {str(self)!r} no longer exists")
        return d

    @property
    def name(self) -> str:
        """Dataset name."""
        return self.document["columns"]["name"]

    @property
    def point_count(self) -> int:
        """Number of points."""
        return int(self.document["columns"]["point_count"])

    @property
    def derivation(self) -> str:
        """How the dataset was derived from its parent ("" for an imported scan)."""
        return self.document["columns"]["derivation"]

    @property
    def parent(self) -> Optional["PointCloud"]:
        """The dataset it was derived from (``None`` for an imported scan)."""
        pid = self.document["columns"]["parent_dataset_uuid"]
        return PointCloud(self._project, pid) if pid else None

    @property
    def transform(self) -> np.ndarray:
        """Registration transform (stored to project coordinates), 4×4 row-major."""
        return np.array([float(v) for v in self.document["columns"]["transform"].split()]).reshape(4, 4)

    @property
    def attributes(self) -> dict:
        """Which per-point attributes the dataset stores (intensity, rgb, normals, …)."""
        import json
        return json.loads(self.document["columns"]["attributes"])

    # ---- data ----------------------------------------------------------------------------------------
    def points(self, box: Optional[Sequence[float]] = None) -> dict:
        """Points in project coordinates as arrays (``xyz`` and the stored attributes), optionally
        inside a box ``(xmin, ymin, zmin, xmax, ymax, zmax)``."""
        return self._project.survey.points(self, box)

    @property
    def xyz(self) -> np.ndarray:
        """All points ``(n, 3)`` in project coordinates."""
        return self.points()["xyz"]

    def statistics(self) -> dict:
        """Count, bounds, centroid and mean/median nearest-neighbour spacing (project coordinates)."""
        return _core.cloud_statistics(self.xyz)

    def export(self, file: PathType, progress: Optional[_core.Progress] = None) -> None:
        """Writes the dataset in project coordinates; the format follows the extension (LAS/LAZ,
        E57, PLY, XYZ/CSV, and the PDAL formats when built with PDAL)."""
        self._project.survey.export(self, file, progress)

    # ---- processing (each returns a new derived dataset) -------------------------------------------
    def _derive(self, operation: str, params: dict, engine: str, name: str, progress) -> "PointCloud":
        r = self._project._native.derive_point_cloud(str(self), operation, params, engine, name, progress)
        return PointCloud(self._project, r["id"])

    def remove_outliers(self, method: str = "statistical", k: int = 16, std_ratio: float = 2.0, radius: Optional[float] = None,
                        min_neighbours: int = 4, engine: str = "openmine", name: str = "",
                        progress: Optional[_core.Progress] = None) -> "PointCloud":
        """Removes outliers into a new dataset.

        ``method="statistical"``: a point is kept when the mean distance to its ``k`` nearest
        neighbours is at most μ + ``std_ratio``·σ of all points' mean distances.
        ``method="radius"``: a point is kept with at least ``min_neighbours`` others within
        ``radius``. ``engine`` is ``"openmine"`` or ``"open3d"`` (when built with Open3D)."""
        if method == "statistical":
            return self._derive("statistical_outliers", {"k": k, "std_ratio": std_ratio}, engine, name, progress)
        if method == "radius":
            if radius is None:
                raise ValueError("Radius outlier removal needs radius=")
            return self._derive("radius_outliers", {"radius": radius, "min_neighbours": min_neighbours}, engine, name, progress)
        raise ValueError("method must be 'statistical' or 'radius'")

    def voxel_downsample(self, voxel: float, mode: str = "nearest", engine: str = "openmine", name: str = "",
                         progress: Optional[_core.Progress] = None) -> "PointCloud":
        """One point per voxel: ``mode="nearest"`` keeps the original point nearest the voxel centre
        (no coordinate is invented); ``"centroid"`` averages the voxel (Open3D supports centroids
        only)."""
        if mode not in ("nearest", "centroid"):
            raise ValueError("mode must be 'nearest' or 'centroid'")
        return self._derive("voxel" if mode == "nearest" else "voxel_centroid", {"voxel": voxel}, engine, name, progress)

    def subsample(self, spacing: Optional[float] = None, count: Optional[int] = None, seed: int = 1, name: str = "",
                  progress: Optional[_core.Progress] = None) -> "PointCloud":
        """Minimum-spacing (``spacing=``) or random (``count=``) subsample."""
        if (spacing is None) == (count is None):
            raise ValueError("Give exactly one of spacing= or count=")
        if spacing is not None:
            return self._derive("minimum_spacing", {"spacing": spacing}, "openmine", name, progress)
        return self._derive("random", {"count": count, "seed": seed}, "openmine", name, progress)

    def estimate_normals(self, k: int = 16, radius: float = 0.0, orientation: str = "up", viewpoint: Optional[PointLike] = None,
                         curvature: bool = True, engine: str = "openmine", name: str = "",
                         progress: Optional[_core.Progress] = None) -> "PointCloud":
        """PCA normals (and curvature) into a new dataset. ``orientation`` is ``"up"``,
        ``"towards_viewpoint"`` or ``"away_from_viewpoint"`` (the viewpoint defaults to the
        centroid)."""
        params: Dict[str, Any] = {"k": k, "radius": radius, "orientation": orientation, "curvature": curvature}
        if viewpoint is not None:
            params["viewpoint"] = [float(v) for v in viewpoint]
        return self._derive("normals", params, engine, name, progress)

    def crop(self, box: Sequence[float], inside: bool = True, name: str = "",
             progress: Optional[_core.Progress] = None) -> "PointCloud":
        """Keeps the points inside (or, with ``inside=False``, outside) a box
        ``(xmin, ymin, zmin, xmax, ymax, zmax)`` in project coordinates."""
        return self._derive("crop_box", {"box": [float(v) for v in box], "inside": inside}, "openmine", name, progress)

    def crop_polygon(self, polygon, inside: bool = True, view_normal: PointLike = (0.0, 0.0, 1.0), name: str = "",
                     progress: Optional[_core.Progress] = None) -> "PointCloud":
        """Keeps the points inside (or outside) a polygon seen along ``view_normal`` (plan by default)."""
        return self._derive("crop_polygon", {"polygon": _points(polygon), "inside": inside, "view_normal": list(_vec3(view_normal))}, "openmine", name,
                            progress)

    def segment(self, method: str = "clusters", tolerance: float = 0.1, angle_deg: float = 5.0, min_size: int = 50, drop_unclustered: bool = False,
                name: str = "", progress: Optional[_core.Progress] = None) -> "PointCloud":
        """Labels points with a ``cluster`` scalar (−1 = none): ``method="clusters"`` (connected
        within ``tolerance``) or ``"regions"`` (region growing on normals within ``angle_deg``)."""
        if method == "clusters":
            return self._derive("clusters", {"tolerance": tolerance, "min_size": min_size, "drop_unclustered": drop_unclustered}, "openmine", name, progress)
        if method == "regions":
            return self._derive("regions", {"angle_deg": angle_deg, "min_size": min_size, "drop_unclustered": drop_unclustered}, "openmine", name, progress)
        raise ValueError("method must be 'clusters' or 'regions'")

    def classify_ground(self, cell: float = 1.0, slope: float = 0.15, window: float = 18.0, threshold: float = 0.5, name: str = "",
                        progress: Optional[_core.Progress] = None) -> "PointCloud":
        """SMRF ground classification (PDAL): class 2 = ground, 1 = other."""
        return self._derive("ground", {"cell": cell, "slope": slope, "window": window, "threshold": threshold}, "openmine", name, progress)

    def distance_to(self, reference, max_distance: float = 0.0, signed: bool = False, name: str = "",
                    progress: Optional[_core.Progress] = None) -> "PointCloud":
        """Adds a ``distance`` scalar: the nearest distance of each point to a reference point cloud
        or mesh/solid (NaN beyond ``max_distance``). The statistics are stored as an analysis;
        ``last_statistics`` holds them."""
        r = self._project._native.derive_point_cloud(str(self), "distance", {"reference": _id(reference), "max_distance": max_distance, "signed": signed},
                                                     "openmine", name, progress)
        out = PointCloud(self._project, r["id"])
        out.last_statistics = r.get("statistics")
        out.analysis = r.get("analysis")
        return out

    def fit_planes(self, threshold: float, max_planes: int = 5, min_inliers: int = 100, store: bool = False, seed: int = 1) -> List[dict]:
        """RANSAC planes with dip / dip direction; with ``store`` they are added as structural
        measurements (set "Scan planes")."""
        return self._project._native.fit_planes(str(self), threshold, max_planes, min_inliers, store, seed)

    def surface(self, method: str = "delaunay", engine: str = "", name: str = "", layer: str = "Surfaces", progress: Optional[_core.Progress] = None,
                **params) -> dict:
        """A mesh object from the points: ``delaunay`` (voxel, max_edge_length, target_triangles)
        or ``poisson`` (voxel, depth, orient_towards_centroid; engine "pdal" or "open3d")."""
        return self._project._native.surface_from_cloud(str(self), method, params, engine, name, layer, progress)

    # ---- registration and as-built -------------------------------------------------------------------
    def register_pairs(self, pairs: Sequence[dict], allow_scale: bool = False, apply: bool = False) -> dict:
        """Point-pair registration. Each pair: ``{"name", "source": (x, y, z) in the dataset's
        stored coordinates, "target": (x, y, z) or "control": control-point name/id, "role":
        "fit" | "check"}``. The registration is stored with its residuals; the dataset's
        transform changes only with ``apply`` (and can be reverted)."""
        prepared = []
        for pr in pairs:
            d = dict(pr)
            d["source"] = list(_vec3(d["source"]))
            if "target" in d:
                d["target"] = list(_vec3(d["target"]))
            prepared.append(d)
        return self._project._native.pair_registration(str(self), prepared, allow_scale, apply)

    def register_icp(self, target, apply: bool = False, progress: Optional[_core.Progress] = None, **options) -> dict:
        """ICP of this dataset onto ``target``; the registration is stored (and applied to this
        dataset's transform only with ``apply=True``)."""
        return self._project.survey.register_icp(self, target, apply=apply, progress=progress, **options)

    def build_as_built(self, development, corridor: float = 6.0, voxel: float = 0.05, depth: int = 9, engine: str = "",
                       name: str = "", layer: str = "As-built", fill_holes_up_to: int = 32,
                       progress: Optional[_core.Progress] = None) -> dict:
        """As-built surface of a heading from this (registered) scan — see :meth:`Survey.build_as_built`."""
        return self._project.survey.build_as_built(self, development, corridor=corridor, voxel=voxel, depth=depth, engine=engine,
                                                   name=name, layer=layer, fill_holes_up_to=fill_holes_up_to, progress=progress)


class Survey(_Namespace):
    def import_pointcloud(self, file: PathType, name: str = "", source_crs: str = "",
                          progress: Optional[_core.Progress] = None) -> PointCloud:
        """Imports a scan (LAS/LAZ, E57, PLY, XYZ/CSV/TXT/ASC, and the PDAL formats when built with
        PDAL) into an octree dataset; ``source_crs`` reprojects it into the project CRS."""
        return PointCloud(self._p, self._n.import_point_cloud(str(file), name, source_crs, progress))

    import_point_cloud = import_pointcloud

    def pointcloud(self, id: str) -> PointCloud:
        """The dataset with this id."""
        if self._p.documents.get("PointCloudDataset", str(id)) is None:
            raise KeyError(f"No point cloud {id!r}")
        return PointCloud(self._p, str(id))

    def pointclouds(self) -> List[PointCloud]:
        """Every point-cloud dataset."""
        return [PointCloud(self._p, d["uuid"]) for d in self._p.documents.list("PointCloudDataset")]

    def add_point_cloud(self, xyz, name: str) -> PointCloud:
        """A dataset from an ``(n, 3)`` array of project coordinates."""
        return PointCloud(self._p, self._n.add_point_cloud(_points(xyz), name))

    def points(self, dataset, box: Optional[Sequence[float]] = None) -> dict:
        """A dataset's points in project coordinates as arrays (``xyz`` and stored attributes),
        optionally inside a box ``(xmin, ymin, zmin, xmax, ymax, zmax)``."""
        return self._n.point_cloud_points(str(dataset), list(box) if box is not None else None)

    def export(self, dataset, file: PathType, progress: Optional[_core.Progress] = None) -> None:
        """Writes a dataset in project coordinates; the format follows the extension."""
        self._n.export_point_cloud(str(dataset), str(file), progress)

    def register_icp(self, moving, target, apply: bool = False, progress: Optional[_core.Progress] = None, **options) -> dict:
        """ICP of ``moving`` onto ``target`` (options: mode, max_correspondence, max_iterations, sample_size,
        trim_fraction, engine); stored as a registration, applied only with ``apply``."""
        return self._n.icp_registration(str(moving), str(target), options, apply, progress)

    def add_control_point(self, name: str, position: PointLike, code: str = "", horizontal_sigma: float = 0.005, vertical_sigma: float = 0.005,
                          role: str = "Control", source: str = "Python") -> str:
        """A survey control point with its stated 1σ accuracy; returns its id."""
        return self._n.add_control_point(name, list(_vec3(position)), code, horizontal_sigma, vertical_sigma, role, source)

    def revert_registration(self, registration: str) -> None:
        """Reverts the most recent applied registration of its dataset (the record is kept)."""
        self._n.revert_registration(registration)

    # surfaces (the source is never changed: each result is a new mesh object recording its method)
    def decimate_surface(self, surface: str, target_triangles: int = 0, max_error: float = 0.0, preserve_boundary: bool = True, name: str = "") -> dict:
        """Quadric decimation to ``target_triangles`` and/or ``max_error`` into a new mesh."""
        return self._n.surface_tool(_id(surface), "decimate", {"target_triangles": target_triangles, "max_error": max_error,
                                                               "preserve_boundary": preserve_boundary}, None, name, None)

    def smooth_surface(self, surface: str, iterations: int = 10, lambda_: float = 0.5, mu: float = -0.53, fix_boundary: bool = True, name: str = "") -> dict:
        """Taubin λ|μ smoothing (volume-preserving) into a new mesh."""
        return self._n.surface_tool(_id(surface), "smooth", {"iterations": iterations, "lambda": lambda_, "mu": mu, "fix_boundary": fix_boundary}, None, name,
                                    None)

    def repair_surface(self, surface: str, merge_tolerance: float = 1e-6, fill_holes_up_to: int = 0, orient_outward: bool = True, name: str = "") -> dict:
        """Merges duplicate vertices, removes degenerate and duplicate triangles, orients consistently and
        fills holes up to ``fill_holes_up_to`` edges, into a new mesh with a report."""
        return self._n.surface_tool(_id(surface), "repair", {"merge_tolerance": merge_tolerance, "fill_holes_up_to": fill_holes_up_to,
                                                             "orient_outward": orient_outward}, None, name, None)

    def compare_surfaces(self, surface: str, reference: str, signed: bool = True, max_distance: float = 0.0, name: str = "") -> dict:
        """Distance of every vertex of ``surface`` to ``reference`` (a new mesh with a "distance"
        scalar, and statistics)."""
        return self._n.surface_tool(_id(surface), "compare", {"signed": signed, "max_distance": max_distance}, _id(reference), name, None)

    def add_as_built(self, dataset, vertices, triangles, name: str, layer: str = "0", development: str = "") -> str:
        """Stores a mesh as the as-built model of a scan (and optionally a heading); returns its id."""
        return self._n.add_as_built(str(dataset), _points(vertices), np.ascontiguousarray(triangles, dtype=np.uint32), name, layer,
                                    _id(development) if development else "")

    def build_as_built(self, dataset, development, corridor: float = 6.0, voxel: float = 0.05, depth: int = 9, engine: str = "",
                       name: str = "", layer: str = "As-built", fill_holes_up_to: int = 32,
                       progress: Optional[_core.Progress] = None) -> dict:
        """As-built surface of a development heading from a registered scan.

        The scan points within ``corridor`` metres of the centreline are voxel-thinned, given
        normals towards the centreline (the surface faces into the excavation) and reconstructed
        by screened Poisson (``engine="pdal"`` or ``"open3d"``; default: the first available).
        Triangles farther from the scan than about twice its spacing are trimmed and the mesh is
        repaired; holes with at most ``fill_holes_up_to`` boundary edges (scan gaps) are filled. The
        result is stored as an as-built model (one undo step): ``{"id", "vertices", "triangles",
        "method", "engine"}``."""
        return self._n.build_as_built(str(dataset), _id(development), corridor, voxel, depth, engine, name, layer, fill_holes_up_to, progress)

    def conformance(self, as_built: str, development, interval: float = 1.0, commit: bool = False, max_gap: float = 0.5) -> dict:
        """Design conformance of a stored as-built model against a heading (see :meth:`compare_design`)."""
        return self._n.conformance(str(as_built), _id(development), interval, commit, max_gap)

    def compare_design(self, design, asbuilt, interval: float = 1.0, commit: bool = True, max_gap: float = 0.5,
                       **as_built_options) -> dict:
        """Design conformance of a heading: overbreak and underbreak volumes, depths and the
        clearance, overall and per station every ``interval`` metres.

        ``asbuilt`` is an as-built model id, or a scan (:class:`PointCloud` or dataset id) from
        which the as-built surface is first built (``as_built_options`` go to
        :meth:`build_as_built`; its id is returned as ``"as_built"``). With ``commit`` the result is
        stored (``"result"``) for reports. Holes in the as-built surface up to ``max_gap`` m are
        bridged when sections are assembled; sections left open that do not enclose the design
        centroid (partial sections at unscanned ends) are not used."""
        target = _id(asbuilt)
        built = None
        if isinstance(asbuilt, PointCloud) or self._p.documents.get("PointCloudDataset", target) is not None:
            built = self.build_as_built(target, design, **as_built_options)
            target = built["id"]
        elif as_built_options:
            raise TypeError("as-built options apply only when comparing against a scan")
        elif self._p.documents.get("AsBuiltModel", target) is None:
            raise KeyError(f"{target!r} is neither an as-built model nor a point cloud")
        r = self._n.conformance(target, _id(design), interval, commit, max_gap)
        r["as_built"] = target
        if built is not None:
            r["as_built_method"] = built["method"]
        return r


class Solids(_Namespace):
    """Exact solid modelling (Open CASCADE B-rep). Every operation is validated and one undo step;
    solids are stored as immutable BREP assets with a display tessellation. Available when
    ``openmine.FEATURES["solids"]`` is true."""

    def _need(self):
        if not hasattr(self._n, "solid_primitive"):
            raise NotImplementedError("This build has no solid modeller (Open CASCADE)")

    # primitives
    def box(self, corner1: PointLike, corner2: PointLike, name: str = "", layer: str = "0") -> str:
        """An axis-aligned box between two corners; returns its id."""
        self._need()
        return self._n.solid_primitive("box", {"corner1": list(corner1), "corner2": list(corner2)}, name, layer)

    def cylinder(self, base: PointLike, axis: PointLike, radius: float, height: float, name: str = "", layer: str = "0") -> str:
        """A cylinder from its base centre along ``axis``; returns its id."""
        self._need()
        return self._n.solid_primitive("cylinder", {"base": list(base), "axis": list(axis), "radius": radius, "height": height}, name, layer)

    def cone(self, base: PointLike, axis: PointLike, radius_base: float, radius_top: float, height: float, name: str = "", layer: str = "0") -> str:
        """A (truncated) cone from its base centre along ``axis``; returns its id."""
        self._need()
        return self._n.solid_primitive("cone", {"base": list(base), "axis": list(axis), "radius_base": radius_base, "radius_top": radius_top,
                                                "height": height}, name, layer)

    def sphere(self, centre: PointLike, radius: float, name: str = "", layer: str = "0") -> str:
        """A sphere; returns its id."""
        self._need()
        return self._n.solid_primitive("sphere", {"centre": list(centre), "radius": radius}, name, layer)

    # from curves
    def extrude(self, curves, distance: float = 5.0, direction: Union[str, PointLike] = "vertical", symmetric: bool = False, holes: bool = True,
                name: str = "", layer: str = "0") -> List[str]:
        """Closed curves to solids: ``direction`` "vertical" (+Z), "normal" (each curve's plane
        normal) or a vector (then ``distance`` is ignored). Curves inside an outer curve in the
        same plane become holes."""
        self._need()
        if isinstance(direction, str):
            return self._n.solid_extrude([_id(c) for c in _as_list(curves)], direction, distance, [0.0, 0.0, distance], symmetric, holes, name, layer)
        return self._n.solid_extrude([_id(c) for c in _as_list(curves)], "vector", distance, list(_vec3(direction)), symmetric, holes, name, layer)

    def revolve(self, profiles, axis_point: PointLike, axis_direction: PointLike, angle_deg: float = 360.0, name: str = "", layer: str = "0") -> List[str]:
        """Revolves closed planar curves about an axis by ``angle_deg`` (360: a solid of revolution)."""
        self._need()
        return self._n.solid_revolve([_id(c) for c in _as_list(profiles)], list(_vec3(axis_point)), list(_vec3(axis_direction)), angle_deg, name, layer)

    def sweep(self, paths, profile: Optional[str] = None, width: float = 5.0, height: float = 5.0, transition: str = "right", name: str = "",
              layer: str = "0") -> List[str]:
        """Sweeps a tunnel profile (``profile`` = TunnelProfile id) or a ``width`` × ``height``
        rectangle (floor-centre reference) along each path; ``transition`` "right" (exact mitred
        corners), "round" or "transformed"."""
        self._need()
        return self._n.solid_sweep([_id(c) for c in _as_list(paths)], profile, width, height, transition, name, layer)

    def loft(self, sections: Sequence[str], solid: bool = True, ruled: bool = False, name: str = "", layer: str = "0") -> str:
        """A solid (or, with ``solid=False``, a surface) through closed sections in order; ``ruled``
        uses straight lines between sections."""
        self._need()
        return self._n.solid_loft([_id(c) for c in sections], solid, ruled, name, layer)

    def from_mesh(self, mesh: str, require_closed: bool = True) -> str:
        """A solid from a closed mesh (sewn, coplanar faces merged)."""
        self._need()
        return self._n.solid_from_mesh(_id(mesh), require_closed)

    def of_development(self, development, layer: str = "0", transition: str = "right") -> str:
        """Exact excavation solid of a heading (its profile swept along its centreline)."""
        self._need()
        return self._n.development_solid(_id(development), layer, transition)

    # operations
    def boolean(self, a: str, b: str, op: str, remove_inputs: bool = False, name: str = "", fuzzy: float = 0.0) -> str:
        """``op`` "union", "difference" (a − b) or "intersection"; solids, closed meshes and closed
        planar curves are accepted."""
        self._need()
        return self._n.solid_boolean(_id(a), _id(b), op, remove_inputs, name, fuzzy)

    def union(self, a: str, b: str, **kw) -> str:
        """Union of two objects as a new solid (see :meth:`boolean`)."""
        return self.boolean(a, b, "union", **kw)

    def subtract(self, a: str, b: str, **kw) -> str:
        """``a`` minus ``b`` as a new solid (see :meth:`boolean`)."""
        return self.boolean(a, b, "difference", **kw)

    def intersect(self, a: str, b: str, **kw) -> str:
        """Intersection of two objects as a new solid (see :meth:`boolean`)."""
        return self.boolean(a, b, "intersection", **kw)

    def fillet_edges(self, solid: str, radius: float) -> dict:
        """Rounds every edge with ``radius`` (in place; one undo step); returns the new analysis."""
        self._need()
        return self._n.solid_fillet(_id(solid), radius)

    def offset(self, solid: str, distance: float) -> dict:
        """Offsets the faces by ``distance`` (outwards > 0, inwards < 0), in place; returns the analysis."""
        self._need()
        return self._n.solid_offset(_id(solid), distance)

    def info(self, solid: str) -> dict:
        """Validity, closure, volume, area, bounds, centre of mass and topology counts."""
        self._need()
        return self._n.solid_info(_id(solid))

    def heal(self, solid: str) -> dict:
        """Shape fixing, orientation and face unification in place: ``{"before", "after", "fixes"}``."""
        self._need()
        return self._n.solid_heal(_id(solid))

    def section(self, ids, point: PointLike, normal: PointLike, layer: str = "0") -> List[str]:
        """Section curves of solids (exact) and meshes by a plane."""
        self._need()
        return self._n.section_objects([_id(i) for i in _as_list(ids)], list(_vec3(point)), list(_vec3(normal)), layer)

    def distance(self, a: str, b: str) -> dict:
        """Minimum distance between two objects and the closest points."""
        self._need()
        return self._n.solid_distance(_id(a), _id(b))

    def classify(self, solid: str, points, tolerance: float = 1e-7) -> List[str]:
        """"inside", "on" or "outside" for each point."""
        self._need()
        return self._n.solid_classify(_id(solid), _points(points), tolerance)

    def import_file(self, file: PathType, layer: str = "0") -> List[str]:
        """STEP (.step/.stp) or IGES (.iges/.igs) solids."""
        self._need()
        return self._n.import_solids(str(file), layer)

    def export_file(self, ids, file: PathType) -> None:
        """Writes solids to STEP (.step/.stp) or IGES (.iges/.igs)."""
        self._need()
        self._n.export_solids([_id(i) for i in _as_list(ids)], str(file))


class Reports(_Namespace):
    """Survey reports (HTML or PDF), stored in the project as Report entities with their file."""

    def conformance(self, result_id: str, format: str = "html", store: bool = True, file: Optional[PathType] = None) -> dict:
        """Report of a stored conformance result (HTML or PDF), stored in the project and/or written to ``file``."""
        return self._n.survey_report("conformance", result_id, format, store, str(file) if file else None, False)

    def registration(self, registration_id: str, format: str = "html", store: bool = True, file: Optional[PathType] = None) -> dict:
        """Report of a stored registration with residuals."""
        return self._n.survey_report("registration", registration_id, format, store, str(file) if file else None, False)

    def point_cloud(self, dataset_id: str, format: str = "html", store: bool = True, file: Optional[PathType] = None,
                    spacing_statistics: bool = False) -> dict:
        """Report of a dataset (provenance, attributes, extents, optional spacing statistics)."""
        return self._n.survey_report("point_cloud", dataset_id, format, store, str(file) if file else None, spacing_statistics)

    def list(self) -> List[dict]:
        """Stored reports: id, name, kind, format and creation time."""
        return self._n.reports()

    def content(self, report_id: str) -> bytes:
        """The bytes of a stored report."""
        return self._n.report_bytes(report_id)


class Geology(_Namespace):
    def import_drillholes(self, collars: Union[str, PathType], surveys: Union[str, PathType] = "",
                          intervals: Optional[Dict[str, Union[str, PathType]]] = None, **options) -> dict:
        """Imports drillholes from CSV text or files; returns the full report (every rejected record)."""
        read = _read_text
        return self._n.import_drillholes(read(collars), read(surveys) if surveys else "",
                                         {k: read(v) for k, v in (intervals or {}).items()}, options)

    def drillholes(self) -> List[str]:
        """Drillhole ids."""
        return self._n.drillhole_ids()

    def trace(self, hole: str, step: float = 1.0, method: str = "minimum_curvature") -> np.ndarray:
        """Desurveyed trace of a hole as ``(n, 3)`` points every ``step`` m (``minimum_curvature`` or ``tangent``)."""
        return self._n.drillhole_trace(hole, step, method)

    def composites(self, mode: str = "length", code_field: str = "", bench_base: float = 0.0, bench_height: float = 5.0, **options) -> List[dict]:
        """Composites of every hole: ``mode`` "length" (options: length, fields, min_coverage, residual,
        min_residual_fraction, break_field, density_field, unsampled_as_zero), "code" (by ``code_field``)
        or "bench" (``bench_base``, ``bench_height``)."""
        return self._n.composites(options, mode, code_field, bench_base, bench_height)

    def create_block_model(self, name: str, origin: PointLike, block_size: PointLike, shape: Sequence[int], rotation_deg: float = 0.0,
                           description: str = "") -> str:
        """A regular block model (optionally rotated about Z); returns its id."""
        return self._n.create_block_model(dict(name=name, origin=list(origin), block_size=list(block_size), shape=list(shape),
                                               rotation_deg=rotation_deg, description=description))

    def block_model(self, id: str) -> dict:
        """Geometry and attribute arrays of a block model."""
        return self._n.block_model(id)

    def set_block_values(self, id: str, values: Dict[str, Any], remove: Sequence[str] = ()) -> None:
        """Sets (or removes) attribute arrays; each array has one value per block (one undo step)."""
        self._n.set_block_values(id, {k: np.ascontiguousarray(v, dtype=float) for k, v in values.items()}, list(remove))

    def centroids(self, id: str) -> np.ndarray:
        """Block centroids ``(n, 3)`` in project coordinates."""
        return self._n.block_centroids(id)

    def domain_proportions(self, block_model: str, mesh: str, field: str, subdivisions: int = 4) -> np.ndarray:
        """Fraction of each block inside a closed mesh (sub-block sampling), stored in ``field``."""
        return self._n.domain_proportions(block_model, mesh, field, subdivisions)

    def estimate(self, block_model: str, composites: List[dict], field: str, progress: Optional[_core.Progress] = None, **options) -> dict:
        """Grade estimation into a block model from composites: ``method`` nearest, idw (``power``),
        simple_kriging (``mean``) or ordinary_kriging (``variogram``), with search ``passes`` and an
        optional ``mask``; returns counts per pass."""
        return self._n.estimate(block_model, composites, field, options, progress)

    def grade_tonnage(self, block_model: str, grade: str, cutoffs: Sequence[float], **options) -> List[dict]:
        """Tonnes and mean grade above each cut-off (``density`` or ``density_field``, ``proportion_field``,
        ``other_fields``)."""
        return self._n.grade_tonnage(block_model, grade, list(cutoffs), options)


class Geotech(_Namespace):
    def record_q(self, development, chainage: float, mapped_by: str = "", **inputs) -> str:
        """Records a Q-system classification (rqd, jn, jr, ja, jw, srf) at a chainage of a heading."""
        return self._n.record_q(_id(development) if development else "", chainage, inputs, mapped_by)


class Formats(_Namespace):
    def import_dxf(self, file: PathType, layer_prefix: str = "", chord_tolerance: float = 0.001) -> dict:
        """Imports a DXF (R12–R2018 ASCII) with layers, blocks and attributes; returns the import report."""
        return self._n.import_dxf(str(file), layer_prefix, chord_tolerance)

    def export_dxf(self, file: PathType, ids: Sequence[str] = ()) -> dict:
        """Exports objects (default: all) to DXF R2004; returns the export report."""
        return self._n.export_dxf(str(file), [_id(i) for i in ids])

    def import_omf(self, file: PathType) -> dict:
        """Imports an OMF v1 file; returns what was imported and notes for what was not."""
        return self._n.import_omf(str(file))

    def export_omf(self, file: PathType, drillholes: bool = True, point_clouds: Sequence[str] = ()) -> List[str]:
        """Exports curves, points, meshes, drillholes, block models and selected point clouds to OMF v1."""
        return self._n.export_omf(str(file), drillholes, list(point_clouds))

    def import_mesh(self, file: PathType, layer: str = "Surfaces") -> dict:
        """OBJ, STL (ASCII/binary) or PLY mesh as a mesh object (``{"id", "triangles", "warnings"}``)."""
        return self._n.import_mesh(str(file), layer)

    def export_mesh(self, id: str, file: PathType) -> List[str]:
        """A mesh or solid surface as OBJ, binary STL or binary PLY (by extension); returns warnings,
        e.g. when binary STL's single precision would lose more than 0.1 mm."""
        return self._n.export_mesh(_id(id), str(file))

    # GIS (GDAL): Shapefile, GeoPackage, GeoJSON and KML vectors; GeoTIFF and ESRI ASCII grid DEMs
    def _need_gis(self):
        if not hasattr(self._n, "import_vector"):
            raise NotImplementedError("This build has no GIS support (GDAL)")

    def import_vector(self, file: PathType, source_layer: str = "", default_z: float = 0.0, layer: str = "") -> dict:
        """Features as points and polylines with their attributes (definitions are added), on
        ``layer`` (default: the file name). Coordinates are not transformed: a CRS different from
        the project's is reported in ``warnings``. KML is longitude/latitude."""
        self._need_gis()
        return self._n.import_vector(str(file), source_layer, default_z, layer)

    def export_vector(self, ids: Sequence[str], file: PathType, closed_as_polygons: bool = True) -> None:
        """Points and curves with attributes, in the project CRS (KML is written in WGS 84)."""
        self._need_gis()
        self._n.export_vector([_id(i) for i in ids], str(file), closed_as_polygons)

    def import_dem(self, file: PathType, step: int = 1, z_scale: float = 1.0, name: str = "", layer: str = "Surfaces") -> dict:
        """An elevation raster as a surface (two triangles per cell; no-data cells leave holes)."""
        self._need_gis()
        return self._n.import_dem_surface(str(file), step, z_scale, name, layer)

    def export_dem(self, surfaces: Sequence[str], file: PathType, cell: float = 1.0) -> dict:
        """The topmost of the surfaces sampled at cell centres (.tif GeoTIFF, .asc ESRI ASCII grid)."""
        self._need_gis()
        return self._n.export_dem([_id(i) for i in _as_list(surfaces)], str(file), cell)


class Crs(_Namespace):
    """Coordinate reference systems (PROJ). Coordinates are never changed silently: transformations
    create copies and store a transformation record."""

    def _need(self):
        if not hasattr(self._n, "declare_crs"):
            raise NotImplementedError("This build has no PROJ coordinate support")

    def declare(self, definition: str, set_as_project_crs: bool = False) -> str:
        """Declares a CRS ("EPSG:2051", a PROJ string or WKT) and returns its id."""
        self._need()
        return self._n.declare_crs(definition, set_as_project_crs)

    def set_project_crs(self, crs: str) -> None:
        """Makes a declared CRS the project CRS (coordinates are not changed)."""
        self._need()
        self._n.set_project_crs(crs)

    def search(self, fragment: str, max_results: int = 50) -> List[dict]:
        """CRSs whose name contains ``fragment`` (PROJ database)."""
        self._need()
        return list(_core.search_crs(fragment, max_results))

    def operations(self, source: str, target: str) -> List[dict]:
        """Candidate operations between two CRS definitions, best first, with their accuracy and grids."""
        self._need()
        return list(_core.candidate_operations(source, target))

    def transform_points(self, points, source_crs: str, target_crs: str, operation: int = 0) -> np.ndarray:
        """Points transformed between two declared CRSs (local grids included) with candidate ``operation``."""
        self._need()
        return self._n.transform_points(_points(points), source_crs, target_crs, operation)

    def transform_objects(self, ids, source_crs: str, target_crs: str, layer: str = "0", operation: int = 0) -> List[str]:
        """Copies of the objects in another CRS (the originals are untouched); a transformation
        record stores the operation used."""
        self._need()
        return self._n.transform_objects([_id(i) for i in _as_list(ids)], source_crs, target_crs, layer, operation)

    def define_local_grid(self, name: str, base_crs: str, pairs: Sequence[dict], method: str = "Helmert2D") -> dict:
        """Fits a local mine grid to a base CRS from control pairs ``{"name", "local": (x, y, z),
        "base": (x, y, z), "use": True}`` (``use=False`` = check pair) and declares it as a CRS."""
        self._need()
        return self._n.define_local_grid(name, base_crs, [dict(pr, local=list(_vec3(pr["local"])), base=list(_vec3(pr["base"]))) for pr in pairs], method)


def _read_text(src) -> str:
    """CSV text, or the content of a file path."""
    if isinstance(src, str) and ("\n" in src or "," in src and not src.lower().endswith((".csv", ".txt"))):
        return src
    with open(src, "r", encoding="utf-8-sig") as f:
        return f.read()


# ---- project ---------------------------------------------------------------------------------------------
class Project:
    """An OpenMine project (a ``.openmine`` file with its asset folder)."""

    def __init__(self, native: _core.NativeProject):
        self._native = native
        self.documents = Documents(self)
        self.cad = Cad(self)
        self.solids = Solids(self)
        self.design = Design(self)
        self.scheduler = Scheduler(self)
        self.survey = Survey(self)
        self.geology = Geology(self)
        self.geotech = Geotech(self)
        self.formats = Formats(self)
        self.crs = Crs(self)
        self.reports = Reports(self)

    # lifecycle
    @classmethod
    def new(cls, name: str, start: str) -> "Project":
        """A new project named ``name`` whose schedule starts on ``start`` ("YYYY-MM-DD")."""
        return cls(_core.NativeProject.create(name, start))

    @classmethod
    def open(cls, path: PathType) -> "Project":
        """Opens a ``.openmine`` file (verified on load; an older schema is migrated in memory, a newer one refused)."""
        native, report = _core.NativeProject.load(str(path))
        p = cls(native)
        p.load_report = report
        return p

    def save(self, path: Optional[PathType] = None) -> None:
        """Saves atomically (written to a temporary file, verified, the previous version kept as ``.bak``);
        without ``path`` to the project's own file."""
        target = path if path is not None else self.file_path
        if not target:
            raise ValueError("The project has no file yet: give a path")
        self._native.save(str(target))

    def close(self) -> None:
        """Releases the project; the object must not be used afterwards."""
        self._native.close()

    @property
    def name(self) -> str:
        """Project name."""
        return self._native.name

    @property
    def id(self) -> str:
        """Project identifier (uuid)."""
        return self._native.id

    @property
    def file_path(self) -> str:
        """The project file, or ``""`` before the first save."""
        return str(self._native.file_path) if str(self._native.file_path) not in ("", ".") else ""

    @property
    def is_modified(self) -> bool:
        """True when there are changes since the last save."""
        return self._native.is_modified

    @property
    def info(self) -> dict:
        """Project settings: name, schedule start, status date, project CRS, default calendar, units."""
        return self._native.info()

    def set_info(self, **fields) -> None:
        """Changes project settings (validated; one undo step)."""
        self._native.set_info(fields)

    def set_status_date(self, date: Optional[str]) -> None:
        """The data date for progress and reforecasting ("YYYY-MM-DD")."""
        self._native.set_info({"status_date": date})

    # undo / audit
    def undo(self) -> None:
        """Undoes the last change (a whole ``edit()`` group counts as one)."""
        self._native.undo()

    def redo(self) -> None:
        """Redoes the last undone change."""
        self._native.redo()

    @property
    def undo_text(self) -> str:
        """Description of the change ``undo()`` would revert ("" when there is none)."""
        return self._native.undo_text

    @contextlib.contextmanager
    def edit(self, description: str) -> Iterator["Project"]:
        """Groups changes into one undo step; an exception rolls all of them back."""
        self._native.begin_macro(description)
        try:
            yield self
        except BaseException:
            self._native.end_macro(False)
            raise
        else:
            self._native.end_macro(True)

    def audit_log(self) -> List[dict]:
        """The append-only audit log: every change with time, user and description."""
        return self._native.audit_log()

    def check(self) -> List[str]:
        """Model integrity problems (dangling references, invalid values); empty when consistent."""
        return self._native.check()

    # coordinates (when PROJ is available)
    def declare_crs(self, definition: str, set_as_project_crs: bool = True) -> str:
        """Declares a CRS (see :meth:`Crs.declare`); by default it also becomes the project CRS."""
        if not hasattr(self._native, "declare_crs"):
            raise NotImplementedError("This build has no PROJ coordinate support")
        return self._native.declare_crs(definition, set_as_project_crs)

    def transform_points(self, points, source_crs: str, target_crs: str) -> np.ndarray:
        """Points transformed between two declared CRSs (see :meth:`Crs.transform_points`)."""
        return self.crs.transform_points(points, source_crs, target_crs)

    def __repr__(self) -> str:
        return f"Project({self.name!r}, id={self.id!r})"


def current_project() -> Optional[Project]:
    """The project open in the OpenMine application (inside the embedded console or a script run
    from the application); ``None`` in a standalone interpreter."""
    getter = getattr(_core, "current_project", None)
    if getter is None:
        return None
    native = getter()
    return Project(native) if native is not None else None
