find_package(ZLIB REQUIRED)
find_path(XLSXWRITER_INCLUDE_DIR xlsxwriter.h)
find_library(XLSXWRITER_LIBRARY xlsxwriter)
if(NOT XLSXWRITER_INCLUDE_DIR OR NOT XLSXWRITER_LIBRARY)
    message(FATAL_ERROR "libxlsxwriter not found")
endif()
add_library(openmine_reporting
    src/Pdf.cpp
    src/ScheduleExport.cpp
)
add_library(OpenMine::reporting ALIAS openmine_reporting)
target_include_directories(openmine_reporting PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(openmine_reporting SYSTEM PRIVATE ${XLSXWRITER_INCLUDE_DIR})
target_link_libraries(openmine_reporting PUBLIC openmine_scheduling openmine_core PRIVATE ZLIB::ZLIB ${XLSXWRITER_LIBRARY})
openmine_set_warnings(openmine_reporting)
