# SPDX-License-Identifier: Apache-2.0
"""Shared multi-user repository on PostgreSQL/PostGIS.

Usage::

    from openmine.repository import Repository
    repo = Repository("host=db dbname=mine user=alice")
    project, wc = repo.checkout(project_id)
    ...edit project...
    result = repo.pull(project, wc)            # merge others' revisions (conflicts reported)
    result = repo.publish(project, wc, "alice", "Survey pickup 12 Nov")

The working copy ``wc`` is a small object to keep with the project (``wc.save(path)``)."""
from __future__ import annotations

import json
from typing import List, Optional, Tuple

from . import _core
from ._project import Project


class WorkingCopy:
    def __init__(self, text: str):
        self.text = text

    @property
    def base_revision(self) -> int:
        return json.loads(self.text)["base_revision"]

    @property
    def project_id(self) -> str:
        return json.loads(self.text)["project"]

    def save(self, path) -> None:
        with open(path, "w", encoding="utf-8") as f:
            f.write(self.text)

    @classmethod
    def load(cls, path) -> "WorkingCopy":
        with open(path, encoding="utf-8") as f:
            return cls(f.read())


class Repository:
    def __init__(self, connection: str, initialise: bool = True):
        if not hasattr(_core, "Repository"):
            raise NotImplementedError("This build has no PostgreSQL repository support (libpq was not available)")
        self._r = _core.Repository(connection)
        if initialise:
            self._r.initialise()

    def projects(self) -> List[Tuple[str, str]]:
        return self._r.projects()

    def create_project(self, project: Project, author: str, message: str = "Initial import") -> WorkingCopy:
        return WorkingCopy(self._r.create_project(project._native, author, message))

    def checkout(self, project_id: str, revision: int = 0) -> Tuple[Project, WorkingCopy]:
        native, wc = self._r.checkout(project_id, revision)
        return Project(native), WorkingCopy(wc)

    def delete_project(self, project_id: str) -> None:
        """Removes a project, its history and unreferenced assets from the repository."""
        self._r.delete_project(project_id)

    def head_revision(self, project_id: str) -> int:
        return self._r.head_revision(project_id)

    def revisions(self, project_id: str) -> list:
        return self._r.revisions(project_id)

    def pull(self, project: Project, wc: WorkingCopy, policy: str = "abort") -> dict:
        """policy: "abort" (default), "take_remote" or "keep_local"."""
        result, wc.text = self._r.pull(project._native, wc.text, policy)
        return result

    def publish(self, project: Project, wc: WorkingCopy, author: str, message: str) -> dict:
        result, wc.text = self._r.publish(project._native, wc.text, author, message)
        return result

    def intersecting(self, project_id: str, box) -> list:
        return self._r.intersecting(project_id, list(box))

    def lock(self, project_id: str, type: str, key: str, holder: str, seconds: int = 3600) -> bool:
        return self._r.lock(project_id, type, key, holder, seconds)

    def unlock(self, project_id: str, type: str, key: str, holder: str) -> None:
        self._r.unlock(project_id, type, key, holder)
