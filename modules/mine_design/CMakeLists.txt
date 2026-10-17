find_package(nlohmann_json 3.10 REQUIRED)
add_library(openmine_mine_design
    src/Development.cpp
    src/Layout.cpp
    src/Revisions.cpp
    src/Stopes.cpp
    src/Headings.cpp
)
add_library(OpenMine::mine_design ALIAS openmine_mine_design)
target_include_directories(openmine_mine_design PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_mine_design PUBLIC openmine_core PRIVATE openmine_cad nlohmann_json::nlohmann_json)
openmine_set_warnings(openmine_mine_design)
