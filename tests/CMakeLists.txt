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
)
target_link_libraries(openmine_unit_tests PRIVATE
    openmine_core openmine_scheduling openmine_mine_design openmine_cad openmine_formats SQLite::SQLite3 GTest::gtest_main)
gtest_discover_tests(openmine_unit_tests DISCOVERY_TIMEOUT 60)

if(TARGET openmine_occt)
    add_executable(openmine_occt_tests unit/test_occt.cpp)
    target_link_libraries(openmine_occt_tests PRIVATE openmine_occt GTest::gtest_main)
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
