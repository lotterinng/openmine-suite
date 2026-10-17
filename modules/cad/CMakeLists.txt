add_library(openmine_cad
    src/Primitives.cpp
    src/Edit.cpp
    src/Annotation.cpp
    src/Snap.cpp
)
add_library(OpenMine::cad ALIAS openmine_cad)
target_include_directories(openmine_cad PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_cad PUBLIC openmine_core)
openmine_set_warnings(openmine_cad)
