find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
# Prefer a pybind11 matching the interpreter (pip install pybind11) over an older system copy.
execute_process(COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
                OUTPUT_VARIABLE _pybind11_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(_pybind11_dir)
    list(PREPEND CMAKE_PREFIX_PATH "${_pybind11_dir}")
endif()
find_package(pybind11 2.12 CONFIG REQUIRED)

set(_pkg_dir ${CMAKE_BINARY_DIR}/python/openmine)
pybind11_add_module(openmine_python src/bindings.cpp)
set_target_properties(openmine_python PROPERTIES OUTPUT_NAME _core LIBRARY_OUTPUT_DIRECTORY ${_pkg_dir})
target_link_libraries(openmine_python PRIVATE openmine_core openmine_scheduling openmine_mine_design)
configure_file(openmine/__init__.py ${_pkg_dir}/__init__.py COPYONLY)

if(OPENMINE_BUILD_TESTS)
    add_test(NAME python_api_tests
             COMMAND ${Python_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    set_tests_properties(python_api_tests PROPERTIES ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}/python" LABELS python)
endif()
