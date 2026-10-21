# Engine benchmarks (no UI). `openmine_bench --quick` runs as a ctest (label "benchmark");
# full sizes: openmine_bench --check benchmarks/budgets.json (Release build).
find_package(nlohmann_json 3.10 REQUIRED)
add_executable(openmine_bench openmine_bench.cpp)
target_link_libraries(openmine_bench PRIVATE openmine_core openmine_scheduling openmine_survey nlohmann_json::nlohmann_json)
openmine_set_warnings(openmine_bench)
set_target_properties(openmine_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(OPENMINE_BUILD_TESTS)
    add_test(NAME benchmarks_quick COMMAND openmine_bench --quick)
    set_tests_properties(benchmarks_quick PROPERTIES LABELS "benchmark" TIMEOUT 600)
endif()
