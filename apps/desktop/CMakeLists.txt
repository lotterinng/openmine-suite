# OpenMine Suite desktop application (Qt 6 + VTK).
#
# VTK is used without its Qt module (GUISupportQt): distributions often build that module against
# Qt 5, so the application carries its own QOpenGLWidget adapter (vtk/QtVtkWidget). VTK's MPI
# dependency (when VTK was built with MPI) needs the C language enabled before find_package(VTK).
enable_language(C)
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGLWidgets Svg PrintSupport)
find_package(Qt6 6.4 QUIET COMPONENTS PdfWidgets)
find_package(VTK REQUIRED COMPONENTS
    CommonCore CommonDataModel CommonTransforms FiltersCore FiltersGeneral FiltersSources
    InteractionStyle InteractionWidgets RenderingAnnotation RenderingCore RenderingFreeType
    RenderingOpenGL2 RenderingLabel IOImage)

find_package(nlohmann_json 3.10 REQUIRED)
set(CMAKE_AUTOMOC ON)

set(OPENMINE_DESKTOP_SOURCES
    app/AppContext.cpp app/AppContext.h
    app/JobManager.cpp app/JobManager.h
    vtk/QtVtkWidget.cpp vtk/QtVtkWidget.h
    viewport/SceneTypes.cpp viewport/SceneTypes.h
    viewport/SceneBuilder.cpp viewport/SceneBuilder.h
    viewport/SceneCache.cpp viewport/SceneCache.h
    viewport/PointCloudLod.cpp viewport/PointCloudLod.h
    viewport/Viewport3D.cpp viewport/Viewport3D.h
    viewport/Tools.cpp viewport/Tools.h
    viewport/ViewportArea.cpp viewport/ViewportArea.h
    theme/Theme.cpp theme/Theme.h theme/Icons.cpp
    app/EntityInfo.cpp app/EntityInfo.h
    panels/PropertiesPanel.cpp panels/PropertiesPanel.h
    panels/ProjectExplorer.cpp panels/ProjectExplorer.h
    panels/LayerManager.cpp panels/LayerManager.h
    panels/JobsPanel.cpp panels/JobsPanel.h
    panels/PythonConsolePanel.cpp panels/PythonConsolePanel.h
    panels/PlaybackPanel.cpp panels/PlaybackPanel.h
    panels/StereonetPanel.cpp panels/StereonetPanel.h
    panels/ReportsPanel.cpp panels/ReportsPanel.h
    scheduler/GanttChart.cpp scheduler/GanttChart.h
    scheduler/ScheduleCharts.cpp scheduler/ScheduleCharts.h
    scheduler/ScheduleDialogs.cpp scheduler/ScheduleDialogs.h
    scheduler/SchedulerPanel.cpp scheduler/SchedulerPanel.h
    dialogs/DialogUtil.cpp dialogs/DialogUtil.h
    dialogs/SurveyDialogs.cpp dialogs/SurveyDialogs.h
    dialogs/DesignDialogs.cpp dialogs/DesignDialogs.h
    dialogs/GeologyDialogs.cpp dialogs/GeologyDialogs.h
    dialogs/DataDialogs.cpp dialogs/DataDialogs.h
    dialogs/ModelDialogs.cpp dialogs/ModelDialogs.h
    dialogs/MeshDialogs.cpp dialogs/MeshDialogs.h
    dialogs/SettingsDialogs.cpp dialogs/SettingsDialogs.h
    scheduler/LibraryDialogs.cpp scheduler/LibraryDialogs.h
    console/CommandConsole.cpp console/CommandConsole.h
    console/StandardCommands.cpp
    ribbon/Ribbon.cpp ribbon/Ribbon.h
    MainWindow.cpp MainWindow.h MainWindowActions.cpp
    SmokeTest.cpp SmokeTest.h
)

add_library(openmine_desktop_lib STATIC ${OPENMINE_DESKTOP_SOURCES})
target_include_directories(openmine_desktop_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openmine_desktop_lib PUBLIC
    openmine_core openmine_cad openmine_scheduling openmine_mine_design openmine_planning openmine_survey
    openmine_geology openmine_geotechnical openmine_reporting openmine_survey_reports openmine_formats openmine_omf
    openmine_plugin_host
    nlohmann_json::nlohmann_json Qt6::Widgets Qt6::OpenGLWidgets Qt6::Svg Qt6::PrintSupport ${VTK_LIBRARIES})
if(TARGET openmine_occt)
    target_link_libraries(openmine_desktop_lib PUBLIC openmine_occt)
    target_compile_definitions(openmine_desktop_lib PUBLIC OPENMINE_APP_HAVE_OCCT)
endif()
foreach(opt pdal open3d crs gis optimisation repository)
    if(TARGET openmine_${opt})
        target_link_libraries(openmine_desktop_lib PUBLIC openmine_${opt})
        string(TOUPPER ${opt} OPTU)
        target_compile_definitions(openmine_desktop_lib PUBLIC OPENMINE_APP_HAVE_${OPTU})
    endif()
endforeach()
if(TARGET openmine_python_embed)
    target_link_libraries(openmine_desktop_lib PUBLIC openmine_python_embed)
    target_compile_definitions(openmine_desktop_lib PUBLIC OPENMINE_APP_HAVE_PYTHON)
endif()
if(TARGET Qt6::PdfWidgets)
    target_link_libraries(openmine_desktop_lib PUBLIC Qt6::PdfWidgets)
    target_compile_definitions(openmine_desktop_lib PUBLIC OPENMINE_APP_HAVE_QTPDF)
endif()
vtk_module_autoinit(TARGETS openmine_desktop_lib MODULES ${VTK_LIBRARIES})
openmine_set_warnings(openmine_desktop_lib)

if(WIN32)
    # Icon and version information of the .exe.
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/packaging/windows/openmine-suite.ico" OPENMINE_ICON)
    configure_file(openmine-desktop.rc.in ${CMAKE_CURRENT_BINARY_DIR}/openmine-desktop.rc @ONLY)
    set(OPENMINE_DESKTOP_RC ${CMAKE_CURRENT_BINARY_DIR}/openmine-desktop.rc)
endif()
add_executable(openmine-desktop WIN32 main.cpp ${OPENMINE_DESKTOP_RC})
target_link_libraries(openmine-desktop PRIVATE openmine_desktop_lib)
openmine_set_warnings(openmine-desktop)
set_target_properties(openmine-desktop PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
install(TARGETS openmine-desktop RUNTIME DESTINATION bin)

if(OPENMINE_BUILD_TESTS)
    # Headless run on Linux needs a display (xvfb-run in CI); Windows/macOS use the desktop session.
    find_program(XVFB_RUN xvfb-run)
    if(XVFB_RUN)
        set(_smoke_launcher ${XVFB_RUN} -a -s "-screen 0 1920x1080x24")
    endif()
    add_test(NAME gui_smoke_test
             COMMAND ${_smoke_launcher} $<TARGET_FILE:openmine-desktop> --smoke-test ${CMAKE_BINARY_DIR}/gui-smoke)
    set(_smoke_env "OPENMINE_PLUGIN_PATH=${CMAKE_BINARY_DIR}/plugins")
    if(UNIX AND NOT APPLE)
        list(APPEND _smoke_env "QT_QPA_PLATFORM=xcb" "LIBGL_ALWAYS_SOFTWARE=1")
    endif()
    set_tests_properties(gui_smoke_test PROPERTIES TIMEOUT 1800 LABELS "gui" ENVIRONMENT "${_smoke_env}")
    # docs/console-commands.md lists every console command of a build with PROJ (CRS is the only
    # conditional command).
    if(TARGET openmine_crs)
        add_test(NAME console_reference_up_to_date
                 COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:openmine-desktop> -DDOC=${CMAKE_SOURCE_DIR}/docs/console-commands.md
                         -DOUT=${CMAKE_BINARY_DIR}/console-commands.generated.md -P ${CMAKE_SOURCE_DIR}/tests/tools/check_command_reference.cmake)
        set_tests_properties(console_reference_up_to_date PROPERTIES LABELS "docs" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
