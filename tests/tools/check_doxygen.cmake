# Runs Doxygen on the public headers and fails on any warning, or when the Doxyfile's version
# differs from the project version.
#   cmake -DDOXYGEN=<doxygen> -DSOURCE=<repo root> -DVERSION=<x.y.z> -P check_doxygen.cmake
file(STRINGS "${SOURCE}/Doxyfile" number REGEX "^PROJECT_NUMBER")
string(REGEX REPLACE "^PROJECT_NUMBER *= *" "" number "${number}")
if(NOT number STREQUAL VERSION)
    message(FATAL_ERROR "Doxyfile PROJECT_NUMBER is '${number}' but the project version is ${VERSION}")
endif()
file(MAKE_DIRECTORY "${SOURCE}/build/doxygen")
execute_process(COMMAND "${DOXYGEN}" Doxyfile WORKING_DIRECTORY "${SOURCE}" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "doxygen failed (${rc}): ${err}")
endif()
file(READ "${SOURCE}/build/doxygen/warnings.log" warnings)
if(NOT warnings STREQUAL "")
    message(FATAL_ERROR "Doxygen warnings:\n${warnings}")
endif()
message(STATUS "C++ API reference built without warnings")
