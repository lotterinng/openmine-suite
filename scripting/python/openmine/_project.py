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


# ---- domain handles -----------------------------------------------------------------------------------
@dataclass(frozen=True)
class Development:
    """A development heading (references its centreline geometry)."""
    project: "Project"
    id: str

    @property
    def document(self) -> dict:
        return self.project.documents.get("MineDevelopment", self.id)

    @property
    def heading_id(self) -> str:
        return self.document["columns"]["heading_id"]

    @property
    def geometry_id(self) -> str:
        return self.document["columns"]["geometry_uuid"]

    @property
    def metrics(self) -> dict:
        return self.project._native.development_metrics(self.id)

    @property
    def length(self) -> float:
        return self.metrics["length"]

    @property
    def info(self) -> dict:
        d = dict(self.document["columns"])
        d.update(self.metrics)
        return d

    def centreline(self) -> np.ndarray:
        return self.project._native.development_centreline(self.id)

    def stations(self, interval: float = 10.0) -> List[dict]:
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
        return self.project.documents.get("ScheduleActivity", self.id)

    @property
    def info(self) -> dict:
        d = dict(self.document["columns"])
        d["linked_designs"] = self.project.scheduler.linked_designs(self.id)
        return d

    def record_actual(self, quantity: float, date: str, note: str = "") -> str:
        return self.project._native.record_actual(self.id, date, float(quantity), note)


class ScheduleResult:
    def __init__(self, raw: dict):
        self.raw = raw
        self.ok: bool = raw["ok"]
        self.errors: List[str] = raw["errors"]
        self.warnings: List[str] = raw["warnings"]
        self.start: Optional[str] = raw.get("start")
        self.finish: Optional[str] = raw.get("finish")

    def activity(self, activity_id) -> dict:
        return self.raw["activities"][_id(activity_id)]

    @property
    def activities(self) -> Dict[str, dict]:
        return self.raw["activities"]

    @property
    def critical_path(self) -> List[str]:
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
        return self._n.documents(type)

    def get(self, type: str, key: str) -> Optional[dict]:
        return self._n.document(type, key)

    def template(self, type: str) -> dict:
        return _core.NativeProject.template(type)

    def put(self, type: str, document: dict, text: str = "") -> str:
        return self._n.put(type, document, text)

    def delete(self, type: str, key: str) -> None:
        self._n.delete(type, key)

    def check(self) -> List[str]:
        return self._n.check()


class Cad(_Namespace):
    def layers(self) -> List[dict]:
        return self._n.layers()

    def add_layer(self, name: str, color: int = 0xD0D0D0) -> str:
        return self._n.add_layer(name, color)

    def add_point(self, point: PointLike, name: str = "", layer: str = "0", attributes: Optional[dict] = None) -> str:
        return self._n.add_geometry("Point", _points([point]), name, layer, False, attributes or {}, [])

    def add_polyline(self, points, name: str = "", layer: str = "0", closed: bool = False, attributes: Optional[dict] = None,
                     bulges: Optional[Sequence[float]] = None) -> str:
        return self._n.add_geometry("Polyline", _points(points), name, layer, closed, attributes or {}, list(bulges or []))

    def add_line(self, a: PointLike, b: PointLike, name: str = "", layer: str = "0") -> str:
        return self._n.add_geometry("Line", _points([a, b]), name, layer, False, {}, [])

    def add_mesh(self, vertices, triangles, name: str = "", layer: str = "0") -> str:
        return self._n.add_mesh(_points(vertices), np.ascontiguousarray(triangles, dtype=np.uint32), name, layer)

    def geometry(self, id: str) -> dict:
        return self._n.geometry(_id(id))

    def ids(self, type: Optional[str] = None, layer: Optional[str] = None) -> List[str]:
        return self._n.geometry_ids(type, layer)

    def remove(self, id: str) -> None:
        self._n.remove_geometry(_id(id))

    def offset(self, id: str, distance: float, side_point: PointLike) -> str:
        return self._n.offset_curve(_id(id), distance, list(side_point))

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
    def developments(self) -> List[Development]:
        return [Development(self._p, d["uuid"]) for d in self._n.documents("MineDevelopment")]

    def development(self, id: str) -> Development:
        return Development(self._p, _id(id))


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
        return Activity(self._p, self._n.add_activity(dict(fields, name=name, milestone=True)))

    def link(self, predecessor, successor, type: str = "FinishToStart", lag_hours: float = 0.0) -> str:
        return self._n.add_dependency(_id(predecessor), _id(successor), type, lag_hours)

    def add_resource(self, name: str, capacity: float = 1.0) -> str:
        return self._n.add_resource(name, capacity)

    def assign(self, activity, resource: str, units: float = 1.0) -> str:
        return self._n.assign_resource(_id(activity), resource, units)

    def calculate(self, scenario: Optional[str] = None) -> ScheduleResult:
        return ScheduleResult(self._n.calculate_schedule(scenario))

    def table(self, scenario: Optional[str] = None, baseline: Optional[str] = None) -> List[dict]:
        return self._n.schedule_table(scenario, baseline)

    def create_baseline(self, name: str) -> str:
        return self._n.create_baseline(name)

    def compare_to_baseline(self, baseline: str) -> List[dict]:
        return self._n.compare_to_baseline(baseline)

    def add_scenario(self, name: str, overrides: Iterable[Tuple[str, str, str]] = ()) -> str:
        return self._n.add_scenario(name, [(_id(e) if e else "", f, str(v)) for e, f, v in overrides])

    def snapshot(self, at: str, baseline: Optional[str] = None, scenario: Optional[str] = None) -> dict:
        """4D state of every scheduled design object at a date-time."""
        return self._n.plan_snapshot(at, baseline, scenario)

    def export_gantt_pdf(self, file: PathType, scenario: Optional[str] = None, title: str = "") -> int:
        return self._n.export_gantt_pdf(str(file), scenario, title, None)

    def export_xlsx(self, file: PathType) -> None:
        self._n.export_schedule_xlsx(str(file))

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
        return str(self)

    @property
    def project(self) -> "Project":
        return self._project

    @property
    def document(self) -> dict:
        d = self._project.documents.get("PointCloudDataset", str(self))
        if d is None:
            raise KeyError(f"The point cloud {str(self)!r} no longer exists")
        return d

    @property
    def name(self) -> str:
        return self.document["columns"]["name"]

    @property
    def point_count(self) -> int:
        return int(self.document["columns"]["point_count"])

    @property
    def derivation(self) -> str:
        """How the dataset was derived from its parent ("" for an imported scan)."""
        return self.document["columns"]["derivation"]

    @property
    def parent(self) -> Optional["PointCloud"]:
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

    # ---- registration and as-built -------------------------------------------------------------------
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
        return [PointCloud(self._p, d["uuid"]) for d in self._p.documents.list("PointCloudDataset")]

    def add_point_cloud(self, xyz, name: str) -> PointCloud:
        return PointCloud(self._p, self._n.add_point_cloud(_points(xyz), name))

    def points(self, dataset, box: Optional[Sequence[float]] = None) -> dict:
        return self._n.point_cloud_points(str(dataset), list(box) if box is not None else None)

    def export(self, dataset, file: PathType, progress: Optional[_core.Progress] = None) -> None:
        self._n.export_point_cloud(str(dataset), str(file), progress)

    def register_icp(self, moving, target, apply: bool = False, progress: Optional[_core.Progress] = None, **options) -> dict:
        return self._n.icp_registration(str(moving), str(target), options, apply, progress)

    def add_as_built(self, dataset, vertices, triangles, name: str, layer: str = "0", development: str = "") -> str:
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


class Reports(_Namespace):
    """Survey reports (HTML or PDF), stored in the project as Report entities with their file."""

    def conformance(self, result_id: str, format: str = "html", store: bool = True, file: Optional[PathType] = None) -> dict:
        return self._n.survey_report("conformance", result_id, format, store, str(file) if file else None, False)

    def registration(self, registration_id: str, format: str = "html", store: bool = True, file: Optional[PathType] = None) -> dict:
        return self._n.survey_report("registration", registration_id, format, store, str(file) if file else None, False)

    def point_cloud(self, dataset_id: str, format: str = "html", store: bool = True, file: Optional[PathType] = None,
                    spacing_statistics: bool = False) -> dict:
        return self._n.survey_report("point_cloud", dataset_id, format, store, str(file) if file else None, spacing_statistics)

    def list(self) -> List[dict]:
        return self._n.reports()

    def content(self, report_id: str) -> bytes:
        return self._n.report_bytes(report_id)


class Geology(_Namespace):
    def import_drillholes(self, collars: Union[str, PathType], surveys: Union[str, PathType] = "",
                          intervals: Optional[Dict[str, Union[str, PathType]]] = None, **options) -> dict:
        """Imports drillholes from CSV text or files; returns the full report (every rejected record)."""
        read = _read_text
        return self._n.import_drillholes(read(collars), read(surveys) if surveys else "",
                                         {k: read(v) for k, v in (intervals or {}).items()}, options)

    def drillholes(self) -> List[str]:
        return self._n.drillhole_ids()

    def trace(self, hole: str, step: float = 1.0, method: str = "minimum_curvature") -> np.ndarray:
        return self._n.drillhole_trace(hole, step, method)

    def composites(self, mode: str = "length", code_field: str = "", bench_base: float = 0.0, bench_height: float = 5.0, **options) -> List[dict]:
        return self._n.composites(options, mode, code_field, bench_base, bench_height)

    def create_block_model(self, name: str, origin: PointLike, block_size: PointLike, shape: Sequence[int], rotation_deg: float = 0.0,
                           description: str = "") -> str:
        return self._n.create_block_model(dict(name=name, origin=list(origin), block_size=list(block_size), shape=list(shape),
                                               rotation_deg=rotation_deg, description=description))

    def block_model(self, id: str) -> dict:
        return self._n.block_model(id)

    def set_block_values(self, id: str, values: Dict[str, Any], remove: Sequence[str] = ()) -> None:
        self._n.set_block_values(id, {k: np.ascontiguousarray(v, dtype=float) for k, v in values.items()}, list(remove))

    def centroids(self, id: str) -> np.ndarray:
        return self._n.block_centroids(id)

    def domain_proportions(self, block_model: str, mesh: str, field: str, subdivisions: int = 4) -> np.ndarray:
        return self._n.domain_proportions(block_model, mesh, field, subdivisions)

    def estimate(self, block_model: str, composites: List[dict], field: str, progress: Optional[_core.Progress] = None, **options) -> dict:
        return self._n.estimate(block_model, composites, field, options, progress)

    def grade_tonnage(self, block_model: str, grade: str, cutoffs: Sequence[float], **options) -> List[dict]:
        return self._n.grade_tonnage(block_model, grade, list(cutoffs), options)


class Geotech(_Namespace):
    def record_q(self, development, chainage: float, mapped_by: str = "", **inputs) -> str:
        return self._n.record_q(_id(development) if development else "", chainage, inputs, mapped_by)


class Formats(_Namespace):
    def import_dxf(self, file: PathType, layer_prefix: str = "", chord_tolerance: float = 0.001) -> dict:
        return self._n.import_dxf(str(file), layer_prefix, chord_tolerance)

    def export_dxf(self, file: PathType, ids: Sequence[str] = ()) -> dict:
        return self._n.export_dxf(str(file), [_id(i) for i in ids])

    def import_omf(self, file: PathType) -> dict:
        return self._n.import_omf(str(file))

    def export_omf(self, file: PathType, drillholes: bool = True, point_clouds: Sequence[str] = ()) -> List[str]:
        return self._n.export_omf(str(file), drillholes, list(point_clouds))


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
        self.design = Design(self)
        self.scheduler = Scheduler(self)
        self.survey = Survey(self)
        self.geology = Geology(self)
        self.geotech = Geotech(self)
        self.formats = Formats(self)
        self.reports = Reports(self)

    # lifecycle
    @classmethod
    def new(cls, name: str, start: str) -> "Project":
        return cls(_core.NativeProject.create(name, start))

    @classmethod
    def open(cls, path: PathType) -> "Project":
        native, report = _core.NativeProject.load(str(path))
        p = cls(native)
        p.load_report = report
        return p

    def save(self, path: Optional[PathType] = None) -> None:
        target = path if path is not None else self.file_path
        if not target:
            raise ValueError("The project has no file yet: give a path")
        self._native.save(str(target))

    def close(self) -> None:
        self._native.close()

    @property
    def name(self) -> str:
        return self._native.name

    @property
    def id(self) -> str:
        return self._native.id

    @property
    def file_path(self) -> str:
        return str(self._native.file_path) if str(self._native.file_path) not in ("", ".") else ""

    @property
    def is_modified(self) -> bool:
        return self._native.is_modified

    @property
    def info(self) -> dict:
        return self._native.info()

    def set_info(self, **fields) -> None:
        self._native.set_info(fields)

    def set_status_date(self, date: Optional[str]) -> None:
        self._native.set_info({"status_date": date})

    # undo / audit
    def undo(self) -> None:
        self._native.undo()

    def redo(self) -> None:
        self._native.redo()

    @property
    def undo_text(self) -> str:
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
        return self._native.audit_log()

    def check(self) -> List[str]:
        return self._native.check()

    # coordinates (when PROJ is available)
    def declare_crs(self, definition: str, set_as_project_crs: bool = True) -> str:
        if not hasattr(self._native, "declare_crs"):
            raise NotImplementedError("This build has no PROJ coordinate support")
        return self._native.declare_crs(definition, set_as_project_crs)

    def transform_points(self, points, source_crs: str, target_crs: str) -> np.ndarray:
        return self._native.transform_points(_points(points), source_crs, target_crs)

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
