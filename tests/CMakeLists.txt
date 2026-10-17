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
)
target_link_libraries(openmine_unit_tests PRIVATE
    openmine_core openmine_scheduling openmine_mine_design openmine_cad openmine_formats openmine_survey openmine_planning openmine_geology openmine_omf SQLite::SQLite3 GTest::gtest_main)
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
             COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_dxf_ezdxf.py)
    set_tests_properties(dxf_ezdxf_validation PROPERTIES ENVIRONMENT "OPENMINE_DXF_TOOL=$<TARGET_FILE:dxf_tool>" LABELS integration)
endif()

if(TARGET openmine_pdal)
    add_executable(openmine_pdal_tests unit/test_pdal.cpp)
    target_link_libraries(openmine_pdal_tests PRIVATE openmine_pdal openmine_survey GTest::gtest_main)
    gtest_discover_tests(openmine_pdal_tests DISCOVERY_TIMEOUT 60 PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=/opt/pdal/lib")
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME mesh_trimesh_validation
             COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_mesh_trimesh.py)
    set_tests_properties(mesh_trimesh_validation PROPERTIES ENVIRONMENT "OPENMINE_DXF_TOOL=$<TARGET_FILE:dxf_tool>" LABELS integration)
endif()

if(TARGET openmine_crs)
    add_executable(openmine_crs_tests unit/test_crs.cpp)
    target_link_libraries(openmine_crs_tests PRIVATE openmine_crs openmine_cad GTest::gtest_main)
    gtest_discover_tests(openmine_crs_tests DISCOVERY_TIMEOUT 60)
endif()

if(TARGET openmine_gis)
    add_executable(openmine_gis_tests unit/test_gis.cpp)
    target_link_libraries(openmine_gis_tests PRIVATE openmine_gis openmine_cad GTest::gtest_main)
    gtest_discover_tests(openmine_gis_tests DISCOVERY_TIMEOUT 60)
endif()

if(TARGET openmine_optimisation)
    add_executable(openmine_optimisation_tests unit/test_optimiser.cpp)
    target_link_libraries(openmine_optimisation_tests PRIVATE openmine_optimisation GTest::gtest_main)
    gtest_discover_tests(openmine_optimisation_tests DISCOVERY_TIMEOUT 60)
endif()

# OMF v1 validation against the GMG reference library (omf 1.0.x). Skipped by pytest if missing.
add_executable(omf_tool tools/omf_tool.cpp)
target_link_libraries(omf_tool PRIVATE openmine_omf openmine_geology openmine_survey nlohmann_json::nlohmann_json)
if(Python3_EXECUTABLE)
    add_test(NAME omf_reference_validation COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_omf.py)
    set_tests_properties(omf_reference_validation PROPERTIES ENVIRONMENT "OPENMINE_OMF_TOOL=$<TARGET_FILE:omf_tool>" LABELS integration)
endif()

add_executable(report_tool tools/report_tool.cpp)
target_link_libraries(report_tool PRIVATE openmine_reporting nlohmann_json::nlohmann_json)
if(Python3_Interpreter_FOUND)
    add_test(NAME report_validation COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_reports.py)
    set_tests_properties(report_validation PROPERTIES ENVIRONMENT "OPENMINE_REPORT_TOOL=$<TARGET_FILE:report_tool>" LABELS integration)
endif()
