find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGLWidgets)
find_package(VTK REQUIRED COMPONENTS
    CommonCore CommonDataModel FiltersSources GUISupportQt InteractionStyle InteractionWidgets
    RenderingAnnotation RenderingCore RenderingFreeType RenderingOpenGL2)

set(CMAKE_AUTOMOC ON)

add_executable(openmine-desktop WIN32
    main.cpp
    AppContext.cpp AppContext.h
    MainWindow.cpp MainWindow.h MainWindowActions.h
    SmokeTest.cpp SmokeTest.h
    theme/Theme.cpp theme/Theme.h
    ribbon/Ribbon.cpp ribbon/Ribbon.h
    viewport/Viewport3D.cpp viewport/Viewport3D.h
    project_explorer/ProjectExplorer.cpp project_explorer/ProjectExplorer.h
    properties/PropertiesPanel.cpp properties/PropertiesPanel.h
    console/CommandConsole.cpp console/CommandConsole.h console/StandardCommands.cpp
    scheduler/SchedulerPanel.cpp scheduler/SchedulerPanel.h
)
target_link_libraries(openmine-desktop PRIVATE
    openmine_core openmine_scheduling openmine_mine_design
    Qt6::Widgets Qt6::OpenGLWidgets ${VTK_LIBRARIES})
if(TARGET openmine_occt)
    target_link_libraries(openmine-desktop PRIVATE openmine_occt)
endif()
vtk_module_autoinit(TARGETS openmine-desktop MODULES ${VTK_LIBRARIES})
openmine_set_warnings(openmine-desktop)

if(OPENMINE_BUILD_TESTS)
    # GUI smoke test: needs a display (on Linux CI use xvfb-run).
    add_test(NAME gui_smoke_test COMMAND openmine-desktop --smoke-test ${CMAKE_BINARY_DIR}/smoke)
    set_tests_properties(gui_smoke_test PROPERTIES LABELS gui TIMEOUT 180)
endif()
