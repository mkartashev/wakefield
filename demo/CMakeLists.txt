cmake_minimum_required(VERSION 3.18)
project(wakefield-demo C)

set(CMAKE_C_STANDARD 11)

find_program(WAYLAND_SCANNER wayland-scanner)
if (NOT WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner not found")
endif ()

find_file(XDG_SHELL_XML
        xdg-shell.xml
        PATHS /usr/share/wayland-protocols/stable/xdg-shell/ ${CMAKE_CURRENT_BINARY_DIR})

if (NOT XDG_SHELL_XML)
    message(FATAL_ERROR "can't find xdg-shell.xml")
endif()

add_custom_command(
        OUTPUT xdg-shell-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML} xdg-shell-client-protocol.h
        DEPENDS ${XDG_SHELL_XML}
        VERBATIM)

add_custom_command(
        OUTPUT xdg-shell-client-protocol.c
        COMMAND ${WAYLAND_SCANNER} public-code ${XDG_SHELL_XML} xdg-shell-client-protocol.c
        DEPENDS ${XDG_SHELL_XML}
        VERBATIM)

add_custom_command(
        OUTPUT wakefield-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header ${CMAKE_SOURCE_DIR}/../protocol/wakefield.xml wakefield-client-protocol.h
        DEPENDS ${CMAKE_SOURCE_DIR}/../protocol/wakefield.xml
        VERBATIM)

add_custom_command(
        OUTPUT wakefield-client-protocol.c
        COMMAND ${WAYLAND_SCANNER} code ${CMAKE_SOURCE_DIR}/../protocol/wakefield.xml wakefield-client-protocol.c
        DEPENDS ${CMAKE_SOURCE_DIR}/../protocol/wakefield.xml
        VERBATIM)

find_library(WAYLAND_CLIENT wayland-client)

add_executable(wakefield-demo demo.c xdg-shell-client-protocol.h xdg-shell-client-protocol.c
        wakefield-client-protocol.h wakefield-client-protocol.c)
target_include_directories(wakefield-demo PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wakefield-demo PRIVATE ${WAYLAND_CLIENT} rt)

