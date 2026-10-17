// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Annotation.h"
#include "openmine/cad/Edit.h"
#include "openmine/formats/Dxf.h"
#include "openmine/formats/DxfProject.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/project/ProjectEditor.h"
#include "../support/DxfSample.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <numbers>

using namespace openmine;
using namespace openmine::formats;
using geometry::curveLength;
using openmine::testing::Drawing;
using openmine::testing::sampleDrawing;

namespace {
constexpr double kPi = std::numbers::pi;

void expectNear(Vec3 a, Vec3 b, double tol = 1e-9) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

const GeometryObject* byName(const DxfImportResult& r, const std::string& n) {
    for (const auto& o : r.objects)
        if (o.name == n) return &o;
    return nullptr;
}
const CadLayer* layerOf(const DxfImportResult& r, const GeometryObject& g) {
    for (const auto& l : r.layers)
        if (l.id == g.layerId) return &l;
    return nullptr;
}

std::string minimalDxf(const std::string& header, const std::string& tables, const std::string& blocks, const std::string& entities) {
    return "0\nSECTION\n2\nHEADER\n" + header + "0\nENDSEC\n0\nSECTION\n2\nTABLES\n" + tables + "0\nENDSEC\n0\nSECTION\n2\nBLOCKS\n" +
           blocks + "0\nENDSEC\n0\nSECTION\n2\nENTITIES\n" + entities + "0\nENDSEC\n0\nEOF\n";
}
} // namespace

TEST(Dxf, AciPaletteAndUnits) {
    EXPECT_EQ(aciToRgb(1), 0xFF0000u);
    EXPECT_EQ(aciToRgb(7), 0xFFFFFFu);
    EXPECT_EQ(rgbToAci(0xFF0000), 1);
    EXPECT_EQ(rgbToAci(0x0000FF), 5);
    EXPECT_DOUBLE_EQ(metresPerInsUnit(4), 0.001);
    EXPECT_DOUBLE_EQ(metresPerInsUnit(2), 0.3048);
    EXPECT_DOUBLE_EQ(metresPerInsUnit(0), 0.0);
}

TEST(Dxf, RoundTripEveryGeometryType) {
    const auto d = sampleDrawing();
    DxfExportReport rep;
    const std::string text = writeDxfString(d.layers, d.ptrs(), {}, &rep);
    EXPECT_EQ(rep.skipped, 0);
    EXPECT_EQ(rep.written["DIMENSION"], 6);
    EXPECT_EQ(rep.written["HATCH"], 4);
    EXPECT_EQ(rep.written["SPLINE"], 2);
    EXPECT_FALSE(rep.warnings.empty()); // the 3D bulged polyline was tessellated

    const auto r = readDxfString(text);
    EXPECT_EQ(r.acadVersion, "AC1018");
    EXPECT_EQ(r.insUnits, 6);
    EXPECT_TRUE(r.skipped.empty()) << (r.warnings.empty() ? "" : r.warnings.front());
    ASSERT_EQ(r.objects.size(), d.objects.size());

    // Layers: names sanitised, colours, visibility, lock, line type, transparency, width, description.
    const auto* line = byName(r, "line");
    ASSERT_TRUE(line);
    const auto* la = layerOf(r, *line);
    ASSERT_TRUE(la);
    EXPECT_EQ(la->name, "Design_Centrelines");
    EXPECT_EQ(la->colorRgb, 0x12AB34u);
    EXPECT_EQ(la->lineType, "Dashed");
    EXPECT_NEAR(la->transparency, 0.4, 0.5 / 255);
    EXPECT_DOUBLE_EQ(la->lineWidth, 3.5);
    EXPECT_EQ(la->description, "Décline centrelines");
    const auto* lb = layerOf(r, *byName(r, "station"));
    EXPECT_FALSE(lb->visible);
    EXPECT_TRUE(lb->locked);

    for (const auto& orig : d.objects) {
        SCOPED_TRACE(orig.name);
        const auto* g = byName(r, orig.name);
        ASSERT_TRUE(g);
        if (orig.name == "3d bulged") {
            EXPECT_EQ(g->type, GeometryType::Polyline);
            EXPECT_NEAR(geometry::length3d(g->vertices, false), geometry::length3dBulged(orig.vertices, orig.bulges, false), 1e-3);
            continue;
        }
        EXPECT_EQ(g->type, orig.type);
        EXPECT_EQ(g->colorOverride, orig.colorOverride);
        switch (orig.type) {
        case GeometryType::Point:
        case GeometryType::Line:
        case GeometryType::Polyline:
        case GeometryType::Spline:
            ASSERT_EQ(g->vertices.size(), orig.vertices.size());
            for (std::size_t i = 0; i < orig.vertices.size(); ++i) expectNear(g->vertices[i], orig.vertices[i], 1e-9);
            EXPECT_EQ(g->closed, orig.closed);
            if (!orig.bulges.empty()) {
                ASSERT_EQ(g->bulges.size(), orig.bulges.size());
                for (std::size_t i = 0; i < orig.bulges.size(); ++i) EXPECT_NEAR(g->bulges[i], orig.bulges[i], 1e-12);
            }
            break;
        case GeometryType::Arc:
        case GeometryType::Circle: {
            EXPECT_NEAR(curveLength(*g), curveLength(orig), 1e-9);
            expectNear(g->arc.center, orig.arc.center);
            expectNear(*geometry::curveStart(*g), *geometry::curveStart(orig), 1e-9);
            expectNear(*geometry::curveEnd(*g), *geometry::curveEnd(orig), 1e-9);
            break;
        }
        case GeometryType::Text:
            expectNear(g->vertices[0], orig.vertices[0]);
            EXPECT_EQ(g->text.text, orig.text.text);
            EXPECT_DOUBLE_EQ(g->text.height, orig.text.height);
            EXPECT_NEAR(g->text.rotation, orig.text.rotation, 1e-12);
            EXPECT_EQ(g->text.align, orig.text.align);
            break;
        case GeometryType::Dimension:
            EXPECT_EQ(g->dimension.kind, orig.dimension.kind);
            EXPECT_EQ(g->dimension.precision, orig.dimension.precision);
            EXPECT_DOUBLE_EQ(g->dimension.textHeight, orig.dimension.textHeight);
            EXPECT_NEAR(cad::dimensionGraphics(*g).value, cad::dimensionGraphics(orig).value, 1e-9);
            EXPECT_EQ(cad::dimensionGraphics(*g).text, cad::dimensionGraphics(orig).text);
            break;
        case GeometryType::Hatch:
            EXPECT_EQ(g->hatch.pattern, orig.hatch.pattern);
            EXPECT_NEAR(cad::hatchArea(*g), cad::hatchArea(orig), 1e-9);
            if (orig.hatch.pattern != "SOLID") {
                EXPECT_NEAR(g->hatch.spacing, orig.hatch.spacing, 1e-9);
                EXPECT_NEAR(g->hatch.angle, orig.hatch.angle, 1e-9);
            }
            EXPECT_EQ(g->hatch.loopStarts, orig.hatch.loopStarts);
            break;
        case GeometryType::Mesh:
            ASSERT_TRUE(g->mesh);
            EXPECT_EQ(g->mesh->vertices, orig.mesh->vertices);
            EXPECT_EQ(g->mesh->triangles, orig.mesh->triangles);
            EXPECT_NEAR(*g->transparency, *orig.transparency, 0.5 / 255);
            break;
        default: break;
        }
    }
}

TEST(Dxf, FileRoundTripThroughProject) {
    auto pp = Project::createNew("DXF", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid layer = ed.addLayer("Imported", 0x00FF00);
    GeometryObject g = cad::makePolyline({{0, 0, 0}, {10, 0, 0}, {10, 10, 0}}, false);
    g.layerId = layer;
    g.name = "centreline";
    ed.addGeometry(g);
    const auto dir = std::filesystem::temp_directory_path() / ("openmine_dxf_" + Uuid::generate().toString());
    std::filesystem::create_directories(dir);
    const auto file = dir / "out.dxf";
    exportProjectDxf(p, file);
    const auto r = readDxf(file);
    ASSERT_EQ(r.objects.size(), 1u);

    auto qq = Project::createNew("Target", Date::fromYmd(2026, 11, 2));
    Project& q = *qq;
    ProjectEditor eq(q);
    eq.addLayer("Imported", 0x0000FF); // existing layer is reused, not altered
    const std::size_t before = q.model().geometry.size();
    const auto s = importDxfIntoProject(q, r);
    EXPECT_EQ(s.objects.size(), 1u);
    EXPECT_EQ(s.reusedLayers.size(), 1u);
    EXPECT_EQ(q.model().geometry.size(), before + 1);
    EXPECT_EQ(eq.findLayerByName("Imported")->colorRgb, 0x0000FFu);
    q.commands().undo(); // one undo step removes the whole import
    EXPECT_EQ(q.model().geometry.size(), before);
    std::filesystem::remove_all(dir);
}

TEST(Dxf, UnitsOcsAndLegacyPolylines) {
    // R12-style file: millimetres, LWPOLYLINE with -Z extrusion, 2D POLYLINE with bulge, ARC in OCS.
    const std::string ents =
        "0\nLWPOLYLINE\n8\nA\n90\n2\n70\n0\n38\n500\n10\n1000\n20\n0\n42\n1.0\n10\n2000\n20\n0\n210\n0\n220\n0\n230\n-1\n"
        "0\nPOLYLINE\n8\nA\n66\n1\n10\n0\n20\n0\n30\n-2000\n70\n1\n"
        "0\nVERTEX\n8\nA\n10\n0\n20\n0\n42\n0\n0\nVERTEX\n8\nA\n10\n4000\n20\n0\n42\n0\n0\nVERTEX\n8\nA\n10\n4000\n20\n3000\n0\nSEQEND\n"
        "0\nARC\n8\nA\n10\n1000\n20\n0\n30\n0\n40\n1000\n50\n0\n51\n90\n210\n0\n220\n0\n230\n-1\n"
        "0\nCUSTOMTHING\n8\nA\n";
    const auto r = readDxfString(minimalDxf("9\n$INSUNITS\n70\n4\n", "", "", ents));
    EXPECT_DOUBLE_EQ(r.unitScale, 0.001);
    ASSERT_EQ(r.objects.size(), 3u);
    // LWPOLYLINE in OCS -Z: x is mirrored, elevation 500 mm along -Z → z = -0.5 m; bulge sign flips.
    const auto& lw = r.objects[0];
    expectNear(lw.vertices[0], {-1.0, 0, -0.5});
    expectNear(lw.vertices[1], {-2.0, 0, -0.5});
    ASSERT_EQ(lw.bulges.size(), 2u);
    EXPECT_DOUBLE_EQ(lw.bulges[0], -1.0);
    EXPECT_NEAR(geometry::length3dBulged(lw.vertices, lw.bulges, false), kPi * 0.5, 1e-9);
    // Closed 2D POLYLINE at elevation -2 m: triangle 4 × 3 m.
    const auto& pl = r.objects[1];
    EXPECT_TRUE(pl.closed);
    EXPECT_NEAR(geometry::length3d(pl.vertices, true), 12.0, 1e-9);
    EXPECT_DOUBLE_EQ(pl.vertices[2].z, -2.0);
    // ARC with -Z extrusion: centre (−1, 0, 0) m, quarter circle radius 1 m.
    const auto& arc = r.objects[2];
    expectNear(arc.arc.center, {-1, 0, 0});
    EXPECT_NEAR(curveLength(arc), kPi / 2, 1e-9);
    expectNear(*geometry::curveStart(arc), {-2, 0, 0}, 1e-9);
    expectNear(*geometry::curveEnd(arc), {-1, 1, 0}, 1e-9);
    EXPECT_EQ(r.skipped.at("CUSTOMTHING"), 1);
    EXPECT_FALSE(r.warnings.empty());
}

TEST(Dxf, BlockInsertsNestedArraysAndByBlockColour) {
    const std::string tables = "0\nTABLE\n2\nLAYER\n0\nLAYER\n2\nWALLS\n70\n0\n62\n3\n6\nCONTINUOUS\n0\nENDTAB\n";
    const std::string blocks =
        "0\nBLOCK\n8\n0\n2\nUNIT\n70\n0\n10\n1\n20\n1\n30\n0\n"
        "0\nLINE\n8\n0\n62\n0\n10\n1\n20\n1\n30\n0\n11\n2\n21\n1\n31\n0\n"
        "0\nCIRCLE\n8\nOTHER\n10\n1\n20\n1\n30\n0\n40\n0.5\n"
        "0\nENDBLK\n"
        "0\nBLOCK\n8\n0\n2\nPAIR\n70\n0\n10\n0\n20\n0\n30\n0\n"
        "0\nINSERT\n8\n0\n2\nUNIT\n10\n0\n20\n0\n30\n0\n"
        "0\nINSERT\n8\n0\n2\nUNIT\n10\n10\n20\n0\n30\n0\n"
        "0\nENDBLK\n";
    const std::string ents =
        "0\nINSERT\n8\nWALLS\n62\n1\n2\nUNIT\n10\n100\n20\n200\n30\n5\n41\n2\n42\n2\n43\n2\n50\n90\n"
        "0\nINSERT\n8\nWALLS\n2\nPAIR\n10\n0\n20\n0\n30\n0\n"
        "0\nINSERT\n8\nWALLS\n2\nUNIT\n10\n0\n20\n50\n30\n0\n70\n3\n71\n2\n44\n5\n45\n7\n"
        "0\nINSERT\n8\nWALLS\n2\nUNIT\n10\n0\n20\n0\n30\n0\n41\n2\n42\n1\n43\n1\n";
    DxfImportOptions opt;
    opt.chordTolerance = 1e-5;
    const auto r = readDxfString(minimalDxf("", tables, blocks, ents), opt);
    // 1 + 2 + 6 + 1 inserts of UNIT → 10 lines and 10 circles (the last one becomes an ellipse polyline).
    int lines = 0, circles = 0, polys = 0;
    for (const auto& o : r.objects) {
        if (o.type == GeometryType::Line) ++lines;
        if (o.type == GeometryType::Circle) ++circles;
        if (o.type == GeometryType::Polyline) ++polys;
    }
    EXPECT_EQ(lines, 10);
    EXPECT_EQ(circles, 9);
    EXPECT_EQ(polys, 1);
    // First insert: base (1,1) → insertion (100,200,5), scale 2, rotation 90°. Line (1,1)→(2,1) becomes (100,200)→(100,202).
    const auto& first = r.objects[0];
    ASSERT_EQ(first.type, GeometryType::Line);
    expectNear(first.vertices[0], {100, 200, 5});
    expectNear(first.vertices[1], {100, 202, 5});
    EXPECT_EQ(first.colorOverride, std::optional<std::uint32_t>(0xFF0000)); // BYBLOCK → insert colour (ACI 1)
    const auto* wl = layerOf(r, first);
    EXPECT_EQ(wl->name, "WALLS"); // layer 0 inherits the insert layer
    EXPECT_EQ(wl->colorRgb, aciToRgb(3));
    EXPECT_EQ(layerOf(r, r.objects[1])->name, "OTHER");
    EXPECT_NEAR(r.objects[1].arc.radius, 1.0, 1e-12);
    // Nested: second line of PAIR is at x+10.
    expectNear(r.objects[4].vertices[0], {10, 0, 0});
    // MINSERT array: column spacing 5, row spacing 7.
    expectNear(r.objects[6 + 2 * 5].vertices[0], {10, 57, 0}); // row 1, col 2
    // Non-uniform scale turns the circle into a closed polyline ellipse of semi-axes 1 × 0.5.
    const auto& ell = r.objects.back();
    EXPECT_EQ(ell.type, GeometryType::Polyline);
    EXPECT_TRUE(ell.closed);
    const double h = std::pow((1 - 0.5) / (1 + 0.5), 2);
    const double ramanujan = kPi * (1 + 0.5) * (1 + 3 * h / (10 + std::sqrt(4 - 3 * h)));
    EXPECT_NEAR(geometry::length3d(ell.vertices, true), ramanujan, 1e-4);
}

TEST(Dxf, FacesMeshesSplinesTextAndHatchEdges) {
    const std::string ents =
        // Two 3DFACEs sharing an edge are merged into one mesh with 4 vertices.
        "0\n3DFACE\n8\nDTM\n10\n0\n20\n0\n30\n0\n11\n1\n21\n0\n31\n0\n12\n1\n22\n1\n32\n1\n13\n1\n23\n1\n33\n1\n"
        "0\n3DFACE\n8\nDTM\n10\n0\n20\n0\n30\n0\n11\n1\n21\n1\n31\n1\n12\n0\n22\n1\n32\n0\n13\n0\n23\n1\n33\n0\n"
        // Clamped cubic B-spline with control points only (a single Bézier span).
        "0\nSPLINE\n8\nS\n70\n8\n71\n3\n72\n8\n73\n4\n40\n0\n40\n0\n40\n0\n40\n0\n40\n1\n40\n1\n40\n1\n40\n1\n"
        "10\n0\n20\n0\n30\n0\n10\n1\n20\n2\n30\n0\n10\n3\n20\n2\n30\n0\n10\n4\n20\n0\n30\n0\n"
        // MTEXT with formatting and a Unicode escape.
        "0\nMTEXT\n8\nT\n10\n1\n20\n2\n30\n0\n40\n2.5\n71\n5\n1\n{\\fArial|b1;Stope \\U+00C9}\\PLevel 2\n"
        // HATCH with an edge path: line, CCW arc, line, line (a 2 × 2 square with a semicircular bump).
        "0\nHATCH\n8\nH\n10\n0\n20\n0\n30\n0\n2\nSOLID\n70\n1\n71\n0\n91\n1\n92\n1\n93\n3\n"
        "72\n1\n10\n0\n20\n0\n11\n2\n21\n0\n"
        "72\n2\n10\n2\n20\n1\n40\n1\n50\n270\n51\n90\n73\n1\n"
        "72\n1\n10\n2\n20\n2\n11\n0\n21\n0\n"
        "97\n0\n75\n0\n76\n1\n98\n0\n";
    const auto r = readDxfString(minimalDxf("", "", "", ents));
    ASSERT_EQ(r.objects.size(), 4u) << (r.warnings.empty() ? "" : r.warnings.back());
    const GeometryObject* mesh = nullptr;
    for (const auto& o : r.objects)
        if (o.type == GeometryType::Mesh) mesh = &o;
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->mesh->vertices.size(), 4u);
    EXPECT_EQ(mesh->mesh->triangles.size(), 2u);
    EXPECT_EQ(r.imported.at("3DFACE"), 2);
    // Spline → polyline through the Bézier: endpoints and mid point B(0.5) = (2, 1.5).
    const auto& sp = r.objects[0];
    EXPECT_EQ(sp.type, GeometryType::Polyline);
    expectNear(sp.vertices.front(), {0, 0, 0});
    expectNear(sp.vertices.back(), {4, 0, 0});
    double best = 1e9;
    for (const auto& v : sp.vertices) best = std::min(best, distance(v, {2, 1.5, 0}));
    EXPECT_LT(best, 0.05);
    const auto& tx = r.objects[1];
    EXPECT_EQ(tx.text.text, "Stope \xC3\x89\nLevel 2");
    EXPECT_EQ(tx.text.align, TextAlign::Center);
    // Hatch area: triangle (0,0)-(2,0)-(2,2) with … the third edge closes back to the start: area 2 + half disc.
    const auto& h = r.objects[2];
    EXPECT_EQ(h.hatch.pattern, "SOLID");
    EXPECT_NEAR(cad::hatchArea(h), 2.0 + kPi / 2, 2e-3);
}

TEST(Dxf, ErrorsAreReported) {
    EXPECT_THROW(readDxfString("0\nSECTION\n2\nENTITIES\nabc\nLINE\n"), DxfError);
    EXPECT_THROW(readDxfString(std::string("AutoCAD Binary DXF\r\n\x1a\0", 22)), DxfError);
    EXPECT_THROW(readDxf("/nonexistent/file.dxf"), DxfError);
    // A reference to an undefined block is skipped with a warning, not a crash.
    const auto r = readDxfString(minimalDxf("", "", "", "0\nINSERT\n8\n0\n2\nMISSING\n10\n0\n20\n0\n30\n0\n"));
    EXPECT_TRUE(r.objects.empty());
    EXPECT_EQ(r.skipped.at("INSERT"), 1);
    // Recursive blocks are stopped.
    const auto rec = readDxfString(minimalDxf("", "", "0\nBLOCK\n2\nSELF\n10\n0\n20\n0\n30\n0\n0\nINSERT\n8\n0\n2\nSELF\n10\n0\n20\n0\n30\n0\n0\nENDBLK\n", "0\nINSERT\n8\n0\n2\nSELF\n10\n0\n20\n0\n30\n0\n"));
    EXPECT_TRUE(rec.objects.empty());
    EXPECT_GE(rec.skipped.at("INSERT"), 1);
}
