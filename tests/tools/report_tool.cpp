// SPDX-License-Identifier: Apache-2.0
// Test helper for tests/integration/test_reports.py (validated with pypdf, pdfplumber, openpyxl
// and the HTML parser of the standard library).
//   report_tool sample-schedule <dir>   Gantt PDF and XLSX of a sample schedule
//   report_tool survey-reports <dir>    conformance, registration and point-cloud reports (HTML + PDF)
#include "openmine/cad/Edit.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/reporting/ScheduleExport.h"
#include "openmine/reporting/SurveyReports.h"
#include "openmine/scheduling/Cpm.h"
#include "openmine/survey/Conformance.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/Registration.h"

#include <fstream>
#include <functional>

#include <nlohmann/json.hpp>

#include <iostream>

using namespace openmine;
using json = nlohmann::json;

namespace {
/// Closed tube through rings of a varying rectangular section (width w(x), height h(x)) along +X.
MeshData varyingTube(double x0, double x1, double z, const std::function<std::pair<double, double>(double)>& wh, int steps) {
    MeshData m;
    const int n = 4;
    for (int i = 0; i <= steps; ++i) {
        const double x = x0 + (x1 - x0) * i / steps;
        const auto [w, h] = wh(x);
        const double ys[4] = {-w / 2, w / 2, w / 2, -w / 2}, zs[4] = {z, z, z + h, z + h};
        for (int k = 0; k < n; ++k) m.vertices.push_back({x, ys[k], zs[k]});
    }
    auto id = [&](int i, int k) { return static_cast<std::uint32_t>(i * n + (k % n)); };
    for (int i = 0; i < steps; ++i)
        for (int k = 0; k < n; ++k) {
            m.triangles.push_back({id(i, k), id(i + 1, k), id(i + 1, k + 1)});
            m.triangles.push_back({id(i, k), id(i + 1, k + 1), id(i, k + 1)});
        }
    m.triangles.push_back({id(0, 0), id(0, 2), id(0, 1)});
    m.triangles.push_back({id(0, 0), id(0, 3), id(0, 2)});
    m.triangles.push_back({id(steps, 0), id(steps, 1), id(steps, 2)});
    m.triangles.push_back({id(steps, 0), id(steps, 2), id(steps, 3)});
    return m;
}

int surveyReports(const std::filesystem::path& dir) {
    auto pp = Project::createNew("Kopanong Mine — 675 Level", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    p.setUserName("A. Surveyor");
    ProjectEditor ed(p);
    const Uuid layer = ed.addLayer("As-built", 0x00AAFF);
    GeometryObject cl = cad::makeLine({0, 0, -100}, {40, 0, -100});
    cl.layerId = layer;
    mine_design::NewDevelopment nd;
    nd.headingId = "675_XC1";
    nd.name = "Crosscut 1";
    nd.width = 5;
    nd.height = 5;
    const Uuid dev = mine_design::createDevelopment(p, ed.addGeometry(cl), nd);
    // Scan of targets (scanner coordinates) and its registration to surveyed control.
    const Transform3 truth = Transform3::translation({-25000, 2900000, -900}) * Transform3::rotation({0, 0, 1}, 0.3);
    {
        std::ofstream o(dir / "scan.xyz");
        o.precision(12);
        for (int i = 0; i < 40; ++i)
            for (int j = 0; j < 40; ++j) o << i * 0.5 << " " << j * 0.5 << " " << 0.01 * i * j << "\n";
    }
    survey::ImportOptions io;
    io.scanner = "Synthetic TLS";
    io.scanDate = Date::fromYmd(2026, 11, 20);
    io.sourceCrs = "Scanner local";
    const Uuid ds = survey::importPointCloud(p, dir / "scan.xyz", io);
    const std::vector<Vec3> targets = {{1, 1, 0.5}, {18, 2, 1}, {17, 19, -0.3}, {2, 18, 0.7}, {9, 9, 2}, {5, 14, 1.2}};
    std::vector<RegistrationPair> pairs;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        SurveyControlPoint c;
        c.name = "T" + std::to_string(i + 1);
        // Check points carry a few millimetres of survey error (an honest check).
        c.position = truth.apply(targets[i]) + (i >= 4 ? Vec3{0.004, -0.003, 0.002} : Vec3{});
        c.role = i < 4 ? ControlPointRole::Control : ControlPointRole::Check;
        RegistrationPair pr;
        pr.name = c.name;
        pr.source = targets[i];
        pr.targetControlId = ed.add(c);
        pairs.push_back(pr);
    }
    const auto reg = survey::computeControlRegistration(p, ds, pairs, false);
    const Uuid rid = survey::commitRegistration(p, reg, true);
    // As-built: overbreak of 0.3 m on each side for x < 25, underbreak (4.6 m wide) beyond 30 m.
    const auto mesh = varyingTube(-1, 41, -100, [](double x) {
        if (x < 25) return std::make_pair(5.6, 5.2);
        if (x > 30) return std::make_pair(4.6, 5.0);
        const double t = (x - 25) / 5;
        return std::make_pair(5.6 + t * (4.6 - 5.6), 5.2 + t * (5.0 - 5.2));
    }, 84);
    const Uuid ab = survey::addAsBuiltModel(p, ds, mesh, "675_XC1 as-built 20-Nov", layer, dev, "Synthetic sweep");
    survey::ConformanceOptions co;
    co.interval = 2;
    const auto rep = survey::analyseAsBuilt(p, ab, dev, co);
    const Uuid cid = survey::commitConformance(p, ab, dev, rep, co);

    const auto conf = reporting::conformanceReport(p, cid, &rep);
    const auto regDoc = reporting::registrationReport(p, rid);
    const auto pc = reporting::pointCloudReport(p, ds, true);
    json out;
    for (const auto& [name, doc] : {std::pair{"conformance", &conf}, std::pair{"registration", &regDoc}, std::pair{"pointcloud", &pc}}) {
        std::ofstream(dir / (std::string(name) + ".html"), std::ios::binary) << reporting::renderHtml(*doc);
        std::size_t pages = 0;
        const auto pdf = reporting::renderPdf(*doc, &pages);
        std::ofstream(dir / (std::string(name) + ".pdf"), std::ios::binary) << pdf;
        out[name] = {{"title", doc->title}, {"pages", pages}};
    }
    const Uuid stored = reporting::storeReport(p, conf, "Survey", "pdf");
    const auto* rr = p.model().reports.find(stored);
    out["stored"] = {{"kind", rr->kind}, {"format", rr->format}, {"bytes", p.assets().readBytes(*p.model().assets.find(rr->asset.id)).size()}};
    const auto* c = p.model().conformanceResults.find(cid);
    out["conformance"]["stations"] = c->stationCount;
    out["conformance"]["valid"] = rep.validStations;
    out["conformance"]["overbreak_volume"] = c->overbreakVolume;
    out["conformance"]["underbreak_volume"] = c->underbreakVolume;
    json under = json::array();
    for (const auto& s : rep.stations)
        if (s.valid && s.maxUnderbreakDepth > 0.05) under.push_back(s.chainage);
    out["conformance"]["underbreak_stations"] = under;
    out["registration"]["fit_rms"] = reg.fitRms;
    out["registration"]["check_rms"] = reg.checkRms;
    out["registration"]["pairs"] = pairs.size();
    out["pointcloud"]["points"] = survey::dataset(p, ds).pointCount;
    std::cout << out.dump() << "\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "survey-reports") {
        try {
            return surveyReports(argv[2]);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
    }
    if (argc != 3 || std::string(argv[1]) != "sample-schedule") {
        std::cerr << "usage: report_tool sample-schedule|survey-reports <dir>\n";
        return 2;
    }
    try {
        const std::filesystem::path dir = argv[2];
        auto pp = Project::createNew("Kopanong Mine", Date::fromYmd(2026, 11, 2));
        Project& p = *pp;
        ProjectEditor ed(p);
        auto add = [&](const std::string& name, double days, Uuid parent = {}, ActivityKind kind = ActivityKind::Task, std::string wbs = {}) {
            ScheduleActivity a;
            a.name = name;
            a.durationHours = days * 24;
            a.kind = kind;
            a.parentId = parent;
            a.wbsCode = std::move(wbs);
            return ed.addActivity(a);
        };
        auto dep = [&](Uuid a, Uuid b, DependencyType t = DependencyType::FinishToStart) {
            ActivityDependency d;
            d.predecessorId = a;
            d.successorId = b;
            d.type = t;
            ed.addDependency(d);
        };
        const Uuid dec = add("Decline development", 0, {}, ActivityKind::Summary, "1");
        const Uuid d1 = add("Decline 1 — 0 to 150 m", 30, dec, ActivityKind::Task, "1.1");
        const Uuid d2 = add("Decline 1 — 150 to 300 m", 30, dec, ActivityKind::Task, "1.2");
        const Uuid lv = add("675 Level access", 0, {}, ActivityKind::Summary, "2");
        const Uuid xc = add("675 crosscut", 12, lv, ActivityKind::Task, "2.1");
        const Uuid od = add("675 ore drive", 40, lv, ActivityKind::Task, "2.2");
        const Uuid ms = add("Level 675 ready", 0, {}, ActivityKind::Milestone, "3");
        dep(d1, d2);
        dep(d2, xc);
        dep(xc, od, DependencyType::StartToStart);
        dep(od, ms);
        auto in = scheduling::buildScheduleInput(p);
        auto r = scheduling::calculateSchedule(in);
        auto bl = scheduling::makeBaseline(p, r, "Approved budget 2027");
        ed.addBaseline(bl);
        // Slip: decline 2 now takes longer; status date one month in.
        auto a2 = *p.model().activities.find(d2);
        a2.durationHours = 36 * 24;
        ed.updateActivity(a2);
        ProjectInfo info = p.model().info;
        info.statusDate = DateTime::fromDate(Date::fromYmd(2026, 12, 1));
        ed.setProjectInfo(info);
        auto a1 = *p.model().activities.find(d1);
        a1.actualStart = DateTime::fromDate(Date::fromYmd(2026, 11, 2));
        a1.manualPercentComplete = 90;
        ed.updateActivity(a1);
        in = scheduling::buildScheduleInput(p);
        r = scheduling::calculateSchedule(in);
        const auto* base = &p.model().baselines.all().front();
        const auto rows = scheduling::scheduleTable(p, in, r, base);
        const auto pages = reporting::writeGanttPdf(p, in, r, base, dir / "gantt.pdf");
        reporting::writeScheduleXlsx(p, rows, dir / "schedule.xlsx");
        reporting::writeXlsx(dir / "table.xlsx", {{"Quantities", {"Heading", "Metres", "Planned"}, {{std::string("DEC1"), 150.5, DateTime::fromDate(Date::fromYmd(2026, 12, 3))}, {std::string("XC1"), 12.0, reporting::Cell{}}}}});
        json out;
        out["pages"] = pages;
        json acts = json::array();
        for (const auto& row : rows)
            acts.push_back({{"name", row.name}, {"start", row.start}, {"finish", row.finish}, {"critical", row.critical}, {"duration", row.durationDays},
                            {"baselineFinish", row.baselineFinish}, {"kind", row.kind}});
        out["rows"] = acts;
        std::cout << out.dump() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
