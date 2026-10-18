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
