# PDAL (BSD-3-Clause) — linked dynamically.
add_library(openmine_pdal src/Pdal.cpp)
add_library(OpenMine::pdal ALIAS openmine_pdal)
target_include_directories(openmine_pdal PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_pdal PUBLIC openmine_survey PRIVATE pdalcpp)
target_compile_definitions(openmine_pdal PUBLIC OPENMINE_HAS_PDAL=1)
openmine_set_warnings(openmine_pdal)
