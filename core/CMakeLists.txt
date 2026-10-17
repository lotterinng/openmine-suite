find_package(SQLite3 REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)
find_package(Eigen3 3.3 REQUIRED NO_MODULE)

add_library(openmine_core
    src/objects/Uuid.cpp
    src/objects/DateTime.cpp
    src/objects/AttributeSchema.cpp
    src/geometry/Transform.cpp
    src/geometry/Polyline.cpp
    src/geometry/Curves.cpp
    src/geometry/MeshOps.cpp
    src/geometry/Polygon2D.cpp
    src/geometry/Profile.cpp
    src/geometry/Sweep.cpp
    src/geometry/LocalGrid.cpp
    src/util/Sha256.cpp
    src/util/Csv.cpp
    src/diagnostics/Log.cpp
    src/jobs/Job.cpp
    src/assets/AssetStore.cpp
    src/commands/Commands.cpp
    src/project/Project.cpp
    src/project/ProjectEditor.cpp
    src/project/EditorValidation.cpp
    src/persistence/SchemaV2.cpp
    src/persistence/LegacyV1.cpp
    src/persistence/ProjectFile.cpp
)
add_library(OpenMine::core ALIAS openmine_core)
target_include_directories(openmine_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_core PRIVATE SQLite::SQLite3 nlohmann_json::nlohmann_json Eigen3::Eigen PUBLIC Threads::Threads)
target_compile_features(openmine_core PUBLIC cxx_std_20)
openmine_set_warnings(openmine_core)
