# SPDX-License-Identifier: Apache-2.0
"""Runs every ``python`` code block of docs/python.md, in order, in one namespace.

Blocks fenced as ``py`` are illustrations that need external files or servers (a laser scan, a
PostgreSQL repository) and are not run; their APIs are covered by test_api.py.
"""
import re
from pathlib import Path

DOC = Path(__file__).resolve().parents[3] / "docs" / "python.md"


def test_python_guide_examples_run(tmp_path, monkeypatch):
    blocks = re.findall(r"^```python\n(.*?)^```", DOC.read_text(encoding="utf-8"), re.S | re.M)
    assert len(blocks) >= 8, "the guide's runnable examples were not found"
    monkeypatch.chdir(tmp_path)  # files the examples write land in the test's directory
    namespace = {}
    for number, block in enumerate(blocks, 1):
        exec(compile(block, f"docs/python.md, python block {number}", "exec"), namespace)
