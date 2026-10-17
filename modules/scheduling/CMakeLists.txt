add_library(openmine_scheduling
    src/CalendarTime.cpp
    src/Production.cpp
    src/ScheduleInput.cpp
    src/Cpm.cpp
    src/Levelling.cpp
    src/Periods.cpp
    src/ScheduleTable.cpp
)
add_library(OpenMine::scheduling ALIAS openmine_scheduling)
target_include_directories(openmine_scheduling PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_scheduling PUBLIC openmine_core)
openmine_set_warnings(openmine_scheduling)
