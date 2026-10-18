# SPDX-License-Identifier: Apache-2.0
"""Point-cloud and surface algorithms on arrays (project operations are on ``Project.survey``)."""
from __future__ import annotations

import numpy as np

from . import _core


def fit_transform(source, target, allow_scale: bool = False) -> dict:
    """Least-squares rigid (or similarity) transform from paired points; residuals included."""
    return _core.fit_transform(np.asarray(source, dtype=float), np.asarray(target, dtype=float), allow_scale)


def delaunay(points, max_edge_length: float = 0.0):
    """2.5D Delaunay surface (vertices, triangles) of a point set."""
    return _core.delaunay(np.asarray(points, dtype=float), max_edge_length)


def decimate(vertices, triangles, target_triangles: int = 0, max_error: float = 1e300):
    return _core.decimate(np.asarray(vertices, dtype=float), np.asarray(triangles, dtype=np.uint32), target_triangles, max_error)


def volume_between(earlier, later, cell: float = 0.5) -> dict:
    """Cut and fill between two surfaces given as (vertices, triangles)."""
    return _core.volume_between(np.asarray(earlier[0], dtype=float), np.asarray(earlier[1], dtype=np.uint32),
                                np.asarray(later[0], dtype=float), np.asarray(later[1], dtype=np.uint32), cell)


def statistics(points) -> dict:
    return _core.cloud_statistics(np.asarray(points, dtype=float))


def voxel_downsample(points, voxel: float):
    return _core.voxel_downsample(np.asarray(points, dtype=float), voxel)


def cloud_to_mesh(points, vertices, triangles, signed: bool = True):
    return _core.cloud_to_mesh(np.asarray(points, dtype=float), np.asarray(vertices, dtype=float), np.asarray(triangles, dtype=np.uint32), signed)
