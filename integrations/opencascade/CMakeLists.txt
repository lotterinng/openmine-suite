# Open CASCADE Technology (LGPL-2.1 with OCCT exception) — linked dynamically.
add_library(openmine_occt src/SweptSolid.cpp)
add_library(OpenMine::occt ALIAS openmine_occt)
target_include_directories(openmine_occt PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(openmine_occt SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(openmine_occt
    PUBLIC openmine_core
    PRIVATE TKernel TKMath TKBRep TKTopAlgo TKGeomBase TKGeomAlgo TKG3d TKG2d TKOffset TKPrim)
target_compile_definitions(openmine_occt PUBLIC OPENMINE_HAS_OCCT=1)
openmine_set_warnings(openmine_occt)
