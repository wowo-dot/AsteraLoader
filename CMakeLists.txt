cmake_minimum_required(VERSION 3.25)

project(astera_loader VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Astera Loader targets 64-bit Windows only")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Astera Loader currently requires MSVC")
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

# Static CRT: the early-loaded AsteraLoader.dll should import no DLL beyond KERNEL32.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Git QUIET)
set(AL_GIT_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE AL_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

# Project code builds with warnings as errors. Third-party code, when there is
# some, gets its own targets without these options.
function(al_project_target target)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8
        "$<$<COMPILE_LANGUAGE:CXX>:/permissive-;/Zc:__cplusplus>"
        "$<$<COMPILE_LANGUAGE:CXX>:$<IF:$<BOOL:$<TARGET_PROPERTY:AL_TEST_C_EXCEPTIONS>>,/EHs,/EHsc>>")
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
        AL_BUILD_VERSION=L"${PROJECT_VERSION}-dev"
        AL_GIT_COMMIT=L"${AL_GIT_COMMIT}")
    target_link_options(${target} PRIVATE /WX)
endfunction()

# Install layout: dist/<config>/AsteraLoader.exe and dist/<config>/AsteraLoader/AsteraLoader.dll.
set(AL_DIST "${CMAKE_BINARY_DIR}/dist/$<CONFIG>")

add_library(al_common STATIC
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
target_include_directories(al_common PUBLIC src)
al_project_target(al_common)

# The public plugin API: one C header, the only thing plugins build against.
add_library(al_api INTERFACE)
target_include_directories(al_api INTERFACE include)

# Plugin manifests and load order (core side of the plugin API).
add_library(al_plugins STATIC
    src/core/input/service.cpp
    src/core/input/service.hpp
    src/core/crash/handler.cpp
    src/core/crash/handler.hpp
    src/core/crash/report.cpp
    src/core/crash/report.hpp
    src/core/frame/clock.cpp
    src/core/frame/clock.hpp
    src/core/game_hooks/registry.cpp
    src/core/game_hooks/registry.hpp
    src/core/graphics/hub.cpp
    src/core/graphics/hub.hpp
    src/core/graphics/layers.cpp
    src/core/plugins/discovery.cpp
    src/core/plugins/discovery.hpp
    src/core/plugins/host.cpp
    src/core/plugins/host.hpp
    src/core/plugins/guard.cpp
    src/core/plugins/guard.hpp
    src/core/plugins/load_order.cpp
    src/core/plugins/load_order.hpp
    src/core/plugins/manifest.cpp
    src/core/plugins/manifest.hpp
    src/core/plugins/services.cpp
    src/core/plugins/services.hpp)
target_link_libraries(al_plugins PUBLIC al_api al_common)
al_project_target(al_plugins)

# Process-only thread IDs for MinHook; project code keeps /W4 /WX.
add_library(al_thread_snapshot STATIC src/core/hooks/thread_snapshot.cpp src/core/hooks/thread_snapshot.hpp)
target_include_directories(al_thread_snapshot PUBLIC src)
al_project_target(al_thread_snapshot)

# MinHook 1.3.4, c3fcafdc10146beb5919319d0683e44e3c30d537.
# Vendored source; BSD-2 notices in third_party/minhook/LICENSE.txt.
add_library(al_minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(al_minhook PUBLIC third_party/minhook/include)
target_compile_definitions(al_minhook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX AL_MINHOOK_PROCESS_THREADS)
target_link_libraries(al_minhook PRIVATE al_thread_snapshot)

set(AL_STARTUP_SOURCES
    src/core/input/native.cpp
    src/core/input/native.hpp
    src/core/input/sites.cpp
    src/core/input/sites.hpp
    src/core/frame/layer.cpp
    src/core/frame/layer.hpp
    src/core/frame/sites.cpp
    src/core/frame/sites.hpp
    src/core/game_hooks/installer.cpp
    src/core/game_hooks/installer.hpp
    src/core/graphics/installer.cpp
    src/core/graphics/installer.hpp
    src/core/graphics/interface_limits.cpp
    src/core/phases/imports.cpp
    src/core/phases/sites.cpp
    src/core/phases/runtime.cpp
    src/core/startup/entry_cache.cpp
    src/core/startup/entry_cache.hpp
    src/core/startup/entry_signature.cpp
    src/core/startup/entry_signature.hpp
    src/core/startup/protect_trace.hpp
    src/core/startup/runtime.cpp
    src/core/startup/runtime.hpp)
add_library(al_startup STATIC ${AL_STARTUP_SOURCES})
target_link_libraries(al_startup PUBLIC al_plugins PRIVATE al_minhook)
al_project_target(al_startup)

add_executable(al_launcher WIN32 src/launcher/main.cpp)
target_link_libraries(al_launcher PRIVATE al_common shell32 user32)
al_project_target(al_launcher)
set_target_properties(al_launcher PROPERTIES OUTPUT_NAME AsteraLoader RUNTIME_OUTPUT_DIRECTORY "${AL_DIST}")

add_library(al_loader SHARED src/core/dllmain.cpp)
target_link_libraries(al_loader PRIVATE al_startup)
al_project_target(al_loader)
set_target_properties(al_loader PROPERTIES OUTPUT_NAME AsteraLoader RUNTIME_OUTPUT_DIRECTORY "${AL_DIST}/AsteraLoader")

# Package only production binaries and the licence, never stand-ins or fixtures.
install(FILES "$<TARGET_FILE:al_launcher>" DESTINATION .)
install(FILES "$<TARGET_FILE:al_loader>" DESTINATION AsteraLoader)
install(FILES LICENSE DESTINATION .)
install(FILES third_party/minhook/LICENSE.txt DESTINATION licences RENAME MinHook-BSD-2.txt)
install(FILES third_party/notices/SharpPluginLoader-MIT.txt DESTINATION licences)

# Stracker's Loader compatibility, shipped with AL as a plugin:
# AsteraLoader/plugins/stracker_compat/stracker_compat.dll (the AL plugin) and
# loader.dll beside it, which exports Stracker's exact ABI. Like Stracker's
# loader.dll it is MSVC release with the static CRT (the project default):
# Stracker plugins run their own inline operator<< on its std::stringstream.
set(AL_STRACKER_DIST "${AL_DIST}/AsteraLoader/plugins/stracker_compat")
add_library(al_stracker_logic STATIC
    plugins/stracker_compat/stracker_logic.cpp
    plugins/stracker_compat/stracker_logic.hpp)
target_include_directories(al_stracker_logic PUBLIC plugins/stracker_compat)
al_project_target(al_stracker_logic)

add_library(al_stracker_loader SHARED
    plugins/stracker_compat/loader.cpp
    plugins/stracker_compat/loader_abi.h
    plugins/stracker_compat/shim.h)
target_link_libraries(al_stracker_loader PRIVATE al_stracker_logic al_common)
al_project_target(al_stracker_loader)
set_target_properties(al_stracker_loader PROPERTIES
    OUTPUT_NAME loader
    RUNTIME_OUTPUT_DIRECTORY "${AL_STRACKER_DIST}")

add_library(al_stracker_compat SHARED plugins/stracker_compat/stracker_compat.cpp)
target_link_libraries(al_stracker_compat PRIVATE al_api)
target_include_directories(al_stracker_compat PRIVATE plugins/stracker_compat)
al_project_target(al_stracker_compat)
set_target_properties(al_stracker_compat PROPERTIES
    OUTPUT_NAME stracker_compat
    RUNTIME_OUTPUT_DIRECTORY "${AL_STRACKER_DIST}")

install(FILES "$<TARGET_FILE:al_stracker_compat>" "$<TARGET_FILE:al_stracker_loader>"
    DESTINATION AsteraLoader/plugins/stracker_compat)
install(FILES third_party/notices/StrackerLoader-MIT.txt third_party/notices/StrackerLoader-log-BSD-2.txt
    DESTINATION licences)

# The smallest AL plugin, in C, as an example for plugin authors. It is not
# part of the AL package: `cmake --install <build> --component examples`
# stages it in its own mod layout, AsteraLoader/plugins/hello/hello.dll.
add_library(al_hello SHARED examples/hello/hello.c)
target_link_libraries(al_hello PRIVATE al_api)
al_project_target(al_hello)
set_target_properties(al_hello PROPERTIES
    OUTPUT_NAME hello
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/$<CONFIG>/AsteraLoader/plugins/hello")
install(FILES "$<TARGET_FILE:al_hello>" DESTINATION AsteraLoader/plugins/hello COMPONENT examples EXCLUDE_FROM_ALL)

# probes/site_probe.cpp found the frame and exit sites for API 1.5, and
# probes/input_probe.cpp the input the game reads for API 1.6. al.frame and
# al.input replaced them (tested in game: frame-input-1, input-check-3), and
# they are no longer built.

# A test plugin for al.input in game, staged with the examples for test runs
# only: hotkeys toggle capture, as a menu would; it logs counts only.
add_library(al_input_check SHARED probes/input_check.cpp)
target_link_libraries(al_input_check PRIVATE al_api)
al_project_target(al_input_check)
set_target_properties(al_input_check PROPERTIES
    OUTPUT_NAME input_check
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/$<CONFIG>/AsteraLoader/plugins/input_check")
install(FILES "$<TARGET_FILE:al_input_check>" DESTINATION AsteraLoader/plugins/input_check COMPONENT examples EXCLUDE_FROM_ALL)

include(CTest)
if(BUILD_TESTING)

add_executable(al_thread_snapshot_tests tests/test_thread_snapshot.cpp)
target_link_libraries(al_thread_snapshot_tests PRIVATE al_thread_snapshot)
al_project_target(al_thread_snapshot_tests)
add_test(NAME hook_thread_snapshot COMMAND al_thread_snapshot_tests)

add_executable(al_hook_thread_tests tests/test_hook_threads.cpp)
target_link_libraries(al_hook_thread_tests PRIVATE al_minhook)
al_project_target(al_hook_thread_tests)
add_test(NAME hook_threads COMMAND al_hook_thread_tests)

# Force enumeration failure only here to exercise unchanged Toolhelp fallback.
add_library(al_minhook_fallback STATIC
    third_party/minhook/src/buffer.c third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c
    tests/thread_snapshot_failure.c)
target_include_directories(al_minhook_fallback PUBLIC third_party/minhook/include)
target_compile_definitions(al_minhook_fallback PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX AL_MINHOOK_PROCESS_THREADS)
add_executable(al_hook_fallback_tests tests/test_hook_threads.cpp)
target_link_libraries(al_hook_fallback_tests PRIVATE al_minhook_fallback)
al_project_target(al_hook_fallback_tests)
add_test(NAME hook_thread_fallback COMMAND al_hook_fallback_tests)
set_tests_properties(hook_thread_snapshot hook_threads hook_thread_fallback PROPERTIES TIMEOUT 30)

add_executable(al_tests tests/test_common.cpp)
target_link_libraries(al_tests PRIVATE al_common shell32)
al_project_target(al_tests)
add_test(NAME common COMMAND al_tests)

add_executable(al_core_tests tests/test_core.cpp)
target_link_libraries(al_core_tests PRIVATE al_common)
al_project_target(al_core_tests)
add_test(NAME core_startup_abi COMMAND al_core_tests "$<TARGET_FILE:al_loader>")

# The plugin ABI layout, compiled as C and as C++ (tests/abi_layout.h).
foreach(language IN ITEMS c cpp)
    add_executable(al_abi_${language} tests/test_abi.${language})
    target_link_libraries(al_abi_${language} PRIVATE al_api)
    al_project_target(al_abi_${language})
    add_test(NAME plugin_abi_${language} COMMAND al_abi_${language})
endforeach()

# OpenAL uses the AL_ prefix too; the header must compile after its names.
add_executable(al_openal_names tests/test_openal_names.c)
target_link_libraries(al_openal_names PRIVATE al_api)
al_project_target(al_openal_names)
add_test(NAME openal_names COMMAND al_openal_names)

add_executable(al_plugin_tests tests/test_plugins.cpp)
target_link_libraries(al_plugin_tests PRIVATE al_plugins)
al_project_target(al_plugin_tests)
add_test(NAME plugin_manifest_order COMMAND al_plugin_tests)

add_executable(al_d3d12_hub_tests tests/test_d3d12_hub.cpp)
target_link_libraries(al_d3d12_hub_tests PRIVATE al_plugins)
al_project_target(al_d3d12_hub_tests)
add_test(NAME d3d12_hub COMMAND al_d3d12_hub_tests)

add_executable(al_graphics_installer_tests tests/test_graphics_installer.cpp)
target_link_libraries(al_graphics_installer_tests PRIVATE al_startup al_minhook d3d12 dxgi)
al_project_target(al_graphics_installer_tests)
add_test(NAME graphics_installer COMMAND al_graphics_installer_tests)
set_tests_properties(graphics_installer PROPERTIES TIMEOUT 30)

# The crash handler: child processes crash a fixture loaded from plugin paths.
add_library(al_crash_fixture SHARED tests/crash_fixture.cpp)
al_project_target(al_crash_fixture)
set_target_properties(al_crash_fixture PROPERTIES
    OUTPUT_NAME crash_fixture
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/crash_fixture/$<CONFIG>")
add_executable(al_crash_tests tests/test_crash.cpp)
target_link_libraries(al_crash_tests PRIVATE al_plugins)
al_project_target(al_crash_tests)
add_dependencies(al_crash_tests al_crash_fixture)
add_test(NAME crash_handler
    COMMAND al_crash_tests "$<TARGET_FILE:al_crash_fixture>" "${CMAKE_BINARY_DIR}/crash_handler")
set_tests_properties(crash_handler PROPERTIES TIMEOUT 180)

add_executable(al_game_hooks_tests tests/test_game_hooks.cpp)
target_link_libraries(al_game_hooks_tests PRIVATE al_plugins)
al_project_target(al_game_hooks_tests)
add_test(NAME game_hooks COMMAND al_game_hooks_tests)

# "al.frame" core side: order, threads, faults, the last frame, no allocation per frame.
add_executable(al_frame_clock_tests tests/test_frame_clock.cpp)
target_link_libraries(al_frame_clock_tests PRIVATE al_plugins)
al_project_target(al_frame_clock_tests)
add_test(NAME frame_clock COMMAND al_frame_clock_tests)

add_executable(al_input_tests tests/test_input.cpp)
target_link_libraries(al_input_tests PRIVATE al_plugins)
al_project_target(al_input_tests)
add_test(NAME input_service COMMAND al_input_tests)
add_executable(al_input_native_tests tests/test_input_native.cpp)
target_link_libraries(al_input_native_tests PRIVATE al_startup user32)
al_project_target(al_input_native_tests)
add_test(NAME input_native COMMAND al_input_native_tests)
add_test(NAME input_native_refusal COMMAND al_input_native_tests refusal)
add_test(NAME input_native_unavailable COMMAND al_input_native_tests unavailable)
add_test(NAME input_native_late COMMAND al_input_native_tests late)
set_tests_properties(input_service input_native input_native_refusal input_native_unavailable input_native_late PROPERTIES TIMEOUT 30)
add_library(al_input_fixture SHARED tests/input_plugin.cpp)
target_link_libraries(al_input_fixture PRIVATE al_api)
al_project_target(al_input_fixture)
set_target_properties(al_input_fixture PROPERTIES OUTPUT_NAME input_fixture RUNTIME_OUTPUT_DIRECTORY "${AL_PLUGIN_FIXTURES}")
add_executable(al_input_host_tests tests/test_input_host.cpp)
target_link_libraries(al_input_host_tests PRIVATE al_plugins)
al_project_target(al_input_host_tests)
add_test(NAME input_host COMMAND al_input_host_tests "$<TARGET_FILE:al_input_fixture>" "${CMAKE_BINARY_DIR}/input_host")
set_tests_properties(input_host PROPERTIES TIMEOUT 30)

add_executable(al_frame_layer_tests tests/test_frame_layer.cpp)
target_link_libraries(al_frame_layer_tests PRIVATE al_startup)
al_project_target(al_frame_layer_tests)
foreach(mode IN ITEMS identity bad_signature first_failure second_failure title_failure ready_rules cost)
    add_test(NAME frame_layer_${mode} COMMAND al_frame_layer_tests "${mode}" "${CMAKE_BINARY_DIR}/frame_layer_tests")
    set_tests_properties(frame_layer_${mode} PROPERTIES TIMEOUT 30)
endforeach()

# Same runtime and hook layer, with synthetic sites in place of the MHW-only
# resolver. These targets are test-only and are never installed or packaged.
set(AL_FRAME_FIXTURE_SOURCES ${AL_STARTUP_SOURCES})
list(REMOVE_ITEM AL_FRAME_FIXTURE_SOURCES src/core/frame/sites.cpp)
add_library(al_frame_runtime_fixture STATIC ${AL_FRAME_FIXTURE_SOURCES} tests/frame_sites_fixture.cpp)
target_link_libraries(al_frame_runtime_fixture PUBLIC al_plugins PRIVATE al_minhook)
al_project_target(al_frame_runtime_fixture)
add_library(al_frame_loader_fixture SHARED src/core/dllmain.cpp)
target_link_libraries(al_frame_loader_fixture PRIVATE al_frame_runtime_fixture)
al_project_target(al_frame_loader_fixture)
set_target_properties(al_frame_loader_fixture PROPERTIES OUTPUT_NAME AsteraLoader
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/frame_loader_fixture/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/frame_loader_fixture/$<CONFIG>")
add_executable(fake_frames_game WIN32 tests/fake_frames_game.cpp tests/fake_packed_entry.cpp)
target_link_libraries(fake_frames_game PRIVATE al_common)
target_compile_definitions(fake_frames_game PRIVATE AL_FAKE_PHASES)
target_link_options(fake_frames_game PRIVATE /ENTRY:al_test_packed_entry)
al_project_target(fake_frames_game)
set_target_properties(fake_frames_game PROPERTIES OUTPUT_NAME MonsterHunterWorld
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_frames_game/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_frames_game/$<CONFIG>")
foreach(role IN ITEMS probe reflex)
    add_library(al_frame_${role} SHARED tests/frame_plugin.cpp)
    target_link_libraries(al_frame_${role} PRIVATE al_api)
    al_project_target(al_frame_${role})
    set_target_properties(al_frame_${role} PROPERTIES OUTPUT_NAME frame_${role}
        RUNTIME_OUTPUT_DIRECTORY "${AL_PLUGIN_FIXTURES}")
endforeach()
target_compile_definitions(al_frame_reflex PRIVATE FRAME_REFLEX)
foreach(mode IN ITEMS normal backup)
    add_test(NAME frame_layer_${mode} COMMAND "${CMAKE_COMMAND}" "-DLAUNCHER=$<TARGET_FILE:al_launcher>"
        "-DCORE=$<TARGET_FILE:al_frame_loader_fixture>" "-DFAKE_GAME=$<TARGET_FILE:fake_frames_game>"
        "-DPROBE=$<TARGET_FILE:al_frame_probe>" "-DREFLEX=$<TARGET_FILE:al_frame_reflex>"
        "-DBACKUP=$<STREQUAL:${mode},backup>" "-DWORK=${CMAKE_BINARY_DIR}/frame_layer_${mode}"
        -P "${CMAKE_SOURCE_DIR}/tests/frame_layer.cmake")
    set_tests_properties(frame_layer_${mode} PROPERTIES TIMEOUT 45)
endforeach()

add_executable(al_game_hook_installer_tests tests/test_game_hook_installer.cpp)
target_link_libraries(al_game_hook_installer_tests PRIVATE al_startup al_minhook)
al_project_target(al_game_hook_installer_tests)
add_test(NAME game_hook_installer COMMAND al_game_hook_installer_tests)
set_tests_properties(game_hook_installer PROPERTIES TIMEOUT 30)

add_executable(al_services_tests tests/test_services.cpp)
target_link_libraries(al_services_tests PRIVATE al_plugins)
al_project_target(al_services_tests)
add_test(NAME services COMMAND al_services_tests "${CMAKE_BINARY_DIR}/services")

# The plugin host against fixture plugins, one DLL per declared case.
add_executable(al_plugin_host_tests tests/test_plugin_host.cpp)
target_link_libraries(al_plugin_host_tests PRIVATE al_plugins)
al_project_target(al_plugin_host_tests)
set(AL_PLUGIN_FIXTURES "${CMAKE_BINARY_DIR}/plugin_fixtures/$<CONFIG>")
function(al_plugin_fixture name)
    add_library(al_plugin_fixture_${name} SHARED tests/plugin_fixture.cpp)
    target_link_libraries(al_plugin_fixture_${name} PRIVATE al_api)
    target_compile_definitions(al_plugin_fixture_${name} PRIVATE "FIXTURE_NAME=\"${name}\"" ${ARGN})
    al_project_target(al_plugin_fixture_${name})
    set_target_properties(al_plugin_fixture_${name} PROPERTIES
        OUTPUT_NAME ${name}
        RUNTIME_OUTPUT_DIRECTORY "${AL_PLUGIN_FIXTURES}")
    add_dependencies(al_plugin_host_tests al_plugin_fixture_${name})
endfunction()
al_plugin_fixture(alpha)
al_plugin_fixture(beta "FIXTURE_AFTER=\"gamma\"")
al_plugin_fixture(gamma FIXTURE_ORDER=5)
al_plugin_fixture(delta FIXTURE_ORDER=10 "FIXTURE_BEFORE=\"beta\"")
al_plugin_fixture(cycle_x "FIXTURE_AFTER=\"cycle_y\"")
al_plugin_fixture(cycle_y "FIXTURE_AFTER=\"cycle_x\"")
al_plugin_fixture(newer_api FIXTURE_API=0x20000u)
al_plugin_fixture(failing FIXTURE_FAIL FIXTURE_PHASES=0x1u)
al_plugin_fixture(wrong_exe FIXTURE_WRONG_EXE)
al_plugin_fixture(no_exports FIXTURE_NO_EXPORTS)
al_plugin_fixture(phased FIXTURE_PHASES=0x21u)
al_plugin_fixture(pre_main_probe FIXTURE_RUNTIME FIXTURE_PHASES=0x1u)
al_plugin_fixture(seh_info FIXTURE_FAULT_INFO)
al_plugin_fixture(seh_init FIXTURE_NATIVE_INIT FIXTURE_PHASES=0x1u)
al_plugin_fixture(seh_phase FIXTURE_NATIVE_PHASE FIXTURE_PHASES=0x1u)
al_plugin_fixture(cpp_init FIXTURE_THROW_INIT)
# This fixture deliberately violates the no-exceptions C plugin contract.
set_target_properties(al_plugin_fixture_cpp_init PROPERTIES AL_TEST_C_EXCEPTIONS TRUE)
# API 1.1: dependencies, a plugin built for 1.0, start-up timing.
al_plugin_fixture(needs_missing "FIXTURE_DEPENDS=\"ghost\"")
al_plugin_fixture(needs_newer "FIXTURE_DEPENDS=\"provider\"" "FIXTURE_DEPENDS_VERSION=\"9.0\"")
al_plugin_fixture(needs_failing "FIXTURE_DEPENDS=\"failing\"")
al_plugin_fixture(legacy10 FIXTURE_API=0x10000u FIXTURE_INFO_SIZE=72u "FIXTURE_DEPENDS=\"BadName\"")
al_plugin_fixture(slow FIXTURE_SLEEP_MS=30)
# API 1.1: a plugin publishing an interface and one using it and the services.
add_executable(al_plugin_services_tests tests/test_plugin_services.cpp)
target_link_libraries(al_plugin_services_tests PRIVATE al_plugins)
al_project_target(al_plugin_services_tests)
foreach(role IN ITEMS provider consumer)
    add_library(al_plugin_fixture_${role} SHARED tests/plugin_fixture_services.cpp)
    target_link_libraries(al_plugin_fixture_${role} PRIVATE al_api)
    al_project_target(al_plugin_fixture_${role})
    set_target_properties(al_plugin_fixture_${role} PROPERTIES
        OUTPUT_NAME ${role}
        RUNTIME_OUTPUT_DIRECTORY "${AL_PLUGIN_FIXTURES}")
    add_dependencies(al_plugin_services_tests al_plugin_fixture_${role})
endforeach()
target_compile_definitions(al_plugin_fixture_provider PRIVATE FIXTURE_PROVIDER)
add_dependencies(al_plugin_services_tests al_plugin_fixture_needs_missing al_plugin_fixture_needs_newer
    al_plugin_fixture_failing al_plugin_fixture_needs_failing al_plugin_fixture_legacy10
    al_plugin_fixture_slow)
add_test(NAME plugin_services
    COMMAND al_plugin_services_tests "${AL_PLUGIN_FIXTURES}" "${CMAKE_BINARY_DIR}/plugin_services")
add_test(NAME plugin_host
    COMMAND al_plugin_host_tests "${AL_PLUGIN_FIXTURES}" "${CMAKE_BINARY_DIR}/plugin_host")

add_executable(al_example_hello_tests tests/test_example_hello.cpp)
target_link_libraries(al_example_hello_tests PRIVATE al_plugins)
al_project_target(al_example_hello_tests)
add_dependencies(al_example_hello_tests al_hello)
add_test(NAME example_hello
    COMMAND al_example_hello_tests "$<TARGET_FILE:al_hello>" "${CMAKE_BINARY_DIR}/example_hello")

add_executable(al_stracker_logic_tests tests/test_stracker_logic.cpp)
target_link_libraries(al_stracker_logic_tests PRIVATE al_stracker_logic)
al_project_target(al_stracker_logic_tests)
add_test(NAME stracker_logic COMMAND al_stracker_logic_tests)

add_executable(al_pattern_tests tests/test_pattern.cpp)
target_link_libraries(al_pattern_tests PRIVATE al_common)
al_project_target(al_pattern_tests)
add_test(NAME pattern COMMAND al_pattern_tests)

# A stand-in Stracker plugin that imports loader.dll, kept away from the shim.
add_library(al_stracker_plugin_fixture SHARED tests/stracker_plugin_fixture.cpp)
target_link_libraries(al_stracker_plugin_fixture PRIVATE al_stracker_loader)
target_include_directories(al_stracker_plugin_fixture PRIVATE plugins/stracker_compat)
al_project_target(al_stracker_plugin_fixture)
set_target_properties(al_stracker_plugin_fixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stracker_fixture/$<CONFIG>")
add_executable(al_stracker_shim_tests tests/test_stracker_shim.cpp)
target_include_directories(al_stracker_shim_tests PRIVATE plugins/stracker_compat)
al_project_target(al_stracker_shim_tests)
add_dependencies(al_stracker_shim_tests al_stracker_loader al_stracker_plugin_fixture)
add_test(NAME stracker_shim
    COMMAND al_stracker_shim_tests "$<TARGET_FILE:al_stracker_loader>"
        "$<TARGET_FILE:al_stracker_plugin_fixture>" "${CMAKE_BINARY_DIR}/stracker_shim")
# The same, with AL passing the game version it already found.
add_test(NAME stracker_shim_known_version
    COMMAND al_stracker_shim_tests "$<TARGET_FILE:al_stracker_loader>"
        "$<TARGET_FILE:al_stracker_plugin_fixture>" "${CMAKE_BINARY_DIR}/stracker_shim" known)

add_executable(al_plugin_fault_tests tests/test_plugin_faults.cpp)
target_link_libraries(al_plugin_fault_tests PRIVATE al_plugins)
al_project_target(al_plugin_fault_tests)
add_dependencies(al_plugin_fault_tests al_plugin_host_tests)
add_test(NAME plugin_faults COMMAND al_plugin_fault_tests "${AL_PLUGIN_FIXTURES}" "${CMAKE_BINARY_DIR}/plugin_faults")

add_executable(al_startup_signature_tests tests/test_startup_signature.cpp)
target_link_libraries(al_startup_signature_tests PRIVATE al_startup)
al_project_target(al_startup_signature_tests)
add_test(NAME startup_signature COMMAND al_startup_signature_tests)
add_executable(al_entry_cache_tests tests/test_entry_cache.cpp)
target_link_libraries(al_entry_cache_tests PRIVATE al_startup)
al_project_target(al_entry_cache_tests)
add_test(NAME startup_entry_cache COMMAND al_entry_cache_tests "${CMAKE_BINARY_DIR}/entry_cache")

add_executable(al_protect_trace_tests tests/test_protect_trace.cpp)
target_link_libraries(al_protect_trace_tests PRIVATE al_common)
al_project_target(al_protect_trace_tests)
add_test(NAME startup_protect_trace COMMAND al_protect_trace_tests)

add_executable(al_phase_site_tests tests/test_phase_sites.cpp)
target_link_libraries(al_phase_site_tests PRIVATE al_startup)
al_project_target(al_phase_site_tests)
add_test(NAME phase_sites COMMAND al_phase_site_tests)

add_executable(fake_phases_game WIN32 tests/fake_phases_game.cpp tests/fake_packed_entry.cpp)
target_link_libraries(fake_phases_game PRIVATE al_common d3d12 dxgi delayimp)
target_compile_definitions(fake_phases_game PRIVATE AL_FAKE_PHASES)
target_link_options(fake_phases_game PRIVATE /ENTRY:al_test_packed_entry /DELAYLOAD:d3d12.dll)
al_project_target(fake_phases_game)
set_target_properties(fake_phases_game PROPERTIES OUTPUT_NAME MonsterHunterWorld
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_phases_game/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_phases_game/$<CONFIG>")
add_library(al_phase_probe SHARED tests/phase_probe.cpp)
target_link_libraries(al_phase_probe PRIVATE al_api al_common al_minhook d3d12)
al_project_target(al_phase_probe)
set_target_properties(al_phase_probe PROPERTIES OUTPUT_NAME phase_probe
    RUNTIME_OUTPUT_DIRECTORY "${AL_PLUGIN_FIXTURES}")
add_test(NAME remaining_phases
    COMMAND "${CMAKE_COMMAND}" "-DLAUNCHER=$<TARGET_FILE:al_launcher>" "-DCORE=$<TARGET_FILE:al_loader>"
    "-DFAKE_GAME=$<TARGET_FILE:fake_phases_game>" "-DPLUGIN=$<TARGET_FILE:al_phase_probe>"
    "-DWORK=${CMAKE_BINARY_DIR}/remaining_phases" -P "${CMAKE_SOURCE_DIR}/tests/remaining_phases.cmake")
set_tests_properties(remaining_phases PROPERTIES TIMEOUT 45)
add_test(NAME remaining_phases_setup_thread
    COMMAND "${CMAKE_COMMAND}" "-DLAUNCHER=$<TARGET_FILE:al_launcher>" "-DCORE=$<TARGET_FILE:al_loader>"
    "-DFAKE_GAME=$<TARGET_FILE:fake_phases_game>" "-DPLUGIN=$<TARGET_FILE:al_phase_probe>"
    "-DDEVICE_THREAD=1" "-DWORK=${CMAKE_BINARY_DIR}/remaining_phases_setup_thread"
    -P "${CMAKE_SOURCE_DIR}/tests/remaining_phases.cmake")
set_tests_properties(remaining_phases_setup_thread PROPERTIES TIMEOUT 45)

add_executable(fake_graphics_game WIN32 tests/fake_graphics_game.cpp tests/fake_packed_entry.cpp)
target_link_libraries(fake_graphics_game PRIVATE al_common d3d12 dxgi delayimp user32)
target_compile_definitions(fake_graphics_game PRIVATE AL_FAKE_PHASES)
target_link_options(fake_graphics_game PRIVATE /ENTRY:al_test_packed_entry /DELAYLOAD:d3d12.dll)
al_project_target(fake_graphics_game)
set_target_properties(fake_graphics_game PROPERTIES OUTPUT_NAME MonsterHunterWorld
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_graphics_game/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_graphics_game/$<CONFIG>")
foreach(role IN ITEMS owner probe)
    add_library(al_graphics_${role} SHARED tests/graphics_plugin.cpp)
    target_link_libraries(al_graphics_${role} PRIVATE al_api al_minhook d3d12 dxgi)
    al_project_target(al_graphics_${role})
    set_target_properties(al_graphics_${role} PROPERTIES OUTPUT_NAME graphics_${role}
        RUNTIME_OUTPUT_DIRECTORY "${AL_PLUGIN_FIXTURES}")
endforeach()
target_compile_definitions(al_graphics_owner PRIVATE GRAPHICS_OWNER)
add_test(NAME graphics_native_chains COMMAND "${CMAKE_COMMAND}"
    "-DLAUNCHER=$<TARGET_FILE:al_launcher>" "-DCORE=$<TARGET_FILE:al_loader>"
    "-DFAKE_GAME=$<TARGET_FILE:fake_graphics_game>" "-DOWNER=$<TARGET_FILE:al_graphics_owner>"
    "-DPROBE=$<TARGET_FILE:al_graphics_probe>" "-DWORK=${CMAKE_BINARY_DIR}/graphics_native_chains"
    -P "${CMAKE_SOURCE_DIR}/tests/graphics.cmake")
set_tests_properties(graphics_native_chains PROPERTIES TIMEOUT 45)

add_executable(fake_game_hooks WIN32 tests/fake_game_hooks.cpp tests/fake_packed_entry.cpp)
target_link_libraries(fake_game_hooks PRIVATE al_common)
target_compile_definitions(fake_game_hooks PRIVATE AL_FAKE_PHASES)
target_link_options(fake_game_hooks PRIVATE /ENTRY:al_test_packed_entry)
al_project_target(fake_game_hooks)
set_target_properties(fake_game_hooks PROPERTIES OUTPUT_NAME MonsterHunterWorld
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_game_hooks/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_game_hooks/$<CONFIG>")
set(role_id 0)
foreach(role IN ITEMS alpha beta gamma)
    math(EXPR role_id "${role_id} + 1")
    add_library(al_game_hook_${role} SHARED tests/game_hook_plugin.cpp)
    target_link_libraries(al_game_hook_${role} PRIVATE al_api al_minhook)
    target_compile_definitions(al_game_hook_${role} PRIVATE GAME_HOOK_ROLE=${role_id})
    al_project_target(al_game_hook_${role})
    set_target_properties(al_game_hook_${role} PROPERTIES OUTPUT_NAME hook_${role}
        RUNTIME_OUTPUT_DIRECTORY "${AL_PLUGIN_FIXTURES}")
endforeach()
add_test(NAME game_hooks_native COMMAND "${CMAKE_COMMAND}" "-DLAUNCHER=$<TARGET_FILE:al_launcher>"
    "-DCORE=$<TARGET_FILE:al_loader>" "-DFAKE_GAME=$<TARGET_FILE:fake_game_hooks>"
    "-DPLUGIN_alpha=$<TARGET_FILE:al_game_hook_alpha>" "-DPLUGIN_beta=$<TARGET_FILE:al_game_hook_beta>"
    "-DPLUGIN_gamma=$<TARGET_FILE:al_game_hook_gamma>" "-DWORK=${CMAKE_BINARY_DIR}/game_hooks_native"
    -P "${CMAKE_SOURCE_DIR}/tests/game_hooks_native.cmake")
set_tests_properties(game_hooks_native PROPERTIES TIMEOUT 45)

# A stand-in named MonsterHunterWorld.exe, so the launcher's whole path
# (suspended start, injection, confirmation, exit code) runs outside the game.
add_executable(fake_game WIN32 tests/fake_game.cpp)
target_link_libraries(fake_game PRIVATE al_common shell32)
al_project_target(fake_game)
set_target_properties(fake_game PROPERTIES
    OUTPUT_NAME MonsterHunterWorld
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_game/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_game/$<CONFIG>")

add_executable(fake_packed_game WIN32 tests/fake_game.cpp tests/fake_packed_entry.cpp)
target_link_libraries(fake_packed_game PRIVATE al_common shell32)
target_compile_definitions(fake_packed_game PRIVATE AL_FAKE_PACKED)
al_project_target(fake_packed_game)
# Only this test entry runs before its own CRT; it uses plain Win32 calls.
set_source_files_properties(tests/fake_packed_entry.cpp PROPERTIES COMPILE_OPTIONS /GS-)
target_link_options(fake_packed_game PRIVATE /ENTRY:al_test_packed_entry)
set_target_properties(fake_packed_game PROPERTIES
    OUTPUT_NAME MonsterHunterWorld
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_packed_game/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_packed_game/$<CONFIG>")
add_test(NAME startup_phase
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:al_launcher>"
        "-DCORE=$<TARGET_FILE:al_loader>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_packed_game>"
        "-DPLUGIN=$<TARGET_FILE:al_plugin_fixture_pre_main_probe>"
        "-DWORK=${CMAKE_BINARY_DIR}/startup_phase"
        -P "${CMAKE_SOURCE_DIR}/tests/startup_phase.cmake")
set_tests_properties(startup_phase PROPERTIES TIMEOUT 45)
add_test(NAME startup_helper_transition
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:al_launcher>"
        "-DCORE=$<TARGET_FILE:al_loader>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_packed_game>"
        "-DPLUGIN=$<TARGET_FILE:al_plugin_fixture_pre_main_probe>"
        "-DWORK=${CMAKE_BINARY_DIR}/startup_helper_transition"
        -DEXPECT_HELPER=1
        -P "${CMAKE_SOURCE_DIR}/tests/startup_phase.cmake")
set_tests_properties(startup_helper_transition PROPERTIES TIMEOUT 45)
add_test(NAME startup_cached_transition
    COMMAND "${CMAKE_COMMAND}" "-DLAUNCHER=$<TARGET_FILE:al_launcher>"
        "-DCORE=$<TARGET_FILE:al_loader>" "-DFAKE_GAME=$<TARGET_FILE:fake_packed_game>"
        "-DPLUGIN=$<TARGET_FILE:al_plugin_fixture_pre_main_probe>"
        "-DWORK=${CMAKE_BINARY_DIR}/startup_cached_transition"
        -P "${CMAKE_SOURCE_DIR}/tests/startup_cache.cmake")
set_tests_properties(startup_cached_transition PROPERTIES TIMEOUT 60)
add_test(NAME startup_trace_overflow
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:al_launcher>"
        "-DCORE=$<TARGET_FILE:al_loader>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_packed_game>"
        "-DPLUGIN=$<TARGET_FILE:al_plugin_fixture_pre_main_probe>"
        "-DWORK=${CMAKE_BINARY_DIR}/startup_trace_overflow"
        -DEXPECT_TRUNCATION=1
        -P "${CMAKE_SOURCE_DIR}/tests/startup_phase.cmake")
set_tests_properties(startup_trace_overflow PROPERTIES TIMEOUT 45)
add_test(NAME startup_missing_transition
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:al_launcher>"
        "-DCORE=$<TARGET_FILE:al_loader>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_packed_game>"
        "-DPLUGIN=$<TARGET_FILE:al_plugin_fixture_pre_main_probe>"
        "-DWORK=${CMAKE_BINARY_DIR}/startup_missing_transition"
        -DEXPECT_SKIP=1
        -P "${CMAKE_SOURCE_DIR}/tests/startup_phase.cmake")
set_tests_properties(startup_missing_transition PROPERTIES TIMEOUT 45)

add_test(NAME launcher_injection
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:al_launcher>"
        "-DCORE=$<TARGET_FILE:al_loader>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_game>"
        "-DWORK=${CMAKE_BINARY_DIR}/launcher_injection"
        -P "${CMAKE_SOURCE_DIR}/tests/launcher_injection.cmake")

# Same implementation; dialogs are compiled out only in this test binary.
add_executable(al_test_launcher src/launcher/main.cpp)
target_link_libraries(al_test_launcher PRIVATE al_common shell32)
target_compile_definitions(al_test_launcher PRIVATE AL_TEST_NO_DIALOGS)
al_project_target(al_test_launcher)
add_test(NAME launcher_failures
    COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:al_test_launcher>"
        "-DCORE=$<TARGET_FILE:al_loader>"
        "-DFAKE_GAME=$<TARGET_FILE:fake_game>"
        "-DWORK=${CMAKE_BINARY_DIR}/launcher_failures"
        -P "${CMAKE_SOURCE_DIR}/tests/launcher_failures.cmake")
set_tests_properties(launcher_injection launcher_failures PROPERTIES TIMEOUT 60)

foreach(mode IN ITEMS 1 2 3 4)
    add_library(al_fixture_${mode} SHARED tests/core_fixture.cpp)
    target_compile_definitions(al_fixture_${mode} PRIVATE AL_FIXTURE_MODE=${mode})
    al_project_target(al_fixture_${mode})
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
            "-DLAUNCHER=$<TARGET_FILE:al_test_launcher>"
            "-DCORE=$<TARGET_FILE:al_fixture_${mode}>"
            "-DFAKE_GAME=$<TARGET_FILE:fake_game>"
            "-DWORK=${CMAKE_BINARY_DIR}/launcher_core_failure_${mode}"
            "-DEXPECT_RESULT=${expected_result}"
            "-DEXPECT_CORE=${expected_core}"
            "-DEXPECT_EXE_HASH_FAILURE=$<IF:$<EQUAL:${mode},4>,1,0>"
            -P "${CMAKE_SOURCE_DIR}/tests/launcher_continuation.cmake")
    set_tests_properties(launcher_core_failure_${mode} PROPERTIES TIMEOUT 45)
endforeach()
endif()
