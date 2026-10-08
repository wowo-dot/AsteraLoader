cmake_minimum_required(VERSION 3.25)

project(cmfm VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "CMFM targets 64-bit Windows only")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "CMFM currently requires MSVC")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Configure a 64-bit build")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# C only checks that the public plugin header builds as plain C.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Static CRT: the early-loaded cmfm.dll should import no DLL beyond KERNEL32.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Git QUIET)
set(CMFM_GIT_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE CMFM_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

# Project code builds with warnings as errors. Third-party code, when there is
# some, gets its own targets without these options.
function(cmfm_project_target target)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8
        "$<$<COMPILE_LANGUAGE:CXX>:/permissive-;/Zc:__cplusplus;/EHsc>")
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
        CMFM_VERSION=L"${PROJECT_VERSION}-dev"
        CMFM_GIT_COMMIT=L"${CMFM_GIT_COMMIT}")
    target_link_options(${target} PRIVATE /WX)
endfunction()

# Install layout: dist/<config>/cmfm_launcher.exe and dist/<config>/cmfm/cmfm.dll.
set(CMFM_DIST "${CMAKE_BINARY_DIR}/dist/$<CONFIG>")

add_library(cmfm_common STATIC
    src/common/launch_plan.cpp
    src/common/launch_plan.hpp
    src/common/log_file.cpp
    src/common/log_file.hpp
    src/common/paths.cpp
    src/common/paths.hpp
    src/common/sha256.cpp
    src/common/sha256.hpp
    src/common/startup.hpp
    src/common/windows.hpp)
target_include_directories(cmfm_common PUBLIC src)
cmfm_project_target(cmfm_common)

# The public plugin API: one C header, the only thing plugins build against.
add_library(cmfm_api INTERFACE)
target_include_directories(cmfm_api INTERFACE include)

# Plugin manifests and load order (core side of the plugin API).
add_library(cmfm_plugins STATIC
    src/core/plugins/discovery.cpp
    src/core/plugins/discovery.hpp
    src/core/plugins/host.cpp
    src/core/plugins/host.hpp
    src/core/plugins/load_order.cpp
    src/core/plugins/load_order.hpp
    src/core/plugins/manifest.cpp
    src/core/plugins/manifest.hpp)
target_link_libraries(cmfm_plugins PUBLIC cmfm_api cmfm_common)
cmfm_project_target(cmfm_plugins)

add_executable(cmfm_launcher WIN32 src/launcher/main.cpp)
target_link_libraries(cmfm_launcher PRIVATE cmfm_common shell32 user32)
cmfm_project_target(cmfm_launcher)
set_target_properties(cmfm_launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMFM_DIST}")

add_library(cmfm SHARED src/core/dllmain.cpp)
target_link_libraries(cmfm PRIVATE cmfm_common)
cmfm_project_target(cmfm)
set_target_properties(cmfm PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMFM_DIST}/cmfm")

# Package only production binaries and the licence, never stand-ins or fixtures.
install(FILES "$<TARGET_FILE:cmfm_launcher>" DESTINATION .)
install(FILES "$<TARGET_FILE:cmfm>" DESTINATION cmfm)
install(FILES LICENSE DESTINATION .)

include(CTest)
if(BUILD_TESTING)

add_executable(cmfm_tests tests/test_common.cpp)
target_link_libraries(cmfm_tests PRIVATE cmfm_common shell32)
cmfm_project_target(cmfm_tests)
add_test(NAME common COMMAND cmfm_tests)

add_executable(cmfm_core_tests tests/test_core.cpp)
target_link_libraries(cmfm_core_tests PRIVATE cmfm_common)
cmfm_project_target(cmfm_core_tests)
add_test(NAME core_startup_abi COMMAND cmfm_core_tests "$<TARGET_FILE:cmfm>")

# The plugin ABI layout, compiled as C and as C++ (tests/abi_layout.h).
foreach(language IN ITEMS c cpp)
    add_executable(cmfm_abi_${language} tests/test_abi.${language})
    target_link_libraries(cmfm_abi_${language} PRIVATE cmfm_api)
    cmfm_project_target(cmfm_abi_${language})
    add_test(NAME plugin_abi_${language} COMMAND cmfm_abi_${language})
endforeach()

add_executable(cmfm_plugin_tests tests/test_plugins.cpp)
target_link_libraries(cmfm_plugin_tests PRIVATE cmfm_plugins)
cmfm_project_target(cmfm_plugin_tests)
add_test(NAME plugin_manifest_order COMMAND cmfm_plugin_tests)

# The plugin host against fixture plugins, one DLL per declared case.
add_executable(cmfm_plugin_host_tests tests/test_plugin_host.cpp)
target_link_libraries(cmfm_plugin_host_tests PRIVATE cmfm_plugins)
cmfm_project_target(cmfm_plugin_host_tests)
set(CMFM_PLUGIN_FIXTURES "${CMAKE_BINARY_DIR}/plugin_fixtures/$<CONFIG>")
function(cmfm_plugin_fixture name)
    add_library(cmfm_plugin_fixture_${name} SHARED tests/plugin_fixture.cpp)
    target_link_libraries(cmfm_plugin_fixture_${name} PRIVATE cmfm_api)
    target_compile_definitions(cmfm_plugin_fixture_${name} PRIVATE "FIXTURE_NAME=\"${name}\"" ${ARGN})
    cmfm_project_target(cmfm_plugin_fixture_${name})
    set_target_properties(cmfm_plugin_fixture_${name} PROPERTIES
        OUTPUT_NAME ${name}
        RUNTIME_OUTPUT_DIRECTORY "${CMFM_PLUGIN_FIXTURES}")
    add_dependencies(cmfm_plugin_host_tests cmfm_plugin_fixture_${name})
endfunction()
cmfm_plugin_fixture(alpha)
cmfm_plugin_fixture(beta "FIXTURE_AFTER=\"gamma\"")
cmfm_plugin_fixture(gamma FIXTURE_ORDER=5)
cmfm_plugin_fixture(delta FIXTURE_ORDER=10 "FIXTURE_BEFORE=\"beta\"")
cmfm_plugin_fixture(cycle_x "FIXTURE_AFTER=\"cycle_y\"")
cmfm_plugin_fixture(cycle_y "FIXTURE_AFTER=\"cycle_x\"")
cmfm_plugin_fixture(newer_api FIXTURE_API=0x20000u)
cmfm_plugin_fixture(failing FIXTURE_FAIL FIXTURE_PHASES=0x1u)
cmfm_plugin_fixture(wrong_exe FIXTURE_WRONG_EXE)
cmfm_plugin_fixture(no_exports FIXTURE_NO_EXPORTS)
cmfm_plugin_fixture(phased FIXTURE_PHASES=0x21u)
add_test(NAME plugin_host
    COMMAND cmfm_plugin_host_tests "${CMFM_PLUGIN_FIXTURES}" "${CMAKE_BINARY_DIR}/plugin_host")

# A stand-in named MonsterHunterWorld.exe, so the launcher's whole path
# (suspended start, injection, confirmation, exit code) runs outside the game.
add_executable(fake_game WIN32 tests/fake_game.cpp)
target_link_libraries(fake_game PRIVATE cmfm_common shell32)
cmfm_project_target(fake_game)
set_target_properties(fake_game PROPERTIES
    OUTPUT_NAME MonsterHunterWorld
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_game/$<CONFIG>")

add_test(NAME launcher_injection
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:cmfm_launcher>"
        "-DCORE=$<TARGET_FILE:cmfm>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_game>"
        "-DWORK=${CMAKE_BINARY_DIR}/launcher_injection"
        -P "${CMAKE_SOURCE_DIR}/tests/launcher_injection.cmake")

# Same implementation; dialogs are compiled out only in this test binary.
add_executable(cmfm_test_launcher src/launcher/main.cpp)
target_link_libraries(cmfm_test_launcher PRIVATE cmfm_common shell32)
target_compile_definitions(cmfm_test_launcher PRIVATE CMFM_TEST_NO_DIALOGS)
cmfm_project_target(cmfm_test_launcher)
add_test(NAME launcher_failures
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:cmfm_test_launcher>"
        "-DCORE=$<TARGET_FILE:cmfm>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_game>"
        "-DWORK=${CMAKE_BINARY_DIR}/launcher_failures"
        -P "${CMAKE_SOURCE_DIR}/tests/launcher_failures.cmake")
set_tests_properties(launcher_injection launcher_failures PROPERTIES TIMEOUT 60)

foreach(mode IN ITEMS 1 2 3 4)
    add_library(cmfm_fixture_${mode} SHARED tests/core_fixture.cpp)
    target_compile_definitions(cmfm_fixture_${mode} PRIVATE CMFM_FIXTURE_MODE=${mode})
    cmfm_project_target(cmfm_fixture_${mode})
    if(mode EQUAL 3)
        set(expected_result 1)
    else()
        set(expected_result 37)
    endif()
    if(mode EQUAL 2)
        set(expected_core 0)
    else()
        set(expected_core 1)
    endif()
    add_test(NAME launcher_core_failure_${mode}
        COMMAND "${CMAKE_COMMAND}"
            "-DLAUNCHER=$<TARGET_FILE:cmfm_test_launcher>"
            "-DCORE=$<TARGET_FILE:cmfm_fixture_${mode}>"
            "-DFAKE_GAME=$<TARGET_FILE:fake_game>"
            "-DWORK=${CMAKE_BINARY_DIR}/launcher_core_failure_${mode}"
            "-DEXPECT_RESULT=${expected_result}"
            "-DEXPECT_CORE=${expected_core}"
            "-DEXPECT_EXE_HASH_FAILURE=$<IF:$<EQUAL:${mode},4>,1,0>"
            -P "${CMAKE_SOURCE_DIR}/tests/launcher_continuation.cmake")
    set_tests_properties(launcher_core_failure_${mode} PROPERTIES TIMEOUT 45)
endforeach()
endif()
