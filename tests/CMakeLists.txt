find_package(GTest REQUIRED)
find_package(SQLite3 REQUIRED)
include(GoogleTest)

add_executable(openmine_unit_tests
    unit/test_core.cpp
    unit/test_geometry.cpp
    unit/test_infrastructure.cpp
    unit/test_commands.cpp
    unit/test_persistence.cpp
    unit/test_scheduling.cpp
    unit/test_mine_design.cpp
    unit/test_cad.cpp
    unit/test_dxf.cpp
    unit/test_mesh_formats.cpp
    unit/test_survey_io.cpp
    unit/test_survey_processing.cpp
    unit/test_planning.cpp
    unit/test_geology.cpp
    unit/test_omf.cpp
    unit/test_geotechnical.cpp
    unit/test_reporting.cpp
)
target_link_libraries(openmine_unit_tests PRIVATE
    openmine_reporting openmine_core openmine_scheduling openmine_mine_design openmine_cad openmine_formats openmine_survey openmine_planning openmine_geology openmine_geotechnical openmine_omf SQLite::SQLite3 GTest::gtest_main)
gtest_discover_tests(openmine_unit_tests DISCOVERY_TIMEOUT 60)

if(TARGET openmine_occt)
    add_executable(openmine_occt_tests unit/test_occt.cpp)
    target_link_libraries(openmine_occt_tests PRIVATE openmine_occt openmine_cad openmine_mine_design GTest::gtest_main)
    gtest_discover_tests(openmine_occt_tests DISCOVERY_TIMEOUT 60)
endif()

# DXF validation against ezdxf (independent implementation). Skipped by pytest if ezdxf is missing.
find_package(nlohmann_json 3.10 REQUIRED)
add_executable(dxf_tool tools/dxf_tool.cpp)
target_link_libraries(dxf_tool PRIVATE openmine_formats openmine_cad nlohmann_json::nlohmann_json)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME dxf_ezdxf_validation
             COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider --basetemp=${CMAKE_CURRENT_BINARY_DIR}/pytest-test_dxf_ezdxf ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_dxf_ezdxf.py)
    set_tests_properties(dxf_ezdxf_validation PROPERTIES ENVIRONMENT "OPENMINE_DXF_TOOL=$<TARGET_FILE:dxf_tool>" LABELS integration)
endif()

if(TARGET openmine_pdal)
    add_executable(openmine_pdal_tests unit/test_pdal.cpp)
    target_link_libraries(openmine_pdal_tests PRIVATE openmine_pdal openmine_survey GTest::gtest_main)
    gtest_discover_tests(openmine_pdal_tests DISCOVERY_TIMEOUT 60 PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=/opt/pdal/lib")
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME mesh_trimesh_validation
             COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider --basetemp=${CMAKE_CURRENT_BINARY_DIR}/pytest-test_mesh_trimesh ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_mesh_trimesh.py)
    set_tests_properties(mesh_trimesh_validation PROPERTIES ENVIRONMENT "OPENMINE_DXF_TOOL=$<TARGET_FILE:dxf_tool>" LABELS integration)
endif()

if(TARGET openmine_crs)
    add_executable(openmine_crs_tests unit/test_crs.cpp)
    target_link_libraries(openmine_crs_tests PRIVATE openmine_crs openmine_cad GTest::gtest_main)
    gtest_discover_tests(openmine_crs_tests DISCOVERY_TIMEOUT 60)
endif()

if(TARGET openmine_gis)
    add_executable(openmine_gis_tests unit/test_gis.cpp)
    # GDAL directly as well: the KML test checks the transformation against OGR independently.
    target_link_libraries(openmine_gis_tests PRIVATE openmine_gis openmine_cad GDAL::GDAL GTest::gtest_main)
    gtest_discover_tests(openmine_gis_tests DISCOVERY_TIMEOUT 60)
endif()

if(TARGET openmine_optimisation)
    add_executable(openmine_optimisation_tests unit/test_optimiser.cpp)
    target_link_libraries(openmine_optimisation_tests PRIVATE openmine_optimisation GTest::gtest_main)
    gtest_discover_tests(openmine_optimisation_tests DISCOVERY_TIMEOUT 60)
endif()

# Open3D engines, cross-validated against the built-in implementations (and PDAL when built).
if(TARGET openmine_open3d)
    add_executable(openmine_open3d_tests unit/test_open3d.cpp)
    target_link_libraries(openmine_open3d_tests PRIVATE openmine_open3d openmine_survey GTest::gtest_main)
    if(TARGET openmine_pdal)
        target_link_libraries(openmine_open3d_tests PRIVATE openmine_pdal)
    endif()
    gtest_discover_tests(openmine_open3d_tests DISCOVERY_TIMEOUT 120 PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=/opt/pdal/lib" LABELS open3d)
endif()

# PostgreSQL/PostGIS repository (needs a database: -DOPENMINE_TEST_PG_DSN="host=… dbname=… user=…").
if(TARGET openmine_repository)
    set(OPENMINE_TEST_PG_DSN "" CACHE STRING "libpq connection string of a scratch database with PostGIS for repository tests")
    add_executable(openmine_pg_repository_tests unit/test_pg_repository.cpp)
    target_link_libraries(openmine_pg_repository_tests PRIVATE openmine_repository openmine_core GTest::gtest_main)
    gtest_discover_tests(openmine_pg_repository_tests DISCOVERY_TIMEOUT 60 PROPERTIES ENVIRONMENT "OPENMINE_PG_DSN=${OPENMINE_TEST_PG_DSN}" LABELS database)
endif()

# Native plugins: the sample plugin and a misbehaving test plugin, loaded by the host tests.
if(TARGET openmine_plugin_host)
    openmine_add_plugin(openmine_test_misbehaving ${CMAKE_CURRENT_SOURCE_DIR}/plugins/misbehaving
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/plugins/misbehaving/misbehaving.cpp)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/plugins/future/plugin.json ${CMAKE_BINARY_DIR}/plugins/future/plugin.json COPYONLY)
    add_executable(openmine_plugin_tests unit/test_plugins.cpp)
    target_link_libraries(openmine_plugin_tests PRIVATE openmine_plugin_host openmine_core GTest::gtest_main)
    target_compile_definitions(openmine_plugin_tests PRIVATE OPENMINE_TEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins")
    add_dependencies(openmine_plugin_tests openmine_station_import openmine_test_misbehaving)
    gtest_discover_tests(openmine_plugin_tests DISCOVERY_TIMEOUT 60)
endif()

# OMF v1 validation against the GMG reference library (omf 1.0.x). Skipped by pytest if missing.
add_executable(omf_tool tools/omf_tool.cpp)
target_link_libraries(omf_tool PRIVATE openmine_omf openmine_geology openmine_survey nlohmann_json::nlohmann_json)
if(Python3_EXECUTABLE)
    add_test(NAME omf_reference_validation COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider --basetemp=${CMAKE_CURRENT_BINARY_DIR}/pytest-test_omf ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_omf.py)
    set_tests_properties(omf_reference_validation PROPERTIES ENVIRONMENT "OPENMINE_OMF_TOOL=$<TARGET_FILE:omf_tool>" LABELS integration)
endif()

# Stereonet statistics validation against mplstereonet (independent). Skipped by pytest if missing.
add_executable(geotech_tool tools/geotech_tool.cpp)
target_link_libraries(geotech_tool PRIVATE openmine_geotechnical nlohmann_json::nlohmann_json)
if(Python3_EXECUTABLE)
    add_test(NAME stereonet_mplstereonet_validation COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider --basetemp=${CMAKE_CURRENT_BINARY_DIR}/pytest-test_stereonet ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_stereonet.py)
    set_tests_properties(stereonet_mplstereonet_validation PROPERTIES ENVIRONMENT "OPENMINE_GEOTECH_TOOL=$<TARGET_FILE:geotech_tool>" LABELS integration)
endif()

add_executable(report_tool tools/report_tool.cpp)
target_link_libraries(report_tool PRIVATE openmine_reporting openmine_survey_reports openmine_survey openmine_mine_design openmine_cad nlohmann_json::nlohmann_json)
if(Python3_Interpreter_FOUND)
    add_test(NAME report_validation COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider --basetemp=${CMAKE_CURRENT_BINARY_DIR}/pytest-test_reports ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_reports.py)
    set_tests_properties(report_validation PROPERTIES ENVIRONMENT "OPENMINE_REPORT_TOOL=$<TARGET_FILE:report_tool>" LABELS integration)
endif()

add_test(NAME version_strings_agree
         COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_SOURCE_DIR} -DVERSION=${PROJECT_VERSION} -P ${CMAKE_SOURCE_DIR}/tests/tools/check_versions.cmake)
set_tests_properties(version_strings_agree PROPERTIES LABELS "docs")

# Documentation builds: the C++ API reference (Doxygen) without warnings, and the MkDocs site in
# strict mode (every link and page resolves). Each runs when its tool is installed.
find_program(DOXYGEN_EXECUTABLE doxygen)
if(DOXYGEN_EXECUTABLE)
    add_test(NAME doxygen_reference_builds
             COMMAND ${CMAKE_COMMAND} -DDOXYGEN=${DOXYGEN_EXECUTABLE} -DSOURCE=${CMAKE_SOURCE_DIR} -DVERSION=${PROJECT_VERSION}
                     -P ${CMAKE_SOURCE_DIR}/tests/tools/check_doxygen.cmake)
    set_tests_properties(doxygen_reference_builds PROPERTIES LABELS "docs" RUN_SERIAL TRUE)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import mkdocs, material" RESULT_VARIABLE _mkdocs_rc OUTPUT_QUIET ERROR_QUIET)
    if(_mkdocs_rc EQUAL 0)
        add_test(NAME docs_site_builds COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/docs/tools/build_site.py)
        set_tests_properties(docs_site_builds PROPERTIES LABELS "docs" RUN_SERIAL TRUE)
    endif()
endif()
