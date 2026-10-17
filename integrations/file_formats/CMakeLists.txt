add_library(openmine_formats
    src/DxfCommon.cpp
    src/DxfReader.cpp
    src/DxfWriter.cpp
    src/DxfProject.cpp
)
add_library(OpenMine::formats ALIAS openmine_formats)
target_include_directories(openmine_formats PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_formats PUBLIC openmine_core PRIVATE openmine_cad)
openmine_set_warnings(openmine_formats)
