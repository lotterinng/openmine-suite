# PROJ (MIT) — linked dynamically.
add_library(openmine_crs src/Crs.cpp)
add_library(OpenMine::crs ALIAS openmine_crs)
target_include_directories(openmine_crs PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_crs PUBLIC openmine_core PRIVATE PROJ::proj)
target_compile_definitions(openmine_crs PUBLIC OPENMINE_HAS_PROJ=1)
openmine_set_warnings(openmine_crs)
