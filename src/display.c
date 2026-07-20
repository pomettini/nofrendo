#include <pd_api.h>
#include <string.h>
#include <osd.h>
#include <nes.h>
#include "diag.h"

extern PlaydateAPI *pd;

/* NES 256x240 centred: 72px padding each side (divisible by 8) */
#define XOFFSET  ((LCD_COLUMNS - NES_SCREEN_WIDTH)  / 2)
#define YOFFSET  ((LCD_ROWS    - NES_SCREEN_HEIGHT) / 2)

/* Bayer 4×4 ordered dither matrix, values 0–15 */
static const uint8_t bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/*
 * white4[bayer_row][palette_index] — precomputed dither LUT.
 *
 * Each byte encodes whether the palette entry would render white in each of
 * the 4 Bayer columns, packed twice so one table lookup covers both nibbles
 * of an 8-pixel output byte:
 *
 *   bit 7 = col 0 (upper nibble)   bit 3 = col 0 (lower nibble)
 *   bit 6 = col 1                  bit 2 = col 1
 *   bit 5 = col 2                  bit 1 = col 2
 *   bit 4 = col 3                  bit 0 = col 3
 *
 * Output byte for 8 consecutive pixels p[0..7] (cols 0,1,2,3,0,1,2,3):
 *   byte = (w4[p[0]] & 0x80) | (w4[p[1]] & 0x40) | ... | (w4[p[7]] & 0x01)
 */
#ifdef PPU_DIRECT_1BIT
static uint8_t white4[4][4][256];
#else
static uint8_t white4[4][256];
#endif

/* Cached raw frame buffer pointer — stable for the app lifetime */
static uint8_t *fb_data = NULL;

static void ensure_framebuffer(void) {
    if (!fb_data) {
        fb_data = pd->graphics->getFrame();
        memset(fb_data, 0x00, LCD_ROWSIZE * LCD_ROWS);
    }
}

#ifdef PPU_DIRECT_1BIT
/* Direct PPU path accessors. The returned row covers only the centred
   256-pixel NES image; horizontal padding remains untouched. */
uint8_t *vid_direct_row(int scanline) {
    ensure_framebuffer();
    int y = scanline + YOFFSET;
    return fb_data + y * LCD_ROWSIZE + XOFFSET / 8;
}

const uint8_t *vid_dither_row(int scanline, int phase) {
    return white4[(scanline + YOFFSET) & 3][phase & 3];
}
#endif

/* Called by the PPU when it loads the colour palette */
void vid_setpalette(rgb_t *pal) {
    for (int row = 0; row < 4; row++) {
#ifdef PPU_DIRECT_1BIT
        for (int phase = 0; phase < 4; phase++) {
#else
        {
            int phase = 0;
#endif
            for (int i = 0; i < 256; i++) {
                uint8_t l = (uint8_t)((pal[i].r * 77 + pal[i].g * 150 + pal[i].b * 29) >> 8);
                uint8_t w = 0;
                for (int col = 0; col < 4; col++) {
                    int screen_col = (phase + col) & 3;
                    if (l >= ((bayer4[row][screen_col] << 4) | 8))
                        w |= (0x88u >> col);
                }
#ifdef PPU_DIRECT_1BIT
                white4[row][phase][i] = w;
#else
                white4[row][i] = w;
#endif
            }
        }
    }
}

/* Called per-scanline by nes_renderframe */
void ppu_scanline_blit(uint8_t *bmp, int scanline, bool draw_flag) {
    /* One-time init regardless of draw_flag: skipped frames must show the
       last rendered frame, not an uninitialised (white) framebuffer. */
    ensure_framebuffer();

#ifdef DISABLE_PPU_BLIT
    (void)bmp;
    (void)scanline;
    (void)draw_flag;
    return;
#endif

    if (!draw_flag || scanline < 0 || scanline >= NES_SCREEN_HEIGHT)
        return;

#ifdef PPU_DIRECT_1BIT
    if (ppu_scanline_direct_rendered(scanline))
        return;
#endif

    bmp += 8;  /* skip 8-pixel left overdraw */

    int y = scanline + YOFFSET;
    uint8_t *row = fb_data + y * LCD_ROWSIZE + XOFFSET / 8;
#ifdef PPU_DIRECT_1BIT
    const uint8_t *w4 = white4[y & 3][0];
#else
    const uint8_t *w4 = white4[y & 3];
#endif

    /* 32 bytes = 256 pixels, 8 per byte; 1 = white on Playdate (MSB first).
       Each iteration: 8 LUT lookups + 7 OR + 8 AND — no branches. */
    for (int bx = 0; bx < NES_SCREEN_WIDTH / 8; bx++) {
        const uint8_t *px = bmp + bx * 8;
        row[bx] = (w4[px[0]] & 0x80) | (w4[px[1]] & 0x40) |
                  (w4[px[2]] & 0x20) | (w4[px[3]] & 0x10) |
                  (w4[px[4]] & 0x08) | (w4[px[5]] & 0x04) |
                  (w4[px[6]] & 0x02) | (w4[px[7]] & 0x01);
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
