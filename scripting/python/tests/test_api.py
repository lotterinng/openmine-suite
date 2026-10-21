# SPDX-License-Identifier: Apache-2.0
"""Tests of the public Python API (the facade over the native core)."""
import json
import math
import os

import numpy as np
import pytest

import openmine
from openmine import Project, ValidationError, geology, geotech, survey, formats
from openmine.plugins import NativePlugins, Registry


def spec_project():
    p = Project.new("Jupiter Mine", start="2026-11-02")
    design = p.cad.create_development(name="LEG2_675_D3_DEC", length=150, width=5.5, height=5.5,
                                      start=(-25000.0, 2900000.0, 1200.0), azimuth_deg=270, gradient_percent=-12.5, level="675")
    act = p.scheduler.create_activity(name="Develop LEG2_675_D3_DEC", geometry=design, production_rate=5.0)
    return p, design, act


def test_specification_workflow_and_bit_exact_round_trip(tmp_path):
    p, design, act = spec_project()
    assert design.length == pytest.approx(150.0, abs=1e-9)
    assert design.metrics["gradient_percent"] == pytest.approx(-12.5)
    assert act.info["linked_designs"] == [design.id]
    r = p.scheduler.calculate()
    assert r.ok, r.errors
    t = r.activity(act)
    assert t["start_date"] == "2026-11-02"
    assert t["duration_hours"] > 0
    f = tmp_path / "JUP_Mine.openmine"
    p.save(f)
    assert not p.is_modified
    q = Project.open(f)
    assert q.name == "Jupiter Mine"
    assert np.array_equal(p.cad.geometry(design.geometry_id)["vertices"], q.cad.geometry(design.geometry_id)["vertices"])
    assert q.scheduler.calculate().activity(act)["finish"] == t["finish"]
    assert q.load_report["schema_version"] == openmine.SCHEMA_VERSION


def test_documents_are_validated_undoable_and_lossless():
    p = Project.new("Docs", start="2026-11-02")
    assert "SurveyControlPoint" in p.documents.types()
    doc = p.documents.template("SurveyControlPoint")
    with pytest.raises(ValidationError):
        p.documents.put("SurveyControlPoint", doc)  # empty name is invalid
    doc["columns"].update(name="CP1", position_x=1.5, position_y=2.25, position_z=-3.0)
    key = p.documents.put("SurveyControlPoint", doc)
    back = p.documents.get("SurveyControlPoint", key)
    assert back["columns"]["position_y"] == 2.25
    assert "Create" in p.undo_text or p.undo_text
    p.undo()
    assert p.documents.get("SurveyControlPoint", key) is None
    p.redo()
    assert p.check() == []
    # edit(): one undo step, rolled back on error.
    with p.edit("Two layers"):
        p.cad.add_layer("A")
        p.cad.add_layer("B")
    assert p.undo_text == "Two layers"
    n = len(p.cad.layers())
    with pytest.raises(KeyError):
        with p.edit("Failing"):
            p.cad.add_layer("C")
            p.cad.add_point((0, 0, 0), layer="missing")
    assert len(p.cad.layers()) == n
    assert any(e["action"] == "Create" for e in p.audit_log())


def test_cad_geometry_and_meshes(tmp_path):
    p = Project.new("CAD", start="2026-11-02")
    gid = p.cad.add_polyline([(0, 0, 0), (10, 0, 0), (10, 10, 0)], name="L", attributes={})
    g = p.cad.geometry(gid)
    assert g["length"] == pytest.approx(20)
    assert g["vertices"].shape == (3, 3)
    off = p.cad.offset(gid, 1.0, (5, 5, 0))
    assert p.cad.geometry(off)["length"] == pytest.approx(18, abs=1e-9)
    v = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=float)
    t = np.array([[0, 2, 1], [0, 1, 3], [0, 3, 2], [1, 2, 3]])
    mid = p.cad.add_mesh(v, t, name="tet")
    assert p.cad.geometry(mid)["volume"] == pytest.approx(1 / 6)
    for ext in ("obj", "ply"):
        f = tmp_path / f"tet.{ext}"
        formats.write_mesh(f, v, t, "tet")
        m = formats.read_mesh(f)
        assert m["triangles"].shape == (4, 3)
    f = tmp_path / "cad.dxf"
    rep = p.formats.export_dxf(f)
    assert sum(rep["written"].values()) >= 2
    q = Project.new("Import", start="2026-11-02")
    assert q.formats.import_dxf(f)["objects"] >= 2


def test_scheduling_baselines_scenarios_and_exports(tmp_path):
    p, design, act = spec_project()
    nxt = p.scheduler.create_activity("Install services", duration_days=6)
    p.scheduler.link(act, nxt)
    ms = p.scheduler.add_milestone("Level complete")
    p.scheduler.link(nxt, ms)
    r = p.scheduler.calculate()
    assert r.ok
    assert r.activity(ms)["start"] == r.activity(nxt)["finish"]
    assert act.id in r.critical_path
    b = p.scheduler.create_baseline("B0")
    sid = p.scheduler.add_scenario("Faster", [(act.id, "rate", "7.5")])
    faster = p.scheduler.calculate(scenario=sid)
    assert faster.finish < r.finish
    var = p.scheduler.compare_to_baseline(b)
    assert all(abs(v["finish_variance_hours"]) < 1e-9 for v in var)
    rows = p.scheduler.table(baseline=b)
    assert [x["name"] for x in rows][0] == "Develop LEG2_675_D3_DEC"
    assert p.scheduler.export_gantt_pdf(tmp_path / "g.pdf") >= 1
    p.scheduler.export_xlsx(tmp_path / "s.xlsx")
    assert (tmp_path / "s.xlsx").stat().st_size > 1000
    snap = p.scheduler.snapshot("2026-11-14 00:00", baseline=b)
    dev = [o for o in snap["objects"] if o["id"] == design.id][0]
    assert 0 < dev["planned_fraction"] < 1
    if openmine.FEATURES["optimiser"]:
        res = p.scheduler.optimise(store_as="Optimised", time_limit_seconds=5)
        assert res["status"] in ("optimal", "feasible")
        assert "scenario" in res


def test_geology_drillholes_composites_and_estimation():
    p = Project.new("Geo", start="2026-11-02")
    collars = "HOLEID,X,Y,Z,DEPTH\n" + "\n".join(f"H{i},{10 * i},0,100,20" for i in range(5))
    assays = "HOLEID,FROM,TO,AU\n" + "\n".join(f"H{i},{k},{k + 1},{i + 0.1 * k}" for i in range(5) for k in range(20))
    rep = p.geology.import_drillholes(collars, intervals={"assay": assays})
    assert rep["created"] == 5 and rep["rejected_rows"] == 0
    assert np.allclose(p.geology.trace("H2", 5.0)[-1], (20, 0, 80))
    comps = p.geology.composites(length=2.0)
    assert len(comps) == 50
    bm = p.geology.create_block_model("Res", origin=(-5, -5, 79), block_size=(10, 10, 2), shape=(5, 1, 11))
    res = p.geology.estimate(bm, comps, "AU", method="idw", passes=[{"range": 30, "max_samples": 8}], output="au_idw")
    assert res["estimated"] == 55
    vals = p.geology.block_model(bm)["values"]["au_idw"]
    assert vals[0] < vals[4]  # follows the x gradient
    gt = p.geology.grade_tonnage(bm, "au_idw", [0, 2], density=2.8)
    assert gt[0]["tonnes"] == pytest.approx(55 * 200 * 2.8)
    st = geology.statistics([c["values"]["AU"] for c in comps])
    assert st["count"] == 50
    pts, v = geology.composites_to_arrays(comps, "AU")
    lags = geology.experimental_variogram(pts, v, lag=10, lags=3)
    assert lags[0][2] > 0


def test_geotechnical_functions():
    q = geotech.q_system(rqd=90, jn=9, jr=1.5, ja=1)
    assert q["q"] == pytest.approx(15) and q["class"] == "Good"
    s = geotech.q_support(span=6, esr=1.6, rqd=90, jn=9, jr=1.5, ja=1)
    assert s["roof_bolt_length"] == pytest.approx(2 + 0.15 * 6 / 1.6)
    r = geotech.rmr89(ucs_mpa=120, rqd=80, spacing_m=0.5, condition_rating=25, groundwater="damp")
    assert r["rmr"] == 12 + 17 + 10 + 25 + 10
    hb = geotech.hoek_brown(50, 10, 100)
    assert hb["mb"] == pytest.approx(1.677, abs=1e-3)
    planes = [(60 + (i % 5), 45 + (i % 7)) for i in range(40)] + [(80, 150 + (i % 5)) for i in range(40)]
    js = geotech.joint_sets(planes, 2)
    assert sorted(x["count"] for x in js["sets"]) == [40, 40]
    svg = geotech.stereonet_svg(planes, sets=2, friction=30, title="Test")
    assert svg.startswith("<svg") and "J1" in svg
    k = geotech.kinematics([(50, 180)], face=(70, 180), friction=30)
    assert k["planar"] == [0]


def test_survey_algorithms_and_point_clouds():
    rng = np.random.default_rng(1)
    xy = rng.uniform(0, 50, size=(400, 2))
    pts = np.column_stack([xy, 0.1 * xy[:, 0]])
    v, t = survey.delaunay(pts)
    assert t.shape[1] == 3 and len(v) == 400
    vol = survey.volume_between((v, t), (v + [0, 0, 1.0], t), cell=1.0)
    assert vol["fill"] == pytest.approx(vol["plan_area"], rel=1e-6)
    src = rng.uniform(0, 10, size=(10, 3))
    fit = survey.fit_transform(src, src + [5, -2, 1])
    assert fit["rms"] < 1e-9
    p = Project.new("Survey", start="2026-11-02")
    ds = p.survey.add_point_cloud(pts, "synthetic")
    back = p.survey.points(ds)["xyz"]
    assert back.shape == pts.shape
    assert np.allclose(np.sort(back[:, 0]), np.sort(pts[:, 0]))
    rep = p.reports.point_cloud(ds, format="pdf", spacing_statistics=True)
    assert rep["title"] == "Point cloud — synthetic"
    stored = p.reports.list()
    assert stored[0]["format"] == "pdf"
    assert p.reports.content(stored[0]["id"]).startswith(b"%PDF-1.7")


def test_icp_engines_agree():
    from openmine import FEATURES
    engines = survey.icp_engines()
    assert engines[0] == "OpenMine"
    assert ("Open3D" in engines) == FEATURES["open3d"]
    rng = np.random.default_rng(3)
    xy = rng.uniform(0, 30, size=(12000, 2))
    z = 2 * np.sin(xy[:, 0] / 3) * np.cos(xy[:, 1] / 4) + 0.3 * np.sin(xy[:, 1])
    floor = np.column_stack([xy, z])
    wall = np.column_stack([rng.uniform(0, 30, 4000), np.zeros(4000), rng.uniform(0, 6, 4000)])
    target = np.vstack([floor, wall]) + [51234.5, 7012345.0, 1180.0]  # mine coordinates
    shift = np.array([0.2, -0.1, 0.05])
    p = Project.new("ICP", start="2026-11-02")
    t = p.survey.add_point_cloud(target, "target")
    m = p.survey.add_point_cloud(target + shift, "moving")
    for engine in engines:
        r = p.survey.register_icp(m, t, max_correspondence=1.0, engine=engine)
        assert r["converged"], engine
        tr = np.array([float(v) for v in r["transform"].split()]).reshape(4, 4)
        assert np.allclose(tr[:3, 3], -shift, atol=2e-3), (engine, tr[:3, 3])
        assert engine in r["notes"]
    with pytest.raises(Exception):
        p.survey.register_icp(m, t, engine="NoSuchEngine")


def test_omf_round_trip(tmp_path):
    p, design, act = spec_project()
    f = tmp_path / "x.omf"
    notes = p.formats.export_omf(f)
    assert isinstance(notes, list)
    q = Project.new("OMF", start="2026-11-02")
    r = q.formats.import_omf(f)
    assert len(r["geometry"]) >= 1


def test_native_and_python_plugins(tmp_path):
    p = Project.new("Plugins", start="2026-11-02")
    d = os.environ.get("OPENMINE_TEST_PLUGIN_DIR")
    if d:
        nat = NativePlugins([d])
        assert any(c[0] == "stations.summary" for c in nat.commands())
        stn = tmp_path / "a.stn"
        stn.write_text("S1,1,2,3\nS2,4,5,6\n")
        r = nat.import_file(p, stn)
        assert r["ok"] and r["result"]["added"] == 2
        assert nat.run(p, "stations.summary")["result"]["count"] == 2
    reg = Registry()

    @reg.command("count.layers", help="Number of layers")
    def count(project, extra=0):
        project.cad.add_layer("From plugin")
        return len(project.cad.layers()) + extra

    assert reg.run(p, "count.layers", extra=1) == 4
    assert p.undo_text.startswith("count.layers")

    @reg.command("bad")
    def bad(project):
        project.cad.add_layer("Never kept")
        raise RuntimeError("fail")

    with pytest.raises(RuntimeError):
        reg.run(p, "bad")
    assert all(l["name"] != "Never kept" for l in p.cad.layers())
    with pytest.raises(ValueError):
        reg.add_command("bad", bad)


@pytest.mark.skipif(not os.environ.get("OPENMINE_TEST_PLUGIN_DIR"), reason="plugin build folder not given")
def test_sample_python_plugin_discovery_and_desktop_host():
    import json
    import sys
    from openmine import plugins
    folder = os.path.join(os.environ["OPENMINE_TEST_PLUGIN_DIR"], "python")
    described = json.loads(plugins._desktop_discover(json.dumps([folder])))
    assert folder in sys.path
    names = {c["name"]: c for c in described["commands"]}
    assert {"levels.summary", "levels.layers"} <= set(names), described
    assert names["levels.summary"]["plugin"] == "levels" and described["errors"] == []
    reg = plugins.discover()
    p = Project.new("Levels", start="2026-11-02")
    for name, level, length in (("A", "675", 30.0), ("B", "675", 20.0), ("C", "700", 12.5)):
        p.cad.create_development(name=name, length=length, width=5.0, height=5.0, start=(0.0, 0.0, 0.0), azimuth_deg=90,
                                 gradient_percent=0.0, level=level)
    s = reg.run(p, "levels.summary")
    assert s["675"]["headings"] == 2 and s["675"]["metres"] == pytest.approx(50.0)
    assert s["700"]["volume_m3"] == pytest.approx(12.5 * 25.0, rel=1e-6)
    before = len(p.cad.layers())
    assert reg.run(p, "levels.layers", prefix="L") == {"created": ["L675", "L700"]}
    assert len(p.cad.layers()) == before + 2
    assert reg.run(p, "levels.layers", prefix="L") == {"created": []}  # idempotent
    p.undo()
    p.undo()  # the first run was one undo step for both layers
    assert len(p.cad.layers()) == before
    with pytest.raises(ValueError):
        reg.run(p, "levels.layers", prefix=" ")
    assert len(p.cad.layers()) == before
    # The desktop entry point reports errors as JSON instead of raising.
    out = json.loads(plugins._desktop_run("levels.summary", "{}"))
    assert out == {"ok": False, "error": "No project is open", "rolled_back": False}  # standalone interpreter
    assert not json.loads(plugins._desktop_run("no.such", "{}"))["ok"]


@pytest.mark.skipif(not os.environ.get("OPENMINE_PG_DSN") or not openmine.FEATURES["repository"], reason="no PostgreSQL test database")
def test_repository_round_trip():
    from openmine.repository import Repository
    repo = Repository(os.environ["OPENMINE_PG_DSN"])
    p, design, act = spec_project()
    wc = repo.create_project(p, "alice")
    try:
        q, wq = repo.checkout(p.id)
        q.cad.add_layer("Bob's")
        res = repo.publish(q, wq, "bob", "layer")
        assert res["published"], res
        r = repo.pull(p, wc)
        assert r["applied"] >= 1
        assert any(l["name"] == "Bob's" for l in p.cad.layers())
    finally:
        repo.delete_project(p.id)


def _scanned_heading(p, bulge=0.3, n=60000, seed=5):
    """A straight 30 m, 5 × 5 m heading and a synthetic scan of its excavated profile: the walls,
    back and floor (the face and portal are not scanned) with the north wall ``bulge`` m out
    over its full height (overbreak), plus scanner noise."""
    design = p.cad.create_development(name="SCAN_XC", length=30, width=5.0, height=5.0, start=(1000.0, 5000.0, 200.0),
                                      azimuth_deg=90, gradient_percent=0.0, level="200")
    v, _ = p._native.development_envelope(design.id)
    y0, y1 = v[:, 1].min(), v[:, 1].max() + bulge  # the excavated profile: south wall to the over-broken north wall
    z0, z1 = v[:, 2].min(), v[:, 2].max()
    w, h = y1 - y0, z1 - z0
    rng = np.random.default_rng(seed)
    u = rng.uniform(0, 2 * (w + h), n)  # arc length around the profile (floor, north wall, back, south wall)
    y = np.where(u < w, y0 + u, np.where(u < w + h, y1, np.where(u < 2 * w + h, y1 - (u - w - h), y0)))
    z = np.where(u < w, z0, np.where(u < w + h, z0 + (u - w), np.where(u < 2 * w + h, z1, z1 - (u - 2 * w - h))))
    x = rng.uniform(v[:, 0].min(), v[:, 0].max(), n)
    pts = np.column_stack([x, y, z]) + rng.normal(0, 0.005, (n, 3))
    return design, pts


def test_point_cloud_handles_and_derived_processing(tmp_path):
    from openmine import FEATURES, PointCloud
    p = Project.new("Scans", start="2026-11-02")
    rng = np.random.default_rng(2)
    xy = rng.uniform(0, 20, size=(20000, 2))
    ground = np.column_stack([xy, 0.05 * xy[:, 0] + rng.normal(0, 0.01, len(xy))]) + [50000.0, 7000000.0, 900.0]
    spikes = ground[:40] + [0, 0, 25.0]  # 40 isolated points 25 m above the surface
    f = tmp_path / "scan.xyz"
    np.savetxt(f, np.vstack([ground, spikes]), fmt="%.4f")
    scan = p.survey.import_pointcloud(f, name="Ground scan")
    assert isinstance(scan, PointCloud) and isinstance(scan, str)
    assert scan.name == "Ground scan" and scan.point_count == 20040 and scan.derivation == "" and scan.parent is None
    assert p.survey.pointcloud(scan.id) == scan and scan in p.survey.pointclouds()

    clean = scan.remove_outliers(k=16, std_ratio=2.0)
    assert clean.parent == scan and clean.derivation.startswith("statistical outlier removal k=16")
    z = clean.xyz[:, 2]
    assert z.max() < 905 and 19800 <= clean.point_count < 20040
    assert scan.point_count == 20040 and scan.xyz[:, 2].max() > 920  # the source is never altered
    r = scan.remove_outliers(method="radius", radius=0.5, min_neighbours=3, name="radius clean")
    assert r.name == "radius clean" and r.xyz[:, 2].max() < 905
    with pytest.raises(ValueError):
        scan.remove_outliers(method="radius")

    thin = clean.voxel_downsample(0.5)
    assert 1000 < thin.point_count < clean.point_count
    original = {tuple(q) for q in np.round(clean.xyz, 4)}
    assert all(tuple(q) in original for q in np.round(thin.xyz[:200], 4))  # nearest-to-centre keeps real points
    assert thin.statistics()["median_spacing"] > 0.3
    spaced = clean.subsample(spacing=1.0)
    assert spaced.statistics()["count"] == spaced.point_count
    sample = clean.subsample(count=500, seed=7)
    assert sample.point_count == 500
    with pytest.raises(ValueError):
        clean.subsample()

    normals = thin.estimate_normals(k=12)
    assert normals.attributes["normals"] and normals.derivation.startswith("PCA normals k=12")
    n = normals.points()["normals"]
    assert np.all(n[:, 2] > 0.95)  # the surface is nearly horizontal, oriented up

    box = (50005.0, 7000005.0, 0.0, 50010.0, 7000010.0, 2000.0)
    inner = clean.crop(box)
    outer = clean.crop(box, inside=False)
    assert inner.point_count + outer.point_count == clean.point_count
    assert np.all((inner.xyz[:, 0] >= 50005.0) & (inner.xyz[:, 0] <= 50010.0))

    with pytest.raises(ValueError):
        p._native.derive_point_cloud(scan.id, "statistical_outliers", {"no_such": 1}, "openmine", "", None)
    with pytest.raises(ValueError):
        p._native.derive_point_cloud(scan.id, "no_such_operation", {}, "openmine", "", None)

    if FEATURES["open3d"]:
        o = scan.remove_outliers(engine="open3d")
        assert "Open3D" in o.derivation and o.xyz[:, 2].max() < 905
        assert abs(o.point_count - clean.point_count) < 100
        c = clean.voxel_downsample(0.5, mode="centroid", engine="open3d")
        assert abs(c.point_count - clean.voxel_downsample(0.5, mode="centroid").point_count) <= 0.02 * c.point_count
        with pytest.raises(ValueError):
            clean.voxel_downsample(0.5, engine="open3d")  # Open3D has no nearest-to-centre mode
        on = thin.estimate_normals(k=12, engine="open3d")
        assert np.all(on.points()["normals"][:, 2] > 0.95)
    else:
        with pytest.raises(ValueError):
            scan.remove_outliers(engine="open3d")

    count = len(p.survey.pointclouds())
    p.undo()  # the last derivation is one undo step
    assert len(p.survey.pointclouds()) == count - 1
    out = tmp_path / "clean.xyz"
    clean.export(out)
    assert out.read_text().splitlines()[0].split()[:3] == ["x", "y", "z"]
    assert len(np.loadtxt(out, skiprows=1, usecols=(0, 1, 2))) == clean.point_count
    saved = tmp_path / "Scans.openmine"
    p.save(saved)
    q = Project.open(saved)
    assert q.survey.pointcloud(clean.id).derivation == clean.derivation
    p.save()  # saves to its file


@pytest.mark.skipif(not (openmine.FEATURES["pdal"] or openmine.FEATURES["open3d"]), reason="no surface-reconstruction engine")
def test_compare_design_against_a_scan():
    from openmine import FEATURES
    p = Project.new("Conformance", start="2026-11-02")
    design, pts = _scanned_heading(p)
    scan = p.survey.add_point_cloud(pts, "XC scan")
    engines = [e for e in ("pdal", "open3d") if FEATURES[e]]
    results = {}
    for engine in engines:
        r = p.survey.compare_design(design, scan, interval=1.0, depth=8, voxel=0.05, engine=engine)
        assert r["as_built_method"].startswith("Poisson depth=8")
        doc = p.documents.get("AsBuiltModel", r["as_built"])
        assert doc is not None
        assert r["valid_stations"] >= 29, engine
        # The north wall is 0.3 m out over its 5 m height along the 30 m: 0.3 × 5 × 30 = 45 m³ of
        # overbreak; the rest of the profile matches the design within the scan noise.
        assert 0.25 < r["max_overbreak_depth"] < 0.45, (engine, r["max_overbreak_depth"])
        assert r["overbreak_volume"] == pytest.approx(45.0, rel=0.05), (engine, r["overbreak_volume"])
        assert r["underbreak_volume"] < 2.0, (engine, r["underbreak_volume"])
        assert r["overbreak_percent"] == pytest.approx(100 * r["overbreak_volume"] / r["design_volume"])
        assert "result" in r
        results[engine] = r
    # A stored as-built model is compared directly (no rebuild).
    again = p.survey.compare_design(design, results[engines[0]]["as_built"], commit=False)
    assert again["overbreak_volume"] == pytest.approx(results[engines[0]]["overbreak_volume"], rel=1e-9)
    assert "result" not in again and "as_built_method" not in again
    if len(results) == 2:
        a, b = results["pdal"], results["open3d"]
        assert a["overbreak_volume"] == pytest.approx(b["overbreak_volume"], rel=0.03)
    with pytest.raises(KeyError):
        p.survey.compare_design(design, "00000000-0000-4000-8000-000000000000")
    with pytest.raises(TypeError):
        p.survey.compare_design(design, results[engines[0]]["as_built"], depth=8)
    sparse = p.survey.add_point_cloud(pts[:50], "too sparse")
    with pytest.raises(RuntimeError):
        p.survey.build_as_built(sparse, design)


def test_cad_drawing_and_editing_through_the_shared_edit_operations():
    p = Project.new("CAD edits", start="2026-11-02")
    lay = p.cad.add_layer("Drafting")
    arc = p.cad.add_arc((0, 0, 0), 10.0, 0.0, 90.0, layer="Drafting")
    g = p.cad.geometry(arc)
    assert g["type"] == "Arc" and g["length"] == pytest.approx(10 * math.pi / 2)
    assert g["arc"]["radius"] == pytest.approx(10.0) and g["arc"]["end_deg"] == pytest.approx(90.0)
    a3 = p.cad.add_arc_through((10, 0, 0), (0, 10, 0), (-10, 0, 0))
    assert p.cad.geometry(a3)["length"] == pytest.approx(10 * math.pi)
    c = p.cad.add_circle((50, 50, 5), 2.0)
    assert p.cad.geometry(c)["length"] == pytest.approx(4 * math.pi)
    r = p.cad.add_rectangle((0, 0, 0), (4, 3, 0))
    assert p.cad.geometry(r)["length"] == pytest.approx(14.0) and p.cad.geometry(r)["closed"]
    hexagon = p.cad.add_polygon((0, 0, 0), 1.0, 6)
    assert p.cad.geometry(hexagon)["length"] == pytest.approx(6.0)
    sp = p.cad.add_spline([(0, 0, 0), (5, 2, 0), (10, 0, 0)])
    assert p.cad.geometry(sp)["length"] > 10.0
    t = p.cad.add_text((1, 1, 0), "XC 12", height=2.0, rotation_deg=30, align="centre")
    assert p.cad.geometry(t)["text"] == "XC 12" and p.cad.geometry(t)["rotation_deg"] == pytest.approx(30)
    dim = p.cad.add_dimension("Aligned", [(0, 0, 0), (3, 4, 0), (0, 1, 0)], precision=2)
    assert p.cad.geometry(dim)["dimension_value"] == pytest.approx(5.0)
    h = p.cad.add_hatch([[(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0)], [(4, 4, 0), (6, 4, 0), (6, 6, 0), (4, 6, 0)]], pattern="SOLID")
    assert p.cad.geometry(h)["hatch_area"] == pytest.approx(96.0)
    with pytest.raises(ValueError):
        p.cad.add_text((0, 0, 0), "x", align="middle")
    with pytest.raises(ValueError):
        p.cad.add_dimension("Oblique", [(0, 0, 0), (1, 0, 0), (0, 1, 0)])

    # Transforms (row-major 4x4 built in Python) and one undo step each.
    line = p.cad.add_line((0, 0, 0), (10, 0, 0))
    moved = p.cad.move(line, (5, 0, 1))
    assert moved["changed"] == [line]
    assert p.cad.geometry(line)["vertices"][0].tolist() == [5.0, 0.0, 1.0]
    p.undo()
    assert p.cad.geometry(line)["vertices"][0].tolist() == [0.0, 0.0, 0.0]
    cp = p.cad.copy([line], (0, 2))
    assert len(cp["created"]) == 1 and p.cad.geometry(cp["created"][0])["vertices"][1].tolist() == [10.0, 2.0, 0.0]
    p.cad.rotate(line, 90, about=(0, 0, 0))
    assert np.allclose(p.cad.geometry(line)["vertices"][1], [0, 10, 0], atol=1e-12)
    p.cad.scale(line, 2.0, about=(0, 0, 0))
    assert p.cad.geometry(line)["length"] == pytest.approx(20.0)
    p.cad.mirror(line, (1, 0), (1, 1))  # vertical plane x = 1
    assert np.allclose(p.cad.geometry(line)["vertices"][:, 0], [2.0, 2.0])
    with pytest.raises(ValueError):
        p.cad.scale(line, 0)

    # Offset, trim, extend, split, join, explode, fillet, chamfer.
    base = p.cad.add_line((0, 100, 0), (20, 100, 0))
    off = p.cad.offset(base, 3.0, (10, 110, 0))
    assert p.cad.geometry(off)["vertices"][0][1] == pytest.approx(103.0)
    cutter = p.cad.add_line((10, 90, 0), (10, 110, 0))
    tr = p.cad.trim(base, (15, 100, 0), cutting_edges=[cutter])
    assert tr["changed"] == [base] and p.cad.geometry(base)["length"] == pytest.approx(10.0)
    ex = p.cad.extend(base, (9, 100, 0), boundaries=[p.cad.add_line((30, 90, 0), (30, 110, 0))])
    assert p.cad.geometry(base)["length"] == pytest.approx(30.0) and ex["changed"] == [base]
    pl = p.cad.add_polyline([(0, 200, 0), (10, 200, 0), (10, 210, 0)])
    parts = p.cad.split(pl, (10, 200, 0))
    assert len(parts["created"]) == 1
    joined = p.cad.join([pl, parts["created"][0]])
    assert len(joined["created"]) == 1 and set(joined["removed"]) == {pl, parts["created"][0]}
    pieces = p.cad.explode(joined["created"][0])
    assert len(pieces["created"]) == 2
    l1, l2 = pieces["created"]
    f = p.cad.fillet(l1, (8, 200, 0), l2, (10, 202, 0), 1.0)
    assert len(f["created"]) == 1 and p.cad.geometry(f["created"][0])["type"] == "Arc"
    q1 = p.cad.add_line((0, 300, 0), (10, 300, 0))
    q2 = p.cad.add_line((10, 300, 0), (10, 310, 0))
    ch = p.cad.chamfer(q1, (8, 300, 0), q2, (10, 302, 0), 2.0)
    assert p.cad.geometry(ch["created"][0])["length"] == pytest.approx(2 * math.sqrt(2))
    with pytest.raises(ValueError):
        p.cad.join([q1, cutter])  # ends do not meet: nothing changes

    # A heading's centreline: edits flag the linked activity, they never change its quantity.
    design = p.cad.create_development(name="XC1", length=40, width=5, height=5, start=(0, 500, 0), azimuth_deg=90, gradient_percent=0)
    act = p.scheduler.create_activity(name="Develop XC1", geometry=design, production_rate=4.0)
    r = p.cad.scale(design.geometry_id, 1.5, about=(0, 500, 0))
    assert r["flagged_activities"] == 1
    assert act.document["columns"]["design_impact_flag"] in (1, True)
    assert act.document["columns"]["quantity"] == pytest.approx(40.0)
    m1 = p.cad.add_line((0, 600, 0), (5, 600, 0))
    m2 = p.cad.add_line((5, 600, 0), (5, 605, 0))
    j = p.cad.join([design.geometry_id, m1, m2])
    assert any("centreline" in n for n in j["notes"]) and len(j["created"]) == 1


@pytest.mark.skipif(not openmine.FEATURES["solids"], reason="built without Open CASCADE")
def test_solid_modelling_from_curves_and_operations(tmp_path):
    p = Project.new("Solids", start="2026-11-02")
    # Extrusion with a hole: 10 × 10 outer square, 4 × 4 inner square, 3 m high.
    outer = p.cad.add_rectangle((0, 0, 0), (10, 10, 0))
    inner = p.cad.add_rectangle((3, 3, 0), (7, 7, 0))
    [ex] = p.solids.extrude([outer, inner], 3.0)
    info = p.solids.info(ex)
    assert info["valid"] and info["closed"] and info["volume"] == pytest.approx((100 - 16) * 3, rel=1e-9)
    [sym] = p.solids.extrude(inner, 2.0, symmetric=True, holes=False)
    assert p.solids.info(sym)["bounds"][2] == pytest.approx(-1.0) and p.solids.info(sym)["bounds"][5] == pytest.approx(1.0)
    [vec] = p.solids.extrude(inner, direction=(0, 0, -4))
    assert p.solids.info(vec)["volume"] == pytest.approx(64.0)
    with pytest.raises(ValueError):
        p.solids.extrude(p.cad.add_line((0, 0, 0), (1, 0, 0)), 1.0)  # not a closed curve
    # Revolution of a 1 × 2 rectangle 2 m from the Z axis: a ring, volume 2π·r̄·A.
    prof = p.cad.add_polyline([(2, 0, 0), (3, 0, 0), (3, 0, 2), (2, 0, 2)], closed=True)
    [ring] = p.solids.revolve(prof, (0, 0, 0), (0, 0, 1))
    assert p.solids.info(ring)["volume"] == pytest.approx(2 * math.pi * 2.5 * 2.0, rel=1e-6)
    # Sweep of a 5 × 5 rectangle along a 40 m straight path: 1000 m³.
    path = p.cad.add_polyline([(0, 100, 0), (40, 100, 0)])
    [sw] = p.solids.sweep(path, width=5, height=5)
    assert p.solids.info(sw)["volume"] == pytest.approx(1000.0, rel=1e-6)
    # Loft between two squares (frustum): V = h/3 (A1 + A2 + √(A1·A2)).
    s1 = p.cad.add_polyline([(0, 0, 10), (4, 0, 10), (4, 4, 10), (0, 4, 10)], closed=True)
    s2 = p.cad.add_polyline([(1, 1, 13), (3, 1, 13), (3, 3, 13), (1, 3, 13)], closed=True)
    lo = p.solids.loft([s1, s2], ruled=True)
    assert p.solids.info(lo)["volume"] == pytest.approx(3 / 3 * (16 + 4 + 8), rel=1e-6)
    # Primitives and booleans.
    a = p.solids.box((0, 0, 50), (2, 2, 52))
    b = p.solids.box((1, 1, 51), (3, 3, 53))
    u = p.solids.union(a, b)
    assert p.solids.info(u)["volume"] == pytest.approx(15.0, rel=1e-9)
    d = p.solids.subtract(a, b)
    assert p.solids.info(d)["volume"] == pytest.approx(7.0, rel=1e-9)
    i = p.solids.intersect(a, b, remove_inputs=True)
    assert p.solids.info(i)["volume"] == pytest.approx(1.0, rel=1e-9)
    assert p.cad.ids(type="Solid").count(a) == 0  # inputs removed in the same undo step
    cyl = p.solids.cylinder((0, 0, 100), (0, 0, 1), 1.0, 2.0)
    assert p.solids.info(cyl)["volume"] == pytest.approx(2 * math.pi, rel=1e-6)
    sph = p.solids.sphere((0, 0, 200), 1.5)
    assert p.solids.info(sph)["volume"] == pytest.approx(4 / 3 * math.pi * 1.5 ** 3, rel=1e-6)
    cone = p.solids.cone((0, 0, 300), (0, 0, 1), 2.0, 1.0, 3.0)
    assert p.solids.info(cone)["volume"] == pytest.approx(math.pi * 3 / 3 * (4 + 1 + 2), rel=1e-6)
    assert p.solids.classify(sph, [(0, 0, 200), (0, 0, 201.5), (0, 0, 210)]) == ["inside", "on", "outside"]
    assert p.solids.distance(cyl, sph)["distance"] == pytest.approx(200 - 1.5 - 102, rel=1e-9)
    # Fillet edges and offset change the solid in place, one undo step each.
    box = p.solids.box((0, 0, 400), (4, 4, 404))
    f = p.solids.fillet_edges(box, 0.5)
    assert f["valid"] and f["volume"] < 64.0
    p.undo()
    assert p.solids.info(box)["volume"] == pytest.approx(64.0)
    o = p.solids.offset(box, 0.5)
    # Offset with rounded convex edges: V + A·d + (Σ edge length)·πd²/4 + 4/3·πd³ (Steiner formula).
    assert o["volume"] == pytest.approx(64 + 96 * 0.5 + 48 * math.pi * 0.25 / 4 + 4 / 3 * math.pi * 0.125, rel=1e-6)
    with pytest.raises(ValueError):
        p.solids.fillet_edges(box, -1)
    # Sections: solids exactly, meshes by plane section.
    curves = p.solids.section([sw], (20, 100, 0), (1, 0, 0), layer="0")
    assert len(curves) == 1 and p.cad.geometry(curves[0])["length"] == pytest.approx(20.0, rel=1e-6)
    # Moving a solid is exact (B-rep transformed by the kernel).
    moved = p.cad.move(ex, (0, 0, 10))
    assert moved["changed"] == [ex] and p.solids.info(ex)["bounds"][2] == pytest.approx(10.0)
    # STEP round trip.
    f = tmp_path / "solids.step"
    p.solids.export_file([ex, ring], f)
    q = Project.new("STEP", start="2026-11-02")
    back = q.solids.import_file(f)
    assert sorted(round(q.solids.info(x)["volume"], 6) for x in back) == sorted(round(p.solids.info(x)["volume"], 6) for x in (ex, ring))
    # Healing a valid solid keeps it valid.
    h = p.solids.heal(ex)
    assert h["before"]["valid"] and h["after"]["valid"]
    # Excavation solid of a heading.
    dev = p.cad.create_development(name="XC9", length=30, width=5, height=5, start=(0, 0, -100), azimuth_deg=0, gradient_percent=0)
    ds = p.solids.of_development(dev)
    assert p.solids.info(ds)["volume"] == pytest.approx(dev.metrics["volume"], rel=1e-6)


def test_mine_design_layouts_levels_stopes_rules_and_revisions():
    p = Project.new("Design", start="2026-11-02")
    lvl = p.design.add_level(code="675", elevation=675.0)
    # A switchback decline from 750 m to 675 m at -12.5 %: ends exactly at the target.
    [dec] = p.design.layout("decline", start=(0, 0, 750), azimuth_deg=90, grade_percent=-12.5, straight_length=60, radius=20, target_elevation=675)
    assert dec["vertices"][-1][2] == pytest.approx(675.0, abs=1e-6)
    assert dec["plan_length"] == pytest.approx(75.0 / 0.125, rel=1e-6)
    [decl] = p.design.create_headings([dec], prefix="DEC", level="675", width=5.5, height=5.5)
    assert decl.heading_id == "DEC" or decl.heading_id.startswith("DEC")
    assert decl.document["columns"]["level_uuid"] == lvl  # the level code resolves to the MineLevel
    rules = p.design.check_design_rules(decl, max_grade_percent=10.0)
    assert rules and all(v["rule"] for v in rules)  # 12.5 % exceeds a 10 % limit
    assert not p.design.check_design_rules(decl, max_grade_percent=15.0, max_grade_on_curves_percent=15.0, min_radius=15.0)
    grades = p.design.gradient_profile(decl)
    assert all(g["grade_percent"] == pytest.approx(-12.5, abs=1e-6) for g in grades)
    # Horizontal alignment through three PIs with a 30 m curve; regrade to +1 %.
    [h] = p.design.layout("horizontal", points=[(0, 200, 675), (100, 200, 675), (100, 300, 675)], radius=30.0, grade_percent=0.0)
    assert h["plan_length"] == pytest.approx(200 - 2 * 30 + math.pi / 2 * 30, rel=1e-9)
    [drive] = p.design.create_headings([h], prefix="DR")
    act = p.scheduler.create_activity(name="Develop DR", geometry=drive, production_rate=4.0)
    flagged = p.design.grade(drive, grade_percent=1.0, start_elevation=675.0)
    assert flagged == 1
    assert [i["heading_id"] for i in p.design.impacts()] == [drive.heading_id]
    p.design.apply_design_quantities(act)
    assert act.document["columns"]["quantity"] == pytest.approx(drive.length)
    assert p.design.impacts() == []
    # Crosscuts off the drive every 40 m, 20 m long.
    xcs = p.design.create_crosscuts(drive, interval=40, first_chainage=20, length=20, side="left")
    assert len(xcs) >= 3 and all(x.document["columns"]["activity_type"] == "Crosscut" for x in xcs)
    assert all(x.length == pytest.approx(20.0, rel=0.05) for x in xcs)
    q = {r["heading_id"]: r for r in p.design.quantities()}
    assert q[drive.heading_id]["schedule_matches_design"]
    # Stopes: a 20 × 10 × 30 m prism, 2.8 t/m³ default material, then subdivided into panels.
    outline = [(500, 0), (520, 0), (520, 10), (500, 10)]
    st = p.design.create_stope("ST1", outline=outline, floor=600.0, back=630.0, grade=4.0, dilution_percent=10, recovery_percent=90, level="675")
    m = p.design.stope_metrics(st)
    assert m["volume"] == pytest.approx(6000.0) and m["watertight"]
    assert m["diluted_tonnes"] == pytest.approx(m["tonnes"] * 1.1)
    assert m["recovered_tonnes"] == pytest.approx(m["diluted_tonnes"] * 0.9)
    blocks = p.design.subdivide_stope(st, outline, 600.0, 630.0, strike_azimuth_deg=90, panel_length=5.0, lift_height=15.0)
    assert len(blocks) == 8
    with pytest.raises(KeyError):
        p.design.create_stope("ST2", outline=outline, floor=600, back=610, level="999")
    # Revisions: capture, change, compare, approve, restore.
    rev_a = p.design.capture_revision(drive, "A", "issued for construction")
    p.cad.move(drive.geometry_id, (0, 0, 2.0))
    cmp = p.design.compare_revisions(rev_a)
    assert cmp["from"] == "A" and cmp["max_vertex_deviation"] == pytest.approx(2.0)
    p.design.approve_revision(rev_a)
    p.design.restore_revision(rev_a)
    assert p.design.compare_revisions(rev_a)["max_vertex_deviation"] == pytest.approx(0.0, abs=1e-9)
    # Ramp, spiral, access and ore drive layouts.
    [ramp] = p.design.layout("ramp", start=(0, 0, 600), azimuth_deg=0, target_elevation=580, grade_percent=-14.3, radius=20, straight_length=30)
    assert ramp["vertices"][-1][2] == pytest.approx(580.0, abs=1e-6)
    [sp] = p.design.layout("spiral", start=(0, 0, 600), azimuth_deg=0, grade_percent=-12.5, radius=25, target_elevation=590)
    assert sp["plan_length"] == pytest.approx(80.0, rel=1e-6)
    [acc] = p.design.layout("access", start=(0, 0, 600), azimuth_deg=0, target=(100, 100, 595), radius=20)
    assert acc["vertices"][-1][0] == pytest.approx(100.0, abs=1e-6)
    [ore] = p.design.layout("ore_drive", contact=[(0, 0, 500), (100, 0, 500)], offset=5.0, side="left", grade_percent=0.0)
    assert all(v[1] == pytest.approx(5.0) for v in ore["vertices"])
    with pytest.raises(ValueError):
        p.design.layout("labyrinth")


def test_scheduler_calendars_cycles_targets_periods_and_levelling():
    p = Project.new("Scheduling", start="2026-11-02")  # a Monday
    cal = p.scheduler.add_calendar("Weekdays 2×10 h", shifts=[{"name": "Day", "start": "06:00", "hours": 10, "days": ["Mon", "Tue", "Wed", "Thu", "Fri"]},
                                                           {"name": "Night", "start": "18:00", "hours": 10, "days": ["Mon", "Tue", "Wed", "Thu", "Fri"]}],
                                   exceptions=[{"date": "2026-11-04", "reason": "Shutdown"}], hours_per_day=20, make_default=True)
    a = p.scheduler.create_activity("Task A", duration_days=2)  # 2 days × 20 working hours of the default calendar
    r = p.scheduler.calculate()
    assert r.ok
    t = r.activity(a)
    # Mon day + night (20 h), Tue day (10 h), Tue night until midnight (6 h: Wednesday is shut down,
    # including the hours after midnight), then Thursday 06:00–10:00 (4 h).
    assert t["start"] == "2026-11-02 06:00" and t["finish"] == "2026-11-05 10:00", t
    with pytest.raises(ValueError):
        p.scheduler.add_calendar("Bad", shifts=[{"start": "25:00", "hours": 8}])
    with pytest.raises(ValueError):
        p.scheduler.add_calendar("Bad", shifts=[{"start": "06:00", "hours": 8, "days": ["Funday"]}])
    always = p.scheduler.add_calendar("24/7", shifts=[{"start": "00:00", "hours": 24}])
    p.scheduler.set_calendar(a, always)
    assert p.scheduler.calculate().activity(a)["finish"] == "2026-11-03 16:00"  # the same 40 h from Mon 00:00 on a 24/7 calendar
    p.scheduler.set_calendar(a, None)

    # A development cycle drives the rate of a linked heading.
    cyc = p.scheduler.add_cycle("Jumbo 2-boom", round_length=4.0, advance_efficiency=0.9, holes_per_round=50, shift_hours=10, shifts_per_day=2)
    b = p.scheduler.evaluate_cycle(cyc, 5.0, 5.0)
    assert b["advance_per_cycle"] == pytest.approx(3.6)
    assert b["total_minutes"] == pytest.approx(b["drilling_minutes"] + b["charging_minutes"] + b["blasting_minutes"] + b["mucking_minutes"]
                                               + b["support_minutes"] + b["other_minutes"])
    assert b["metres_per_day"] == pytest.approx(b["metres_per_shift"] * 2)
    with pytest.raises(ValueError):
        p.scheduler.add_cycle("Bad", no_such_field=1)
    dev = p.cad.create_development(name="DR1", length=100, width=5, height=5, start=(0, 0, 0), azimuth_deg=90, gradient_percent=0)
    act = p.scheduler.create_activity("Develop DR1", geometry=dev, production_rate=1.0)
    slow = p.scheduler.calculate().activity(act)["duration_hours"]
    p.scheduler.use_cycle(act, cyc)
    fast = p.scheduler.calculate().activity(act)["duration_hours"]
    assert fast < slow  # the cycle advances more than 1 m/day

    # Targets and production by month.
    p.scheduler.add_target("Nov development", "2026-11-01", "2026-11-30", 80.0)
    months = p.scheduler.production("month")
    nov = next(m for m in months if m["start"] == "2026-11-01")
    assert nov["target"] == pytest.approx(80.0) and nov["scheduled"] > 0
    assert sum(m["scheduled"] for m in months) == pytest.approx(100.0, rel=1e-6)
    with pytest.raises(ValueError):
        p.scheduler.production("fortnight")

    # Two activities competing for one jumbo: levelling delays one of them in a levelled scenario.
    jumbo = p.scheduler.add_resource("Jumbo", capacity=1)
    x = p.scheduler.create_activity("X", duration_days=3)
    y = p.scheduler.create_activity("Y", duration_days=3)
    p.scheduler.assign(x, jumbo)
    p.scheduler.assign(y, jumbo)
    usage = {u["name"]: u for u in p.scheduler.resource_usage()}
    assert usage["Jumbo"]["overallocated"] and usage["Jumbo"]["peak"] == pytest.approx(2.0)
    lev = p.scheduler.add_scenario("Levelled", levelling="Heuristic")
    rl = p.scheduler.calculate(scenario=lev)
    assert rl.levelling and rl.levelling["delayed_activities"] == 1
    assert {rl.activity(x)["start"], rl.activity(y)["start"]} != {p.scheduler.calculate().activity(x)["start"]}
    assert not {u["name"]: u for u in p.scheduler.resource_usage(scenario=lev)}["Jumbo"]["overallocated"]
    assert p.scheduler.calculate().levelling is None
    with pytest.raises(ValueError):
        p.scheduler.add_scenario("Bad", levelling="Magic")


def test_survey_segmentation_distances_planes_surfaces_and_pair_registration(tmp_path):
    from openmine import FEATURES
    p = Project.new("Survey tools", start="2026-11-02")
    rng = np.random.default_rng(11)
    # Two separate boxes of points 10 m apart, plus a sloping floor plane.
    a = rng.uniform(0, 1, (3000, 3)) + [1000.0, 2000.0, 100.0]
    b = rng.uniform(0, 1, (3000, 3)) + [1010.0, 2000.0, 100.0]
    scan = p.survey.add_point_cloud(np.vstack([a, b]), "two blobs")
    seg = scan.segment("clusters", tolerance=0.3, min_size=100)
    labels = seg.points()["cluster"]
    assert set(np.unique(labels)) == {0.0, 1.0} and seg.derivation.startswith("connected components")
    # Distances of the blobs to a plane mesh z = 100.
    plane = p.cad.add_mesh([(990, 1990, 100), (1030, 1990, 100), (1030, 2010, 100), (990, 2010, 100)], [(0, 1, 2), (0, 2, 3)], name="z100")
    dist = scan.distance_to(plane, signed=True)
    d = dist.points()["distance"]
    assert np.all(d >= -1e-6) and np.all(d <= 1.0 + 1e-6)
    assert dist.last_statistics["mean"] == pytest.approx(0.5, abs=0.03)
    assert dist.derivation == "distance to z100"
    assert any(x["uuid"] == dist.analysis for x in p.documents.list("AnalysisResult"))
    # A cloud-to-cloud distance: blob b is ≥ 9 m from blob a.
    blob_a = p.survey.add_point_cloud(a, "a")
    blob_b = p.survey.add_point_cloud(b, "b")
    c2c = blob_b.distance_to(blob_a)
    assert c2c.points()["distance"].min() > 8.9
    # RANSAC planes on a cloud of two perpendicular walls.
    yz = rng.uniform(0, 10, (4000, 2))
    walls = np.vstack([np.column_stack([np.zeros(2000), yz[:2000]]), np.column_stack([yz[2000:, 0], np.zeros(2000), yz[2000:, 1]])]) + [500, 500, 50]
    wc = p.survey.add_point_cloud(walls + rng.normal(0, 0.002, walls.shape), "walls")
    planes = wc.fit_planes(0.01, max_planes=2, min_inliers=500, store=True)
    assert len(planes) == 2 and all(pl["dip"] == pytest.approx(90, abs=1.0) for pl in planes)
    assert sorted(round(pl["dip_direction"]) % 180 for pl in planes) == [0, 90]
    assert len(p.documents.list("StructuralMeasurement")) == 2
    # Delaunay surface of a terrain, then decimate / smooth / repair / compare (sources unchanged).
    xy = rng.uniform(0, 50, (5000, 2))
    terrain = np.column_stack([xy, 3 * np.sin(xy[:, 0] / 8) + 0.1 * xy[:, 1]]) + [0, 0, 200]
    tc = p.survey.add_point_cloud(terrain, "terrain")
    surf = tc.surface("delaunay", max_edge_length=5.0)
    assert surf["method"] == "Delaunay 2.5D" and surf["triangles"] > 9000
    g = p.cad.geometry(surf["id"])
    assert g["attributes"]["source_object"] == tc.id and p.cad.geometry(surf["id"])["layer"]
    dec = p.survey.decimate_surface(surf["id"], target_triangles=1000)
    assert dec["triangles"] <= 1000 and p.cad.geometry(dec["id"])["attributes"]["source_object"] == surf["id"]
    sm = p.survey.smooth_surface(dec["id"], iterations=5)
    assert sm["method"].startswith("Taubin")
    rp = p.survey.repair_surface(surf["id"])
    assert rp["repair"]["non_manifold_edges"] == 0
    cmp = p.survey.compare_surfaces(dec["id"], surf["id"])
    assert cmp["distances"]["measured"] == cmp["distances"]["vertices"] and abs(cmp["distances"]["mean"]) < 0.1
    assert p.cad.geometry(surf["id"])["mesh"][1].shape[0] == surf["triangles"]  # the source is unchanged
    with pytest.raises(ValueError):
        p.survey.smooth_surface(surf["id"], iterations=0)
    if FEATURES["pdal"] or FEATURES["open3d"]:
        # Poisson of a sphere sample.
        u = rng.normal(size=(4000, 3))
        sph = u / np.linalg.norm(u, axis=1)[:, None] * 5 + [0, 0, 300]
        sc = p.survey.add_point_cloud(sph, "sphere")
        ps = sc.surface("poisson", depth=7, orient_towards_centroid=False)
        assert ps["method"].startswith("Poisson depth=7")
    if FEATURES["pdal"]:
        gr = tc.classify_ground(cell=1.0, threshold=0.5)
        cls = gr.points()["classification"]
        assert (cls == 2).mean() > 0.9  # a smooth terrain is ground
    # Pair registration onto control points: a dataset shifted by (10, -5, 2).
    shift = np.array([10.0, -5.0, 2.0])
    for name, xyz in (("CP1", (0, 0, 0)), ("CP2", (100, 0, 1)), ("CP3", (0, 80, 2)), ("CP4", (60, 60, 3))):
        p.survey.add_control_point(name, xyz)
    moved = p.survey.add_point_cloud(np.array([(0, 0, 0), (100, 0, 1), (0, 80, 2), (60, 60, 3)]) - shift, "local scan")
    reg = moved.register_pairs([{"source": np.array(xyz) - shift, "control": n} for n, xyz in
                                (("CP1", (0, 0, 0)), ("CP2", (100, 0, 1)), ("CP3", (0, 80, 2)))] +
                               [{"source": np.array((60, 60, 3)) - shift, "control": "CP4", "role": "check"}], apply=True)
    assert reg["fit_rms"] < 1e-9 and reg["check_rms"] < 1e-9 and reg["fit_count"] == 3
    assert np.allclose(moved.xyz[0], [0, 0, 0], atol=1e-9)
    p.survey.revert_registration(reg["registration"])
    assert np.allclose(moved.xyz[0], -shift, atol=1e-9)
    with pytest.raises(KeyError):
        moved.register_pairs([{"source": (0, 0, 0), "control": "NOPE"}] * 3)


@pytest.mark.skipif(not (openmine.FEATURES["gis"] and openmine.FEATURES["crs"]), reason="built without GDAL or PROJ")
def test_gis_exchange_crs_transforms_and_local_grids(tmp_path):
    p = Project.new("GIS", start="2026-11-02")
    lo25 = p.crs.declare("EPSG:2051", set_as_project_crs=True)
    road = p.cad.add_polyline([(22000, 3180000, 1500), (22100, 3180050, 1495)], name="Haul road")
    pit = p.cad.add_polyline([(22300, 3180000, 1480), (22400, 3180000, 1480), (22400, 3180100, 1480)], closed=True, name="Pit")
    f = tmp_path / "features.gpkg"
    p.formats.export_vector([road, pit], f)
    q = Project.new("GIS2", start="2026-11-02")
    r = q.formats.import_vector(f)
    assert len(r["ids"]) == 2 and r["crs"] == "EPSG:2051"
    assert r["warnings"] == ["The file declares EPSG:2051 but the project CRS is not declared; coordinates were imported as they are"]
    q.crs.declare("EPSG:2051", set_as_project_crs=True)
    assert q.formats.import_vector(f)["warnings"] == []
    assert q.cad.geometry(r["ids"][0])["vertices"][1].tolist() == [22100.0, 3180050.0, 1495.0]
    with pytest.raises(openmine.CrsError):  # geographic: project data must be in metres
        q.crs.declare("EPSG:4326", set_as_project_crs=True)
    q.crs.declare("EPSG:2048", set_as_project_crs=True)  # Hartebeesthoek94 / Lo19
    assert any("differs from the project CRS (EPSG:2048)" in w for w in q.formats.import_vector(f)["warnings"])
    # Meshes: OBJ round trip at mine coordinates; binary STL warns about single precision.
    tri = p.cad.add_mesh([(22000.125, 3180000.25, 1500), (22010, 3180000, 1500), (22000, 3180010, 1501)], [(0, 1, 2)], name="tri")
    p.formats.export_mesh(tri, tmp_path / "tri.obj")
    back = p.formats.import_mesh(tmp_path / "tri.obj")
    assert np.array_equal(p.cad.geometry(back["id"])["mesh"][0], p.cad.geometry(tri)["mesh"][0])
    assert p.cad.geometry(back["id"])["attributes"]["source_file"] == "tri.obj"
    assert p.formats.export_mesh(tri, tmp_path / "tri.stl")  # precision warning
    # DEM: export a plane, import it back as a surface (GeoTIFF and ASCII grid).
    plane = p.cad.add_mesh([(0, 0, 10), (100, 0, 20), (100, 60, 20), (0, 60, 10)], [(0, 1, 2), (0, 2, 3)], name="plane")
    for ext in ("tif", "asc"):
        info = p.formats.export_dem([plane], tmp_path / f"dem.{ext}", cell=2.0)
        assert (info["width"], info["height"]) == (50, 30)
        s = p.formats.import_dem(tmp_path / f"dem.{ext}")
        v = p.cad.geometry(s["id"])["mesh"][0]
        assert np.allclose(v[:, 2], 10 + 0.1 * v[:, 0], atol=1e-6)
    # Transforming objects makes copies and keeps the originals.
    lo19 = p.crs.declare("EPSG:2048")
    copies = p.crs.transform_objects([road], lo25, lo19)
    moved = p.cad.geometry(copies[0])["vertices"]
    # 22 km west of 25° E is ~5.8° east of the Lo19 central meridian: a westing of about −580 km.
    assert -600000 < moved[0][0] < -560000 and moved[0][2] == pytest.approx(1500.0)
    assert np.allclose(moved, p.crs.transform_points(p.cad.geometry(road)["vertices"], lo25, lo19), atol=1e-6)
    assert p.cad.geometry(road)["vertices"][0][0] == pytest.approx(22000.0)
    assert any(x for x in p.documents.list("TransformationRecord"))
    assert p.crs.search("Hartebeesthoek94 / Lo25")[0]["code"] == "EPSG:2051"
    assert p.crs.operations("EPSG:2051", "EPSG:4326")
    # A local grid: rotated 30°, shifted, fitted from three pairs plus a check pair.
    ang = math.radians(30)
    def to_base(x, y, z):
        return (20000 + x * math.cos(ang) - y * math.sin(ang), 3180000 + x * math.sin(ang) + y * math.cos(ang), z + 1000)
    pairs = [{"name": f"S{i}", "local": loc, "base": to_base(*loc)} for i, loc in enumerate([(0, 0, 0), (500, 0, 5), (0, 400, -3)])]
    pairs.append({"name": "CHK", "local": (250, 250, 0), "base": to_base(250, 250, 0), "use": False})
    fit = p.crs.define_local_grid("Mine grid", lo25, pairs)
    assert fit["rms"] < 1e-6 and fit["check_rms"] < 1e-6
    pts = p.crs.transform_points([(100, 100, 0)], fit["crs"], lo25)
    assert np.allclose(pts[0], to_base(100, 100, 0), atol=1e-6)
    with pytest.raises(ValueError):
        p.crs.define_local_grid("Bad", lo25, pairs[:1])
