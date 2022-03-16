cmake_minimum_required(VERSION 3.18)
project(wakefield-demo C)

set(CMAKE_C_STANDARD 11)

find_program(WAYLAND_SCANNER wayland-scanner)
if (NOT WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner not found")
endif ()

find_program(CURL curl)
if (NOT CURL)
    message(FATAL_ERROR "install curl")
endif()


add_custom_command(
        OUTPUT xdg-shell.xml
        COMMAND curl -O https://cgit.freedesktop.org/wayland/wayland-protocols/plain/stable/xdg-shell/xdg-shell.xml
        )

add_custom_command(
        OUTPUT xdg-shell-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header xdg-shell.xml xdg-shell-client-protocol.h
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell.xml
        VERBATIM)

add_custom_command(
        OUTPUT xdg-shell-client-protocol.c
        COMMAND ${WAYLAND_SCANNER} code xdg-shell.xml xdg-shell-client-protocol.c
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell.xml
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

