# Installs the plugin author package and builds its hello example on its
# own, as a plugin author would: only what the package contains.
cmake_minimum_required(VERSION 3.25)
file(REMOVE_RECURSE "${WORK}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --config "${CONFIG}" --component sdk --prefix "${WORK}/sdk"
    RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "installing the sdk component failed: ${result}")
endif()
foreach(file IN ITEMS include/al/al_plugin.h examples/hello/hello.c docs/plugin-guide.md CMakeLists.txt README.md LICENSE)
    if(NOT EXISTS "${WORK}/sdk/${file}")
        message(FATAL_ERROR "the plugin author package lacks ${file}")
    endif()
endforeach()
set(platform)
if(PLATFORM)
    set(platform -A "${PLATFORM}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/sdk" -B "${WORK}/build" -G "${GENERATOR}" ${platform}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "configuring the package's example failed: ${output}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK}/build" --config Release
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result EQUAL 0 OR NOT EXISTS "${WORK}/build/Release/hello.dll")
    message(FATAL_ERROR "building the package's example failed: ${output}")
endif()
