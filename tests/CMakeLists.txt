find_package(GTest REQUIRED)
find_package(SQLite3 REQUIRED)
include(GoogleTest)

add_executable(openmine_unit_tests
    unit/test_core.cpp
    unit/test_commands.cpp
    unit/test_persistence.cpp
    unit/test_scheduling.cpp
    unit/test_mine_design.cpp
)
target_link_libraries(openmine_unit_tests PRIVATE
    openmine_core openmine_scheduling openmine_mine_design SQLite::SQLite3 GTest::gtest_main)
gtest_discover_tests(openmine_unit_tests DISCOVERY_TIMEOUT 60)

if(TARGET openmine_occt)
    add_executable(openmine_occt_tests unit/test_occt.cpp)
    target_link_libraries(openmine_occt_tests PRIVATE openmine_occt GTest::gtest_main)
    gtest_discover_tests(openmine_occt_tests DISCOVERY_TIMEOUT 60)
endif()
