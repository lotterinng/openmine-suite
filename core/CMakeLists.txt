find_package(SQLite3 REQUIRED)

add_library(openmine_core
    src/objects/Uuid.cpp
    src/objects/DateTime.cpp
    src/objects/Entities.cpp
    src/objects/AttributeSchema.cpp
    src/geometry/Polyline.cpp
    src/commands/Commands.cpp
    src/project/Project.cpp
    src/project/ProjectEditor.cpp
    src/persistence/ProjectFile.cpp
)
add_library(OpenMine::core ALIAS openmine_core)
target_include_directories(openmine_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_core PRIVATE SQLite::SQLite3)
target_compile_features(openmine_core PUBLIC cxx_std_20)
openmine_set_warnings(openmine_core)
