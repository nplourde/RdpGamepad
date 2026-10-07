cmake_minimum_required(VERSION 3.16...3.31)
project(rdpgamepad-freerdp VERSION 1.0.0 LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREERDP REQUIRED IMPORTED_TARGET freerdp3 freerdp-client3 winpr3)

# FreeRDP looks for dynamic channel add-ins as <addin dir>/lib<name>-client.so
pkg_get_variable(FREERDP_LIBDIR freerdp3 libdir)
set(FREERDP_ADDIN_DIR "${FREERDP_LIBDIR}/freerdp3" CACHE PATH "FreeRDP add-in directory")

add_library(rdpgamepad-client MODULE
    src/rdpgamepad_main.c
    src/gamepad.c
)
set_target_properties(rdpgamepad-client PROPERTIES
    PREFIX "lib"
    C_STANDARD 11
    C_EXTENSIONS ON
    C_VISIBILITY_PRESET hidden
)
target_compile_options(rdpgamepad-client PRIVATE -Wall -Wextra)
target_link_libraries(rdpgamepad-client PRIVATE PkgConfig::FREERDP)
target_link_options(rdpgamepad-client PRIVATE -Wl,--no-undefined)

install(TARGETS rdpgamepad-client LIBRARY DESTINATION "${FREERDP_ADDIN_DIR}")

option(BUILD_HARNESS "Build the rdpgamepad-harness test tool" OFF)
if(BUILD_HARNESS)
    add_executable(rdpgamepad-harness tests/harness.c)
    target_link_libraries(rdpgamepad-harness PRIVATE PkgConfig::FREERDP ${CMAKE_DL_LIBS})
endif()
