# Plugin SDK: the stable C header (openmine_plugin.h) and the host implementation.
find_package(nlohmann_json 3.10 REQUIRED)

add_library(openmine_plugin_sdk INTERFACE)
add_library(OpenMine::plugin_sdk ALIAS openmine_plugin_sdk)
target_include_directories(openmine_plugin_sdk INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)

add_library(openmine_plugin_host host/src/PluginManager.cpp)
add_library(OpenMine::plugin_host ALIAS openmine_plugin_host)
target_include_directories(openmine_plugin_host PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/host/include>)
target_link_libraries(openmine_plugin_host PUBLIC openmine_core PRIVATE openmine_plugin_sdk nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
openmine_set_warnings(openmine_plugin_host)

# openmine_add_plugin(<target> <folder-with-plugin.json> SOURCES …): builds a plugin module into
# ${CMAKE_BINARY_DIR}/plugins/<folder name>/ next to a copy of its manifest, ready to be discovered.
function(openmine_add_plugin target folder)
    cmake_parse_arguments(P "" "" "SOURCES" ${ARGN})
    get_filename_component(name ${folder} NAME)
    set(out ${CMAKE_BINARY_DIR}/plugins/${name})
    add_library(${target} MODULE ${P_SOURCES})
    target_link_libraries(${target} PRIVATE openmine_plugin_sdk nlohmann_json::nlohmann_json)
    set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${out} RUNTIME_OUTPUT_DIRECTORY ${out}
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    if(APPLE)
        set_target_properties(${target} PROPERTIES SUFFIX ".dylib")
    endif()
    configure_file(${folder}/plugin.json ${out}/plugin.json COPYONLY)
endfunction()

openmine_add_plugin(openmine_station_import ${CMAKE_SOURCE_DIR}/plugins/samples/station_import
    SOURCES ${CMAKE_SOURCE_DIR}/plugins/samples/station_import/station_import.cpp)
