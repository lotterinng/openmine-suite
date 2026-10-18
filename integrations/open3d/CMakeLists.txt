# Open3D (MIT) — linked dynamically. The binary Open3D packages for Linux include components
# built against libc++; the libc++ runtime (libc++1, libc++abi1) must be installed.
add_library(openmine_open3d src/Open3d.cpp)
add_library(OpenMine::open3d ALIAS openmine_open3d)
target_include_directories(openmine_open3d PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(openmine_open3d PUBLIC openmine_survey PRIVATE Open3D::Open3D)
target_compile_definitions(openmine_open3d PUBLIC OPENMINE_HAS_OPEN3D=1)
openmine_set_warnings(openmine_open3d)
# Open3D's headers trigger warnings that are not ours.
get_target_property(_o3d_inc Open3D::Open3D INTERFACE_INCLUDE_DIRECTORIES)
if(_o3d_inc)
    target_include_directories(openmine_open3d SYSTEM PRIVATE ${_o3d_inc})
endif()
