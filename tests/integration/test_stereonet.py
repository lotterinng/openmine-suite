# SPDX-License-Identifier: Apache-2.0
"""Validates stereonet statistics against mplstereonet (independent implementation)."""
import json
import os
import subprocess

import pytest

np = pytest.importorskip("numpy")
mpl = pytest.importorskip("mplstereonet")
from mplstereonet import stereonet_math as sm  # noqa: E402

TOOL = os.environ.get("OPENMINE_GEOTECH_TOOL", "geotech_tool")


def tool(tmp_path, req):
    f = tmp_path / "req.json"
    f.write_text(json.dumps(req))
    r = subprocess.run([TOOL, str(f)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def vec(trend, plunge):
    t, p = np.radians(trend), np.radians(plunge)
    return np.array([np.cos(p) * np.sin(t), np.cos(p) * np.cos(t), -np.sin(p)])


def axial_angle(a, b):
    # atan2 form: accurate for tiny angles (arccos loses precision near 1).
    return np.degrees(np.arctan2(np.linalg.norm(np.cross(a, b)), abs(float(np.dot(a, b)))))


def fisher_cluster(rng, trend, plunge, kappa, n):
    m = vec(trend, plunge)
    ref = np.array([0, 0, 1.0]) if abs(m[2]) < 0.9 else np.array([1.0, 0, 0])
    u = np.cross(m, ref)
    u /= np.linalg.norm(u)
    w = np.cross(m, u)
    x = rng.random(n)
    c = 1 + np.log(x + (1 - x) * np.exp(-2 * kappa)) / kappa
    s = np.sqrt(np.clip(1 - c * c, 0, None))
    ph = 2 * np.pi * rng.random(n)
    v = m[None, :] * c[:, None] + (np.cos(ph)[:, None] * u + np.sin(ph)[:, None] * w) * s[:, None]
    v[v[:, 2] > 0] *= -1  # lower hemisphere
    trend = np.degrees(np.arctan2(v[:, 0], v[:, 1])) % 360
    plunge = np.degrees(np.arcsin(-v[:, 2]))
    return trend, plunge


def test_plane_intersections_match(tmp_path):
    rng = np.random.default_rng(1)
    pairs = [[float(rng.uniform(5, 89)), float(rng.uniform(0, 360)), float(rng.uniform(5, 89)), float(rng.uniform(0, 360))] for _ in range(50)]
    ours = tool(tmp_path, {"op": "intersections", "pairs": pairs})
    for (d1, dd1, d2, dd2), o in zip(pairs, ours):
        plunge, bearing = sm.plane_intersection((dd1 - 90) % 360, d1, (dd2 - 90) % 360, d2)
        assert axial_angle(vec(o[0], o[1]), vec(float(bearing[0]), float(plunge[0]))) < 1e-7


def test_fisher_statistics_match(tmp_path):
    rng = np.random.default_rng(2)
    trend, plunge = fisher_cluster(rng, 210, 55, 40, 150)
    ours = tool(tmp_path, {"op": "fisher", "lines": [[float(t), float(p)] for t, p in zip(trend, plunge)]})
    (mp, mb), (r, angle, kappa) = mpl.find_fisher_stats(plunge, trend, conf=95)
    assert axial_angle(vec(ours["trend"], ours["plunge"]), vec(mb, mp)) < 1e-9
    assert ours["k"] == pytest.approx(kappa, rel=1e-10)
    assert ours["alpha95"] == pytest.approx(angle, rel=1e-10)
    assert ours["r"] == pytest.approx(r, rel=1e-10)


@pytest.mark.parametrize("method", ["schmidt", "kamb"])
def test_pole_density_matches(tmp_path, method):
    rng = np.random.default_rng(3)
    t1, p1 = fisher_cluster(rng, 30, 70, 25, 120)
    t2, p2 = fisher_cluster(rng, 300, 10, 15, 80)
    poles = np.concatenate([np.c_[t1, p1], np.c_[t2, p2]])
    dips = 90 - poles[:, 1]
    dipdirs = (poles[:, 0] + 180) % 360
    strikes = (dipdirs - 90) % 360
    lon, lat, z = mpl.density_grid(strikes, dips, method=method, gridsize=(31, 31))
    plunge, bearing = sm.geographic2plunge_bearing(lon.ravel(), lat.ravel())
    at = [[float(b), float(p)] for b, p in zip(bearing, plunge)]
    res = tool(tmp_path, {"op": "density", "method": method, "planes": [[float(d), float(dd)] for d, dd in zip(dips, dipdirs)], "at": at})
    ours = np.array(res["values"])
    n = len(dips)
    if method == "kamb":
        # mplstereonet subtracts a 0.5 continuity correction and clips negatives; ours reports count/σ.
        a = 9.0 / (n + 9.0)
        sigma = np.sqrt(n * a * (1 - a))
        ours = np.clip(ours - 0.5 / sigma, 0, None)
    assert np.allclose(ours, z.ravel(), atol=1e-9)


def test_joint_set_means_match_kmeans(tmp_path):
    rng = np.random.default_rng(4)
    truth = [(225, 30), (330, 10), (90, 75)]  # pole trend, plunge
    planes = []
    for t, p in truth:
        tt, pp = fisher_cluster(rng, t, p, 60, 120)
        planes += [[float(90 - b), float((a + 180) % 360)] for a, b in zip(tt, pp)]
    ours = tool(tmp_path, {"op": "sets", "planes": planes, "k": 3})
    dips = np.array([p[0] for p in planes])
    strikes = (np.array([p[1] for p in planes]) - 90) % 360
    centers = mpl.kmeans(strikes, dips, num=3, measurement="poles")
    theirs = [vec(float(b[0]), float(p[0])) for p, b in (sm.geographic2plunge_bearing(*c) for c in centers)]
    for o in ours:
        best = min(axial_angle(vec(o[0], o[1]), t) for t in theirs)
        assert best < 1.0
    assert sorted(o[2] for o in ours) == [120, 120, 120]
