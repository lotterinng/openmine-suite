// SPDX-License-Identifier: Apache-2.0
// Report storage: rendered documents and externally written files become Report entities with an
// immutable asset, in one undo step, and travel with the project file.
#include "openmine/persistence/ProjectFile.h"
#include "openmine/reporting/ReportDocument.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace openmine;
namespace fs = std::filesystem;

namespace {
reporting::ReportDocument sampleDocument() {
    reporting::ReportDocument d;
    d.title = "Development quantities";
    d.subtitle = "Test mine";
    d.heading("Summary").paragraph("Two headings.").keyValues({{"Total length", "212.4 m"}});
    reporting::ReportTable t;
    t.header = {"Heading", "Length (m)"};
    t.rows = {{"DEV_01", "100.0"}, {"DEV_02", "112.4"}};
    d.table(t);
    return d;
}
} // namespace

TEST(Reporting, StoreRenderedDocumentAsPdfAndHtml) {
    auto pp = Project::createNew("Reports", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    const Uuid pdf = reporting::storeReport(p, sampleDocument(), "Design", "pdf");
    const Uuid html = reporting::storeReport(p, sampleDocument(), "Design", "html");
    const auto* rp = p.model().reports.find(pdf);
    const auto* rh = p.model().reports.find(html);
    ASSERT_TRUE(rp && rh);
    EXPECT_EQ(rp->name, "Development quantities");
    EXPECT_EQ(rp->kind, "Design");
    EXPECT_EQ(rp->format, "pdf");
    EXPECT_FALSE(rp->createdAt.empty());
    const auto bytes = p.assets().readBytes(*p.model().assets.find(rp->asset.id));
    EXPECT_EQ(bytes.rfind("%PDF-", 0), 0u);
    EXPECT_NE(bytes.find("%%EOF"), std::string::npos);
    const auto page = p.assets().readBytes(*p.model().assets.find(rh->asset.id));
    EXPECT_NE(page.find("<html"), std::string::npos);
    EXPECT_NE(page.find("DEV_02"), std::string::npos);
    EXPECT_THROW(reporting::storeReport(p, sampleDocument(), "Design", "docx"), ValidationError);
}

TEST(Reporting, StoreReportBytesValidatesUndoesAndPersists) {
    const auto dir = fs::temp_directory_path() / ("openmine_reports_" + Uuid::generate().toString());
    fs::create_directories(dir);
    auto pp = Project::createNew("Reports", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    const std::string csv = "activity,start,finish\nDEV_01,2026-11-02,2026-11-20\n";
    // A workbook is binary: bytes must survive exactly (embedded NULs included).
    std::string xlsx("PK\x03\x04", 4);
    xlsx += std::string("\0\x01\x02 binary payload \xff\xfe", 22);

    const std::size_t assets0 = p.model().assets.size();
    const Uuid a = reporting::storeReportBytes(p, csv, "Schedule table", "Schedule", "csv");
    const Uuid b = reporting::storeReportBytes(p, xlsx, "Schedule workbook", "Schedule", "xlsx");
    ASSERT_TRUE(p.model().reports.find(a) && p.model().reports.find(b));
    EXPECT_EQ(p.model().assets.size(), assets0 + 2);
    EXPECT_EQ(p.model().reports.find(b)->format, "xlsx");
    EXPECT_EQ(p.assets().readBytes(*p.model().assets.find(p.model().reports.find(b)->asset.id)), xlsx);
    EXPECT_EQ(p.commands().undoText(), "Store report: Schedule workbook");

    // Refusals: unknown format, empty content. Nothing is stored.
    EXPECT_THROW(reporting::storeReportBytes(p, csv, "X", "Schedule", "exe"), ValidationError);
    EXPECT_THROW(reporting::storeReportBytes(p, "", "X", "Schedule", "csv"), ValidationError);
    EXPECT_EQ(p.model().reports.size(), 2u);

    // One undo step removes the report and its asset record; redo brings both back.
    p.commands().undo();
    EXPECT_FALSE(p.model().reports.find(b));
    EXPECT_EQ(p.model().assets.size(), assets0 + 1);
    p.commands().redo();
    ASSERT_TRUE(p.model().reports.find(b));

    // The files travel with the project and verify.
    const auto file = dir / "reports.openmine";
    persistence::saveProject(p, file);
    auto q = persistence::loadProject(file);
    const auto* qb = q->model().reports.find(b);
    ASSERT_TRUE(qb);
    EXPECT_EQ(q->assets().readBytes(*q->model().assets.find(qb->asset.id)), xlsx);
    EXPECT_EQ(q->assets().readBytes(*q->model().assets.find(q->model().reports.find(a)->asset.id)), csv);
    EXPECT_TRUE(persistence::checkProjectFile(file, true).ok);
    fs::remove_all(dir);
}
