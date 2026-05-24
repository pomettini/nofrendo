#ifdef DIAG

#include <pd_api.h>
#include <stdbool.h>
#include <stdint.h>
#include "diag.h"
#ifdef NES6502_OPPROFILE
#include "nes6502.h"
#endif

extern PlaydateAPI *pd;

#define WINDOW 60
#define DIAG_STRINGIFY_INNER(value) #value
#define DIAG_STRINGIFY(value) DIAG_STRINGIFY_INNER(value)

#if AUDIO
#define DIAG_AUDIO "on"
#else
#define DIAG_AUDIO "off"
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

static uint32_t window_start  = 0;
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
static int      idx           = 0;
static int      total         = 0;
static bool     initialized   = false;
static bool     ppu_bg_enabled = true;
static bool     ppu_sprites_enabled = true;

static const char *diag_onoff(bool enabled) {
    return enabled ? "on" : "off";
}

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
                                 " audio=" DIAG_AUDIO
                                 " bg=" DIAG_BG
                                 " sprites=" DIAG_SPRITES
                                 " blit=" DIAG_BLIT
                                 " hudfps=" DIAG_HUD_FPS
                                 " lcd_dirty=" DIAG_LCD_DIRTY
                                 " ppu_strike=" DIAG_PPU_STRIKE
                                 " cpu_batch=" DIAG_STRINGIFY(NES_CPU_BATCH_SCANLINES)
                                 " cpu_opt=" NES6502_OPT_LEVEL_LABEL
                                 " cpu_loop_align=" DIAG_CPU_LOOP_ALIGN
                                 " cpu_spin=" DIAG_CPU_SPIN
                                 " cpu_prof=" DIAG_CPU_PROF
                                 " cpu_fastpc=" DIAG_CPU_FASTPC
                                 " cpu_hotops=" DIAG_CPU_HOTOPS
                                 " cpu_memio=" DIAG_CPU_MEMIO
                                 " cpu_jmpspin=" DIAG_CPU_JMPSPIN
                                 " cpu_rom=" DIAG_CPU_ROM
                                 " cpu_split=" DIAG_CPU_SPLIT
                                 " prg_align=" DIAG_PRG_ALIGN);
        window_start = pd->system->getCurrentTimeMilliseconds();
        initialized  = true;
    }
}

void diag_render_begin(bool draw_flag) {
    current_draw = draw_flag;
#ifdef DIAG_CPU_EXEC_TIMING
    frame_cpu_exec = 0;
#endif
    render_start = pd->system->getCurrentTimeMilliseconds();
}

#ifdef DIAG_CPU_EXEC_TIMING
void diag_cpu_execute_begin(void) {
    cpu_exec_start = pd->system->getCurrentTimeMilliseconds();
}

void diag_cpu_execute_end(void) {
    frame_cpu_exec += pd->system->getCurrentTimeMilliseconds() - cpu_exec_start;
}
#endif

void diag_render_end(void) {
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
        render_accum = cpu_accum = ppu_accum = 0;
#ifdef DIAG_CPU_EXEC_TIMING
        cpu_exec_accum = ppu_exec_accum = 0;
#endif
        cpu_count = ppu_count = 0;
        return;
    }

    uint32_t fps      = (WINDOW * 1000) / window_ms;
    uint32_t avg_ms   = window_ms / WINDOW;
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

#ifdef NES6502_OPPROFILE
    diag_log_opcode_profile();
#endif

    render_accum = cpu_accum = ppu_accum = 0;
#ifdef DIAG_CPU_EXEC_TIMING
    cpu_exec_accum = ppu_exec_accum = 0;
#endif
    cpu_count = ppu_count = 0;
}

#endif /* DIAG */
