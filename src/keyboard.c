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
/* Input recorder: capture the player's per-frame NES inputs during a live
   playthrough and dump a run-length-encoded pd-playbench script that replays
   them. Crank Start is recorded as the UP token, matching the replay build's
   UP->Start bridge (SMB never uses Up in 1-1). */
#define REC_LEFT  0x01
#define REC_RIGHT 0x02
#define REC_UP    0x04
#define REC_DOWN  0x08
#define REC_A     0x10
#define REC_B     0x20
#define REC_MAX   1024
/* Kept in /Shared so it survives reinstalls and is visible in data-disk mode;
   the replay build loads this exact path. Must match REC_SCRIPT_PATH in main.c. */
#define REC_SCRIPT_PATH "/Shared/Emulation/nes/rec_script.txt"

static struct {
    unsigned tokens;
    int frames;
} rec_runs[REC_MAX];
static int rec_count;
static unsigned rec_prev;
static int rec_frames;

static void rec_flush(void) {
    if (rec_frames > 0 && rec_count < REC_MAX) {
        rec_runs[rec_count].tokens = rec_prev;
        rec_runs[rec_count].frames = rec_frames;
        rec_count++;
    }
    rec_frames = 0;
}

static void rec_sample(unsigned tokens) {
    if (tokens == rec_prev) {
        rec_frames++;
    } else {
        rec_flush();
        rec_prev = tokens;
        rec_frames = 1;
    }
}

/* Write the captured inputs as a pd-playbench script to a file. A file write is
   atomic and lossless, unlike streaming ~200 lines to the serial console (which
   overflows and drops lines). The replay build loads this same file. */
void osd_rec_dump(void) {
    static char out[32768];
    int n = 0;

    rec_flush();
    for (int i = 0; i < rec_count && n < (int)sizeof(out) - 64; i++) {
        unsigned t = rec_runs[i].tokens;
        int f = rec_runs[i].frames;
        if (t == 0) {
            n += snprintf(out + n, sizeof(out) - n, "wait %d\n", f);
            continue;
        }
        char b[48];
        int p = 0;
        if (t & REC_RIGHT) p += snprintf(b + p, sizeof(b) - p, "%sRIGHT", p ? "+" : "");
        if (t & REC_LEFT)  p += snprintf(b + p, sizeof(b) - p, "%sLEFT", p ? "+" : "");
        if (t & REC_UP)    p += snprintf(b + p, sizeof(b) - p, "%sUP", p ? "+" : "");
        if (t & REC_DOWN)  p += snprintf(b + p, sizeof(b) - p, "%sDOWN", p ? "+" : "");
        if (t & REC_A)     p += snprintf(b + p, sizeof(b) - p, "%sA", p ? "+" : "");
        if (t & REC_B)     p += snprintf(b + p, sizeof(b) - p, "%sB", p ? "+" : "");
        n += snprintf(out + n, sizeof(out) - n, "hold %s %d\n", b, f);
    }
    n += snprintf(out + n, sizeof(out) - n, "stop\n");

    SDFile *f = pd->file->open(REC_SCRIPT_PATH, kFileWrite);
    if (!f) {
        pd->system->logToConsole("[rec] FAILED to write %s: %s", REC_SCRIPT_PATH,
                                 pd->file->geterr());
        return;
    }
    pd->file->write(f, out, (unsigned int)n);
    pd->file->close(f);
    pd->system->logToConsole("[rec] wrote %d commands (%d bytes) to %s",
                             rec_count + 1, n, REC_SCRIPT_PATH);
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

    unsigned t = 0;
    if (current & kButtonRight) t |= REC_RIGHT;
    if (current & kButtonLeft)  t |= REC_LEFT;
    if (current & kButtonDown)  t |= REC_DOWN;
    if (current & kButtonUp)    t |= REC_UP;
    if (current & kButtonA)     t |= REC_A;
    if (current & kButtonB)     t |= REC_B;
    if (crank_start_down)       t |= REC_UP; /* Start -> UP token (replay bridge) */
    rec_sample(t);
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
        if (map[i].btn == kButtonUp)
            continue; /* remapped to Start below */
        if (pushed   & map[i].btn) fire(map[i].evt, INP_STATE_MAKE);
        if (released & map[i].btn) fire(map[i].evt, INP_STATE_BREAK);
    }
    if (pushed   & kButtonUp) fire(event_joypad1_start, INP_STATE_MAKE);
    if (released & kButtonUp) fire(event_joypad1_start, INP_STATE_BREAK);
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
