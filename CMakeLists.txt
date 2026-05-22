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

option(DIAG        "Enable performance diagnostics (logToConsole every 60 frames)" OFF)
option(AUDIO       "Enable NES APU" OFF)
option(PPU_BG      "Enable PPU background rendering (OFF to measure cost)" ON)
option(PPU_SPRITES "Enable PPU sprite rendering (OFF to measure cost)" ON)
option(PPU_BLIT    "Enable Playdate scanline blit (OFF to measure cost)" ON)
option(ALIGN_PRG_ROM "Align iNES PRG ROM start for cache experiments" OFF)
set(NES_CPU_BATCH_SCANLINES "1" CACHE STRING
    "Run NES CPU cycles in batches this many scanlines wide for no-hblank mappers")
set(NES6502_OPT_LEVEL "O3" CACHE STRING
    "Optimization level for nes6502.c performance experiments")
option(NES6502_ALIGN_LOOPS "Align 6502 hot loops for performance experiments" OFF)
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

string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")

set(GAME_DEFINES
    NDEBUG
    $<$<BOOL:${AUDIO}>:AUDIO=1>
    $<$<BOOL:${DIAG}>:DIAG=1>
    $<$<BOOL:${DIAG}>:BUILD_TIMESTAMP="${BUILD_TIMESTAMP}">
    $<$<NOT:$<BOOL:${PPU_BG}>>:DISABLE_PPU_BG=1>
    $<$<NOT:$<BOOL:${PPU_SPRITES}>>:DISABLE_PPU_SPRITES=1>
    $<$<NOT:$<BOOL:${PPU_BLIT}>>:DISABLE_PPU_BLIT=1>
    $<$<BOOL:${ALIGN_PRG_ROM}>:ALIGN_PRG_ROM=1>
    NES_CPU_BATCH_SCANLINES=${NES_CPU_BATCH_SCANLINES}
    NES6502_OPT_LEVEL_LABEL="${NES6502_OPT_LEVEL}"
    $<$<BOOL:${NES6502_ALIGN_LOOPS}>:NES6502_ALIGN_LOOPS=1>
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

    # Keep 6502 flags measurable: code shape and I-cache pressure both matter.
    set(NES6502_COMPILE_FLAGS
        "-${NES6502_OPT_LEVEL} -DNES6502_SWITCH -DNES6502_LEGAL_ONLY -DNES6502_PC_PTR")
    if (NES6502_ALIGN_LOOPS)
        string(APPEND NES6502_COMPILE_FLAGS " -falign-loops=32")
    endif()
    set_source_files_properties(
        src/nofrendo/cpu/nes6502.c
        PROPERTIES COMPILE_FLAGS "${NES6502_COMPILE_FLAGS}"
    )
    # nes_ppu.c stays at -O3: ppu_scanline (2.3 KB) already fits alongside
    # nes6502_execute in the 16 KB I-cache, so -Os would only make it slower.

    # Replace the SDK's linker script with our custom one, which pins nes6502_execute
    # first in .text at a 32-byte boundary so it starts clean on a cache line.
    get_target_property(_link_opts ${PLAYDATE_GAME_DEVICE} LINK_OPTIONS)
    set(_sdk_ld "-T${SDK}/C_API/buildsupport/link_map.ld")
    set(_our_ld "-T${CMAKE_CURRENT_SOURCE_DIR}/link_map.ld")
    list(FIND _link_opts "${_sdk_ld}" _idx)
    if(NOT _idx EQUAL -1)
        list(REMOVE_AT _link_opts ${_idx})
        list(INSERT _link_opts ${_idx} "${_our_ld}")
        set_target_properties(${PLAYDATE_GAME_DEVICE} PROPERTIES LINK_OPTIONS "${_link_opts}")
    endif()
endif()
