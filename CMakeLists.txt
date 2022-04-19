cmake_minimum_required(VERSION 3.18)
project(wakefield C)

set(CMAKE_C_STANDARD 11)

set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_SOURCE_DIR})

find_program(WAYLAND_SCANNER wayland-scanner)
if (NOT WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner not found")
endif ()

add_custom_command(
        OUTPUT wakefield-server-protocol.h
        COMMAND ${WAYLAND_SCANNER} server-header ${CMAKE_CURRENT_SOURCE_DIR}/protocol/wakefield.xml wakefield-server-protocol.h
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocol/wakefield.xml
        VERBATIM)

add_custom_command(
        OUTPUT wakefield-server-protocol.c
        COMMAND ${WAYLAND_SCANNER} private-code ${CMAKE_CURRENT_SOURCE_DIR}/protocol/wakefield.xml wakefield-server-protocol.c
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocol/wakefield.xml
        VERBATIM)

find_path(
        WESTON_INCLUDES
        weston/weston.h
        /usr/include /usr/local/include)

if (NOT WESTON_INCLUDES)
    message(FATAL_ERROR "weston/weston.h not found")
endif ()

find_path(
        LIBWESTON_INCLUDES
        libweston/libweston.h
        /usr/include /usr/local/include
        PATH_SUFFIXES libweston-9)

if (NOT LIBWESTON_INCLUDES)
    message(FATAL_ERROR "libweston/libweston.h not found")
endif ()

find_path(
        PIXMAN_INCLUDES
        pixman.h
        /usr/include /usr/local/include
        PATH_SUFFIXES pixman-1)

if (NOT PIXMAN_INCLUDES)
    message(FATAL_ERROR "pixman.h not found")
endif ()

add_library(wakefield SHARED src/wakefield.c wakefield-server-protocol.c wakefield-server-protocol.h)
target_include_directories(wakefield PUBLIC
        ${WESTON_INCLUDES} ${LIBWESTON_INCLUDES} ${PIXMAN_INCLUDES}
        ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS wakefield DESTINATION .)

