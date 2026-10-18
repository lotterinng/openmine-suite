# SPDX-License-Identifier: Apache-2.0
"""Build a small synthetic demo project (CC0 data) with the OpenMine Python API.

    PYTHONPATH=build/core/python python3 samples/create_sample_project.py demo.openmine
"""
import sys

import openmine
from openmine import Project


def main(path: str) -> None:
    p = Project.new("Demo Decline — Level 675", start="2026-11-02")
    if openmine.FEATURES["crs"]:
        p.declare_crs("EPSG:2051")  # Hartebeesthoek94 / Lo25 (declaration only; coordinates are not changed)
    x0, y0, z0 = -25000.0, 2900000.0, 1200.0

    decline = p.cad.create_development(
        name="DEC_675_A", width=5.5, height=5.5, level="675",
        points=[(x0, y0, z0), (x0 - 80, y0, z0 - 10), (x0 - 80, y0 - 64, z0 - 18), (x0, y0 - 64, z0 - 28)])
    xc1 = p.cad.create_development(name="XC_675_01", width=5.0, height=5.0, level="675",
                                   points=[(x0 - 80, y0 - 32, z0 - 14), (x0 - 140, y0 - 32, z0 - 14)])
    xc2 = p.cad.create_development(name="XC_675_02", width=5.0, height=5.0, level="675",
                                   points=[(x0, y0 - 64, z0 - 28), (x0 + 70, y0 - 64, z0 - 28)])
    od = p.cad.create_development(name="OD_675_N", width=4.5, height=4.5, level="675",
                                  points=[(x0 - 140, y0 - 32, z0 - 14), (x0 - 140, y0 + 60, z0 - 14)])

    a_dec = p.scheduler.create_activity("Develop DEC_675_A", geometry=decline, production_rate=4.5)
    a_xc1 = p.scheduler.create_activity("Develop XC_675_01", geometry=xc1, production_rate=4.0)
    a_xc2 = p.scheduler.create_activity("Develop XC_675_02", geometry=xc2, production_rate=4.0)
    a_od = p.scheduler.create_activity("Develop OD_675_N", geometry=od, production_rate=3.5)
    services = p.scheduler.create_activity("Install services DEC_675_A", duration_days=6)
    p.scheduler.link(a_dec, a_xc1, "SS", lag_hours=20 * 24)  # XC1 breaks away once the decline has advanced
    p.scheduler.link(a_dec, a_xc2, "FS")
    p.scheduler.link(a_xc1, a_od, "FS")
    p.scheduler.link(a_dec, services, "FF", lag_hours=3 * 24)
    jumbo = p.scheduler.add_resource("Jumbo J01", capacity=1)
    for a in (a_dec, a_xc1, a_xc2, a_od):
        p.scheduler.assign(a, jumbo)
    a_dec.record_actual(45, date="2026-11-12")
    p.set_status_date("2026-11-13")
    baseline = p.scheduler.create_baseline("BL-Nov-2026")

    r = p.scheduler.calculate()
    print(f"Schedule {r.start} → {r.finish}  ({'ok' if r.ok else r.errors})")
    for a in p.scheduler.activities():
        t = r.activity(a["id"])
        print(f"  {a['name']:<30} {t['start']} → {t['finish']}  TF={t['total_float_hours']:.1f} h{'  CRITICAL' if t['critical'] else ''}")
    if openmine.FEATURES["optimiser"]:
        o = p.scheduler.optimise(store_as="Jumbo-levelled", time_limit_seconds=10)
        print(f"Optimised with one jumbo: {o['status']}, makespan {o['makespan_hours'] / 24:.1f} days "
              f"(CPM ignoring the jumbo: {o['cpm_makespan_hours'] / 24:.1f} days)")
    p.save(path)
    print("saved", path, "with baseline", baseline)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "demo.openmine")
