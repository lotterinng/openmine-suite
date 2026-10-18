# SPDX-License-Identifier: Apache-2.0
"""OpenMine Suite Python API.

Everything a script does goes through the same native core as the desktop application: changes are
validated, undoable (``project.undo()``) and audited. Identifiers are uuid strings, dates are
``"YYYY-MM-DD"`` and date-times ``"YYYY-MM-DD HH:MM"``; point sets are ``numpy`` arrays of shape
``(n, 3)`` and meshes are ``(vertices, triangles)`` pairs.

Example (the workflow of the specification)::

    from openmine import Project
    project = Project.new("Jupiter Mine", start="2026-11-02")
    design = project.cad.create_development(name="LEG2_675_D3_DEC", length=150, width=5.5, height=5.5,
                                            start=(-25000, 2900000, 1200), azimuth_deg=270, gradient_percent=-12.5)
    activity = project.scheduler.create_activity(name="Develop LEG2_675_D3_DEC", geometry=design, production_rate=5.0)
    print(project.scheduler.calculate().activity(activity.id)["finish"])
    project.save("JUP_Mine.openmine")

Modules: :mod:`openmine.geology`, :mod:`openmine.geotech`, :mod:`openmine.survey`,
:mod:`openmine.formats`, :mod:`openmine.plugins`, :mod:`openmine.repository`.
"""
from __future__ import annotations

from . import _core
from ._core import CancelledError, PersistenceError, Progress, ValidationError
from ._project import (Activity, Development, Project, ScheduleResult, current_project)

__all__ = ["Project", "Development", "Activity", "ScheduleResult", "Progress", "ValidationError", "PersistenceError",
           "CancelledError", "FEATURES", "current_project", "__version__"]

__version__ = getattr(_core, "__version__", "0.3.0")
#: Optional native features of this build (``{"optimiser": True, "crs": True, ...}``).
FEATURES = dict(_core.FEATURES)
SCHEMA_VERSION = _core.SCHEMA_VERSION
PLUGIN_API_VERSION = _core.PLUGIN_API_VERSION

from . import formats, geology, geotech, plugins, repository, survey  # noqa: E402,F401
