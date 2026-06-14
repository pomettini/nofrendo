#include <pd_api.h>
#include <osd.h>
#include <event.h>
#include <nesinput.h>

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

static void compute_crank_buttons(bool *select_down, bool *start_down) {
    *select_down = false;
    *start_down  = false;

    if (!pd->system->isCrankDocked()) {
        float angle = pd->system->getCrankAngle();
        while (angle < 0.0f)
            angle += 360.0f;
        while (angle >= 360.0f)
            angle -= 360.0f;

        *select_down = angle < 60.0f;
        *start_down  = angle > 180.0f;
    }
}

static void update_crank_buttons(void) {
    bool select_down, start_down;
    compute_crank_buttons(&select_down, &start_down);

    set_button_state(event_joypad1_select, &crank_select_down, select_down);
    set_button_state(event_joypad1_start, &crank_start_down, start_down);
}

void osd_input_init(void) {
    /* Baseline the crank to its current resting position so a ROM that loads
       while the crank sits in the Start/Select zone doesn't fire a phantom
       press. Start/Select only register when the crank is actively moved into
       the zone afterwards. */
    compute_crank_buttons(&crank_select_down, &crank_start_down);
}

void osd_getinput(void) {
    PDButtons pushed, released;
    pd->system->getButtonState(NULL, &pushed, &released);

    for (int i = 0; i < MAP_LEN; i++) {
        if (pushed   & map[i].btn) fire(map[i].evt, INP_STATE_MAKE);
        if (released & map[i].btn) fire(map[i].evt, INP_STATE_BREAK);
    }

    update_crank_buttons();
}
