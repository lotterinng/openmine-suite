# SPDX-License-Identifier: Apache-2.0
"""Validates OpenMine's own DXF reader and writer against ezdxf, an independent implementation.

* Files written by OpenMine must load in ezdxf and pass its auditor with no errors, and the
  geometry ezdxf reconstructs (arc end points, B-spline shape, hatch areas, dimension
  measurements, polyface meshes) must equal OpenMine's.
* Files written by ezdxf (including entity types OpenMine never writes: ellipses, polygon meshes,
  MESH, MINSERT, attributes, edge-path hatches, rendered dimensions) must import with matching
  lengths, areas and measurements.

The test needs the `dxf_tool` executable (env OPENMINE_DXF_TOOL) and `pip install ezdxf`.
"""
import json
import math
import os
import subprocess

import pytest

ezdxf = pytest.importorskip("ezdxf")
from ezdxf import path as ezpath  # noqa: E402
from ezdxf.math import Vec3  # noqa: E402
from ezdxf.render import MeshBuilder  # noqa: E402

TOOL = os.environ.get("OPENMINE_DXF_TOOL", "dxf_tool")


def run_tool(*args):
    out = subprocess.run([TOOL, *map(str, args)], check=True, capture_output=True, text=True)
    return out.stdout


def path_length(p, tol=1e-5):
    pts = list(p.flattening(tol))
    return sum((b - a).magnitude for a, b in zip(pts, pts[1:]))


def exact_length(e):
    """Length of a (LW)POLYLINE from its exploded LINE/ARC entities (exact for bulges)."""
    total = 0.0
    for v in e.virtual_entities():
        if v.dxftype() == "LINE":
            total += (Vec3(v.dxf.end) - Vec3(v.dxf.start)).magnitude
        elif v.dxftype() == "ARC":
            span = (v.dxf.end_angle - v.dxf.start_angle) % 360.0
            total += math.radians(span) * v.dxf.radius
        else:
            raise AssertionError(v.dxftype())
    return total


def polygon_area(pts):
    pts = list(pts)
    return 0.5 * abs(sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(pts, pts[1:] + pts[:1])))


@pytest.fixture(scope="module")
def sample(tmp_path_factory):
    d = tmp_path_factory.mktemp("dxf")
    dxf_file, expected = d / "sample.dxf", d / "expected.json"
    run_tool("write-sample", dxf_file, expected)
    return dxf_file, json.loads(expected.read_text())


def by_name(doc):
    """Maps the OpenMine object name (XDATA) to the entity."""
    out = {}
    for e in doc.modelspace():
        if e.has_xdata("OPENMINE"):
            tags = e.get_xdata("OPENMINE")
            if tags and tags[0].value:
                out[tags[0].value] = e
    return out


def test_written_file_passes_ezdxf_audit(sample):
    dxf_file, _ = sample
    doc = ezdxf.readfile(dxf_file)
    auditor = doc.audit()
    assert not auditor.has_errors, [str(e.message) for e in auditor.errors]
    assert doc.dxfversion == "AC1018"
    assert doc.header["$INSUNITS"] == 6
    layer = doc.layers.get("Design_Centrelines")
    assert layer.rgb == (0x12, 0xAB, 0x34)
    assert layer.dxf.linetype == "DASHED"
    assert abs(layer.transparency - 0.4) < 0.01
    survey = doc.layers.get("Survey")
    assert survey.is_off() and survey.is_locked()


def test_written_geometry_matches_in_ezdxf(sample):
    dxf_file, exp = sample
    exp = exp["objects"]
    doc = ezdxf.readfile(dxf_file)
    ents = by_name(doc)
    for name in exp:
        assert name in ents or name == "rl", f"{name} not found"

    # Arcs (including one in a tilted plane, which uses the arbitrary-axis OCS).
    for name in ("arc", "tilted arc"):
        e = ents[name]
        assert e.dxftype() == "ARC"
        assert Vec3(e.start_point).isclose(Vec3(exp[name]["start"]), abs_tol=1e-9)
        assert Vec3(e.end_point).isclose(Vec3(exp[name]["end"]), abs_tol=1e-9)
        assert e.dxf.radius == pytest.approx(exp[name]["radius"])
    assert ents["circle"].dxftype() == "CIRCLE"

    # Polylines: LWPOLYLINE with a bulge, 3D POLYLINE, tessellated 3D bulged polyline.
    lw = ents["planar bulged"]
    assert lw.dxftype() == "LWPOLYLINE"
    assert exact_length(lw) == pytest.approx(exp["planar bulged"]["length"], abs=1e-9)
    p3 = ents["3d closed"]
    assert p3.dxftype() == "POLYLINE" and p3.is_3d_polyline and p3.is_closed
    assert [tuple(v.dxf.location) for v in p3.vertices] == [tuple(v) for v in exp["3d closed"]["vertices"]]
    assert exact_length(ents["3d bulged"]) == pytest.approx(exp["3d bulged"]["length"], abs=2e-3)

    # Splines: the B-spline written from Bézier spans must pass through the fit points and have our length.
    for name in ("spline", "closed spline"):
        sp = ents[name]
        bs = sp.construction_tool()
        pts = list(bs.flattening(1e-6))
        for fp in exp[name]["vertices"]:
            assert min((Vec3(fp) - p).magnitude for p in pts) < 1e-5
        length = sum((b - a).magnitude for a, b in zip(pts, pts[1:]))
        assert length == pytest.approx(exp[name]["length"], rel=1e-5)

    # Text and multi-line text (MTEXT) with non-ASCII characters.
    assert ents["label"].dxftype() == "TEXT"
    assert ents["label"].plain_text() == "Level 1 — Décline"
    assert ents["notes"].dxftype() == "MTEXT"
    assert ents["notes"].plain_text() == "Line one\nLine two"

    # Dimensions: ezdxf's measurement equals ours, and each has an anonymous block.
    for name in ("aligned", "horizontal", "vertical", "angular", "radius", "diameter"):
        dim = ents[name]
        assert dim.dxftype() == "DIMENSION"
        assert doc.blocks.get(dim.dxf.geometry) is not None
        value = dim.get_measurement()  # degrees for angular dimensions
        assert value == pytest.approx(exp[name]["value"], abs=1e-9), name

    # Hatches: boundary areas and patterns.
    for name in ("hatch", "solid", "cross", "dots"):
        h = ents[name]
        assert h.dxftype() == "HATCH"
        areas = sorted((polygon_area(p.vertices) for p in h.paths), reverse=True)
        area = areas[0] - sum(areas[1:])
        assert area == pytest.approx(exp[name]["area"], abs=1e-9)
    assert ents["solid"].has_solid_fill
    assert ents["hatch"].dxf.pattern_name == "ANSI31"
    assert ents["hatch"].pattern.lines[0].angle == pytest.approx(45 + math.degrees(0.2))

    # Polyface mesh.
    mesh = ents["tetra"]
    assert mesh.dxftype() == "POLYLINE" and mesh.is_poly_face_mesh
    verts = [v for v in mesh.vertices if v.is_poly_face_mesh_vertex]
    faces = [v for v in mesh.vertices if v.is_face_record]
    assert len(verts) == exp["tetra"]["vertexCount"]
    assert len(faces) == exp["tetra"]["triangleCount"]
    assert MeshBuilder.from_polyface(mesh).optimize_vertices().diagnose().is_closed_surface
    assert mesh.rgb == (0x33, 0x66, 0x99)


def test_round_trip_through_ezdxf_resave(sample, tmp_path):
    """ezdxf re-saves our file (as R2018, UTF-8); OpenMine must read it back unchanged."""
    dxf_file, exp = sample
    doc = ezdxf.readfile(dxf_file)
    doc.saveas(tmp_path / "resaved.dxf")
    r = json.loads(run_tool("read", tmp_path / "resaved.dxf"))
    assert not r["skipped"], r["warnings"]
    got = {o["name"]: o for o in r["objects"]}
    for name, e in exp["objects"].items():
        g = got[name]
        assert g["type"] == e["type"], name
        for key in ("length", "area", "value", "radius"):
            if key in e:
                tol = 2e-3 if name == "3d bulged" else 1e-6  # arcs of 3D polylines are tessellated (1 mm)
                assert g[key] == pytest.approx(e[key], abs=tol), (name, key)
        if "text" in e:
            assert g["text"] == e["text"], name


@pytest.fixture(scope="module")
def foreign(tmp_path_factory):
    """A drawing produced by ezdxf in millimetres with entity types OpenMine never writes."""
    doc = ezdxf.new("R2018", setup=True, units=4)  # millimetres
    msp = doc.modelspace()
    expected = {}

    def layer(name):
        doc.layers.add(name)
        return {"layer": name}

    msp.add_line((0, 0, 0), (3000, 4000, 0), dxfattribs=layer("L_LINE"))
    expected["L_LINE"] = {"length": 5.0}
    msp.add_circle((1000, 1000, 500), 250, dxfattribs={**layer("L_CIRCLE_OCS"), "extrusion": (0, 1, 1)})
    expected["L_CIRCLE_OCS"] = {"length": 2 * math.pi * 0.25}
    msp.add_arc((0, 0), 1000, 30, 120, dxfattribs=layer("L_ARC"))
    expected["L_ARC"] = {"length": math.radians(90) * 1.0}
    lw = msp.add_lwpolyline([(0, 0, 0, 0, 1.0), (2000, 0), (2000, 1000)], format="xyseb", dxfattribs=layer("L_LW"))
    expected["L_LW"] = {"length": exact_length(lw) / 1000}
    p3 = msp.add_polyline3d([(0, 0, 0), (1000, 0, 500), (1000, 1000, 1000)], dxfattribs=layer("L_P3"))
    expected["L_P3"] = {"length": exact_length(p3) / 1000}
    el = msp.add_ellipse((0, 0), major_axis=(2000, 0), ratio=0.5, dxfattribs=layer("L_ELLIPSE"))
    ell_pts = list(el.construction_tool().flattening(1e-5))  # exact ellipse, not the Bézier path approximation
    expected["L_ELLIPSE"] = {"length": sum((b - a).magnitude for a, b in zip(ell_pts, ell_pts[1:])) / 1000}
    fit = [(0, 0), (1000, 2000), (3000, -1000), (5000, 500)]
    sp = msp.add_spline(dxfattribs=layer("L_SPLINE_CTRL"))
    sp.control_points = [(0, 0, 0), (1000, 2000, 0), (3000, -1000, 0), (5000, 500, 0)]
    sp.knots = [0, 0, 0, 0, 1, 1, 1, 1]
    sp.dxf.degree = 3
    expected["L_SPLINE_CTRL"] = {"length": sum((b - a).magnitude for a, b in zip(*(lambda p: (p, p[1:]))(list(sp.construction_tool().flattening(1e-4))))) / 1000}
    msp.add_spline(fit, dxfattribs=layer("L_SPLINE_FIT"))
    msp.add_text("Décline 3", height=2500, rotation=30, dxfattribs=layer("L_TEXT")).set_placement((100, 200))
    msp.add_mtext("{\\fArial|b1;Stope}\\Pnext", dxfattribs={**layer("L_MTEXT"), "char_height": 500})
    pf = msp.add_polyface(dxfattribs=layer("L_POLYFACE"))
    pf.append_faces([[(0, 0, 0), (1000, 0, 0), (1000, 1000, 0), (0, 1000, 0)], [(0, 0, 0), (1000, 0, 0), (500, 500, 1000)]])
    expected["L_POLYFACE"] = {"triangles": 3}
    pm = msp.add_polymesh(size=(3, 4), dxfattribs=layer("L_POLYMESH"))
    for m in range(3):
        for n in range(4):
            pm.set_mesh_vertex((m, n), (m * 1000, n * 1000, (m + n) * 100))
    expected["L_POLYMESH"] = {"triangles": 2 * 2 * 3}
    mesh = msp.add_mesh(dxfattribs=layer("L_MESH"))
    with mesh.edit_data() as md:
        md.vertices = [(0, 0, 0), (1000, 0, 0), (1000, 1000, 0), (0, 1000, 0)]
        md.faces = [(0, 1, 2, 3)]
    expected["L_MESH"] = {"triangles": 2, "area": 1.0}
    msp.add_3dface([(0, 0, 0), (1000, 0, 0), (1000, 1000, 0)], dxfattribs=layer("L_3DFACE"))
    msp.add_3dface([(0, 0, 0), (1000, 1000, 0), (0, 1000, 0)], dxfattribs={"layer": "L_3DFACE"})
    expected["L_3DFACE"] = {"triangles": 2, "area": 1.0}
    msp.add_solid([(0, 0), (1000, 0), (0, 1000), (1000, 1000)], dxfattribs=layer("L_SOLID"))
    expected["L_SOLID"] = {"area": 1.0}
    # Hatch with an edge path: line, clockwise arc, line.
    h = msp.add_hatch(color=2, dxfattribs=layer("L_HATCH_EDGE"))
    ep = h.paths.add_edge_path()
    ep.add_line((0, 0), (2000, 0))
    ep.add_arc((2000, 1000), radius=1000, start_angle=-90, end_angle=90, ccw=True)
    ep.add_line((2000, 2000), (0, 0))
    expected["L_HATCH_EDGE"] = {"area": 2.0 + math.pi / 2}
    h2 = msp.add_hatch(dxfattribs=layer("L_HATCH_PAT"))
    h2.set_pattern_fill("ANSI31", scale=10)
    h2.paths.add_polyline_path([(0, 0), (4000, 0), (4000, 3000), (0, 3000)], is_closed=True)
    h2.paths.add_polyline_path([(1000, 1000), (2000, 1000), (2000, 2000), (1000, 2000)], is_closed=True)
    expected["L_HATCH_PAT"] = {"area": 11.0}
    # Block with attribute, inserted as a 2 × 3 MINSERT array.
    blk = doc.blocks.new("TAG")
    blk.add_circle((0, 0), 100)
    blk.add_attdef("ID", (0, 150), dxfattribs={"height": 50})
    ins = msp.add_blockref("TAG", (10000, 10000), dxfattribs={**layer("L_INSERT"), "column_count": 2, "row_count": 3, "column_spacing": 500, "row_spacing": 400, "rotation": 90})
    msp.add_blockref("TAG", (0, -5000), dxfattribs={"layer": "L_INSERT"}).add_auto_attribs({"ID": "DDH-001"})
    expected["L_INSERT"] = {"circles": 7}
    # Dimensions rendered by ezdxf.
    dims = {
        "L_DIM_LIN": msp.add_linear_dim(base=(0, 2000), p1=(0, 0), p2=(3000, 0), dxfattribs={"layer": "L_DIM_LIN"}),
        "L_DIM_ALI": msp.add_aligned_dim(p1=(0, 0), p2=(3000, 4000), distance=500, dxfattribs={"layer": "L_DIM_ALI"}),
        "L_DIM_RAD": msp.add_radius_dim(center=(0, 0), radius=750, angle=45, dxfattribs={"layer": "L_DIM_RAD"}),
        "L_DIM_DIA": msp.add_diameter_dim(center=(0, 0), radius=750, angle=45, dxfattribs={"layer": "L_DIM_DIA"}),
        "L_DIM_ANG": msp.add_angular_dim_3p(base=(800, 800), center=(0, 0), p1=(1000, 0), p2=(0, 1000), dxfattribs={"layer": "L_DIM_ANG"}),
        "L_DIM_ORD": msp.add_ordinate_x_dim(feature_location=(1500, 700), offset=(0, 1000), dxfattribs={"layer": "L_DIM_ORD"}),
    }
    for name, d in dims.items():
        doc.layers.add(name)
        d.render()
        m = d.dimension.get_measurement()
        expected[name] = {"value": m if name == "L_DIM_ANG" else (m / 1000 if name != "L_DIM_ORD" else None)}
    return doc, expected


def test_ezdxf_drawing_imports(foreign, tmp_path):
    doc, exp = foreign
    f = tmp_path / "foreign.dxf"
    doc.saveas(f)
    r = json.loads(run_tool("read", f, 1e-7))  # fine chord tolerance: curves converted to polylines are exact to 0.1 µm
    assert r["insUnits"] == 4 and r["unitScale"] == pytest.approx(0.001)
    assert not r["skipped"], (r["skipped"], r["warnings"])
    by_layer = {}
    for o in r["objects"]:
        by_layer.setdefault(o["layer"], []).append(o)

    def one(name):
        objs = by_layer[name]
        assert len(objs) == 1, (name, objs)
        return objs[0]

    for name in ("L_LINE", "L_CIRCLE_OCS", "L_ARC", "L_LW", "L_P3", "L_SPLINE_CTRL"):
        assert one(name)["length"] == pytest.approx(exp[name]["length"], rel=1e-6, abs=1e-6), name
    assert one("L_ELLIPSE")["length"] == pytest.approx(exp["L_ELLIPSE"]["length"], rel=1e-5)
    assert one("L_SPLINE_FIT")["type"] == "Spline"
    t = one("L_TEXT")
    assert t["text"] == "Décline 3" and t["height"] == pytest.approx(2.5)
    assert t["rotation"] == pytest.approx(math.radians(30))
    assert t["position"][:2] == pytest.approx([0.1, 0.2])
    assert one("L_MTEXT")["text"] == "Stope\nnext"
    for name in ("L_POLYFACE", "L_POLYMESH", "L_MESH", "L_3DFACE"):
        o = one(name)
        assert o["type"] == "Mesh" and o["triangleCount"] == exp[name]["triangles"], name
        if "area" in exp[name]:
            assert o["area"] == pytest.approx(exp[name]["area"]), name
    assert one("L_SOLID")["area"] == pytest.approx(1.0)
    assert one("L_HATCH_EDGE")["area"] == pytest.approx(exp["L_HATCH_EDGE"]["area"], rel=1e-5)
    hp = one("L_HATCH_PAT")
    assert hp["pattern"] == "ANSI31" and hp["area"] == pytest.approx(11.0)
    circles = [o for o in by_layer["L_INSERT"] if o["type"] == "Circle"]
    assert len(circles) == exp["L_INSERT"]["circles"]
    assert any(o["type"] == "Text" and o["text"] == "DDH-001" for o in by_layer["L_INSERT"])
    for name in ("L_DIM_LIN", "L_DIM_ALI", "L_DIM_RAD", "L_DIM_DIA", "L_DIM_ANG"):
        d = one(name)
        assert d["type"] == "Dimension", name
        assert d["value"] == pytest.approx(exp[name]["value"], abs=1e-9), name
    # Ordinate dimensions have no OpenMine equivalent: their drawn lines and text are imported.
    assert {o["type"] for o in by_layer["L_DIM_ORD"]} >= {"Line", "Text"}
