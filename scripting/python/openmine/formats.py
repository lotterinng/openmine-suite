# SPDX-License-Identifier: Apache-2.0
"""File formats that do not need a project (project import/export is on ``Project.formats``)."""
from __future__ import annotations

from os import PathLike
from typing import Union

import numpy as np

from . import _core


def read_mesh(file: Union[str, PathLike]) -> dict:
    """OBJ/STL/PLY → ``{"vertices", "triangles", "name", "warnings"}``."""
    return _core.read_mesh(str(file))


def write_mesh(file: Union[str, PathLike], vertices, triangles, name: str = "") -> list:
    """Writes OBJ/STL/PLY (by extension); returns precision warnings (STL/PLY store float32)."""
    return _core.write_mesh(str(file), np.asarray(vertices, dtype=float), np.asarray(triangles, dtype=np.uint32), name)
