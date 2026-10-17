# GDAL/OGR (MIT) — linked dynamically.
add_library(openmine_gis src/Gis.cpp)
add_library(OpenMine::gis ALIAS openmine_gis)
target_include_directories(openmine_gis PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_gis PUBLIC openmine_core PRIVATE GDAL::GDAL)
target_compile_definitions(openmine_gis PUBLIC OPENMINE_HAS_GDAL=1)
openmine_set_warnings(openmine_gis)
