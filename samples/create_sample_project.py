# SPDX-License-Identifier: Apache-2.0
"""Build a small synthetic demo project (CC0 data) with the OpenMine Python API.

    PYTHONPATH=build/linux-debug/python python3 samples/create_sample_project.py demo.openmine
"""
import sys

from openmine import Project


def main(path: str) -> None:
    p = Project.new("Demo Decline — Level 675", start="2026-11-02")
    p.set_crs("Hartebeesthoek94 / Lo25", "EPSG:2050")
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
    p.scheduler.add_dependency(a_dec, a_xc1, "SS", 20)   # XC1 breaks away once the decline passes it
    p.scheduler.add_dependency(a_dec, a_xc2, "FS")
    p.scheduler.add_dependency(a_xc1, a_od, "FS")
    p.scheduler.add_dependency(a_dec, services, "FF", 3)
    a_dec.record_actual(45)
    p.scheduler.capture_baseline("BL-Nov-2026")

    r = p.scheduler.calculate()
    print(f"Schedule {r.project_start} → {r.project_finish}  ({'ok' if r.ok else r.errors})")
    for a in p.scheduler.activities():
        t = r.activity(a["id"])
        print(f"  {a['name']:<30} {t['start']} → {t['finish']}  TF={t['total_float']:.1f}{'  CRITICAL' if t['critical'] else ''}")
    p.save(path)
    print("saved", path)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "demo.openmine")
