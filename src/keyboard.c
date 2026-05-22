#include <pd_api.h>
#include <osd.h>
#include <event.h>
#include <nesinput.h>
#include "diag.h"

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

/* Flags set by menu callbacks while the game loop is paused.
   The press is latched here and released on the next osd_getinput call,
   so the NES sees the button held for exactly one frame. */
static bool start_pending  = false;
static bool select_pending = false;

static void menu_start(void *ud)  { start_pending  = true; fire(event_joypad1_start,  INP_STATE_MAKE); }
static void menu_select(void *ud) { select_pending = true; fire(event_joypad1_select, INP_STATE_MAKE); }

#ifdef DIAG
#ifndef DISABLE_PPU_BG
static PDMenuItem *draw_bg_item = NULL;
static void menu_draw_bg(void *ud) {
    (void)ud;
    diag_set_ppu_bg_enabled(pd->system->getMenuItemValue(draw_bg_item) != 0);
}
#endif

#ifndef DISABLE_PPU_SPRITES
static PDMenuItem *draw_sprites_item = NULL;
static void menu_draw_sprites(void *ud) {
    (void)ud;
    diag_set_ppu_sprites_enabled(pd->system->getMenuItemValue(draw_sprites_item) != 0);
}
#endif
#endif

void osd_input_init(void) {
    pd->system->addMenuItem("Start",  menu_start,  NULL);
    pd->system->addMenuItem("Select", menu_select, NULL);

#ifdef DIAG
#ifndef DISABLE_PPU_BG
    draw_bg_item = pd->system->addCheckmarkMenuItem("Draw BG", 1, menu_draw_bg, NULL);
#endif
#ifndef DISABLE_PPU_SPRITES
    draw_sprites_item = pd->system->addCheckmarkMenuItem("Draw Sprites", 1,
                                                         menu_draw_sprites, NULL);
#endif
#endif
}

void osd_getinput(void) {
    PDButtons pushed, released;
    pd->system->getButtonState(NULL, &pushed, &released);

    for (int i = 0; i < MAP_LEN; i++) {
        if (pushed   & map[i].btn) fire(map[i].evt, INP_STATE_MAKE);
        if (released & map[i].btn) fire(map[i].evt, INP_STATE_BREAK);
    }

    /* Release latched Start/Select now that one frame has run with them pressed */
    if (start_pending)  { fire(event_joypad1_start,  INP_STATE_BREAK); start_pending  = false; }
    if (select_pending) { fire(event_joypad1_select, INP_STATE_BREAK); select_pending = false; }
}
