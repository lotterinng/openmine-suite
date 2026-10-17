# SPDX-License-Identifier: Apache-2.0
"""Validates the native PDF writer and the XLSX exports with independent readers (pypdf,
pdfplumber, openpyxl)."""
import datetime
import json
import os
import subprocess

import pytest

pypdf = pytest.importorskip("pypdf")
openpyxl = pytest.importorskip("openpyxl")
TOOL = os.environ.get("OPENMINE_REPORT_TOOL", "report_tool")


@pytest.fixture(scope="module")
def sample(tmp_path_factory):
    d = tmp_path_factory.mktemp("reports")
    out = subprocess.run([TOOL, "sample-schedule", str(d)], check=True, capture_output=True, text=True).stdout
    return d, json.loads(out)


def test_gantt_pdf_is_valid_and_contains_the_schedule(sample):
    d, exp = sample
    reader = pypdf.PdfReader(d / "gantt.pdf", strict=True)
    assert len(reader.pages) == exp["pages"]
    assert reader.metadata.title == "Kopanong Mine schedule"
    text = "".join(p.extract_text() for p in reader.pages)
    for row in exp["rows"]:
        assert row["name"][:15] in text, row["name"]
    assert "Status 2026-12-01" in text
    assert "baseline: Approved budget 2027" in text
    box = reader.pages[0].mediabox
    assert (float(box.width), float(box.height)) == pytest.approx((1190.55, 841.89))


def test_gantt_text_positions_are_measured_correctly(sample):
    pdfplumber = pytest.importorskip("pdfplumber")
    d, _ = sample
    with pdfplumber.open(d / "gantt.pdf") as pdf:
        words = pdf.pages[0].extract_words()
        # Right-aligned numeric columns: words in the duration column end at the same x.
        durations = [w for w in words if w["text"] == "30.0" or w["text"] == "36.0" or w["text"] == "12.0"]
        assert durations
        right = {round(w["x1"], 1) for w in durations}
        assert len(right) == 1, right


def test_schedule_workbook_has_real_dates_and_values(sample):
    d, exp = sample
    wb = openpyxl.load_workbook(d / "schedule.xlsx")
    assert wb.sheetnames == ["Schedule", "Gantt"]
    ws = wb["Schedule"]
    header = [c.value for c in ws[1]]
    assert header[:5] == ["WBS", "Activity", "Kind", "Start", "Finish"]
    assert ws.freeze_panes == "C2"
    rows = list(ws.iter_rows(min_row=2, values_only=True))
    assert len(rows) == len(exp["rows"])
    for got, e in zip(rows, exp["rows"]):
        assert got[1] == e["name"]
        assert isinstance(got[3], datetime.datetime)
        assert got[3].strftime("%Y-%m-%d %H:%M") == e["start"]
        assert got[4].strftime("%Y-%m-%d %H:%M") == e["finish"]
        assert bool(got[8]) == e["critical"]
        if e["kind"] != "Summary":
            assert got[5] == pytest.approx(e["duration"], abs=1e-9)
        if e["baselineFinish"]:
            assert got[18].strftime("%Y-%m-%d %H:%M") == e["baselineFinish"]
    g = wb["Gantt"]
    filled = [c for row in g.iter_rows(min_row=2) for c in row[1:] if c.fill and c.fill.fgColor and c.fill.fgColor.rgb not in (None, "00000000")]
    assert filled


def test_generic_table_workbook(sample):
    d, _ = sample
    ws = openpyxl.load_workbook(d / "table.xlsx")["Quantities"]
    assert [c.value for c in ws[1]] == ["Heading", "Metres", "Planned"]
    assert ws["B2"].value == 150.5
    assert ws["C2"].value == datetime.datetime(2026, 12, 3)
    assert ws["C3"].value is None
