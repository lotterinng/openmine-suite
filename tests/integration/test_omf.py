# SPDX-License-Identifier: Apache-2.0
"""Validates the native OMF v1 reader/writer against the GMG reference library `omf` 1.0.x.

The reference library is independent of OpenMine: files we write must load and validate in it,
and files it writes must load (and import) in OpenMine with identical values.
"""
import json
import math
import os
import subprocess
import warnings

import pytest

warnings.filterwarnings("ignore")
omf = pytest.importorskip("omf")
np = pytest.importorskip("numpy")
TOOL = os.environ.get("OPENMINE_OMF_TOOL", "omf_tool")


def tool(*args):
    r = subprocess.run([TOOL, *map(str, args)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def by_name(elements, name):
    for e in elements:
        if e.name == name:
            return e
    raise KeyError(name)


def test_files_we_write_load_and_validate_in_omf(tmp_path):
    f = tmp_path / "sample.omf"
    ours = tool("write-sample", f)
    proj = omf.OMFReader(str(f)).get_project()
    proj.validate()
    assert proj.author == "omf_tool"
    assert proj.units == "m"
    names = {e.name for e in proj.elements}
    assert {"Drive CL", "Pillar", "Points", "Stope", "Drillhole intervals", "Drillhole collars", "Resource"} <= names

    # Polyline with a bulge: tessellated to a 10 mm chord tolerance, so the chord length is a little
    # shorter than the exact helical length (error ≈ L·s/3r).
    drive = by_name(proj.elements, "Drive CL")
    assert isinstance(drive, omf.LineSetElement)
    v = drive.geometry.vertices.array
    seg = drive.geometry.segments.array
    length = sum(np.linalg.norm(v[b] - v[a]) for a, b in seg)
    assert 0 <= ours["arc_length"] - length < 0.01
    pillar = by_name(proj.elements, "Pillar")
    assert len(pillar.geometry.segments.array) == 4  # closed: 4 vertices, 4 segments
    pts = by_name(proj.elements, "Points")
    assert list(pts.data[0].array.array) == ["STN1", "STN2"]

    # Mesh: closed box volume by the divergence theorem in numpy.
    stope = by_name(proj.elements, "Stope")
    sv = stope.geometry.vertices.array
    tri = stope.geometry.triangles.array
    vol = sum(np.dot(sv[a], np.cross(sv[b], sv[c])) for a, b, c in tri) / 6
    assert vol == pytest.approx(ours["mesh_volume"], rel=1e-12)

    # Drillholes: desurveyed interval end points and values per segment, NaN where missing.
    dh = by_name(proj.elements, "Drillhole intervals")
    assert dh.subtype == "borehole"
    dv = dh.geometry.vertices.array
    dseg = dh.geometry.segments.array
    data = {d.name: d for d in dh.data}
    au = data["au"].array.array
    lith = data["lith"]
    lith_names = list(lith.legends[0].values.array)
    for k, iv in enumerate(ours["intervals"]):
        a, b = dseg[k]
        assert np.allclose(dv[a], iv["from"], atol=1e-9)
        assert np.allclose(dv[b], iv["to"], atol=1e-9)
        if iv["au"] is None:
            assert math.isnan(au[k])
        else:
            assert au[k] == pytest.approx(iv["au"], abs=0)
        idx = lith.array.array[k]
        assert (idx == -1) if iv["lith"] is None else (lith_names[idx] == iv["lith"])

    # Block model: rotated axes and u-fastest cell order — centroids computed the OMF way match ours.
    bm = by_name(proj.elements, "Resource")
    g = bm.geometry
    exp = ours["block_model"]
    assert np.allclose(g.origin, exp["origin"])
    assert np.allclose(g.axis_u, exp["axis_u"]) and np.allclose(g.axis_v, exp["axis_v"])
    nu, nv, nw = len(g.tensor_u), len(g.tensor_v), len(g.tensor_w)
    assert [nu, nv, nw] == exp["n"]
    ijk = {d.name: d for d in bm.data}["ijk"].array.array
    cube = np.asarray(ijk).reshape((nu, nv, nw), order="F")
    for i in range(nu):
        for j in range(nv):
            for k in range(nw):
                c = (np.asarray(g.origin) + (np.cumsum(g.tensor_u)[i] - g.tensor_u[i] / 2) * np.asarray(g.axis_u)
                     + (np.cumsum(g.tensor_v)[j] - g.tensor_v[j] / 2) * np.asarray(g.axis_v)
                     + (np.cumsum(g.tensor_w)[k] - g.tensor_w[k] / 2) * np.asarray(g.axis_w))
                assert cube[i, j, k] == i + 10 * j + 100 * k
                cell = exp["cells"][i + nu * (j + nv * k)]
                assert np.allclose(c, cell["centroid"], atol=1e-9)


def build_reference_project(path):
    proj = omf.Project(name="Ref", description="written by omf", units="m", origin=[100.0, 200.0, 0.0], author="gmg")
    pts = omf.PointSetElement(
        name="samples",
        geometry=omf.PointSetGeometry(vertices=np.array([[0.0, 0, 0], [1.5, 2.5, -3.25], [10, 20, 30]]), origin=[1.0, 1.0, 1.0]),
        data=[
            omf.ScalarData(name="cu", array=[0.25, 1.75, 3.5], location="vertices"),
            omf.MappedData(name="dom", array=[0, 1, -1], location="vertices",
                           legends=[omf.Legend(name="names", values=["OX", "FR"]), omf.Legend(name="colors", values=[[255, 0, 0], [0, 0, 255]])]),
            omf.StringData(name="id", array=["a", "b", "c"], location="vertices"),
            omf.ColorData(name="c", array=[[1, 2, 3], [4, 5, 6], [7, 8, 9]], location="vertices"),
            omf.Vector3Data(name="v", array=[[1.0, 0, 0], [0, 1, 0], [0, 0, 1]], location="vertices"),
        ],
    )
    lines = omf.LineSetElement(
        name="faults",
        geometry=omf.LineSetGeometry(vertices=[[0.0, 0, 0], [1, 0, 0], [2, 1, 0], [5, 5, 5], [6, 5, 5], [6, 6, 5]],
                                     segments=[[0, 1], [1, 2], [3, 4], [4, 5], [5, 3]]),
        data=[omf.ScalarData(name="throw", array=[1.0, 2.0, 3.0, 4.0, 5.0], location="segments")],
    )
    surf = omf.SurfaceElement(
        name="topo",
        geometry=omf.SurfaceGeometry(vertices=[[0.0, 0, 0], [10, 0, 1], [0, 10, 2], [10, 10, 3]], triangles=[[0, 1, 3], [0, 3, 2]]),
        data=[omf.ScalarData(name="elev", array=[0.0, 1, 2, 3], location="vertices")],
    )
    grid = omf.SurfaceElement(
        name="grid",
        geometry=omf.SurfaceGridGeometry(tensor_u=[1.0, 1.0, 2.0], tensor_v=[2.0, 2.0], origin=[0, 0, 50],
                                         offset_w=np.arange(12, dtype=float)),
    )
    vol = omf.VolumeElement(
        name="bm",
        geometry=omf.VolumeGridGeometry(tensor_u=[5.0] * 4, tensor_v=[5.0] * 3, tensor_w=[2.5] * 2, origin=[1000.0, 2000.0, 300.0],
                                        axis_u=[math.cos(math.radians(20)), math.sin(math.radians(20)), 0],
                                        axis_v=[-math.sin(math.radians(20)), math.cos(math.radians(20)), 0], axis_w=[0, 0, 1]),
        data=[omf.ScalarData(name="au", array=np.arange(24, dtype=float) / 10, location="cells"),
              omf.MappedData(name="rock", array=np.array([0, 1] * 12), location="cells", legends=[omf.Legend(name="n", values=["W", "O"])])],
    )
    irregular = omf.VolumeElement(name="irregular", geometry=omf.VolumeGridGeometry(tensor_u=[1.0, 2.0], tensor_v=[1.0], tensor_w=[1.0]))
    proj.elements = [pts, lines, surf, grid, vol, irregular]
    proj.validate()
    omf.OMFWriter(proj, str(path))
    return proj


def test_files_omf_writes_read_identically(tmp_path):
    f = tmp_path / "ref.omf"
    ref = build_reference_project(f)
    doc = tool("read", f)
    assert doc["name"] == "Ref" and doc["author"] == "gmg"
    assert doc["origin"] == [100.0, 200.0, 0.0]
    els = {e["name"]: e for e in doc["elements"]}
    p = els["samples"]
    assert p["kind"] == "pointset" and p["origin"] == [1.0, 1.0, 1.0]
    assert np.array_equal(np.array(p["vertices"]), ref.elements[0].geometry.vertices.array)
    d = {x["name"]: x for x in p["data"]}
    assert d["cu"]["values"] == [0.25, 1.75, 3.5]
    assert d["dom"]["indices"] == [0, 1, -1] and d["dom"]["legends"][0]["strings"] == ["OX", "FR"]
    assert d["dom"]["legends"][1]["colors"] == [[255, 0, 0], [0, 0, 255]]
    assert d["id"]["values"] == ["a", "b", "c"]
    assert d["c"]["values"] == [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
    assert d["v"]["values"][2] == [0, 0, 1]
    assert els["faults"]["segments"] == [[0, 1], [1, 2], [3, 4], [4, 5], [5, 3]]
    assert els["topo"]["triangles"] == [[0, 1, 3], [0, 3, 2]]
    g = els["grid"]
    assert g["kind"] == "surfacegrid" and g["tensor_u"] == [1.0, 1.0, 2.0] and g["offset_w"] == list(range(12))
    v = els["bm"]
    assert v["kind"] == "volume" and v["tensor_w"] == [2.5, 2.5]
    assert [x["values"] for x in v["data"] if x["name"] == "au"][0] == pytest.approx(list(np.arange(24) / 10), abs=0)


def test_files_omf_writes_import_into_a_project(tmp_path):
    f = tmp_path / "ref.omf"
    build_reference_project(f)
    r = tool("import", f)
    # Point set → point cloud in project coordinates (project origin + element origin + vertex).
    (pc,) = r["point_clouds"]
    assert pc["points"] == 3 and pc["fields"] == ["cu"]
    xyz = sorted(tuple(x) for x in pc["xyz"])
    assert xyz[0] == pytest.approx((101.0, 201.0, 1.0))
    assert any(abs(x[2] - (-2.25)) < 1e-9 for x in xyz)
    geo = {g["name"]: g for g in r["geometry"]}
    # Line set: an open chain and a closed triangle loop.
    open_chain = [g for g in r["geometry"] if g["type"] == "Polyline" and not g["closed"]]
    closed = [g for g in r["geometry"] if g["type"] == "Polyline" and g["closed"]]
    assert len(open_chain) == 1 and open_chain[0]["vertices"] == 3
    assert len(closed) == 1 and closed[0]["vertices"] == 3
    assert geo["topo"]["triangles"] == 2 and geo["topo"]["scalar"] == "elev"
    assert geo["grid"]["triangles"] == 2 * 3 * 2
    # Regular rotated volume → block model with identical cell values; irregular volume reported.
    (bm,) = r["block_models"]
    assert bm["n"] == [4, 3, 2] and bm["size"] == [5.0, 5.0, 2.5]
    assert bm["rotation"] == pytest.approx(20.0)
    assert bm["origin"] == pytest.approx([1100.0, 2200.0, 300.0])
    assert bm["values"]["au"] == pytest.approx(list(np.arange(24) / 10), abs=0)
    assert bm["values"]["rock"][:4] == [0, 1, 0, 1]
    assert "0=W, 1=O" in bm["description"]
    notes = " ".join(r["notes"])
    assert "irregular" in notes and "only regular grids" in notes
    assert "line data 'throw' not imported" in notes
    assert "data 'dom'" in notes  # mapped point data cannot go into the point cloud
    # One undo step removes everything.
    assert r["after_undo"] == {"geometry": 0, "block_models": 0, "point_clouds": 0}
