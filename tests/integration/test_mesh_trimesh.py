# SPDX-License-Identifier: Apache-2.0
"""Validates the native OBJ/STL/PLY mesh reader and writer against trimesh (independent)."""
import json
import os
import subprocess

import pytest

trimesh = pytest.importorskip("trimesh")
TOOL = os.environ.get("OPENMINE_DXF_TOOL", "dxf_tool")


def tool(*args):
    return json.loads(subprocess.run([TOOL, *map(str, args)], check=True, capture_output=True, text=True).stdout)


@pytest.mark.parametrize("ext", ["obj", "stl", "ply"])
def test_files_we_write_load_in_trimesh(tmp_path, ext):
    f = tmp_path / f"prism.{ext}"
    ours = tool("mesh-sample", f)
    m = trimesh.load(f, process=False, force="mesh")
    assert len(m.faces) == ours["triangles"]
    m.merge_vertices()
    assert m.is_watertight and m.is_winding_consistent
    # Binary STL is single precision: at 2.9e6 m a float32 step is 0.25 m, so only a coarse check.
    rel = 2e-2 if ext == "stl" else 1e-12
    assert m.volume == pytest.approx(ours["volume"], rel=rel)
    assert m.area == pytest.approx(ours["area"], rel=rel)


@pytest.mark.parametrize("ext", ["obj", "stl", "ply"])
def test_files_trimesh_writes_load_in_openmine(tmp_path, ext):
    m = trimesh.creation.icosphere(subdivisions=3, radius=4.0)
    m.apply_translation([1000.0, 2000.0, -300.0])
    f = tmp_path / f"sphere.{ext}"
    m.export(f)
    ours = tool("mesh-read", f)
    assert ours["triangles"] == len(m.faces)
    assert ours["watertight"]
    rel = 1e-5 if ext in ("stl", "ply") else 1e-9  # trimesh writes float32 for STL and PLY
    assert ours["volume"] == pytest.approx(m.volume, rel=rel)
