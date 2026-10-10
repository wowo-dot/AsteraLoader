# Builds the hello example against the Astera Loader plugin header: a
# starting point for your own plugin. Windows, MSVC, 64-bit:
#
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
#
# The plugin is build/Release/hello.dll. Install it as
# <game>\AsteraLoader\plugins\hello\hello.dll (under MO2: its own mod with
# that layout).
cmake_minimum_required(VERSION 3.25)
project(astera_loader_plugin LANGUAGES C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Astera Loader plugins are 64-bit: configure with -A x64")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(hello SHARED examples/hello/hello.c)
target_include_directories(hello PRIVATE include)
target_compile_definitions(hello PRIVATE WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(hello PRIVATE /W4 /utf-8)
    # The static C runtime: the plugin needs no Visual C++ runtime installed.
    set_property(TARGET hello PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
