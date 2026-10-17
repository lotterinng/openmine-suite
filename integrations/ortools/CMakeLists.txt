# Google OR-Tools (Apache-2.0) — CP-SAT for resource-constrained schedule optimisation.
add_library(openmine_optimisation src/Optimiser.cpp)
add_library(OpenMine::optimisation ALIAS openmine_optimisation)
target_include_directories(openmine_optimisation PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_optimisation PUBLIC openmine_scheduling openmine_core PRIVATE ortools::ortools)
target_compile_definitions(openmine_optimisation PUBLIC OPENMINE_HAS_ORTOOLS=1)
openmine_set_warnings(openmine_optimisation)
# OR-Tools headers trigger warnings we do not control.
target_compile_options(openmine_optimisation PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-sign-conversion -Wno-conversion -Wno-shadow>)
# The OR-Tools binary distribution (and its bundled Abseil) is built with NDEBUG; Abseil's
# container layouts differ without it, so this translation unit must match.
target_compile_definitions(openmine_optimisation PRIVATE NDEBUG)
