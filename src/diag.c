#ifdef DIAG

#include <pd_api.h>
#include <stdbool.h>
#include <stdint.h>
#include "diag.h"

extern PlaydateAPI *pd;

#define WINDOW 60

static uint32_t window_start  = 0;
static uint32_t render_start  = 0;
static uint32_t render_accum  = 0;
static int      idx           = 0;
static int      total         = 0;
static bool     initialized   = false;

void diag_frame_begin(void) {
    if (!initialized) {
        pd->system->logToConsole("[diag] build=" BUILD_TIMESTAMP);
        window_start = pd->system->getCurrentTimeMilliseconds();
        initialized  = true;
    }
}

void diag_render_begin(void) {
    render_start = pd->system->getCurrentTimeMilliseconds();
}

void diag_render_end(void) {
    render_accum += pd->system->getCurrentTimeMilliseconds() - render_start;
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
        render_accum = 0;
        return;
    }

    uint32_t fps        = (WINDOW * 1000) / window_ms;
    uint32_t avg_ms     = window_ms / WINDOW;
    uint32_t avg_render = render_accum / WINDOW;
    uint32_t avg_other  = avg_ms > avg_render ? avg_ms - avg_render : 0;

    pd->system->logToConsole(
        "[diag] frame=%-5d  fps=%3d  avg=%2dms  render=%2dms  other=%2dms",
        total, fps, avg_ms, avg_render, avg_other);

    render_accum = 0;
}

#endif /* DIAG */
