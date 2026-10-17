// SPDX-License-Identifier: Apache-2.0
#include "openmine/omf/OmfProject.h"
#include "openmine/project/ProjectEditor.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace openmine;
using namespace openmine::omf;

namespace {
Document sample() {
    Document d;
    d.name = "t";
    d.origin = {10, 20, 30};
    Element p;
    p.kind = Element::Kind::PointSet;
    p.name = "pts";
    p.vertices = {{0, 0, 0}, {1, 2, 3}};
    Data s;
    s.kind = Data::Kind::Scalar;
    s.name = "au";
    s.location = "vertices";
    s.scalars = {1.5, std::nan("")};
    p.data.push_back(s);
    Data m;
    m.kind = Data::Kind::Mapped;
    m.name = "dom";
    m.location = "vertices";
    m.indices = {1, -1};
    Legend l;
    l.name = "names";
    l.strings = {"A", "B"};
    m.legends = {l};
    p.data.push_back(m);
    d.elements.push_back(p);
    Element v;
    v.kind = Element::Kind::Volume;
    v.name = "bm";
    v.tensorU = {2, 2};
    v.tensorV = {3};
    v.tensorW = {1, 1, 1};
    Data c;
    c.kind = Data::Kind::Scalar;
    c.name = "cu";
    c.location = "cells";
    c.scalars = {0, 1, 2, 3, 4, 5};
    v.data.push_back(c);
    d.elements.push_back(v);
    return d;
}
} // namespace

TEST(Omf, ByteRoundTripPreservesEverything) {
    const auto bytes = serializeOmf(sample());
    ASSERT_GT(bytes.size(), 60u);
    EXPECT_EQ(static_cast<unsigned char>(bytes[0]), 0x84u);
    EXPECT_EQ(bytes.substr(4, 10), "OMF-v0.9.0");
    const auto d = parseOmf(bytes);
    EXPECT_EQ(d.origin, (Vec3{10, 20, 30}));
    ASSERT_EQ(d.elements.size(), 2u);
    const auto& p = d.elements[0];
    EXPECT_EQ(p.vertices[1], (Vec3{1, 2, 3}));
    EXPECT_DOUBLE_EQ(p.data[0].scalars[0], 1.5);
    EXPECT_TRUE(std::isnan(p.data[0].scalars[1]));
    EXPECT_EQ(p.data[1].indices, (std::vector<std::int64_t>{1, -1}));
    EXPECT_EQ(p.data[1].legends[0].strings, (std::vector<std::string>{"A", "B"}));
    EXPECT_EQ(d.elements[1].tensorW.size(), 3u);
    EXPECT_EQ(d.elements[1].data[0].scalars, (std::vector<double>{0, 1, 2, 3, 4, 5}));
}

TEST(Omf, RejectsInvalidAndForeignFiles) {
    auto bad = sample();
    bad.elements[0].data[0].scalars.push_back(1); // 3 values for 2 vertices
    EXPECT_THROW(serializeOmf(bad), OmfError);
    bad = sample();
    bad.elements[0].data[1].indices = {2, 0}; // legend has 2 entries
    EXPECT_THROW(serializeOmf(bad), OmfError);
    bad = sample();
    bad.elements[1].tensorU = {2, -1};
    EXPECT_THROW(serializeOmf(bad), OmfError);
    EXPECT_THROW(parseOmf("PK\x03\x04 not an omf file at all, but long enough to have a header.........."), OmfError);
    auto bytes = serializeOmf(sample());
    auto v2 = bytes;
    v2[13] = '2'; // "OMF-v0.9.2"
    EXPECT_THROW(parseOmf(v2), OmfError);
    auto trunc = bytes.substr(0, 70);
    EXPECT_THROW(parseOmf(trunc), OmfError);
    auto corrupt = bytes;
    corrupt[62] = static_cast<char>(corrupt[62] ^ 0x5A); // inside the first compressed array
    EXPECT_THROW(parseOmf(corrupt), OmfError);
}

TEST(Omf, ChainSegmentsIntoPolylines) {
    // Open chain 0-1-2-3, a junction at 5 (three branches), and a closed loop 6-7-8.
    const std::vector<std::array<std::int64_t, 2>> s = {{0, 1}, {2, 1}, {2, 3}, {4, 5}, {5, 9}, {5, 10}, {6, 7}, {7, 8}, {8, 6}};
    const auto c = chainSegments(s, 11);
    ASSERT_EQ(c.size(), 5u);
    std::size_t total = 0, loops = 0;
    for (const auto& x : c) {
        total += x.size() - 1;
        if (x.front() == x.back()) ++loops;
    }
    EXPECT_EQ(total, s.size());
    EXPECT_EQ(loops, 1u);
    EXPECT_EQ(c[0], (std::vector<std::int64_t>{0, 1, 2, 3}));
}

TEST(Omf, ProjectExportImportRoundTrip) {
    auto pp = Project::createNew("OMF", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    GeometryObject g;
    g.name = "line";
    g.layerId = p.model().layers.all()[1].id;
    g.vertices = {{0, 0, 0}, {10, 0, 0}, {10, 10, 1}};
    ed.addGeometry(g);
    std::vector<std::string> notes;
    const auto doc = exportProjectOmf(p, {}, &notes);
    const auto back = parseOmf(serializeOmf(doc));
    auto qq = Project::createNew("In", Date::fromYmd(2026, 11, 2));
    const auto s = importOmf(*qq, back);
    ASSERT_EQ(s.geometry.size(), 1u);
    const auto& ig = *qq->model().geometry.find(s.geometry[0]);
    EXPECT_EQ(ig.vertices, g.vertices);
    EXPECT_EQ(ig.name, "line");
}
