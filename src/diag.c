#ifdef DIAG

#include <pd_api.h>
#include <stdbool.h>
#include <stdint.h>
#include "diag.h"
#if defined(NES6502_OPPROFILE) || defined(NES6502_TCMHOT_CORE_STATS) || defined(NES6502_PRGPROFILE) || defined(NES_PRG_DTCM)
#include "nes6502.h"
#endif

extern PlaydateAPI *pd;

#define WINDOW 60
#define DIAG_STRINGIFY_INNER(value) #value
#define DIAG_STRINGIFY(value) DIAG_STRINGIFY_INNER(value)

#ifdef DIAG_FPS_ONLY
#define DIAG_MODE "fps"
#else
#define DIAG_MODE "split"
#endif

#if AUDIO
#define DIAG_AUDIO "on"
#ifdef AUDIO_DIRECT_RING
#define DIAG_AUDIO_FILL "direct"
#else
#define DIAG_AUDIO_FILL "copy"
#endif
#else
#define DIAG_AUDIO "off"
#define DIAG_AUDIO_FILL "off"
#endif

#ifdef DISABLE_PPU_BG
#define DIAG_BG "off"
#else
#define DIAG_BG "on"
#endif

#ifdef DISABLE_PPU_SPRITES
#define DIAG_SPRITES "off"
#else
#define DIAG_SPRITES "on"
#endif

#ifdef DISABLE_PPU_BLIT
#define DIAG_BLIT "off"
#else
#define DIAG_BLIT "on"
#endif

#ifdef DIAG_DRAW_FPS
#define DIAG_HUD_FPS "on"
#define DIAG_LCD_DIRTY "all"
#else
#define DIAG_HUD_FPS "off"
#define DIAG_LCD_DIRTY "draw"
#endif

#ifdef PPU_FAST_STRIKE
#define DIAG_PPU_STRIKE "early"
#else
#define DIAG_PPU_STRIKE "cycle"
#endif

#ifdef PPU_SPRITE_LIVE_CHR
#define DIAG_PPU_SPRCACHE "live"
#elif defined(PPU_SPRITE_CACHE_DRAW_ONLY)
#define DIAG_PPU_SPRCACHE "draw"
#else
#define DIAG_PPU_SPRCACHE "all"
#endif

#ifdef PPU_FAST_OAMDMA
#define DIAG_PPU_OAMDMA "fast"
#else
#define DIAG_PPU_OAMDMA "byte"
#endif

#ifdef NES_FIXED_SCANLINE_CYCLES
#define DIAG_CYCLE_ACCUM "fixed3"
#else
#define DIAG_CYCLE_ACCUM "float"
#endif

#ifndef NES_CPU_CYCLE_PERCENT
#define NES_CPU_CYCLE_PERCENT 100
#endif

#ifdef ALIGN_PRG_ROM
#define DIAG_PRG_ALIGN "16k"
#else
#define DIAG_PRG_ALIGN "off"
#endif

#ifdef NES6502_ALIGN_LOOPS
#define DIAG_CPU_LOOP_ALIGN "32"
#else
#define DIAG_CPU_LOOP_ALIGN "off"
#endif

#ifdef NES6502_JUMPTABLE_DISPATCH
#define DIAG_CPU_DISPATCH "jump"
#else
#define DIAG_CPU_DISPATCH "switch"
#endif

#ifdef NES6502_LAZY_CYCLES
#define DIAG_CPU_CYCLES "lazy"
#else
#define DIAG_CPU_CYCLES "global"
#endif

#ifdef NES6502_SPINHACK
#define DIAG_CPU_SPIN "ppustatus"
#else
#define DIAG_CPU_SPIN "off"
#endif

#ifdef NES6502_OPPROFILE
#define DIAG_CPU_PROF "opcode"
#else
#define DIAG_CPU_PROF "off"
#endif

#ifdef NES6502_FAST_PC_OPS
#define DIAG_CPU_FASTPC "on"
#else
#define DIAG_CPU_FASTPC "off"
#endif

#ifdef NES6502_HOTOPS
#define DIAG_CPU_HOTOPS "on"
#else
#define DIAG_CPU_HOTOPS "off"
#endif

#ifdef NES6502_DIRECT_MEMIO
#define DIAG_CPU_MEMIO "direct"
#elif defined(NES6502_FAST_MEMIO)
#define DIAG_CPU_MEMIO "ram"
#else
#define DIAG_CPU_MEMIO "table"
#endif

#ifdef NES6502_FAST_JMP_ABS
#define DIAG_CPU_FASTJMP "on"
#else
#define DIAG_CPU_FASTJMP "off"
#endif

#ifdef NES6502_FAST_BNE
#define DIAG_CPU_FASTBNE "on"
#else
#define DIAG_CPU_FASTBNE "off"
#endif

#ifdef NES6502_FAST_BPL
#define DIAG_CPU_FASTBPL "on"
#else
#define DIAG_CPU_FASTBPL "off"
#endif

#ifdef NES6502_FAST_BEQ
#define DIAG_CPU_FASTBEQ "on"
#else
#define DIAG_CPU_FASTBEQ "off"
#endif

#ifdef NES6502_FAST_BRANCHES
#define DIAG_CPU_FASTBRANCH "on"
#else
#define DIAG_CPU_FASTBRANCH "off"
#endif

#ifdef NES6502_FAST_OPERAND_BYTES
#define DIAG_CPU_FASTOPBYTE "on"
#else
#define DIAG_CPU_FASTOPBYTE "off"
#endif

#ifdef NES6502_FAST_MEMOPS
#define DIAG_CPU_FASTMEMOPS "on"
#else
#define DIAG_CPU_FASTMEMOPS "off"
#endif

#ifdef NES6502_HOT_CLUSTER
#define DIAG_CPU_HOTCLUSTER "on"
#else
#define DIAG_CPU_HOTCLUSTER "off"
#endif

#ifdef NES_IRQ_MAPPER_BATCH
#define DIAG_IRQ_BATCH "on"
#else
#define DIAG_IRQ_BATCH "off"
#endif

#ifdef NES6502_JMP_SPIN
#define DIAG_CPU_JMPSPIN "on"
#else
#define DIAG_CPU_JMPSPIN "off"
#endif

#ifdef NES6502_LINEAR_ROM
#define DIAG_CPU_ROM "linear"
#else
#define DIAG_CPU_ROM "page"
#endif

#ifdef DIAG_CPU_EXEC_TIMING
#define DIAG_CPU_SPLIT "on"
#else
#define DIAG_CPU_SPLIT "off"
#endif

#ifdef NES6502_TCMHOT_CORE
#define DIAG_CPU_TCMCORE "on"
#else
#define DIAG_CPU_TCMCORE "off"
#endif

static uint32_t window_start  = 0;
#ifndef DIAG_FPS_ONLY
static uint32_t render_start  = 0;
#ifdef DIAG_CPU_EXEC_TIMING
static uint32_t cpu_exec_start = 0;
static uint32_t frame_cpu_exec = 0;
#endif
static uint32_t render_accum  = 0;
/* Split tracking: cpu_accum = render_false frames, ppu_accum = render_true frames */
static uint32_t cpu_accum     = 0;
static uint32_t ppu_accum     = 0;
#ifdef DIAG_CPU_EXEC_TIMING
static uint32_t cpu_exec_accum = 0;
static uint32_t ppu_exec_accum = 0;
#endif
static int      cpu_count     = 0;
static int      ppu_count     = 0;
static bool     current_draw  = false;
#endif
static int      idx           = 0;
static int      total         = 0;
static bool     initialized   = false;
static bool     ppu_bg_enabled = true;
static bool     ppu_sprites_enabled = true;

static const char *diag_onoff(bool enabled) {
    return enabled ? "on" : "off";
}

#ifdef NES6502_TCMHOT_CORE_STATS
static void diag_log_tcmcore_stats(void) {
    nes6502_tcmhot_core_stats_t stats;
    uint32_t total_cycles;
    uint32_t core_pct;

    nes6502_tcmhot_core_stats_snapshot(&stats, true);
    total_cycles = stats.core_cycles + stats.fallback_cycles;
    core_pct = total_cycles ? (stats.core_cycles * 100u) / total_cycles : 0;

    pd->system->logToConsole(
        "[tcmcore] calls=%u hit=%u miss=%u core=%u fallback=%u pct=%u max=%u returned=%u",
        (unsigned int) stats.calls,
        (unsigned int) stats.hit_calls,
        (unsigned int) stats.miss_calls,
        (unsigned int) stats.core_cycles,
        (unsigned int) stats.fallback_cycles,
        (unsigned int) core_pct,
        (unsigned int) stats.max_core_run,
        (unsigned int) stats.returned_cycles);
}
#endif

#ifdef NES6502_OPPROFILE
static void diag_log_opcode_profile(void) {
    enum { TOP_COUNT = 8 };
    uint32 counts[256];
    uint32 total_count = 0;
    uint8_t top_op[TOP_COUNT] = {0};
    uint32 top_count[TOP_COUNT] = {0};

    nes6502_opcode_profile_snapshot(counts, &total_count);
    if (total_count == 0) return;

    for (int op = 0; op < 256; op++) {
        uint32 count = counts[op];
        if (count == 0 || count <= top_count[TOP_COUNT - 1]) continue;

        int slot = TOP_COUNT - 1;
        while (slot > 0 && count > top_count[slot - 1]) {
            top_count[slot] = top_count[slot - 1];
            top_op[slot] = top_op[slot - 1];
            slot--;
        }
        top_count[slot] = count;
        top_op[slot] = (uint8_t) op;
    }

    pd->system->logToConsole(
        "[diag] op_total=%u top=%02X:%u %02X:%u %02X:%u %02X:%u %02X:%u %02X:%u %02X:%u %02X:%u",
        (unsigned int) total_count,
        top_op[0], (unsigned int) top_count[0],
        top_op[1], (unsigned int) top_count[1],
        top_op[2], (unsigned int) top_count[2],
        top_op[3], (unsigned int) top_count[3],
        top_op[4], (unsigned int) top_count[4],
        top_op[5], (unsigned int) top_count[5],
        top_op[6], (unsigned int) top_count[6],
        top_op[7], (unsigned int) top_count[7]);
}
#endif

#ifdef NES6502_PRGPROFILE
static void diag_log_prg_profile(void) {
    uint32 counts[16];
    uint32 total = 0;
    nes6502_prg_profile_snapshot(counts, &total);
    if (total == 0) return;

    /* Per-PRG-page ($8000-$FFFF = pages 8-15) share of executed instructions,
       in tenths of a percent. Shows whether execution concentrates in a few
       fixed pages (good DTCM-relocation candidates) or spreads across 32KB. */
    pd->system->logToConsole(
        "[prgprof] total=%u p8=%u p9=%u pA=%u pB=%u pC=%u pD=%u pE=%u pF=%u (per-mille)",
        (unsigned int) total,
        (unsigned int) ((uint64_t) counts[8]  * 1000u / total),
        (unsigned int) ((uint64_t) counts[9]  * 1000u / total),
        (unsigned int) ((uint64_t) counts[10] * 1000u / total),
        (unsigned int) ((uint64_t) counts[11] * 1000u / total),
        (unsigned int) ((uint64_t) counts[12] * 1000u / total),
        (unsigned int) ((uint64_t) counts[13] * 1000u / total),
        (unsigned int) ((uint64_t) counts[14] * 1000u / total),
        (unsigned int) ((uint64_t) counts[15] * 1000u / total));
}
#endif

bool diag_ppu_bg_enabled(void) {
    return ppu_bg_enabled;
}

bool diag_ppu_sprites_enabled(void) {
    return ppu_sprites_enabled;
}

void diag_set_ppu_bg_enabled(bool enabled) {
    ppu_bg_enabled = enabled;
    pd->system->logToConsole("[diag] runtime bg=%s sprites=%s",
                             diag_onoff(ppu_bg_enabled),
                             diag_onoff(ppu_sprites_enabled));
}

void diag_set_ppu_sprites_enabled(bool enabled) {
    ppu_sprites_enabled = enabled;
    pd->system->logToConsole("[diag] runtime bg=%s sprites=%s",
                             diag_onoff(ppu_bg_enabled),
                             diag_onoff(ppu_sprites_enabled));
}

void diag_frame_begin(void) {
    if (!initialized) {
        pd->system->logToConsole("[diag] build=" BUILD_TIMESTAMP
                                 " diag=" DIAG_MODE
                                 " audio=" DIAG_AUDIO
                                 " audio_fill=" DIAG_AUDIO_FILL
                                 " bg=" DIAG_BG
                                 " sprites=" DIAG_SPRITES
                                 " blit=" DIAG_BLIT
                                 " hudfps=" DIAG_HUD_FPS
                                 " lcd_dirty=" DIAG_LCD_DIRTY
                                 " ppu_strike=" DIAG_PPU_STRIKE
                                 " sprcache=" DIAG_PPU_SPRCACHE
                                 " oamdma=" DIAG_PPU_OAMDMA
                                 " cycleacc=" DIAG_CYCLE_ACCUM
                                 " cyclepct=" DIAG_STRINGIFY(NES_CPU_CYCLE_PERCENT)
                                 " cpu_batch=" DIAG_STRINGIFY(NES_CPU_BATCH_SCANLINES)
                                 " cpu_opt=" NES6502_OPT_LEVEL_LABEL
                                 " cpu_dispatch=" DIAG_CPU_DISPATCH
                                 " cpu_cycles=" DIAG_CPU_CYCLES
                                 " cpu_loop_align=" DIAG_CPU_LOOP_ALIGN
                                 " cpu_spin=" DIAG_CPU_SPIN
                                 " cpu_prof=" DIAG_CPU_PROF
                                 " cpu_fastpc=" DIAG_CPU_FASTPC
                                 " cpu_hotops=" DIAG_CPU_HOTOPS
                                 " cpu_memio=" DIAG_CPU_MEMIO
                                 " cpu_fastjmp=" DIAG_CPU_FASTJMP
                                 " cpu_fastbne=" DIAG_CPU_FASTBNE
                                 " cpu_fastbpl=" DIAG_CPU_FASTBPL
                                 " cpu_fastbeq=" DIAG_CPU_FASTBEQ
                                 " cpu_fastbranch=" DIAG_CPU_FASTBRANCH
                                 " cpu_fastopbyte=" DIAG_CPU_FASTOPBYTE
                                 " cpu_fastmemops=" DIAG_CPU_FASTMEMOPS
                                 " cpu_hotcluster=" DIAG_CPU_HOTCLUSTER
                                 " irq_batch=" DIAG_IRQ_BATCH
                                 " cpu_jmpspin=" DIAG_CPU_JMPSPIN
                                 " cpu_rom=" DIAG_CPU_ROM
                                 " cpu_split=" DIAG_CPU_SPLIT
                                 " cpu_tcmcore=" DIAG_CPU_TCMCORE
                                 " prg_align=" DIAG_PRG_ALIGN);
        window_start = pd->system->getCurrentTimeMilliseconds();
        initialized  = true;
    }
}

void diag_render_begin(bool draw_flag) {
#ifdef DIAG_FPS_ONLY
    (void) draw_flag;
#else
    current_draw = draw_flag;
#ifdef DIAG_CPU_EXEC_TIMING
    frame_cpu_exec = 0;
#endif
    render_start = pd->system->getCurrentTimeMilliseconds();
#endif
}

#ifdef DIAG_CPU_EXEC_TIMING
void diag_cpu_execute_begin(void) {
#ifndef DIAG_FPS_ONLY
    cpu_exec_start = pd->system->getCurrentTimeMilliseconds();
#endif
}

void diag_cpu_execute_end(void) {
#ifndef DIAG_FPS_ONLY
    frame_cpu_exec += pd->system->getCurrentTimeMilliseconds() - cpu_exec_start;
#endif
}
#endif

void diag_render_end(void) {
#ifndef DIAG_FPS_ONLY
    uint32_t dt = pd->system->getCurrentTimeMilliseconds() - render_start;
    render_accum += dt;
    if (current_draw) {
        ppu_accum += dt;
#ifdef DIAG_CPU_EXEC_TIMING
        ppu_exec_accum += frame_cpu_exec;
#endif
        ppu_count++;
    } else {
        cpu_accum += dt;
#ifdef DIAG_CPU_EXEC_TIMING
        cpu_exec_accum += frame_cpu_exec;
#endif
        cpu_count++;
    }
#endif
}

void diag_frame_end(void) {
    idx++;
    total++;

    if (idx < WINDOW) return;
    idx = 0;

    uint32_t now       = pd->system->getCurrentTimeMilliseconds();
    uint32_t window_ms = now - window_start;
    window_start       = now;

    if (window_ms == 0) {
        pd->system->logToConsole("[diag] frame=%-5d fps=>1000", total);
#ifndef DIAG_FPS_ONLY
        render_accum = cpu_accum = ppu_accum = 0;
#ifdef DIAG_CPU_EXEC_TIMING
        cpu_exec_accum = ppu_exec_accum = 0;
#endif
        cpu_count = ppu_count = 0;
#endif
        return;
    }

    uint32_t fps      = (WINDOW * 1000) / window_ms;
    uint32_t avg_ms   = window_ms / WINDOW;
#ifdef DIAG_FPS_ONLY
    pd->system->logToConsole(
        "[diag] frame=%-5d  fps=%3d  avg=%2dms",
        total, fps, avg_ms);
#else
    uint32_t avg_cpu  = cpu_count  ? cpu_accum  / cpu_count  : 0;
    uint32_t avg_ppu  = ppu_count  ? ppu_accum  / ppu_count  : 0;
#ifdef DIAG_CPU_EXEC_TIMING
    uint32_t avg_cpu_exec = cpu_count ? cpu_exec_accum / cpu_count : 0;
    uint32_t avg_ppu_exec = ppu_count ? ppu_exec_accum / ppu_count : 0;
    uint32_t avg_cpu_misc = avg_cpu > avg_cpu_exec ? avg_cpu - avg_cpu_exec : 0;

    pd->system->logToConsole(
        "[diag] frame=%-5d  fps=%3d  avg=%2dms  cpu_only=%2dms  cpu_exec=%2dms  cpu_misc=%2dms  ppu_full=%2dms  ppu_exec=%2dms",
        total, fps, avg_ms, avg_cpu, avg_cpu_exec, avg_cpu_misc, avg_ppu, avg_ppu_exec);
#else
    pd->system->logToConsole(
        "[diag] frame=%-5d  fps=%3d  avg=%2dms  cpu_only=%2dms  ppu_full=%2dms",
        total, fps, avg_ms, avg_cpu, avg_ppu);
#endif
#endif

#ifdef NES6502_OPPROFILE
    diag_log_opcode_profile();
#endif

#ifdef NES6502_PRGPROFILE
    diag_log_prg_profile();
#endif

#ifdef NES6502_TCMHOT_CORE_STATS
    diag_log_tcmcore_stats();
#endif

#if defined(NES_PRG_DTCM) && defined(TARGET_PLAYDATE) && defined(__ELF__)
    /* total copies into DTCM since boot: ~1 if page C is a fixed bank (ideal),
       growing fast if the mapper swaps it (relocation cost may then dominate). */
    pd->system->logToConsole("[prgdtcm] copies=%u",
                             (unsigned int) nes6502_prg_dtcm_copies());
#endif

#ifndef DIAG_FPS_ONLY
    render_accum = cpu_accum = ppu_accum = 0;
#ifdef DIAG_CPU_EXEC_TIMING
    cpu_exec_accum = ppu_exec_accum = 0;
#endif
    cpu_count = ppu_count = 0;
#endif
}

#endif /* DIAG */
