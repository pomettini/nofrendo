#ifdef DIAG

#include <pd_api.h>
#include "diag.h"

extern PlaydateAPI *pd;

#define WINDOW 60  /* report every N frames */

static uint32_t t_begin;
static uint32_t samples[WINDOW];
static int      idx         = 0;
static int      total       = 0;

void diag_frame_begin(void) {
    t_begin = pd->system->getCurrentTimeMilliseconds();
}

void diag_frame_end(void) {
    uint32_t ms = pd->system->getCurrentTimeMilliseconds() - t_begin;
    samples[idx++] = ms;
    total++;

    if (idx < WINDOW) return;
    idx = 0;

    uint32_t sum = 0, lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < WINDOW; i++) {
        sum += samples[i];
        if (samples[i] < lo) lo = samples[i];
        if (samples[i] > hi) hi = samples[i];
    }
    uint32_t avg = sum / WINDOW;
    uint32_t fps = avg > 0 ? 1000 / avg : 0;

    pd->system->logToConsole(
        "[diag] frame=%-5d  fps=%2d  avg=%2dms  min=%2dms  max=%2dms",
        total, fps, avg, lo, hi);
}

#endif /* DIAG */
