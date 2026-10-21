# modules/reporting

Reports and exports. Two libraries: `openmine_reporting` (documents, PDF, schedule exports) and
`openmine_survey_reports` (survey reports, kept separate so schedule reporting does not depend on
the survey module).

| Area | Header | Notes |
|---|---|---|
| PDF | `Pdf.h` | dependency-free PDF 1.7 writer: pages, standard Helvetica fonts with exact metrics, lines, rectangles, polygons; Flate-compressed content |
| Documents | `ReportDocument.h` | format-independent technical reports (headings, paragraphs, tables, figures as SVG) rendered to self-contained HTML and to PDF; stored in the project as report assets (one undo step) |
| Schedule exports | `ScheduleExport.h` | Gantt chart PDF (table + timeline, progress, baseline, critical path, milestones, dependency arrows, status line), XLSX workbooks (real Excel dates, WBS indentation, filters, frozen panes, weekly Gantt sheet), generic XLSX writer |
| Survey reports | `SurveyReports.h` | point-cloud, registration (fit vs check residuals) and conformance reports built from stored results |

Tests: `tests/unit/test_reporting.cpp`; `tests/integration/test_reports.py` validates PDF and XLSX
output with pypdf, pdfplumber and openpyxl.
