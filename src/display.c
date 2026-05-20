#include <osd.h>
#include <nes.h>

static int   display_init(int width, int height)            { return 0; }
static void  display_shutdown(void)                         {}
static int   display_set_mode(int width, int height)        { return 0; }
static void  display_set_palette(rgb_t *pal)                {}
static void  display_clear(uint8 color)                     {}
static bitmap_t *display_lock_write(void)                   { return NULL; }
static void  display_free_write(int n, rect_t *dirty)       {}
static void  display_custom_blit(bitmap_t *bmp, int n, rect_t *dirty) {}

viddriver_t playdateDriver = {
    "playdate",
    display_init,
    display_shutdown,
    display_set_mode,
    display_set_palette,
    display_clear,
    display_lock_write,
    display_free_write,
    display_custom_blit,
    false
};

void osd_getvideoinfo(vidinfo_t *info) {
    info->default_width  = NES_SCREEN_WIDTH;
    info->default_height = NES_SCREEN_HEIGHT;
    info->driver         = &playdateDriver;
}

void vid_setpalette(rgb_t *pal) {
}

/* Phase 3: implement dithering and push to Playdate framebuffer */
void ppu_scanline_blit(uint8_t *bmp, int scanline, bool draw_flag) {
}
