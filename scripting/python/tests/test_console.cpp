// SPDX-License-Identifier: Apache-2.0
// The embedded console acts on the application's live project.
#include "PythonConsole.h"
#include "openmine/project/ProjectEditor.h"

#include <gtest/gtest.h>

using namespace openmine;
using namespace openmine::python;

TEST(PythonConsole, InteractiveLinesActOnTheLiveProjectWithUndo) {
    auto p = Project::createNew("Console", Date::fromYmd(2026, 11, 2));
    PythonConsole c(p.get());
    auto r = c.push("print(project.name)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.output, "Console\n");
    // Multi-line input.
    EXPECT_TRUE(c.push("for i in range(3):").needsMore);
    EXPECT_TRUE(c.push("    project.cad.add_point((i, 0, 0), name=f'P{i}')").needsMore);
    r = c.push("");
    EXPECT_FALSE(r.needsMore);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(p->model().geometry.size(), 3u); // the C++ project sees the change
    p->commands().undo();
    EXPECT_EQ(p->model().geometry.size(), 2u);
    // Errors are reported with a traceback, the console keeps working.
    r = c.push("project.cad.add_point((0,0,0), layer='no such layer')");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("no such layer"), std::string::npos);
    r = c.push("1 + 1");
    EXPECT_EQ(r.output, "2\n");
    // Completion.
    const auto comp = c.complete("project.sche");
    ASSERT_FALSE(comp.empty());
    EXPECT_EQ(comp.front(), "project.scheduler");
}

TEST(PythonConsole, ScriptsAndRebinding) {
    auto a = Project::createNew("A", Date::fromYmd(2026, 11, 2));
    auto b = Project::createNew("B", Date::fromYmd(2026, 11, 2));
    PythonConsole c(a.get());
    auto r = c.run("import openmine\np = openmine.current_project()\nwith p.edit('Script'):\n    p.cad.add_layer('Scripted')\nprint(len(p.cad.layers()))\n");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.output, "3\n");
    EXPECT_EQ(a->commands().undoText(), "Script");
    c.setProject(b.get());
    EXPECT_EQ(c.push("print(project.name)").output, "B\n");
    c.setProject(nullptr);
    EXPECT_EQ(c.push("print(project)").output, "None\n");
    r = c.run("raise ValueError('boom')", "bad.py");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("boom"), std::string::npos);
    EXPECT_NE(r.error.find("bad.py"), std::string::npos);
    EXPECT_FALSE(PythonConsole::pythonVersion().empty());
}

TEST(PythonConsole, StreamsOutputPumpsAndInterrupts) {
    auto p = Project::createNew("Stream", Date::fromYmd(2026, 11, 2));
    PythonConsole c(p.get());
    std::string streamed;
    c.setOutputSink([&](int stream, const std::string& t) {
        if (stream == 0) streamed += t;
    });
    int pumps = 0;
    bool runningInPump = false;
    c.setPump([&] {
        ++pumps;
        runningInPump = PythonConsole::isRunning();
        // Interrupt a runaway loop from the pump (as the application's Interrupt button does).
        if (pumps == 5) PythonConsole::interrupt();
    }, 20);
    auto r = c.run("print('start')\nwhile True:\n    pass\n", "loop.py");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("KeyboardInterrupt"), std::string::npos) << r.error;
    EXPECT_EQ(streamed.substr(0, 6), "start\n"); // seen while running, not only at the end
    EXPECT_GE(pumps, 5);
    EXPECT_TRUE(runningInPump);
    EXPECT_FALSE(PythonConsole::isRunning());
    // The console still works afterwards.
    streamed.clear();
    r = c.push("print(project.name)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(streamed, "Stream\n");
}
