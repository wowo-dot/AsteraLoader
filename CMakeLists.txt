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
        "$<$<COMPILE_LANGUAGE:CXX>:/permissive-;/Zc:__cplusplus>"
        "$<$<COMPILE_LANGUAGE:CXX>:$<IF:$<BOOL:$<TARGET_PROPERTY:CMFM_TEST_C_EXCEPTIONS>>,/EHs,/EHsc>>")
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
    src/common/game_version.cpp
    src/common/game_version.hpp
    src/common/log_file.cpp
    src/common/log_file.hpp
    src/common/memory.cpp
    src/common/memory.hpp
    src/common/paths.cpp
    src/common/paths.hpp
    src/common/pattern.cpp
    src/common/pattern.hpp
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
    src/core/plugins/guard.cpp
    src/core/plugins/guard.hpp
    src/core/plugins/load_order.cpp
    src/core/plugins/load_order.hpp
    src/core/plugins/manifest.cpp
    src/core/plugins/manifest.hpp)
target_link_libraries(cmfm_plugins PUBLIC cmfm_api cmfm_common)
cmfm_project_target(cmfm_plugins)

# Process-only thread IDs for MinHook; project code keeps /W4 /WX.
add_library(cmfm_thread_snapshot STATIC src/core/hooks/thread_snapshot.cpp src/core/hooks/thread_snapshot.hpp)
target_include_directories(cmfm_thread_snapshot PUBLIC src)
cmfm_project_target(cmfm_thread_snapshot)

# MinHook 1.3.4, c3fcafdc10146beb5919319d0683e44e3c30d537.
# Vendored source; BSD-2 notices in third_party/minhook/LICENSE.txt.
add_library(cmfm_minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(cmfm_minhook PUBLIC third_party/minhook/include)
target_compile_definitions(cmfm_minhook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX CMFM_MINHOOK_PROCESS_THREADS)
target_link_libraries(cmfm_minhook PRIVATE cmfm_thread_snapshot)

add_library(cmfm_startup STATIC
    src/core/startup/entry_signature.cpp
    src/core/startup/entry_signature.hpp
    src/core/startup/protect_trace.hpp
    src/core/startup/runtime.cpp
    src/core/startup/runtime.hpp)
target_link_libraries(cmfm_startup PUBLIC cmfm_plugins PRIVATE cmfm_minhook)
cmfm_project_target(cmfm_startup)

add_executable(cmfm_launcher WIN32 src/launcher/main.cpp)
target_link_libraries(cmfm_launcher PRIVATE cmfm_common shell32 user32)
cmfm_project_target(cmfm_launcher)
set_target_properties(cmfm_launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMFM_DIST}")

add_library(cmfm SHARED src/core/dllmain.cpp)
target_link_libraries(cmfm PRIVATE cmfm_startup)
cmfm_project_target(cmfm)
set_target_properties(cmfm PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMFM_DIST}/cmfm")

# Package only production binaries and the licence, never stand-ins or fixtures.
install(FILES "$<TARGET_FILE:cmfm_launcher>" DESTINATION .)
install(FILES "$<TARGET_FILE:cmfm>" DESTINATION cmfm)
install(FILES LICENSE DESTINATION .)
install(FILES third_party/minhook/LICENSE.txt DESTINATION licences RENAME MinHook-BSD-2.txt)
install(FILES third_party/notices/SharpPluginLoader-MIT.txt DESTINATION licences)

# Stracker's Loader compatibility, shipped with CMFM as a plugin:
# cmfm/plugins/stracker_compat/stracker_compat.dll (the CMFM plugin) and
# loader.dll beside it, which exports Stracker's exact ABI. Like Stracker's
# loader.dll it is MSVC release with the static CRT (the project default):
# Stracker plugins run their own inline operator<< on its std::stringstream.
set(CMFM_STRACKER_DIST "${CMFM_DIST}/cmfm/plugins/stracker_compat")
add_library(cmfm_stracker_logic STATIC
    plugins/stracker_compat/stracker_logic.cpp
    plugins/stracker_compat/stracker_logic.hpp)
target_include_directories(cmfm_stracker_logic PUBLIC plugins/stracker_compat)
cmfm_project_target(cmfm_stracker_logic)

add_library(cmfm_stracker_loader SHARED
    plugins/stracker_compat/loader.cpp
    plugins/stracker_compat/loader_abi.h
    plugins/stracker_compat/shim.h)
target_link_libraries(cmfm_stracker_loader PRIVATE cmfm_stracker_logic cmfm_common)
cmfm_project_target(cmfm_stracker_loader)
set_target_properties(cmfm_stracker_loader PROPERTIES
    OUTPUT_NAME loader
    RUNTIME_OUTPUT_DIRECTORY "${CMFM_STRACKER_DIST}")

add_library(cmfm_stracker_compat SHARED plugins/stracker_compat/stracker_compat.cpp)
target_link_libraries(cmfm_stracker_compat PRIVATE cmfm_api)
target_include_directories(cmfm_stracker_compat PRIVATE plugins/stracker_compat)
cmfm_project_target(cmfm_stracker_compat)
set_target_properties(cmfm_stracker_compat PROPERTIES
    OUTPUT_NAME stracker_compat
    RUNTIME_OUTPUT_DIRECTORY "${CMFM_STRACKER_DIST}")

install(FILES "$<TARGET_FILE:cmfm_stracker_compat>" "$<TARGET_FILE:cmfm_stracker_loader>"
    DESTINATION cmfm/plugins/stracker_compat)
install(FILES third_party/notices/StrackerLoader-MIT.txt third_party/notices/StrackerLoader-log-BSD-2.txt
    DESTINATION licences)

# The smallest CMFM plugin, in C, as an example for plugin authors. It is not
# part of the CMFM package: `cmake --install <build> --component examples`
# stages it in its own mod layout, cmfm/plugins/hello/hello.dll.
add_library(cmfm_hello SHARED examples/hello/hello.c)
target_link_libraries(cmfm_hello PRIVATE cmfm_api)
cmfm_project_target(cmfm_hello)
set_target_properties(cmfm_hello PROPERTIES
    OUTPUT_NAME hello
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/$<CONFIG>/cmfm/plugins/hello")
install(FILES "$<TARGET_FILE:cmfm_hello>" DESTINATION cmfm/plugins/hello COMPONENT examples EXCLUDE_FROM_ALL)

include(CTest)
if(BUILD_TESTING)

add_executable(cmfm_thread_snapshot_tests tests/test_thread_snapshot.cpp)
target_link_libraries(cmfm_thread_snapshot_tests PRIVATE cmfm_thread_snapshot)
cmfm_project_target(cmfm_thread_snapshot_tests)
add_test(NAME hook_thread_snapshot COMMAND cmfm_thread_snapshot_tests)

add_executable(cmfm_hook_thread_tests tests/test_hook_threads.cpp)
target_link_libraries(cmfm_hook_thread_tests PRIVATE cmfm_minhook)
cmfm_project_target(cmfm_hook_thread_tests)
add_test(NAME hook_threads COMMAND cmfm_hook_thread_tests)

# Force enumeration failure only here to exercise unchanged Toolhelp fallback.
add_library(cmfm_minhook_fallback STATIC
    third_party/minhook/src/buffer.c third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c
    tests/thread_snapshot_failure.c)
target_include_directories(cmfm_minhook_fallback PUBLIC third_party/minhook/include)
target_compile_definitions(cmfm_minhook_fallback PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX CMFM_MINHOOK_PROCESS_THREADS)
add_executable(cmfm_hook_fallback_tests tests/test_hook_threads.cpp)
target_link_libraries(cmfm_hook_fallback_tests PRIVATE cmfm_minhook_fallback)
cmfm_project_target(cmfm_hook_fallback_tests)
add_test(NAME hook_thread_fallback COMMAND cmfm_hook_fallback_tests)
set_tests_properties(hook_thread_snapshot hook_threads hook_thread_fallback PROPERTIES TIMEOUT 30)

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
cmfm_plugin_fixture(pre_main_probe FIXTURE_RUNTIME FIXTURE_PHASES=0x1u)
cmfm_plugin_fixture(seh_info FIXTURE_FAULT_INFO)
cmfm_plugin_fixture(seh_init FIXTURE_NATIVE_INIT FIXTURE_PHASES=0x1u)
cmfm_plugin_fixture(seh_phase FIXTURE_NATIVE_PHASE FIXTURE_PHASES=0x1u)
cmfm_plugin_fixture(cpp_init FIXTURE_THROW_INIT)
# This fixture deliberately violates the no-exceptions C plugin contract.
set_target_properties(cmfm_plugin_fixture_cpp_init PROPERTIES CMFM_TEST_C_EXCEPTIONS TRUE)
add_test(NAME plugin_host
    COMMAND cmfm_plugin_host_tests "${CMFM_PLUGIN_FIXTURES}" "${CMAKE_BINARY_DIR}/plugin_host")

add_executable(cmfm_example_hello_tests tests/test_example_hello.cpp)
target_link_libraries(cmfm_example_hello_tests PRIVATE cmfm_plugins)
cmfm_project_target(cmfm_example_hello_tests)
add_dependencies(cmfm_example_hello_tests cmfm_hello)
add_test(NAME example_hello
    COMMAND cmfm_example_hello_tests "$<TARGET_FILE:cmfm_hello>" "${CMAKE_BINARY_DIR}/example_hello")

add_executable(cmfm_stracker_logic_tests tests/test_stracker_logic.cpp)
target_link_libraries(cmfm_stracker_logic_tests PRIVATE cmfm_stracker_logic)
cmfm_project_target(cmfm_stracker_logic_tests)
add_test(NAME stracker_logic COMMAND cmfm_stracker_logic_tests)

add_executable(cmfm_pattern_tests tests/test_pattern.cpp)
target_link_libraries(cmfm_pattern_tests PRIVATE cmfm_common)
cmfm_project_target(cmfm_pattern_tests)
add_test(NAME pattern COMMAND cmfm_pattern_tests)

# A stand-in Stracker plugin that imports loader.dll, kept away from the shim.
add_library(cmfm_stracker_plugin_fixture SHARED tests/stracker_plugin_fixture.cpp)
target_link_libraries(cmfm_stracker_plugin_fixture PRIVATE cmfm_stracker_loader)
target_include_directories(cmfm_stracker_plugin_fixture PRIVATE plugins/stracker_compat)
cmfm_project_target(cmfm_stracker_plugin_fixture)
set_target_properties(cmfm_stracker_plugin_fixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stracker_fixture/$<CONFIG>")
add_executable(cmfm_stracker_shim_tests tests/test_stracker_shim.cpp)
target_include_directories(cmfm_stracker_shim_tests PRIVATE plugins/stracker_compat)
cmfm_project_target(cmfm_stracker_shim_tests)
add_dependencies(cmfm_stracker_shim_tests cmfm_stracker_loader cmfm_stracker_plugin_fixture)
add_test(NAME stracker_shim
    COMMAND cmfm_stracker_shim_tests "$<TARGET_FILE:cmfm_stracker_loader>"
        "$<TARGET_FILE:cmfm_stracker_plugin_fixture>" "${CMAKE_BINARY_DIR}/stracker_shim")

add_executable(cmfm_plugin_fault_tests tests/test_plugin_faults.cpp)
target_link_libraries(cmfm_plugin_fault_tests PRIVATE cmfm_plugins)
cmfm_project_target(cmfm_plugin_fault_tests)
add_dependencies(cmfm_plugin_fault_tests cmfm_plugin_host_tests)
add_test(NAME plugin_faults COMMAND cmfm_plugin_fault_tests "${CMFM_PLUGIN_FIXTURES}" "${CMAKE_BINARY_DIR}/plugin_faults")

add_executable(cmfm_startup_signature_tests tests/test_startup_signature.cpp)
target_link_libraries(cmfm_startup_signature_tests PRIVATE cmfm_startup)
cmfm_project_target(cmfm_startup_signature_tests)
add_test(NAME startup_signature COMMAND cmfm_startup_signature_tests)

add_executable(cmfm_protect_trace_tests tests/test_protect_trace.cpp)
target_link_libraries(cmfm_protect_trace_tests PRIVATE cmfm_common)
cmfm_project_target(cmfm_protect_trace_tests)
add_test(NAME startup_protect_trace COMMAND cmfm_protect_trace_tests)

# A stand-in named MonsterHunterWorld.exe, so the launcher's whole path
# (suspended start, injection, confirmation, exit code) runs outside the game.
add_executable(fake_game WIN32 tests/fake_game.cpp)
target_link_libraries(fake_game PRIVATE cmfm_common shell32)
cmfm_project_target(fake_game)
set_target_properties(fake_game PROPERTIES
    OUTPUT_NAME MonsterHunterWorld
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_game/$<CONFIG>")

add_executable(fake_packed_game WIN32 tests/fake_game.cpp tests/fake_packed_entry.cpp)
target_link_libraries(fake_packed_game PRIVATE cmfm_common shell32)
target_compile_definitions(fake_packed_game PRIVATE CMFM_FAKE_PACKED)
cmfm_project_target(fake_packed_game)
# Only this test entry runs before its own CRT; it uses plain Win32 calls.
set_source_files_properties(tests/fake_packed_entry.cpp PROPERTIES COMPILE_OPTIONS /GS-)
target_link_options(fake_packed_game PRIVATE /ENTRY:cmfm_test_packed_entry)
set_target_properties(fake_packed_game PROPERTIES
    OUTPUT_NAME MonsterHunterWorld
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_packed_game/$<CONFIG>")
add_test(NAME startup_phase
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:cmfm_launcher>"
        "-DCORE=$<TARGET_FILE:cmfm>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_packed_game>"
        "-DPLUGIN=$<TARGET_FILE:cmfm_plugin_fixture_pre_main_probe>"
        "-DWORK=${CMAKE_BINARY_DIR}/startup_phase"
        -P "${CMAKE_SOURCE_DIR}/tests/startup_phase.cmake")
set_tests_properties(startup_phase PROPERTIES TIMEOUT 45)
add_test(NAME startup_helper_transition
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:cmfm_launcher>"
        "-DCORE=$<TARGET_FILE:cmfm>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_packed_game>"
        "-DPLUGIN=$<TARGET_FILE:cmfm_plugin_fixture_pre_main_probe>"
        "-DWORK=${CMAKE_BINARY_DIR}/startup_helper_transition"
        -DEXPECT_HELPER=1
        -P "${CMAKE_SOURCE_DIR}/tests/startup_phase.cmake")
set_tests_properties(startup_helper_transition PROPERTIES TIMEOUT 45)
add_test(NAME startup_trace_overflow
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:cmfm_launcher>"
        "-DCORE=$<TARGET_FILE:cmfm>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_packed_game>"
        "-DPLUGIN=$<TARGET_FILE:cmfm_plugin_fixture_pre_main_probe>"
        "-DWORK=${CMAKE_BINARY_DIR}/startup_trace_overflow"
        -DEXPECT_TRUNCATION=1
        -P "${CMAKE_SOURCE_DIR}/tests/startup_phase.cmake")
set_tests_properties(startup_trace_overflow PROPERTIES TIMEOUT 45)
add_test(NAME startup_missing_transition
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:cmfm_launcher>"
        "-DCORE=$<TARGET_FILE:cmfm>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_packed_game>"
        "-DPLUGIN=$<TARGET_FILE:cmfm_plugin_fixture_pre_main_probe>"
        "-DWORK=${CMAKE_BINARY_DIR}/startup_missing_transition"
        -DEXPECT_SKIP=1
        -P "${CMAKE_SOURCE_DIR}/tests/startup_phase.cmake")
set_tests_properties(startup_missing_transition PROPERTIES TIMEOUT 45)

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
