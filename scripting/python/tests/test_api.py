# SPDX-License-Identifier: Apache-2.0
import math

import pytest

from openmine import Project, ValidationError, PersistenceError


def make_project():
    p = Project.new("Jupiter Mine", start="2026-11-02")
    p.set_crs("Hartebeesthoek94 / Lo25", "EPSG:2051")
    return p


def test_spec_workflow_round_trip(tmp_path):
    p = make_project()
    design = p.cad.create_development(name="LEG2_675_D3_DEC", length=150, width=5.5, height=5.5,
                                      start=(-25000.0, 2900000.0, 1200.0), azimuth_deg=270, gradient_percent=-12.5, level="675")
    assert design.length == pytest.approx(150.0, abs=1e-9)
    assert design.info["gradient_percent"] == pytest.approx(-12.5)
    act = p.scheduler.create_activity(name="Develop LEG2_675_D3_DEC", geometry=design, production_rate=5.0)
    assert act.info["linked_developments"] == [design.id]
    r = p.scheduler.calculate()
    assert r.ok
    t = r.activity(act.id)
    assert t["duration"] == pytest.approx(30.0)
    assert t["start"] == "2026-11-02" and t["finish"] == "2026-12-11"

    f = tmp_path / "JUP_Mine.openmine"
    p.save(f)
    assert not p.is_modified
    q = Project.open(f)
    assert q.name == "Jupiter Mine"
    assert q._native.crs() == ("Hartebeesthoek94 / Lo25", "EPSG:2051")
    d2 = q._native.development(design.id)
    assert d2["heading_id"] == "LEG2_675_D3_DEC"
    assert q._native.activity(act.id)["linked_developments"] == [design.id]
    g1 = p.cad.geometry(design.geometry_id)["vertices"]
    g2 = q.cad.geometry(design.geometry_id)["vertices"]
    assert g1 == g2  # bit-exact


def test_design_change_is_flagged_not_applied():
    p = make_project()
    d = p.cad.create_development(name="XC1", length=100, width=5, height=5)
    a = p.scheduler.create_activity(name="Develop XC1", geometry=d, production_rate=4.0)
    p.scheduler.capture_baseline("Approved")
    verts = p.cad.geometry(d.geometry_id)["vertices"]
    verts.append((verts[-1][0] + 20.0, verts[-1][1], verts[-1][2]))
    assert p.cad.update_vertices(d.geometry_id, verts) == 1
    info = a.info
    assert info["design_impact"] and info["quantity"] == pytest.approx(100.0)
    a.apply_design_quantities()
    assert a.info["quantity"] == pytest.approx(120.0)
    assert not a.info["design_impact"]
    assert p._native.baseline_names() == ["Approved"]
    assert any(e[2] == "Recalculate" for e in p.audit_log())


def test_reforecast_from_actuals():
    p = make_project()
    d = p.cad.create_development(name="OD1", length=150, width=5, height=5)
    a = p.scheduler.create_activity(name="Develop OD1", geometry=d, production_rate=5.0)
    a.record_actual(50)
    p.scheduler.set_status_date("2026-11-16")
    t = p.scheduler.calculate().activity(a.id)
    assert t["remaining_quantity"] == pytest.approx(100)
    assert t["start"] == "2026-11-16" and t["finish"] == "2026-12-11"


def test_validation_and_undo():
    p = make_project()
    with pytest.raises(ValidationError):
        p.cad.create_polyline([(0, 0, 0)])
    gid = p.cad.create_polyline([(0, 0, 0), (10, 0, 0)])
    p.undo()
    with pytest.raises(KeyError):
        p.cad.geometry(gid)
    p.redo()
    assert p.cad.geometry(gid)["length"] == pytest.approx(10)


def test_cycle_rejected():
    p = make_project()
    a = p.scheduler.create_activity("A", duration_days=2)
    b = p.scheduler.create_activity("B", duration_days=3)
    p.scheduler.add_dependency(a, b)
    with pytest.raises(ValidationError):
        p.scheduler.add_dependency(b, a)


def test_unimplemented_features_do_not_pretend():
    p = make_project()
    with pytest.raises(NotImplementedError):
        p.survey.import_pointcloud("level675_scan.e57")


def test_open_rejects_non_project(tmp_path):
    bad = tmp_path / "bad.openmine"
    bad.write_text("not a database")
    with pytest.raises(PersistenceError):
        Project.open(bad)
