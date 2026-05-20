#ifdef DIAG

#include <pd_api.h>
#include "diag.h"

extern PlaydateAPI *pd;

#define WINDOW 60

static uint32_t window_start   = 0;
static uint32_t render_start   = 0;
static uint32_t render_accum   = 0;  /* total render ms in current window */
static int      idx            = 0;
static int      total          = 0;

void diag_frame_begin(void) {
    if (total == 0)
        window_start = pd->system->getCurrentTimeMilliseconds();
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
        pd->system->logToConsole("[diag] frame=%-5d  fps=>1000 (window<1ms)", total);
        render_accum = 0;
        return;
    }

    uint32_t fps        = (WINDOW * 1000) / window_ms;
    uint32_t avg        = window_ms / WINDOW;
    uint32_t avg_render = render_accum / WINDOW;
    uint32_t avg_other  = avg > avg_render ? avg - avg_render : 0;

    pd->system->logToConsole(
        "[diag] frame=%-5d  fps=%3d  avg=%2dms  (render=%2dms  other=%2dms)",
        total, fps, avg, avg_render, avg_other);

    render_accum = 0;
}

#endif /* DIAG */
