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


# ---- survey reports (HTML and PDF) ---------------------------------------------------------------
from html.parser import HTMLParser  # noqa: E402


class _Html(HTMLParser):
    """Collects tables (rows of cell text), svg count, headings and highlighted rows."""

    def __init__(self):
        super().__init__()
        self.tables, self.svgs, self.headings, self.hl = [], 0, [], 0
        self._cell, self._row, self._in_h, self._stack = None, None, False, []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        self._stack.append(tag)
        if tag == "table":
            self.tables.append([])
        elif tag == "tr":
            self._row = []
            if a.get("class") == "hl":
                self.hl += 1
        elif tag in ("td", "th"):
            self._cell = ""
        elif tag == "svg":
            self.svgs += 1
        elif tag in ("h2", "h3", "h4"):
            self._in_h = True
            self.headings.append("")

    def handle_endtag(self, tag):
        assert self._stack and self._stack[-1] == tag, f"mis-nested </{tag}> after {self._stack[-3:]}"
        self._stack.pop()
        if tag in ("td", "th"):
            self._row.append(self._cell.strip())
            self._cell = None
        elif tag == "tr":
            self.tables[-1].append(self._row)
        elif tag in ("h2", "h3", "h4"):
            self._in_h = False

    def handle_startendtag(self, tag, attrs):
        pass

    def handle_data(self, data):
        if self._cell is not None:
            self._cell += data
        if self._in_h:
            self.headings[-1] += data


def parse_html(path):
    """Parses a report and checks that every element is properly nested and closed (the only
    unclosed element allowed is the void <meta>)."""
    text = path.read_text(encoding="utf-8")
    assert text.startswith("<!DOCTYPE html>")
    p = _Html()
    original = p.handle_starttag

    def start(tag, attrs):
        original(tag, attrs)
        if tag == "meta":  # void element: never closed
            p._stack.pop()
    p.handle_starttag = start
    p.feed(text)
    p.close()
    assert p._stack == [], p._stack
    return p, text


@pytest.fixture(scope="module")
def survey(tmp_path_factory):
    d = tmp_path_factory.mktemp("survey-reports")
    out = subprocess.run([TOOL, "survey-reports", str(d)], check=True, capture_output=True, text=True).stdout
    return d, json.loads(out)


def test_conformance_report_html_and_pdf(survey):
    d, exp = survey
    c = exp["conformance"]
    h, text = parse_html(d / "conformance.html")
    assert "Station table" in h.headings and "Largest deviations" in h.headings
    stations = [t for t in h.tables if t and t[0][:2] == ["Chainage", "Design m²"]]
    assert len(stations) == 1 and len(stations[0]) - 1 == c["stations"]
    # Highlighted rows are exactly the stations with underbreak deeper than the tolerance.
    assert h.hl == len(c["underbreak_stations"])
    assert h.svgs == 2 + 6  # two charts, six cross-sections
    assert f"{c['overbreak_volume']:,.1f} m³" in text
    reader = pypdf.PdfReader(d / "conformance.pdf", strict=True)
    assert len(reader.pages) == c["pages"]
    pdf_text = "\n".join(p.extract_text() for p in reader.pages)
    assert f"Page {c['pages']} of {c['pages']}" in pdf_text
    assert f"{c['overbreak_volume']:,.1f} m³" in pdf_text
    for ch in c["underbreak_stations"]:
        assert f"{ch:.2f}" in pdf_text
    assert "?" not in pdf_text  # every character was representable (or transliterated)


def test_registration_and_point_cloud_reports(survey):
    d, exp = survey
    r = exp["registration"]
    h, text = parse_html(d / "registration.html")
    pairs = [t for t in h.tables if t and t[0][:2] == ["Point", "Role"]]
    assert len(pairs) == 1 and len(pairs[0]) - 1 == r["pairs"]
    roles = [row[1] for row in pairs[0][1:]]
    assert roles.count("Check") == 2
    assert f"RMS {r['check_rms']:.4f} m" in text
    pdf_text = "\n".join(p.extract_text() for p in pypdf.PdfReader(d / "registration.pdf", strict=True).pages)
    assert "Independent check points" in pdf_text and "?" not in pdf_text
    assert "17.188734°" in pdf_text  # rotation about Z of the synthetic transform (0.3 rad)
    p = exp["pointcloud"]
    h, text = parse_html(d / "pointcloud.html")
    assert f"{p['points']:,}" in text and "Synthetic TLS" in text and "Mean point spacing" in text
    pdf_text = "\n".join(pg.extract_text() for pg in pypdf.PdfReader(d / "pointcloud.pdf", strict=True).pages)
    assert "Registrations of this dataset" in pdf_text
    assert exp["stored"] == {"kind": "Survey", "format": "pdf", "bytes": exp["stored"]["bytes"]} and exp["stored"]["bytes"] > 1000
