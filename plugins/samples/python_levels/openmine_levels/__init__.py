# SPDX-License-Identifier: Apache-2.0
"""Sample Python plugin for OpenMine Suite: development quantities by level.

Registered through the ``openmine.plugins`` entry-point group (see ``pyproject.toml``). The
commands use only the public ``openmine`` API, so every change is validated and undoable; the host
wraps each run in one undo step and rolls it back if the command raises.
"""
from __future__ import annotations

from collections import defaultdict

from openmine import Development


def _level_names(project):
    """MineLevel entities by id: their code (e.g. "675"), else their name."""
    return {d["uuid"]: d["columns"].get("code") or d["columns"].get("name") for d in project.documents.list("MineLevel")}


def _developments(project):
    """(level, development) for every heading: the linked MineLevel, else the level text."""
    levels = _level_names(project)
    for doc in project.documents.list("MineDevelopment"):
        cols = doc["columns"]
        level = levels.get(cols.get("level_uuid")) or cols.get("level") or ""
        yield level, Development(project, doc["uuid"])


def register(registry):
    @registry.command("levels.summary", help="Development count, design metres and volume by level")
    def summary(project, **_):
        levels = defaultdict(lambda: {"headings": 0, "metres": 0.0, "volume_m3": 0.0})
        for level, dev in _developments(project):
            m = dev.metrics
            row = levels[level or "(no level)"]
            row["headings"] += 1
            row["metres"] += m["length"]
            row["volume_m3"] += m["volume"]
        return {k: {"headings": v["headings"], "metres": round(v["metres"], 3), "volume_m3": round(v["volume_m3"], 3)}
                for k, v in sorted(levels.items())}

    @registry.command("levels.layers", help='Creates a layer per development level: {"prefix": "Level "}')
    def layers(project, prefix: str = "Level ", color: int = 0x7FB3D5, **_):
        if not isinstance(prefix, str) or not prefix.strip():
            raise ValueError("prefix must be a non-empty text")
        existing = {layer["name"] for layer in project.cad.layers()}
        created = []
        for level in sorted({level for level, _ in _developments(project) if level}):
            name = f"{prefix}{level}"
            if name not in existing:
                project.cad.add_layer(name, color)
                created.append(name)
                existing.add(name)
        return {"created": created}
