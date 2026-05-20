#include <pd_api.h>
#include <string.h>
#include <osd.h>
#include <nes.h>

extern PlaydateAPI *pd;

/* NES 256x240 centred: 72px padding each side (72 is a multiple of 8) */
#define XOFFSET  ((LCD_COLUMNS - NES_SCREEN_WIDTH)  / 2)
#define YOFFSET  ((LCD_ROWS    - NES_SCREEN_HEIGHT) / 2)

/* Rec. 601 luminance LUT, populated by vid_setpalette */
static uint8_t luma[256];

/* Cached raw framebuffer pointer — getFrame() is stable for the app lifetime */
static uint8_t *fb_data = NULL;

/* Bayer 4×4 ordered dither matrix, values 0–15 */
static const uint8_t bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/* Called by the PPU when it loads the colour palette */
void vid_setpalette(rgb_t *pal) {
    for (int i = 0; i < 256; i++)
        luma[i] = (uint8_t)((pal[i].r * 77 + pal[i].g * 150 + pal[i].b * 29) >> 8);
}

/* Called per-scanline by nes_renderframe */
void ppu_scanline_blit(uint8_t *bmp, int scanline, bool draw_flag) {
    if (!draw_flag || scanline < 0 || scanline >= NES_SCREEN_HEIGHT)
        return;

    /* One-time init: grab the raw frame buffer and clear borders to black */
    if (!fb_data) {
        fb_data = pd->graphics->getFrame();
        memset(fb_data, 0x00, LCD_ROWSIZE * LCD_ROWS);
    }

    bmp += 8;  /* skip 8-pixel left overdraw */

    int y = scanline + YOFFSET;
    uint8_t *row = fb_data + y * LCD_ROWSIZE + XOFFSET / 8;
    const uint8_t *th = bayer4[y & 3];

    /* Write 32 bytes = 256 pixels, 8 per byte, MSB-first; 1=white on Playdate */
    for (int bx = 0; bx < NES_SCREEN_WIDTH / 8; bx++) {
        uint8_t byte = 0;
        for (int bit = 0; bit < 8; bit++) {
            int px = bx * 8 + bit;
            /* Bayer threshold: scale 0–15 → 8, 24, 40 … 248 */
            if (luma[bmp[px]] >= ((th[px & 3] << 4) | 8))
                byte |= (0x80 >> bit);
        }
        row[bx] = byte;
    }
}

/* --- viddriver_t (framework stubs, scanline path bypasses these) --- */
static int       display_init(int w, int h)                         { return 0; }
static void      display_shutdown(void)                             {}
static int       display_set_mode(int w, int h)                     { return 0; }
static void      display_set_palette(rgb_t *pal)                    { vid_setpalette(pal); }
static void      display_clear(uint8 color)                         {}
static bitmap_t *display_lock_write(void)                           { return NULL; }
static void      display_free_write(int n, rect_t *dirty)           {}
static void      display_custom_blit(bitmap_t *b, int n, rect_t *d) {}

viddriver_t playdateDriver = {
    "playdate",
    display_init, display_shutdown, display_set_mode,
    display_set_palette, display_clear,
    display_lock_write, display_free_write, display_custom_blit,
    false
};

void osd_getvideoinfo(vidinfo_t *info) {
    info->default_width  = NES_SCREEN_WIDTH;
    info->default_height = NES_SCREEN_HEIGHT;
    info->driver         = &playdateDriver;
}
