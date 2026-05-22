#ifdef DIAG

#include <pd_api.h>
#include <stdbool.h>
#include <stdint.h>
#include "diag.h"

extern PlaydateAPI *pd;

#define WINDOW 60

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

static uint32_t window_start  = 0;
static uint32_t render_start  = 0;
static uint32_t render_accum  = 0;
/* Split tracking: cpu_accum = render_false frames, ppu_accum = render_true frames */
static uint32_t cpu_accum     = 0;
static uint32_t ppu_accum     = 0;
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
                                 " blit=" DIAG_BLIT);
        window_start = pd->system->getCurrentTimeMilliseconds();
        initialized  = true;
    }
}

void diag_render_begin(bool draw_flag) {
    current_draw = draw_flag;
    render_start = pd->system->getCurrentTimeMilliseconds();
}

void diag_render_end(void) {
    uint32_t dt = pd->system->getCurrentTimeMilliseconds() - render_start;
    render_accum += dt;
    if (current_draw) { ppu_accum += dt; ppu_count++; }
    else              { cpu_accum += dt; cpu_count++;  }
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
        cpu_count = ppu_count = 0;
        return;
    }

    uint32_t fps      = (WINDOW * 1000) / window_ms;
    uint32_t avg_ms   = window_ms / WINDOW;
    uint32_t avg_cpu  = cpu_count  ? cpu_accum  / cpu_count  : 0;
    uint32_t avg_ppu  = ppu_count  ? ppu_accum  / ppu_count  : 0;

    pd->system->logToConsole(
        "[diag] frame=%-5d  fps=%3d  avg=%2dms  cpu_only=%2dms  ppu_full=%2dms",
        total, fps, avg_ms, avg_cpu, avg_ppu);

    render_accum = cpu_accum = ppu_accum = 0;
    cpu_count = ppu_count = 0;
}

#endif /* DIAG */
