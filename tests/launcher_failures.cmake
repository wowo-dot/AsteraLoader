cmake_minimum_required(VERSION 3.25)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run)
set(WORK "${WORK}/${run}")
file(MAKE_DIRECTORY "${WORK}/launcher/AsteraLoader" "${WORK}/game")
file(COPY_FILE "${LAUNCHER}" "${WORK}/launcher/AsteraLoader.exe")
file(COPY_FILE "${FAKE_GAME}" "${WORK}/game/MonsterHunterWorld.exe")
set(ENV{AL_FAKE_OUTPUT} "${WORK}/must-not-run.log")
set(ENV{AL_FAKE_EXIT} 37)

# Missing or invalid core must be detected before creating a child.
file(REMOVE "${WORK}/launcher/AsteraLoader/AsteraLoader.dll" "${WORK}/must-not-run.log")
execute_process(COMMAND "${WORK}/launcher/AsteraLoader.exe" "${WORK}/game/MonsterHunterWorld.exe"
    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE result TIMEOUT 30)
if(NOT result STREQUAL "1" OR EXISTS "${WORK}/must-not-run.log")
    message(FATAL_ERROR "Missing DLL did not fail before launch: ${result}")
endif()
file(WRITE "${WORK}/launcher/AsteraLoader/AsteraLoader.dll" "invalid PE")
execute_process(COMMAND "${WORK}/launcher/AsteraLoader.exe" "${WORK}/game/MonsterHunterWorld.exe"
    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE result TIMEOUT 30)
if(NOT result STREQUAL "1" OR EXISTS "${WORK}/must-not-run.log")
    message(FATAL_ERROR "Invalid DLL did not fail before launch: ${result}")
endif()
file(COPY_FILE "${CORE}" "${WORK}/launcher/AsteraLoader/AsteraLoader.dll")
execute_process(COMMAND "${WORK}/launcher/AsteraLoader.exe" "${WORK}/missing/MonsterHunterWorld.exe"
    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE result TIMEOUT 30)
if(NOT result STREQUAL "1" OR EXISTS "${WORK}/must-not-run.log")
    message(FATAL_ERROR "Missing explicit game did not fail: ${result}")
endif()

# Exercise the same implementation used by failure tests on a successful run.
set(ENV{AL_FAKE_OUTPUT} "${WORK}/success.log")
execute_process(COMMAND "${WORK}/launcher/AsteraLoader.exe" "${WORK}/game/MonsterHunterWorld.exe"
    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE result TIMEOUT 30)
if(NOT result STREQUAL "37")
    message(FATAL_ERROR "Headless launcher success returned ${result}")
endif()
