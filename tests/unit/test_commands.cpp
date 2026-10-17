// SPDX-License-Identifier: Apache-2.0
#include "openmine/project/ProjectEditor.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace openmine;

namespace {
GeometryObject line(const Project& p, Vec3 a, Vec3 b) {
    GeometryObject g;
    g.type = GeometryType::Line;
    g.layerId = p.model().layers.all().front().id;
    g.vertices = {a, b};
    return g;
}
} // namespace

TEST(Commands, NewProjectHasDefaults) {
    auto p = Project::createNew("Test", Date::fromYmd(2026, 11, 2));
    EXPECT_EQ(p->model().layers.size(), 2u);
    EXPECT_EQ(p->model().calendars.size(), 3u);
    EXPECT_TRUE(p->model().calendars.contains(p->model().info.defaultCalendarId));
    EXPECT_FALSE(p->isModified());
}

TEST(Commands, UndoRedoCreateModifyDelete) {
    auto p = Project::createNew("Test", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    const Uuid id = ed.addGeometry(line(*p, {0, 0, 0}, {10, 0, 0}));
    EXPECT_TRUE(p->isModified());
    GeometryObject g = *p->model().geometry.find(id);
    EXPECT_EQ(g.meta.revision, 1);
    g.vertices[1] = {20, 0, 0};
    g.name = "Edited";
    ed.updateGeometry(g);
    EXPECT_EQ(p->model().geometry.find(id)->meta.revision, 2);
    ed.removeGeometry(id);
    EXPECT_FALSE(p->model().geometry.contains(id));

    p->commands().undo(); // delete
    ASSERT_TRUE(p->model().geometry.contains(id));
    EXPECT_EQ(p->model().geometry.find(id)->name, "Edited");
    p->commands().undo(); // modify
    EXPECT_DOUBLE_EQ(p->model().geometry.find(id)->vertices[1].x, 10.0);
    p->commands().undo(); // create
    EXPECT_FALSE(p->model().geometry.contains(id));
    EXPECT_FALSE(p->isModified());
    p->commands().redo();
    p->commands().redo();
    EXPECT_DOUBLE_EQ(p->model().geometry.find(id)->vertices[1].x, 20.0);
    EXPECT_EQ(p->model().geometry.find(id)->id, id); // identity is stable across undo/redo
}

TEST(Commands, UndoDeleteRestoresOriginalOrder) {
    auto p = Project::createNew("Test", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    Uuid a = ed.addGeometry(line(*p, {0, 0, 0}, {1, 0, 0}));
    Uuid b = ed.addGeometry(line(*p, {0, 0, 0}, {2, 0, 0}));
    Uuid c = ed.addGeometry(line(*p, {0, 0, 0}, {3, 0, 0}));
    ed.removeGeometry(b);
    p->commands().undo();
    const auto& all = p->model().geometry.all();
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].id, a);
    EXPECT_EQ(all[1].id, b);
    EXPECT_EQ(all[2].id, c);
}

TEST(Commands, ValidationFailureChangesNothing) {
    auto p = Project::createNew("Test", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    GeometryObject bad = line(*p, {0, 0, 0}, {1, 0, 0});
    bad.vertices.push_back({2, 0, 0}); // a Line with 3 vertices
    EXPECT_THROW(ed.addGeometry(bad), ValidationError);
    bad.vertices = {{0, 0, 0}, {std::nan(""), 0, 0}};
    EXPECT_THROW(ed.addGeometry(bad), ValidationError);
    EXPECT_EQ(p->commands().size(), 0u);
    EXPECT_TRUE(p->model().geometry.empty());
}

TEST(Commands, MacroIsOneUndoStepAndCancelRollsBack) {
    auto p = Project::createNew("Test", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    {
        CommandStack::MacroScope m(p->commands(), "Two lines");
        ed.addGeometry(line(*p, {0, 0, 0}, {1, 0, 0}));
        ed.addGeometry(line(*p, {0, 0, 0}, {2, 0, 0}));
        m.commit();
    }
    EXPECT_EQ(p->commands().size(), 1u);
    EXPECT_EQ(p->model().geometry.size(), 2u);
    p->commands().undo();
    EXPECT_EQ(p->model().geometry.size(), 0u);

    try {
        CommandStack::MacroScope m(p->commands(), "Fails half way");
        ed.addGeometry(line(*p, {0, 0, 0}, {1, 0, 0}));
        ed.addLayer("", 0); // throws
        m.commit();
    } catch (const ValidationError&) {
    }
    EXPECT_EQ(p->model().geometry.size(), 0u);
    EXPECT_FALSE(p->commands().inMacro());
}

TEST(Commands, DependencyValidation) {
    auto p = Project::createNew("Test", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    ScheduleActivity a;
    a.name = "A";
    ScheduleActivity b;
    b.name = "B";
    Uuid ia = ed.addActivity(a), ib = ed.addActivity(b);
    ed.addDependency({Uuid{}, ia, ib});
    EXPECT_THROW(ed.addDependency({Uuid{}, ib, ia}), ValidationError); // cycle
    EXPECT_THROW(ed.addDependency({Uuid{}, ia, ia}), ValidationError); // self
    EXPECT_THROW(ed.addDependency({Uuid{}, ia, ib}), ValidationError); // duplicate
    ed.removeActivity(ia); // cascades to the dependency
    EXPECT_TRUE(p->model().dependencies.empty());
    p->commands().undo();
    EXPECT_EQ(p->model().dependencies.size(), 1u);
}

TEST(Commands, AuditLogIsAppendOnly) {
    auto p = Project::createNew("Test", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    const auto before = p->model().audit.size();
    ed.addLayer("Survey", 0xFF0000);
    p->commands().undo();
    EXPECT_EQ(p->model().audit.size(), before + 2); // create + undo
    EXPECT_EQ(p->model().audit.back().action, "Undo");
}

TEST(Commands, CustomAttributesAreSchemaValidated) {
    auto p = Project::createNew("Test", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    AttributeDefinition def;
    def.entityType = "GeometryObject";
    def.key = "rock_class";
    def.displayName = "Rock class";
    def.valueType = AttributeValueType::Integer;
    ed.addAttributeDefinition(def);
    GeometryObject g = line(*p, {0, 0, 0}, {1, 0, 0});
    g.attributes["rock_class"] = "three";
    EXPECT_THROW(ed.addGeometry(g), ValidationError);
    g.attributes["rock_class"] = "3";
    EXPECT_NO_THROW(ed.addGeometry(g));
    g.id = Uuid{};
    g.attributes["undefined_key"] = "x";
    EXPECT_THROW(ed.addGeometry(g), ValidationError);
}
