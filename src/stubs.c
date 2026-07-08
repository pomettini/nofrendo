#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <gui.h>
#include <vid_drv.h>

void gui_tick(int ticks) {}
void gui_setrefresh(int frequency) {}

/* The core reports load failures (unsupported mapper, truncated/invalid image,
   allocation failure) via gui_sendmsg(GUI_RED, ...). The UI has no GUI, so we
   capture the last such message here and let main.c show it on screen instead
   of failing silently back to the ROM picker. */
static char gui_last_error[128];

void gui_sendmsg(int color, char *format, ...) {
  if (color != GUI_RED)
    return;
  va_list ap;
  va_start(ap, format);
  vsnprintf(gui_last_error, sizeof(gui_last_error), format, ap);
  va_end(ap);
}

const char *osd_get_load_error(void) { return gui_last_error; }
void osd_clear_load_error(void) { gui_last_error[0] = '\0'; }
int  gui_init(void) { return 0; }
void gui_shutdown(void) {}
void gui_frame(bool draw) {}
void gui_togglefps(void) {}
void gui_togglegui(void) {}
void gui_togglewave(void) {}
void gui_togglepattern(void) {}
void gui_toggleoam(void) {}
void gui_decpatterncol(void) {}
void gui_incpatterncol(void) {}
void gui_savesnap(void) {}
void gui_togglesprites(void) {}
void gui_togglefs(void) {}
void gui_displayinfo(void) {}
void gui_toggle_chan(int chan) {}
void gui_setfilter(int filter_type) {}

rgb_t gui_pal[GUI_TOTALCOLORS] = {
    { 0x00, 0x00, 0x00 },
    { 0x3F, 0x3F, 0x3F },
    { 0x7F, 0x7F, 0x7F },
    { 0xBF, 0xBF, 0xBF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0x00, 0x00 },
    { 0x00, 0xFF, 0x00 },
    { 0x00, 0x00, 0xFF },
    { 0xFF, 0xFF, 0x00 },
    { 0xFF, 0xAF, 0x00 },
    { 0xFF, 0x00, 0xFF },
    { 0x3F, 0x7F, 0x7F },
    { 0x00, 0x2A, 0x00 },
    { 0x00, 0x00, 0x3F },
};

int  vid_init(int width, int height, viddriver_t *osd_driver) { return 0; }
void vid_flush(void) {}
int  vid_setmode(int width, int height) { return 0; }
void vid_shutdown(void) {}

/* Save states and SRAM battery saves are unsupported for now. */
void state_setslot(int slot) {}
int  state_save(void) { return -1; }
int  state_load(void) { return -1; }
