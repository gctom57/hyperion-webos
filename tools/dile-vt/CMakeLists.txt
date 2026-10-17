# Diagnostic tools for DILE_VT capture on the TV. Not built by default:
#   make dvtest vtreset
add_executable(dvtest EXCLUDE_FROM_ALL dvtest.c)
target_link_libraries(dvtest dile_vt)
# libdile_vt lacks a DT_NEEDED for libPmLogLib (see unicapture/CMakeLists.txt)
set_target_properties(dvtest PROPERTIES LINK_FLAGS "-lPmLogLib")

add_executable(vtreset EXCLUDE_FROM_ALL vtreset.c)
target_link_libraries(vtreset ${CMAKE_DL_LIBS})
