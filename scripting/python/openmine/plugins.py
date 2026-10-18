# SPDX-License-Identifier: Apache-2.0
"""Plugins.

* **Native plugins** (C ABI, see ``sdk/include/openmine_plugin.h``) are loaded by :class:`NativePlugins`.
* **Python plugins** are distributions declaring an entry point in the ``openmine.plugins`` group
  that resolves to a callable ``register(registry)``. The callable registers commands on the
  :class:`Registry`::

      def register(registry):
          @registry.command("tonnes.report", help="Tonnage by level")
          def report(project, **arguments):
              return {...}

  Commands receive a :class:`openmine.Project` and keyword arguments and return JSON-compatible
  data. They change the project only through the public API (validated and undoable); each run is
  wrapped in one undo step and rolled back if the command raises.

  The desktop application discovers Python plugins at start-up — installed distributions, and
  packages placed in a ``python`` subfolder of any plugin folder (next to the executable, the
  user's plugin folder, ``OPENMINE_PLUGIN_PATH``) — and offers their commands in the Plugins menu
  and through the ``PLUGIN`` console command, next to native plugin commands.
"""
from __future__ import annotations

import importlib
import importlib.metadata as _md
import json
import sys
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional

from . import _core
from ._project import Project

ENTRY_POINT_GROUP = "openmine.plugins"


@dataclass
class PythonCommand:
    name: str
    function: Callable[..., Any]
    help: str = ""
    plugin: str = ""


@dataclass
class Registry:
    commands: Dict[str, PythonCommand] = field(default_factory=dict)
    errors: List[str] = field(default_factory=list)
    _current: str = ""

    def command(self, name: str, help: str = "") -> Callable:
        def deco(fn):
            self.add_command(name, fn, help)
            return fn
        return deco

    def add_command(self, name: str, fn: Callable, help: str = "") -> None:
        if name in self.commands:
            raise ValueError(f"Command {name!r} is already registered by {self.commands[name].plugin or 'another plugin'}")
        self.commands[name] = PythonCommand(name, fn, help, self._current)

    def run(self, project: Project, name: str, **arguments) -> Any:
        cmd = self.commands.get(name)
        if cmd is None:
            raise KeyError(f"Unknown command {name!r}")
        with project.edit(f"{name} ({cmd.plugin or 'python'})"):
            return cmd.function(project, **arguments)


def discover(registry: Optional[Registry] = None, group: str = ENTRY_POINT_GROUP) -> Registry:
    """Loads every installed Python plugin; failures are recorded in ``registry.errors``."""
    registry = registry or Registry()
    try:
        eps = _md.entry_points(group=group)
    except TypeError:  # Python < 3.10 API
        eps = _md.entry_points().get(group, [])
    for ep in eps:
        registry._current = ep.name
        try:
            ep.load()(registry)
        except Exception as e:  # a broken plugin must not prevent the others from loading
            registry.errors.append(f"{ep.name}: {e}")
    registry._current = ""
    return registry


# ---- desktop host -----------------------------------------------------------------------------------
# The application calls these through the embedded interpreter (JSON strings in and out).
_desktop_registry: Optional[Registry] = None


def _desktop_discover(paths_json: str = "[]") -> str:
    """Adds plugin folders to ``sys.path``, (re)discovers the plugins and describes them."""
    global _desktop_registry
    for p in json.loads(paths_json):
        if p not in sys.path:
            sys.path.append(p)
    importlib.invalidate_caches()
    _desktop_registry = discover()
    return json.dumps({"commands": [{"name": c.name, "help": c.help, "plugin": c.plugin} for c in _desktop_registry.commands.values()],
                       "errors": list(_desktop_registry.errors)})


def _desktop_run(name: str, arguments_json: str = "{}") -> str:
    """Runs a discovered command on the application's project (one undo step, rolled back on error)."""
    from ._project import current_project
    if _desktop_registry is None or name not in _desktop_registry.commands:
        return json.dumps({"ok": False, "error": f"Unknown Python plugin command {name!r}", "rolled_back": False})
    project = current_project()
    if project is None:
        return json.dumps({"ok": False, "error": "No project is open", "rolled_back": False})
    try:
        arguments = json.loads(arguments_json or "{}")
        if not isinstance(arguments, dict):
            raise TypeError("The arguments must be a JSON object")
        result = _desktop_registry.run(project, name, **arguments)
    except Exception as e:  # reported to the user; the edit was rolled back by Project.edit
        return json.dumps({"ok": False, "error": f"{type(e).__name__}: {e}", "rolled_back": True})
    text = result if isinstance(result, str) else json.dumps(result, default=str)
    return json.dumps({"ok": True, "text": text})


class NativePlugins:
    """Native (C ABI) plugins found in plugin folders."""

    def __init__(self, directories: List[str], disabled: List[str] = ()):
        self._m = _core.PluginManager()
        self.loaded = self._m.load_all([str(d) for d in directories], list(disabled))

    def plugins(self) -> List[dict]:
        return self._m.plugins()

    def commands(self) -> List[tuple]:
        return self._m.commands()

    def run(self, project: Project, name: str, **arguments) -> dict:
        return self._m.run_command(project._native, name, arguments)

    def import_file(self, project: Project, file, **options) -> dict:
        return self._m.run_import(project._native, str(file), options)

    def export_file(self, project: Project, file, **options) -> dict:
        return self._m.run_export(project._native, str(file), options)
