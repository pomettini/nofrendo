cmake_minimum_required(VERSION 3.14)
set(CMAKE_C_STANDARD 11)

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})
if (NOT ${ENVSDK} STREQUAL "")
    file(TO_CMAKE_PATH ${ENVSDK} SDK)
else()
    execute_process(
        COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config"
        COMMAND head -n 1
        COMMAND cut -c9-
        OUTPUT_VARIABLE SDK
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

if (NOT EXISTS ${SDK})
    message(FATAL_ERROR "SDK path not found; set PLAYDATE_SDK_PATH or install the Playdate SDK")
endif()

set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
if (NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

set(PLAYDATE_GAME_NAME nofrendo)
set(PLAYDATE_GAME_DEVICE nofrendo)

project(${PLAYDATE_GAME_NAME} C ASM)

# Nofrendo emulator core
file(GLOB_RECURSE NOFRENDO_CORE CONFIGURE_DEPENDS
    src/nofrendo/*.c
    src/nofrendo/cpu/*.c
    src/nofrendo/nes/*.c
    src/nofrendo/libsnss/*.c
    src/nofrendo/mappers/*.c
)
list(FILTER NOFRENDO_CORE EXCLUDE REGEX ".*/sndhrdw/.*")
# These are replaced by the platform layer (stubs.c / display.c)
list(FILTER NOFRENDO_CORE EXCLUDE REGEX ".*/gui\\.c$")
list(FILTER NOFRENDO_CORE EXCLUDE REGEX ".*/gui_elem\\.c$")
list(FILTER NOFRENDO_CORE EXCLUDE REGEX ".*/intro\\.c$")
list(FILTER NOFRENDO_CORE EXCLUDE REGEX ".*/vid_drv\\.c$")
# Save states removed — stubbed in stubs.c
list(FILTER NOFRENDO_CORE EXCLUDE REGEX ".*/nesstate\\.c$")
list(FILTER NOFRENDO_CORE EXCLUDE REGEX ".*/libsnss/.*")

# Platform layer
set(PLATFORM_SOURCES
    src/main.c
    src/osd.c
    src/display.c
    src/keyboard.c
    src/timing.c
    src/sound.c
    src/stubs.c
    src/diag.c
)


# Newlib syscall stubs are only needed on bare-metal device
if (TOOLCHAIN STREQUAL "armgcc")
    list(APPEND PLATFORM_SOURCES src/syscalls.c)
endif()

option(DIAG  "Enable performance diagnostics (logToConsole every 60 frames)" OFF)
option(AUDIO "Enable NES APU" OFF)
if (AUDIO)
    file(GLOB AUDIO_SOURCES src/nofrendo/sndhrdw/*.c)
    list(APPEND NOFRENDO_CORE ${AUDIO_SOURCES})
endif()

set(GAME_INCLUDES
    src
    src/nofrendo
    src/nofrendo/nes
    src/nofrendo/libsnss
    src/nofrendo/cpu
)
if (AUDIO)
    list(APPEND GAME_INCLUDES src/nofrendo/sndhrdw)
endif()

set(GAME_DEFINES
    NDEBUG
    $<$<BOOL:${AUDIO}>:AUDIO=1>
    $<$<BOOL:${DIAG}>:DIAG=1>
)

# add_executable / add_library must come before including playdate_game.cmake
if (TOOLCHAIN STREQUAL "armgcc")
    add_executable(${PLAYDATE_GAME_DEVICE}
        ${NOFRENDO_CORE}
        ${PLATFORM_SOURCES}
    )
    target_include_directories(${PLAYDATE_GAME_DEVICE} PRIVATE ${GAME_INCLUDES})
    target_compile_definitions(${PLAYDATE_GAME_DEVICE} PRIVATE ${GAME_DEFINES})
else()
    add_library(${PLAYDATE_GAME_NAME} SHARED
        ${NOFRENDO_CORE}
        ${PLATFORM_SOURCES}
    )
    target_include_directories(${PLAYDATE_GAME_NAME} PRIVATE ${GAME_INCLUDES})
    target_compile_definitions(${PLAYDATE_GAME_NAME} PRIVATE ${GAME_DEFINES})
endif()

include(${SDK}/C_API/buildsupport/playdate_game.cmake)

# The SDK unconditionally adds -O2 as a PUBLIC flag, which comes after our -O3
# and wins. Re-append -O3 here so it is the last optimisation flag on the command line.
if (TOOLCHAIN STREQUAL "armgcc")
    target_compile_options(${PLAYDATE_GAME_DEVICE} PRIVATE -O3)
endif()
