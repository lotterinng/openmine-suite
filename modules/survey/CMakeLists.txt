find_package(nlohmann_json 3.10 REQUIRED)
find_package(Eigen3 3.3 REQUIRED NO_MODULE)
find_package(nanoflann REQUIRED)
find_package(Clipper2 QUIET PATHS /opt/clipper2)
if(NOT Clipper2_FOUND)
    find_path(CLIPPER2_INCLUDE_DIR clipper2/clipper.h PATHS /opt/clipper2/include)
    find_library(CLIPPER2_LIBRARY Clipper2 PATHS /opt/clipper2/lib)
    if(NOT CLIPPER2_INCLUDE_DIR OR NOT CLIPPER2_LIBRARY)
        message(FATAL_ERROR "Clipper2 not found (set CMAKE_PREFIX_PATH to its install prefix)")
    endif()
    add_library(Clipper2::Clipper2 UNKNOWN IMPORTED)
    set_target_properties(Clipper2::Clipper2 PROPERTIES IMPORTED_LOCATION ${CLIPPER2_LIBRARY} INTERFACE_INCLUDE_DIRECTORIES ${CLIPPER2_INCLUDE_DIR})
endif()

add_library(openmine_survey
    src/PointBuffer.cpp
    src/TextFormats.cpp
    src/Octree.cpp
    src/PointCloudProject.cpp
    src/KdTree.cpp
    src/Processing.cpp
    src/Registration.cpp
    src/Meshing.cpp
    src/Conformance.cpp
)
add_library(OpenMine::survey ALIAS openmine_survey)
target_include_directories(openmine_survey PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_survey
    PUBLIC openmine_core
    PRIVATE openmine_mine_design nlohmann_json::nlohmann_json Eigen3::Eigen nanoflann::nanoflann Clipper2::Clipper2)
openmine_set_warnings(openmine_survey)
