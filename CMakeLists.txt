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

set(PLAYDATE_GAME_NAME FamiCrank)
set(PLAYDATE_GAME_DEVICE FamiCrank)

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
    third_party/pd-rom-picker/src/rom_picker.c
)


# Newlib syscall stubs are only needed on bare-metal device
if (TOOLCHAIN STREQUAL "armgcc")
    list(APPEND PLATFORM_SOURCES src/syscalls.c)
endif()

# pd-playbench: scripted-input benchmark harness, dev-only (never in release)
option(PD_PLAYBENCH "Build the pd-playbench scripted-input replay/benchmark (dev only)" OFF)
option(PD_PLAYBENCH_KIRBY "Replay the deterministic Kirby's Adventure 1-1 smoke test" OFF)
option(PD_PLAYBENCH_FIXED_SKIP "Use a deterministic draw/skip cadence during replay" OFF)
option(PD_PLAYBENCH_RECORD "Build the pd-playbench recorder: capture a script from live play (dev only)" OFF)
option(PD_PLAYBENCH_RECORD_KIRBY "Record Kirby via the ROM picker with distinct NES Start input" OFF)
if (PD_PLAYBENCH_KIRBY)
    set(PD_PLAYBENCH ON CACHE BOOL
        "Build the pd-playbench scripted-input replay/benchmark (dev only)" FORCE)
endif()
if (PD_PLAYBENCH_RECORD_KIRBY)
    set(PD_PLAYBENCH_RECORD ON CACHE BOOL
        "Build the pd-playbench recorder: capture a script from live play (dev only)" FORCE)
endif()
# Both modes link the library; RECORD additionally selects record behaviour.
if (PD_PLAYBENCH OR PD_PLAYBENCH_RECORD)
    list(APPEND PLATFORM_SOURCES third_party/pd-playbench/src/pd_playbench.c)
endif()

option(DIAG        "Enable performance diagnostics (logToConsole every 60 frames)" OFF)
option(ENABLE_LTO  "Enable link-time optimization for the device build" OFF)
option(ENABLE_LTO_NO_IPA_CLONE "Enable LTO without interprocedural constant-propagation cloning" OFF)
if (ENABLE_LTO_NO_IPA_CLONE)
    set(ENABLE_LTO ON CACHE BOOL "Enable link-time optimization for the device build" FORCE)
endif()
option(DIAG_DRAW_FPS "Draw Playdate's on-screen FPS HUD in diagnostic builds" OFF)
option(DIAG_FPS_ONLY "Log only FPS windows without per-frame render timing" OFF)
option(AUDIO       "Enable NES APU" OFF)
option(AUDIO_DIRECT_RING "Fill the Playdate audio ring directly instead of using a temp buffer" ON)
option(PPU_BG      "Enable PPU background rendering (OFF to measure cost)" ON)
option(PPU_SPRITES "Enable PPU sprite rendering (OFF to measure cost)" ON)
option(PPU_BLIT    "Enable Playdate scanline blit (OFF to measure cost)" ON)
option(PPU_BG_PAIR_FAST "Render the common no-latch background path two tiles at a time" OFF)
option(PPU_BG_PACKED_PAIR "Render no-latch background pairs with packed Cortex-M7 byte selects" OFF)
option(PPU_BG_QUAD_FAST "Render aligned no-latch background attribute groups four tiles at a time" OFF)
option(PPU_DIRECT_1BIT "Compose no-latch PPU scanlines directly into Playdate 1-bit rows" OFF)
option(PPU_BG_TILE_CACHE_SMALL "Cache decoded visible background tile rows in a small tagged table" OFF)
option(PPU_FAST_STRIKE "Return sprite-zero hit as soon as it is detected" OFF)
option(PPU_SPRITE_CACHE_DRAW_ONLY "Build the sprite render cache only on drawn frames" OFF)
option(PPU_FAST_OAMDMA "Copy sprite DMA from mapped CPU memory without 256 helper calls" OFF)
option(NES_RAM_DTCM "Place the 2KB NES work RAM in the DTCM stack pool instead of heap" OFF)
option(DTCM_POOL_SCAN "Paint the DTCM pool with sentinels and report the clean run (discovery probe)" OFF)
option(PPU_SPRITE_LIVE_CHR "Read sprite CHR live instead of the scanline-0 pattern cache (mid-frame bank-switch probe)" OFF)
option(NES_IRQ_MAPPER_BATCH "Batch CPU across IRQ-free scanlines for hblank-IRQ mappers (MMC3 etc.) using the mapper IRQ countdown" OFF)
option(NES_IRQ_MAPPER_BATCH_IRQ_SCOPE "Use per-scanline CPU execution from hardware IRQ entry through RTI" OFF)
option(ALIGN_PRG_ROM "Align iNES PRG ROM start for cache experiments" OFF)
option(NES_PRG_PAGE_COPY "Copy PRG into independently addressable 4KB pages" OFF)
option(NES_PRG_CACHE_COLOR "Skew copied 4KB PRG pages by one D-cache line" OFF)
option(DIAG_CPU_EXEC_TIMING "Time every nes6502_execute call in diagnostic builds" OFF)
set(NES_CPU_BATCH_SCANLINES "1" CACHE STRING
    "Run NES CPU cycles in batches this many scanlines wide for no-hblank mappers")
set(NES_CPU_CYCLE_PERCENT "100" CACHE STRING
    "Percent of normal CPU cycles to run per PPU scanline")
set(NES6502_OPT_LEVEL "O3" CACHE STRING
    "Optimization level for nes6502.c performance experiments")
option(NES6502_JUMPTABLE_DISPATCH "Use GCC computed-goto opcode dispatch instead of switch dispatch" OFF)
option(NES6502_LAZY_CYCLES "Commit total CPU cycles once per execute slice instead of once per opcode" OFF)
option(NES6502_ALIGN_LOOPS "Align 6502 hot loops for performance experiments" OFF)
option(NES6502_SPINHACK "Fast-forward selected PPUSTATUS polling loops" OFF)
option(NES6502_OPPROFILE "Count executed 6502 opcodes in diagnostic windows" OFF)
option(NES6502_PCPROFILE "Count executed 6502 instruction addresses for hot-block selection" OFF)
option(NES6502_PRGPROFILE "Histogram executed instructions per 4KB PRG page (DTCM-relocation candidate finder)" OFF)
option(NES_PRG_DTCM "Mirror the hottest PRG page ($C000) into DTCM via relocate-on-map" OFF)
option(NES6502_FAST_PC_OPS "Use pc_ptr for operand fetches and same-bank branches" OFF)
option(NES6502_HOTOPS "Hand-specialize the hottest measured 6502 opcodes" OFF)
option(NES6502_FAST_MEMIO "Bypass generic memory-handler scans for NES RAM mirror ranges" OFF)
option(NES6502_DIRECT_MEMIO "Bypass memory-handler scans for common RAM/PPU/APU/input ranges" OFF)
option(NES6502_DIRECT_CART_RAM "Bypass memory-handler scans for directly mapped $6000-$7FFF cartridge RAM" OFF)
option(NES6502_FAST_JMP_ABS "Fast-path 6502 JMP absolute operand fetch" OFF)
option(NES6502_FAST_JMP_INDIRECT "Fetch JMP-indirect operands through the cached PC pointer" OFF)
option(NES6502_FAST_BNE "Fast-path only the hot BNE relative branch" OFF)
option(NES6502_FAST_BPL "Fast-path only the hot BPL relative branch" OFF)
option(NES6502_FAST_BEQ "Fast-path only the hot BEQ relative branch" OFF)
option(NES6502_FAST_BRANCHES "Fast-path taken relative branches with pc_ptr" OFF)
option(NES6502_FAST_OPERAND_BYTES "Use pc_ptr for one-byte 6502 operand fetches" OFF)
option(NES6502_FAST_MEMOPS "Hand-specialize hot 6502 memory load/store opcodes" OFF)
option(NES6502_HOT_CLUSTER "Cluster measured-hot opcode case bodies first in the dispatch switch" OFF)
option(NES6502_JMP_SPIN "Fast-forward self-JMP idle loops to the end of the CPU slice" OFF)
option(NES6502_LINEAR_ROM "Fast-path CPU ROM reads when $8000-$FFFF is one contiguous host block" OFF)
option(NES6502_LINKED_CORE "Execute the linked 6502 core directly instead of copying it to heap SRAM" OFF)
option(NES6502_FUSE_INCDEC_BNE "Fuse common register inc/dec plus BNE interpreter pairs" OFF)
option(NES6502_PAIRPROFILE "Profile executed 6502 opcode transitions (diagnostic only)" OFF)
option(NES6502_CHAIN_F0_A5 "Chain the profiled-hot BEQ/LDA-zp opcode transitions" OFF)
option(NES6502_ZP_BEQ_SPIN "Fast-forward stable LDA-zp/BEQ-back polling loops" OFF)
option(NES6502_STA_ABSY_PAGEFILL "Collapse pattern-matched STA-abs,Y RAM page-fill loops" OFF)
option(NES6502_PAD_SERIAL_LOOP "Collapse pattern-matched eight-bit controller serial loops" OFF)
option(NES6502_RESIDUAL_COPY_LOOPS "Collapse profiled RAM-copy and PPU-stream loops" OFF)
option(NES6502_COMPACT_CORE "Compile the linked 6502 interpreter for Thumb-2 code density" OFF)
option(NES6502_DTCM_CODEGEN_PROBE "Generate and execute a tiny Thumb function in uncached DTCM" OFF)
option(NES6502_DTCM_PAGEFILL_BLOCK "Execute the page-fill native block from the DTCM code pool" OFF)
option(NES6502_DTCM_LOOKUP_BLOCK "Execute a profiled multi-instruction lookup/RTS block from DTCM" OFF)
option(NES6502_TCMHOT_PROBE "Relocate tiny 6502 hot-core probes into the Playdate DTCM stack pool" OFF)
option(NES6502_TCMHOT_CORE "Run a tiny relocated 6502 hot-opcode core before falling back" OFF)
option(NES6502_TCMHOT_CORE_STATS "Log DTCM hot-core hit/cycle attribution counters" OFF)
option(NES_FIXED_SCANLINE_CYCLES "Use fixed-point integer scanline cycle accumulation" OFF)
if (NES6502_TCMHOT_CORE_STATS)
    set(NES6502_TCMHOT_CORE ON CACHE BOOL "Run a tiny relocated 6502 hot-opcode core before falling back" FORCE)
endif()
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
    third_party/pd-rom-picker/src
    third_party/pd-playbench/src
)
if (AUDIO)
    list(APPEND GAME_INCLUDES src/nofrendo/sndhrdw)
endif()

string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")

set(GAME_DEFINES
    NDEBUG
    $<$<BOOL:${AUDIO}>:AUDIO=1>
    $<$<AND:$<BOOL:${AUDIO}>,$<BOOL:${AUDIO_DIRECT_RING}>>:AUDIO_DIRECT_RING=1>
    $<$<BOOL:${DIAG}>:DIAG=1>
    $<$<BOOL:${ENABLE_LTO}>:ENABLE_LTO=1>
    $<$<BOOL:${ENABLE_LTO_NO_IPA_CLONE}>:ENABLE_LTO_NO_IPA_CLONE=1>
    $<$<BOOL:${DIAG_DRAW_FPS}>:DIAG_DRAW_FPS=1>
    $<$<BOOL:${DIAG_FPS_ONLY}>:DIAG_FPS_ONLY=1>
    $<$<OR:$<BOOL:${PD_PLAYBENCH}>,$<BOOL:${PD_PLAYBENCH_RECORD}>>:PD_PLAYBENCH_ENABLED=1>
    $<$<OR:$<BOOL:${PD_PLAYBENCH}>,$<BOOL:${PD_PLAYBENCH_RECORD}>>:PD_PLAYBENCH_MAX_COMMANDS=1024>
    $<$<BOOL:${PD_PLAYBENCH_RECORD}>:PD_PLAYBENCH_RECORD=1>
    $<$<BOOL:${PD_PLAYBENCH_KIRBY}>:PD_PLAYBENCH_KIRBY=1>
    $<$<BOOL:${PD_PLAYBENCH_RECORD_KIRBY}>:PD_PLAYBENCH_RECORD_KIRBY=1>
    $<$<OR:$<BOOL:${PD_PLAYBENCH_KIRBY}>,$<BOOL:${PD_PLAYBENCH_RECORD_KIRBY}>>:PD_PLAYBENCH_FRESH_SRAM=1>
    $<$<BOOL:${DIAG}>:BUILD_TIMESTAMP="${BUILD_TIMESTAMP}">
    $<$<NOT:$<BOOL:${PPU_BG}>>:DISABLE_PPU_BG=1>
    $<$<NOT:$<BOOL:${PPU_SPRITES}>>:DISABLE_PPU_SPRITES=1>
    $<$<NOT:$<BOOL:${PPU_BLIT}>>:DISABLE_PPU_BLIT=1>
    $<$<BOOL:${PPU_BG_PAIR_FAST}>:PPU_BG_PAIR_FAST=1>
    $<$<BOOL:${PPU_BG_PACKED_PAIR}>:PPU_BG_PACKED_PAIR=1>
    $<$<BOOL:${PPU_DIRECT_1BIT}>:PPU_DIRECT_1BIT=1>
    $<$<BOOL:${PPU_BG_TILE_CACHE_SMALL}>:PPU_BG_TILE_CACHE_SMALL=1>
    $<$<BOOL:${PPU_FAST_STRIKE}>:PPU_FAST_STRIKE=1>
    $<$<BOOL:${PPU_SPRITE_CACHE_DRAW_ONLY}>:PPU_SPRITE_CACHE_DRAW_ONLY=1>
    $<$<BOOL:${PPU_FAST_OAMDMA}>:PPU_FAST_OAMDMA=1>
    $<$<BOOL:${NES_RAM_DTCM}>:NES_RAM_DTCM=1>
    $<$<BOOL:${DTCM_POOL_SCAN}>:DTCM_POOL_SCAN=1>
    $<$<BOOL:${PPU_SPRITE_LIVE_CHR}>:PPU_SPRITE_LIVE_CHR=1>
    $<$<BOOL:${NES_IRQ_MAPPER_BATCH}>:NES_IRQ_MAPPER_BATCH=1>
    $<$<BOOL:${NES_IRQ_MAPPER_BATCH_IRQ_SCOPE}>:NES_IRQ_MAPPER_BATCH_IRQ_SCOPE=1>
    $<$<BOOL:${ALIGN_PRG_ROM}>:ALIGN_PRG_ROM=1>
    $<$<OR:$<BOOL:${NES_PRG_PAGE_COPY}>,$<BOOL:${NES_PRG_CACHE_COLOR}>>:NES_PRG_PAGE_COPY=1>
    $<$<BOOL:${NES_PRG_CACHE_COLOR}>:NES_PRG_CACHE_COLOR=1>
    $<$<BOOL:${DIAG_CPU_EXEC_TIMING}>:DIAG_CPU_EXEC_TIMING=1>
    NES_CPU_BATCH_SCANLINES=${NES_CPU_BATCH_SCANLINES}
    NES_CPU_CYCLE_PERCENT=${NES_CPU_CYCLE_PERCENT}
    NES6502_OPT_LEVEL_LABEL="${NES6502_OPT_LEVEL}"
    $<$<BOOL:${NES6502_JUMPTABLE_DISPATCH}>:NES6502_JUMPTABLE_DISPATCH=1>
    $<$<BOOL:${NES6502_LAZY_CYCLES}>:NES6502_LAZY_CYCLES=1>
    $<$<BOOL:${NES6502_ALIGN_LOOPS}>:NES6502_ALIGN_LOOPS=1>
    $<$<BOOL:${NES6502_SPINHACK}>:NES6502_SPINHACK=1>
    $<$<BOOL:${NES6502_OPPROFILE}>:NES6502_OPPROFILE=1>
    $<$<BOOL:${NES6502_PCPROFILE}>:NES6502_PCPROFILE=1>
    $<$<BOOL:${NES6502_PRGPROFILE}>:NES6502_PRGPROFILE=1>
    $<$<BOOL:${NES_PRG_DTCM}>:NES_PRG_DTCM=1>
    $<$<BOOL:${NES6502_FAST_PC_OPS}>:NES6502_FAST_PC_OPS=1>
    $<$<BOOL:${NES6502_HOTOPS}>:NES6502_HOTOPS=1>
    $<$<BOOL:${NES6502_FAST_MEMIO}>:NES6502_FAST_MEMIO=1>
    $<$<BOOL:${NES6502_DIRECT_MEMIO}>:NES6502_DIRECT_MEMIO=1>
    $<$<BOOL:${NES6502_DIRECT_CART_RAM}>:NES6502_DIRECT_CART_RAM=1>
    $<$<BOOL:${NES6502_FAST_JMP_ABS}>:NES6502_FAST_JMP_ABS=1>
    $<$<BOOL:${NES6502_FAST_JMP_INDIRECT}>:NES6502_FAST_JMP_INDIRECT=1>
    $<$<BOOL:${NES6502_FAST_BNE}>:NES6502_FAST_BNE=1>
    $<$<BOOL:${NES6502_FAST_BPL}>:NES6502_FAST_BPL=1>
    $<$<BOOL:${NES6502_FAST_BEQ}>:NES6502_FAST_BEQ=1>
    $<$<BOOL:${NES6502_FAST_BRANCHES}>:NES6502_FAST_BRANCHES=1>
    $<$<BOOL:${NES6502_FAST_OPERAND_BYTES}>:NES6502_FAST_OPERAND_BYTES=1>
    $<$<BOOL:${NES6502_FAST_MEMOPS}>:NES6502_FAST_MEMOPS=1>
    $<$<BOOL:${NES6502_HOT_CLUSTER}>:NES6502_HOT_CLUSTER=1>
    $<$<BOOL:${NES6502_JMP_SPIN}>:NES6502_JMP_SPIN=1>
    $<$<BOOL:${NES6502_LINEAR_ROM}>:NES6502_LINEAR_ROM=1>
    $<$<BOOL:${NES6502_LINKED_CORE}>:NES6502_LINKED_CORE=1>
    $<$<BOOL:${NES6502_FUSE_INCDEC_BNE}>:NES6502_FUSE_INCDEC_BNE=1>
    $<$<BOOL:${NES6502_PAIRPROFILE}>:NES6502_PAIRPROFILE=1>
    $<$<BOOL:${NES6502_CHAIN_F0_A5}>:NES6502_CHAIN_F0_A5=1>
    $<$<BOOL:${NES6502_ZP_BEQ_SPIN}>:NES6502_ZP_BEQ_SPIN=1>
    $<$<BOOL:${NES6502_STA_ABSY_PAGEFILL}>:NES6502_STA_ABSY_PAGEFILL=1>
    $<$<BOOL:${NES6502_PAD_SERIAL_LOOP}>:NES6502_PAD_SERIAL_LOOP=1>
    $<$<BOOL:${NES6502_RESIDUAL_COPY_LOOPS}>:NES6502_RESIDUAL_COPY_LOOPS=1>
    $<$<BOOL:${NES6502_COMPACT_CORE}>:NES6502_COMPACT_CORE=1>
    $<$<BOOL:${NES6502_DTCM_CODEGEN_PROBE}>:NES6502_DTCM_CODEGEN_PROBE=1>
    $<$<BOOL:${NES6502_DTCM_PAGEFILL_BLOCK}>:NES6502_DTCM_PAGEFILL_BLOCK=1>
    $<$<BOOL:${NES6502_DTCM_LOOKUP_BLOCK}>:NES6502_DTCM_LOOKUP_BLOCK=1>
    $<$<BOOL:${NES6502_TCMHOT_PROBE}>:NES6502_TCMHOT_PROBE=1>
    $<$<BOOL:${NES6502_TCMHOT_CORE}>:NES6502_TCMHOT_CORE=1>
    $<$<BOOL:${NES6502_TCMHOT_CORE_STATS}>:NES6502_TCMHOT_CORE_STATS=1>
    $<$<BOOL:${NES_FIXED_SCANLINE_CYCLES}>:NES_FIXED_SCANLINE_CYCLES=1>
)

# Keep this PPU-only experiment out of the LTO command line for the 6502 core.
# On the F746, merely perturbing that core's LTO partition changes its hot
# layout enough to swamp sub-millisecond renderer measurements.
if (PPU_BG_QUAD_FAST)
    set_property(SOURCE
        src/nofrendo/nes/nes_ppu.c
        src/main.c
        APPEND PROPERTY COMPILE_DEFINITIONS PPU_BG_QUAD_FAST=1)
endif()

# Benchmark-only scheduling probe. Scope the define to the two consumers so it
# cannot perturb the 6502 translation unit's LTO input.
if (PD_PLAYBENCH_FIXED_SKIP)
    set_property(SOURCE
        src/main.c
        src/osd.c
        APPEND PROPERTY COMPILE_DEFINITIONS PD_PLAYBENCH_FIXED_SKIP=1)
endif()

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

if (TOOLCHAIN STREQUAL "armgcc")
    set(PDX_PACKAGE_DIR "${PRODUCT_DIR}/${PLAYDATE_GAME_DEVICE}.pdx")
    set(PDX_PACKAGE_TARGET ${PLAYDATE_GAME_DEVICE})
else()
    set(PDX_PACKAGE_DIR "${PRODUCT_DIR}/${PLAYDATE_GAME_NAME}.pdx")
    set(PDX_PACKAGE_TARGET ${PLAYDATE_GAME_NAME})
endif()

add_custom_command(
    TARGET ${PDX_PACKAGE_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "${PDX_PACKAGE_DIR}/cartridge.nes"
        "${PDX_PACKAGE_DIR}/._cartridge.nes"
)

# The SDK unconditionally adds -O2 as a PUBLIC flag, which comes after our -O3
# and wins. Re-append -O3 here so it is the last optimisation flag on the command line.
if (TOOLCHAIN STREQUAL "armgcc")
    target_compile_options(${PLAYDATE_GAME_DEVICE} PRIVATE -O3)
    if (ENABLE_LTO)
        target_compile_options(${PLAYDATE_GAME_DEVICE} PRIVATE -flto)
        target_link_options(${PLAYDATE_GAME_DEVICE} PRIVATE -flto)
        if (ENABLE_LTO_NO_IPA_CLONE)
            # Limit one source of interprocedural code growth while retaining
            # whole-program analysis and dead-code removal.
            target_compile_options(${PLAYDATE_GAME_DEVICE} PRIVATE -fno-ipa-cp-clone)
        endif()
        # Keep the SDK allocator bridge as a concrete object. GCC 9's LTO
        # plugin otherwise internalizes its newlib `_realloc_r` definition
        # before archive resolution and the final link cannot satisfy libc.
        set_source_files_properties(
            "${SDK}/C_API/buildsupport/setup.c"
            PROPERTIES COMPILE_OPTIONS "-fno-lto"
        )
    endif()

    # Keep 6502 flags measurable: code shape and I-cache pressure both matter.
    set(NES6502_COMPILE_FLAGS
        "-${NES6502_OPT_LEVEL} -DNES6502_LEGAL_ONLY -DNES6502_PC_PTR")
    if (NOT NES6502_JUMPTABLE_DISPATCH)
        string(APPEND NES6502_COMPILE_FLAGS " -DNES6502_SWITCH")
    endif()
    if (NES6502_ALIGN_LOOPS)
        string(APPEND NES6502_COMPILE_FLAGS " -falign-loops=32")
    endif()
    if (NES6502_COMPACT_CORE)
        # Keep the rest of the emulator at O3; only trade interpreter code
        # expansion for denser Thumb-2 here.  This flag is deliberately last.
        string(APPEND NES6502_COMPILE_FLAGS " -Os")
    endif()
    if (NES6502_TCMHOT_PROBE OR NES6502_TCMHOT_CORE)
        string(APPEND NES6502_COMPILE_FLAGS " -mlong-calls -fno-lto")
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
