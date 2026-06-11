#include <pd_api.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "diag.h"
#include <noftypes.h>
#include <nofconfig.h>
#include <osd.h>
#include <nofrendo.h>
#include <nes.h>

extern PlaydateAPI *pd;

static char configfilename[] = "nofrendo.cfg";

extern void sound_fill_buffer(void);
extern int app_return_to_picker_if_requested(void);

/* User-facing frame skip: Auto adapts between 1 and 2 based on load;
   0..2 are fixed draw intervals (0 draws every frame; N skips N frames
   between draws). Skipped frames still run the 6502 and PPU state machine
   at full speed, so game speed never changes — only visual refresh. */
#define FRAME_SKIP_AUTO (-1)
#define FRAME_SKIP_DEFAULT FRAME_SKIP_AUTO
#define FRAME_SKIP_MIN FRAME_SKIP_AUTO
#define FRAME_SKIP_MAX 2
#define FPS_COUNTER_ROWS 16

/* Adaptive frameskip (Auto mode): base skip 1, boosted to 2 while the
   exponential moving average (alpha 1/4, 1/16-ms fixed point) of per-frame
   emulation work exceeds the 20 ms PAL frame budget. Exit needs the EMA
   comfortably back under budget AND a minimum hold, so the visual refresh
   does not flap between 25 and 16 fps on borderline scenes. */
#define AUTO_SKIP_BASE        1
#define AUTO_SKIP_BOOSTED     2
#define AUTO_EMA_INIT_FP      (16 << 4) /* assume light load at start */
/* Enter at 21 ms, not the 20 ms PAL budget: device rows showed marginal
   20.0-20.5 ms windows flickering into boost at the minimum hold period.
   Un-boosted those windows run 47-49 fps (imperceptible slowdown), which
   beats a second of 16 fps visual refresh. Boost is for real dips only. */
#define AUTO_BOOST_ENTER_FP   (21 << 4)
#define AUTO_BOOST_EXIT_FP    (18 << 4)
#define AUTO_BOOST_MIN_FRAMES 50        /* hold boost for >= ~1 s */

static int frame_skip = FRAME_SKIP_DEFAULT;
static int skip_counter = 0;
static int32_t auto_ema_fp = AUTO_EMA_INIT_FP;
static int auto_boosted = 0;
static int auto_boost_age = 0;
static int show_fps = 1;

int osd_get_frame_skip(void) {
    return frame_skip;
}

void osd_set_frame_skip(int skip) {
    if (skip < FRAME_SKIP_MIN)
        skip = FRAME_SKIP_MIN;
    if (skip > FRAME_SKIP_MAX)
        skip = FRAME_SKIP_MAX;

    frame_skip = skip;
    skip_counter = 0; /* draw on the next frame */
    auto_ema_fp = AUTO_EMA_INIT_FP;
    auto_boosted = 0;
    auto_boost_age = 0;
}

#ifdef NES_RAM_DTCM
/* Hand the NES core a 2KB work-RAM block inside the DTCM stack pool.
   Same frame-derived scheme the validated tcmhot probe uses: the pool top
   sits 0x2180 below the current stack frame (max expected further stack
   growth per the Vecx guide); the block is placed at the pool top, 32-byte
   aligned. NOTE: 2KB exceeds the guide's conservative 1328-byte ceiling, so
   this is probe territory — corruption (OS data or deep stack collision)
   would show up as immediate gameplay chaos. Returns NULL (heap fallback)
   when the result is outside DTCM, which also covers the simulator. */
uint8_t *osd_dtcm_ram_alloc(unsigned int size) {
    uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    uintptr_t pool_top = (frame - 0x2180u) & ~(uintptr_t)0x0fu;
    uintptr_t dest = (pool_top - size) & ~(uintptr_t)0x1fu;

    if (dest < 0x20000000u || (dest + size) > 0x20010000u) {
        pd->system->logToConsole("[dtcmram] out of range (frame=%p), heap fallback",
                                 (void *)frame);
        return NULL;
    }

    pd->system->logToConsole("[dtcmram] dest=%p size=%u frame=%p",
                             (void *)dest, size, (void *)frame);
    return (uint8_t *)dest;
}
#endif

int osd_get_show_fps(void) {
    return show_fps;
}

void osd_set_show_fps(int enabled) {
    show_fps = enabled ? 1 : 0;
}

static int playdate_update(void *ud) {
    if (app_return_to_picker_if_requested())
        return 1;

    int auto_mode = (frame_skip == FRAME_SKIP_AUTO);
    int draw;
    if (skip_counter <= 0) {
        draw = 1;
        skip_counter = auto_mode
                           ? (auto_boosted ? AUTO_SKIP_BOOSTED : AUTO_SKIP_BASE)
                           : frame_skip;
    } else {
        draw = 0;
        skip_counter--;
    }

    uint32_t work_start = 0;
    if (auto_mode)
        work_start = pd->system->getCurrentTimeMilliseconds();

    diag_frame_begin();
    diag_render_begin(draw);
    nes_renderframe(draw);
    diag_render_end();
    osd_getinput();
    sound_fill_buffer();
    diag_frame_end();

    if (auto_mode) {
        uint32_t work = pd->system->getCurrentTimeMilliseconds() - work_start;
        auto_ema_fp += ((int32_t)(work << 4) - auto_ema_fp) >> 2;

        if (auto_boosted) {
            auto_boost_age++;
            if (auto_boost_age >= AUTO_BOOST_MIN_FRAMES &&
                auto_ema_fp < AUTO_BOOST_EXIT_FP) {
                auto_boosted = 0;
#ifdef DIAG
                pd->system->logToConsole("[autoskip] 2->1 after %d frames",
                                         auto_boost_age);
#endif
            }
        } else if (auto_ema_fp > AUTO_BOOST_ENTER_FP) {
            auto_boosted = 1;
            auto_boost_age = 0;
#ifdef DIAG
            pd->system->logToConsole("[autoskip] 1->2 ema=%d/16 ms",
                                     (int)auto_ema_fp);
#endif
        }
    }

    if (show_fps)
        pd->system->drawFPS(0, 0);

    if (draw) {
        pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
        return 1;
    }

    if (show_fps) {
        pd->graphics->markUpdatedRows(0, FPS_COUNTER_ROWS - 1);
        return 1;
    }

    return 0;
}

void osd_start_emulation(void) {
    pd->system->setUpdateCallback(playdate_update, NULL);
}

extern void osd_input_init(void);

int osd_init(void) {
    osd_input_init();
    return 0;
}

void osd_shutdown(void) {
}

int osd_main(int argc, char *argv[]) {
    config.filename = configfilename;
    if (argc <= 1 || !argv || !argv[1] || !argv[1][0]) {
        pd->system->logToConsole("[rom] no ROM selected; use the ROM picker");
        return 1;
    }
    return main_loop(argv[1], system_nes);
}

void osd_getmouse(int *x, int *y, int *button) {
}

void osd_fullname(char *fullname, const char *shortname) {
    strncpy(fullname, shortname, PATH_MAX);
}

char *osd_newextension(char *string, char *ext) {
    int l = strlen(string);
    while (l && string[l] != '.')
        l--;
    if (l) string[l] = 0;
    strcat(string, ext);
    return string;
}

int osd_makesnapname(char *filename, int len) {
    return -1;
}

static char *rom_storage = NULL;
static char *rom_data = NULL;
static unsigned int rom_size = 0;

static void clear_rom_storage(void) {
    free(rom_storage);
    rom_storage = NULL;
    rom_data = NULL;
    rom_size = 0;
}

char *osd_getromdata(const char *name) {
    FileStat stat;
    clear_rom_storage();

    if (pd->file->stat(name, &stat) != 0)
        return NULL;
    if (stat.isdir || stat.size < 16)
        return NULL;

#ifdef ALIGN_PRG_ROM
    const uintptr_t prg_alignment = 16 * 1024;
    const uintptr_t ines_header_size = 16;
    rom_storage = malloc(stat.size + prg_alignment);
    if (rom_storage) {
        uintptr_t prg_start = ((uintptr_t)rom_storage + ines_header_size
                               + (prg_alignment - 1)) & ~(prg_alignment - 1);
        rom_data = (char *)(prg_start - ines_header_size);
    }
#else
    rom_storage = malloc(stat.size);
    rom_data = rom_storage;
#endif
    if (!rom_storage)
        return NULL;

    SDFile *f = pd->file->open(name, kFileRead);
    if (!f) {
        free(rom_storage);
        rom_storage = NULL;
        rom_data = NULL;
        return NULL;
    }

    int bytes_read = pd->file->read(f, rom_data, stat.size);
    pd->file->close(f);
    if (bytes_read != (int)stat.size) {
        clear_rom_storage();
        return NULL;
    }

    rom_size = stat.size;
    return rom_data;
}

unsigned int osd_getromsize(void) {
    return rom_size;
}

void osd_unloadromdata(void) {
    clear_rom_storage();
}
