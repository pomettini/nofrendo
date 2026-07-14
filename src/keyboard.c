#include <pd_api.h>
#include <osd.h>
#include <event.h>
#include <nesinput.h>
#include <stdio.h>

#ifdef PD_PLAYBENCH_ENABLED
#include "pd_playbench.h"
#endif

extern PlaydateAPI *pd;

static const struct {
    PDButtons   btn;
    int         evt;
} map[] = {
    { kButtonLeft,  event_joypad1_left  },
    { kButtonRight, event_joypad1_right },
    { kButtonUp,    event_joypad1_up    },
    { kButtonDown,  event_joypad1_down  },
    { kButtonA,     event_joypad1_a     },
    { kButtonB,     event_joypad1_b     },
};
#define MAP_LEN  (int)(sizeof(map) / sizeof(map[0]))

static void fire(int evt, int state) {
    event_t fn = event_get(evt);
    fn(state);
}

static bool crank_start_down  = false;
static bool crank_select_down = false;

static void set_button_state(int evt, bool *down, bool should_be_down) {
    if (*down == should_be_down)
        return;

    fire(evt, should_be_down ? INP_STATE_MAKE : INP_STATE_BREAK);
    *down = should_be_down;
}

/* Crank -> Select/Start by MOTION, not resting position. Any position-based
   mapping fires whatever button the crank's resting angle lands in, and that
   resting angle is unpredictable (it's wherever the crank hangs when you
   undock it) -- which caused Start to trigger the instant the crank was pulled
   out. Keying off motion instead means an idle or newly-undocked crank sends
   nothing at all, whatever angle it rests at.

   Turning the crank one way presses Start, the other way Select. The button is
   held while you keep turning and released a few frames after you stop, so a
   quick flick reads as a clean tap (all Start/Select uses on the NES are taps). */
#define CRANK_MOVE_DEG        2.0f /* per-frame motion that counts as cranking */
#define CRANK_RELEASE_FRAMES  4    /* frames of stillness before releasing     */

static int crank_dir = 0;         /* -1 = Select, +1 = Start, 0 = none */
static int crank_still_frames = 0;

static void compute_crank_buttons(bool *select_down, bool *start_down) {
    *select_down = false;
    *start_down  = false;

    if (pd->system->isCrankDocked()) {
        crank_dir = 0;
        crank_still_frames = CRANK_RELEASE_FRAMES;
        return;
    }

    float change = pd->system->getCrankChange();
    if (change > CRANK_MOVE_DEG) {
        crank_dir = +1; /* forward -> Start */
        crank_still_frames = 0;
    } else if (change < -CRANK_MOVE_DEG) {
        crank_dir = -1; /* backward -> Select */
        crank_still_frames = 0;
    } else if (crank_still_frames < CRANK_RELEASE_FRAMES) {
        crank_still_frames++;
    } else {
        crank_dir = 0; /* stopped long enough -> release */
    }

    if (crank_dir > 0)
        *start_down = true;
    else if (crank_dir < 0)
        *select_down = true;
}

static void update_crank_buttons(void) {
    bool select_down, start_down;
    compute_crank_buttons(&select_down, &start_down);

    set_button_state(event_joypad1_select, &crank_select_down, select_down);
    set_button_state(event_joypad1_start, &crank_start_down, start_down);
}

void osd_input_init(void) {
    /* Start neutral; motion-based input never fires from a resting crank, so
       there is no phantom press to baseline away anymore. */
    crank_dir = 0;
    crank_still_frames = CRANK_RELEASE_FRAMES;
    crank_select_down = false;
    crank_start_down = false;
}

#ifdef PD_PLAYBENCH_RECORD
/* The recorder itself lives in pd-playbench; this file just samples the
   effective buttons each frame (see osd_getinput) and saves on request. Kept
   in /Shared so recordings survive reinstalls and are visible in data-disk
   mode. Kirby needs real D-pad Up, so its crank Start uses the virtual MENU
   token rather than Mario's historical UP bridge. */
#ifdef PD_PLAYBENCH_RECORD_KIRBY
#define REC_SCRIPT_PATH "/Shared/Emulation/nes/kirby_1_1.txt"
#else
#define REC_SCRIPT_PATH "/Shared/Emulation/nes/rec_script.txt"
#endif

void osd_rec_dump(void) {
    if (pd_playbench_record_save(REC_SCRIPT_PATH))
        pd->system->logToConsole("[rec] wrote script to %s", REC_SCRIPT_PATH);
    else
        pd->system->logToConsole("[rec] FAILED to write %s: %s", REC_SCRIPT_PATH,
                                 pd_playbench_get_last_error());
}
#endif /* PD_PLAYBENCH_RECORD */

void osd_getinput(void) {
#if defined(PD_PLAYBENCH_RECORD)
    /* Record build: play normally (real buttons + crank), and capture the
       effective NES input each frame into a replayable script. */
    PDButtons current, pushed, released;
    pd->system->getButtonState(&current, &pushed, &released);

    for (int i = 0; i < MAP_LEN; i++) {
        if (pushed   & map[i].btn) fire(map[i].evt, INP_STATE_MAKE);
        if (released & map[i].btn) fire(map[i].evt, INP_STATE_BREAK);
    }
    update_crank_buttons();

    /* Record the effective buttons. */
#ifdef PD_PLAYBENCH_RECORD_KIRBY
    int eff = 0;
    if (current & kButtonLeft)  eff |= PD_BENCH_BUTTON_LEFT;
    if (current & kButtonRight) eff |= PD_BENCH_BUTTON_RIGHT;
    if (current & kButtonUp)    eff |= PD_BENCH_BUTTON_UP;
    if (current & kButtonDown)  eff |= PD_BENCH_BUTTON_DOWN;
    if (current & kButtonA)     eff |= PD_BENCH_BUTTON_A;
    if (current & kButtonB)     eff |= PD_BENCH_BUTTON_B;
    if (crank_start_down)
        eff |= PD_BENCH_BUTTON_MENU; /* virtual NES Start */
    pd_playbench_record_sample_mask(eff);
#else
    PDButtons eff = current & (kButtonLeft | kButtonRight | kButtonUp |
                               kButtonDown | kButtonA | kButtonB);
    if (crank_start_down)
        eff |= kButtonUp;
    pd_playbench_record_sample(eff);
#endif
#elif defined(PD_PLAYBENCH_ENABLED)
    /* Benchmark build: scripted input replaces real input (deterministic replay).
       The Playdate has no Start/Select buttons -- we map those to the crank,
       which a script can't move -- so the scripted UP button stands in for NES
       Start (SMB needs Start to begin and never uses Up in 1-1). Edges are
       derived from the effective button level, and the crank is skipped so the
       run is fully deterministic. */
    PDButtons current;
    pd->system->getButtonState(&current, NULL, NULL);
    pd_playbench_update();
    current = pd_playbench_get_buttons(current);
#ifdef PD_PLAYBENCH_KIRBY
    int script_buttons = pd_playbench_get_script_button_mask();
#endif

    /* Frame telemetry so a script can be timed to on-screen events (e.g. the
       frame a pit is reached). Logs every 25 frames (~0.5s at 50fps). */
    static unsigned pb_frame = 0;
    if (++pb_frame % 25 == 0)
        pd->system->logToConsole("[bench] frame=%u", pb_frame);

    static PDButtons prev = 0;
    PDButtons pushed = current & ~prev;
    PDButtons released = ~current & prev;
    prev = current;

    for (int i = 0; i < MAP_LEN; i++) {
#ifndef PD_PLAYBENCH_KIRBY
        if (map[i].btn == kButtonUp)
            continue; /* remapped to Start below */
#endif
        if (pushed   & map[i].btn) fire(map[i].evt, INP_STATE_MAKE);
        if (released & map[i].btn) fire(map[i].evt, INP_STATE_BREAK);
    }
#ifdef PD_PLAYBENCH_KIRBY
    static int prev_virtual = 0;
    int virtual_pushed = script_buttons & ~prev_virtual;
    int virtual_released = ~script_buttons & prev_virtual;
    prev_virtual = script_buttons;
    if (virtual_pushed & PD_BENCH_BUTTON_MENU)
        fire(event_joypad1_start, INP_STATE_MAKE);
    if (virtual_released & PD_BENCH_BUTTON_MENU)
        fire(event_joypad1_start, INP_STATE_BREAK);
#else
    if (pushed   & kButtonUp) fire(event_joypad1_start, INP_STATE_MAKE);
    if (released & kButtonUp) fire(event_joypad1_start, INP_STATE_BREAK);
#endif
#else
    PDButtons pushed, released;
    pd->system->getButtonState(NULL, &pushed, &released);

    for (int i = 0; i < MAP_LEN; i++) {
        if (pushed   & map[i].btn) fire(map[i].evt, INP_STATE_MAKE);
        if (released & map[i].btn) fire(map[i].evt, INP_STATE_BREAK);
    }

    update_crank_buttons();
#endif
}
