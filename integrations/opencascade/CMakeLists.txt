# Open CASCADE Technology (LGPL-2.1 with OCCT exception) — linked dynamically.
add_library(openmine_occt src/SweptSolid.cpp src/Solid.cpp src/SolidObjects.cpp src/Modelling.cpp)
add_library(OpenMine::occt ALIAS openmine_occt)
target_include_directories(openmine_occt PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(openmine_occt SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
# OCCT 7.8 merged the STEP toolkits (TKSTEP, TKSTEPBase, TKSTEPAttr, TKSTEP209) into TKDESTEP and
# TKIGES into TKDEIGES; earlier releases (e.g. Ubuntu 24.04's 7.6) have the separate toolkits.
if(TARGET TKDESTEP)
    set(OPENMINE_OCCT_EXCHANGE TKDESTEP TKDEIGES)
else()
    set(OPENMINE_OCCT_EXCHANGE TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKIGES)
endif()
target_link_libraries(openmine_occt
    PUBLIC openmine_core
    PRIVATE openmine_cad openmine_mine_design
            TKernel TKMath TKBRep TKTopAlgo TKGeomBase TKGeomAlgo TKG3d TKG2d TKOffset TKPrim TKBO TKBool
            TKShHealing TKMesh TKFillet TKXSBase ${OPENMINE_OCCT_EXCHANGE})
target_compile_definitions(openmine_occt PUBLIC OPENMINE_HAS_OCCT=1)
openmine_set_warnings(openmine_occt)
