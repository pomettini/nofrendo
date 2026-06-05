#include <pd_api.h>
#include <stdint.h>
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

/* Render 1 out of every FRAME_SKIP NES frames. 1 = no skip (full render
   every frame). 2 = render every other frame (halves PPU pixel cost).
   Skipped frames still run the 6502 and PPU state machine at full speed. */
#define FRAME_SKIP 2

static int frame_num = 0;

static int playdate_update(void *ud) {
    if (app_return_to_picker_if_requested())
        return 1;

    frame_num++;
    int draw = (frame_num % FRAME_SKIP == 0);

    diag_frame_begin();
    diag_render_begin(draw);
    nes_renderframe(draw);
    diag_render_end();
    osd_getinput();
    sound_fill_buffer();
    diag_frame_end();

#ifdef DIAG_DRAW_FPS
    pd->system->drawFPS(0, 0);
    pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
    return 1;
#else
    if (draw) {
        pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
        return 1;
    }
    return 0;
#endif
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

char *osd_getromdata(const char *name) {
    FileStat stat;
    if (pd->file->stat(name, &stat) != 0)
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

    pd->file->read(f, rom_data, stat.size);
    pd->file->close(f);
    return rom_data;
}

void osd_unloadromdata(void) {
    free(rom_storage);
    rom_storage = NULL;
    rom_data = NULL;
}
