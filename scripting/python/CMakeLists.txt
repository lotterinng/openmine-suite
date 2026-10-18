find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module Development.Embed)
# Prefer a pybind11 matching the interpreter (pip install pybind11) over an older system copy.
execute_process(COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
                OUTPUT_VARIABLE _pybind11_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(_pybind11_dir)
    list(PREPEND CMAKE_PREFIX_PATH "${_pybind11_dir}")
endif()
find_package(pybind11 2.12 CONFIG REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

# Shared binding code (object library so the extension and the embedded interpreter compile it once each with -fPIC).
set(_bind_libs openmine_core openmine_cad openmine_scheduling openmine_mine_design openmine_survey openmine_formats openmine_reporting
               openmine_planning openmine_geology openmine_geotechnical openmine_omf openmine_plugin_host openmine_survey_reports nlohmann_json::nlohmann_json)
set(_bind_defs "")
foreach(pair "openmine_optimisation;OPTIMISER" "openmine_crs;CRS" "openmine_gis;GIS" "openmine_repository;REPOSITORY" "openmine_occt;OCCT" "openmine_pdal;PDAL" "openmine_open3d;OPEN3D")
    list(GET pair 0 tgt)
    list(GET pair 1 def)
    if(TARGET ${tgt})
        list(APPEND _bind_libs ${tgt})
        list(APPEND _bind_defs OPENMINE_PY_HAVE_${def})
    endif()
endforeach()

add_library(openmine_python_bindings OBJECT src/Bindings.cpp)
set_target_properties(openmine_python_bindings PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(openmine_python_bindings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(openmine_python_bindings PUBLIC pybind11::headers Python::Python ${_bind_libs})
target_compile_definitions(openmine_python_bindings PUBLIC ${_bind_defs})

set(_pkg_dir ${CMAKE_BINARY_DIR}/python/openmine)
pybind11_add_module(openmine_python src/module.cpp)
set_target_properties(openmine_python PROPERTIES OUTPUT_NAME _core LIBRARY_OUTPUT_DIRECTORY ${_pkg_dir})
target_link_libraries(openmine_python PRIVATE openmine_python_bindings)
file(GLOB _py_sources RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/openmine ${CMAKE_CURRENT_SOURCE_DIR}/openmine/*.py)
foreach(f ${_py_sources})
    configure_file(openmine/${f} ${_pkg_dir}/${f} COPYONLY)
endforeach()

# Embedded interpreter for the application console and scripts (binds the live project).
add_library(openmine_python_embed embed/PythonConsole.cpp)
target_include_directories(openmine_python_embed PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/embed)
target_link_libraries(openmine_python_embed PUBLIC openmine_core PRIVATE openmine_python_bindings pybind11::embed)
target_compile_definitions(openmine_python_embed PRIVATE OPENMINE_PYTHON_PACKAGE_DIR="${CMAKE_BINARY_DIR}/python")
# pybind11 objects are hidden-visibility types: the console's implementation must be hidden too.
set_target_properties(openmine_python_embed PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

if(OPENMINE_BUILD_TESTS)
    add_test(NAME python_api_tests
             COMMAND ${Python_EXECUTABLE} -m pytest -q -p no:cacheprovider --basetemp=${CMAKE_BINARY_DIR}/pytest-python-api ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    set_tests_properties(python_api_tests PROPERTIES ENVIRONMENT
        "PYTHONPATH=${CMAKE_BINARY_DIR}/python;OPENMINE_TEST_PLUGIN_DIR=${CMAKE_BINARY_DIR}/plugins;OPENMINE_PG_DSN=${OPENMINE_TEST_PG_DSN}" LABELS python)
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_executable(openmine_console_tests tests/test_console.cpp)
    target_link_libraries(openmine_console_tests PRIVATE openmine_python_embed openmine_core GTest::gtest_main)
    gtest_discover_tests(openmine_console_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS python)
endif()
