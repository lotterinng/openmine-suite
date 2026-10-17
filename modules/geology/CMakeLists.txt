find_package(Eigen3 3.3 REQUIRED NO_MODULE)
find_package(nanoflann REQUIRED)
add_library(openmine_geology
    src/Desurvey.cpp
    src/Drillholes.cpp
    src/Compositing.cpp
    src/BlockModel.cpp
    src/Estimation.cpp
)
add_library(OpenMine::geology ALIAS openmine_geology)
target_include_directories(openmine_geology PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_geology PUBLIC openmine_core PRIVATE Eigen3::Eigen nanoflann::nanoflann)
openmine_set_warnings(openmine_geology)
