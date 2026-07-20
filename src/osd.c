#include <pd_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diag.h"
#include <noftypes.h>
#include <nofconfig.h>
#include <osd.h>
#include <nofrendo.h>
#include <nes.h>
#include <nes6502.h>

#ifdef PD_PLAYBENCH_ENABLED
#include "pd_playbench.h"
#endif

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
/* Anti-flap: Punch-Out-shaped loads (draw frames over budget, skip frames
   cheap) sit exactly at the threshold — boosting drives the EMA under the
   exit line, so the controller flapped 25<->16 fps once a second forever.
   If a boost re-enters shortly after the last exit, the load is clearly
   sustained: hold the boost much longer instead of probing every second. */
#define AUTO_FLAP_WINDOW      120       /* re-entry within ~2.4 s = flap */
#define AUTO_BOOST_HOLD_LONG  300       /* escalated hold, ~6 s */

static int frame_skip = FRAME_SKIP_DEFAULT;
static int skip_counter = 0;
static int32_t auto_ema_fp = AUTO_EMA_INIT_FP;
static int auto_boosted = 0;
static int auto_boost_age = 0;
static int auto_boost_hold = AUTO_BOOST_MIN_FRAMES;
static int auto_frames_since_exit = 0x7FFF;
static int show_fps = 0;   /* clean screen by default; toggle in the system menu */

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
    auto_boost_hold = AUTO_BOOST_MIN_FRAMES;
    auto_frames_since_exit = 0x7FFF;
}

#ifdef NES_RAM_DTCM
/* Hand the NES core a 2KB work-RAM block inside the DTCM pool.
   Placement from the 2026-06-12 dtcmscan run: the pool between the firmware
   data floor (0x200074d0) and the deepest observed stack watermark
   (0x200095a8 over a full Mario 1-1 with audio) stayed untouched — 8,408
   bytes. The block sits at the BOTTOM of that pool (64-byte aligned, just
   above the firmware floor), maximizing distance from stack growth: 6.3 KB
   of headroom to the observed watermark. The first probe at 0x200071a0
   crashed because it sat below the firmware floor — placement, not size.
   On the simulator the fixed address is outside DTCM semantics; the bounds
   check below never passes there, so it falls back to heap. */
#define DTCM_RAM_DEST  0x20007500u
#define DTCM_POOL_END  0x200095a8u
uint8_t *osd_dtcm_ram_alloc(unsigned int size) {
#ifdef TARGET_PLAYDATE
    uintptr_t dest = DTCM_RAM_DEST;
    if ((dest + size) > DTCM_POOL_END) {
#ifdef DIAG
        pd->system->logToConsole("[dtcmram] size %u exceeds pool, heap fallback", size);
#endif
        return NULL;
    }
#ifdef DIAG
    pd->system->logToConsole("[dtcmram] dest=%p size=%u (pool 0x200074d0..0x%08x)",
                             (void *)dest, size, (unsigned)DTCM_POOL_END);
#endif
    return (uint8_t *)dest;
#else
    (void)size;
    return NULL;
#endif
}
#endif

#ifdef DTCM_POOL_SCAN
/* Empirical DTCM pool discovery (Vecx PLAYDATE_ITCM_GUIDE.md method, done as
   one paint-and-watermark run): paint the candidate pool between the firmware
   data floor (~0x200074d0 per the guide's bisection) and the startup stack
   frame with a per-address sentinel, then periodically report the largest
   contiguous untouched run. Stack growth (incl. interrupts/audio) disturbs
   the top; firmware data activity disturbs the bottom; what stays clean over
   a full level is the real safe pool for RAM/hot-core relocation. */
#define DTCM_SCAN_FLOOR 0x200074d0u
#define DTCM_SCAN_WORD(p) (0xD7C30000u | ((uintptr_t)(p) & 0xFFFFu))

static uint32_t *dtcm_scan_lo = (uint32_t *)DTCM_SCAN_FLOOR;
static uint32_t *dtcm_scan_hi = NULL;

static void dtcm_scan_paint(void) {
    uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    dtcm_scan_hi = (uint32_t *)((frame - 0x200u) & ~(uintptr_t)0x3fu);
    if ((uintptr_t)dtcm_scan_hi <= (uintptr_t)dtcm_scan_lo) {
        pd->system->logToConsole("[dtcmscan] no pool (frame=%p)", (void *)frame);
        dtcm_scan_hi = NULL;
        return;
    }
    for (volatile uint32_t *p = dtcm_scan_lo; p < dtcm_scan_hi; p++)
        *p = DTCM_SCAN_WORD(p);
    pd->system->logToConsole("[dtcmscan] painted %p..%p (%u bytes, frame=%p)",
                             (void *)dtcm_scan_lo, (void *)dtcm_scan_hi,
                             (unsigned)((uintptr_t)dtcm_scan_hi - (uintptr_t)dtcm_scan_lo),
                             (void *)frame);
}

static void dtcm_scan_report(void) {
    uint32_t *run_start = NULL, *best_start = NULL;
    unsigned run_len = 0, best_len = 0;

    if (!dtcm_scan_hi)
        return;

    for (uint32_t *p = dtcm_scan_lo; p < dtcm_scan_hi; p++) {
        if (*p == DTCM_SCAN_WORD(p)) {
            if (0 == run_len)
                run_start = p;
            run_len++;
            if (run_len > best_len) {
                best_len = run_len;
                best_start = run_start;
            }
        } else {
            run_len = 0;
        }
    }

    pd->system->logToConsole("[dtcmscan] clean run %p..%p (%u bytes)",
                             (void *)best_start,
                             (void *)(best_start + best_len),
                             best_len * 4u);
}
#endif /* DTCM_POOL_SCAN */

int osd_get_show_fps(void) {
    return show_fps;
}

void osd_set_show_fps(int enabled) {
    show_fps = enabled ? 1 : 0;
}

/* --- Persistent settings (saved to the game's data folder) ---------------- */

#define SETTINGS_FILE "settings.cfg"

void osd_save_settings(void) {
    SDFile *f = pd->file->open(SETTINGS_FILE, kFileWrite);
    if (!f)
        return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "frameskip=%d\nshowfps=%d\n",
                     frame_skip, show_fps);
    if (n > 0)
        pd->file->write(f, buf, (unsigned int)n);
    pd->file->close(f);
}

void osd_load_settings(void) {
    SDFile *f = pd->file->open(SETTINGS_FILE, kFileRead | kFileReadData);
    if (!f)
        return;
    char buf[128];
    int n = pd->file->read(f, buf, sizeof(buf) - 1);
    pd->file->close(f);
    if (n <= 0)
        return;
    buf[n] = '\0';

    const char *p = strstr(buf, "frameskip=");
    if (p)
        osd_set_frame_skip(atoi(p + 10));
    p = strstr(buf, "showfps=");
    if (p)
        osd_set_show_fps(atoi(p + 8));
}

static int playdate_update(void *ud) {
    if (app_return_to_picker_if_requested())
        return 1;

#if defined(PD_PLAYBENCH_ENABLED) && !defined(PD_PLAYBENCH_RECORD)
    /* Benchmark over: the Playdate C SDK has no quit-to-launcher, so we stop
       emulating and show a completion screen. The report is already in the
       console / benchmarks/latest.txt; exit via the system menu. */
    if (pd_playbench_is_finished()) {
        static int bench_end_shown = 0;
        if (!bench_end_shown) {
            bench_end_shown = 1;
#ifdef NES6502_PAIRPROFILE
            {
                uint16 pairs[NES6502_PAIRPROFILE_TOP];
                uint32 counts[NES6502_PAIRPROFILE_TOP];
                uint32 total = 0, dropped = 0;
                nes6502_pair_profile_snapshot_top(pairs, counts, &total, &dropped);
                pd->system->logToConsole(
                    "[pairprof] total=%u dropped=%u top=%02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u",
                    (unsigned)total, (unsigned)dropped,
                    pairs[0] >> 8, pairs[0] & 0xFF, (unsigned)counts[0],
                    pairs[1] >> 8, pairs[1] & 0xFF, (unsigned)counts[1],
                    pairs[2] >> 8, pairs[2] & 0xFF, (unsigned)counts[2],
                    pairs[3] >> 8, pairs[3] & 0xFF, (unsigned)counts[3],
                    pairs[4] >> 8, pairs[4] & 0xFF, (unsigned)counts[4],
                    pairs[5] >> 8, pairs[5] & 0xFF, (unsigned)counts[5],
                    pairs[6] >> 8, pairs[6] & 0xFF, (unsigned)counts[6],
                    pairs[7] >> 8, pairs[7] & 0xFF, (unsigned)counts[7]);
                pd->system->logToConsole(
                    "[pairprof] next=%02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u %02X%02X:%u",
                    pairs[8] >> 8, pairs[8] & 0xFF, (unsigned)counts[8],
                    pairs[9] >> 8, pairs[9] & 0xFF, (unsigned)counts[9],
                    pairs[10] >> 8, pairs[10] & 0xFF, (unsigned)counts[10],
                    pairs[11] >> 8, pairs[11] & 0xFF, (unsigned)counts[11],
                    pairs[12] >> 8, pairs[12] & 0xFF, (unsigned)counts[12],
                    pairs[13] >> 8, pairs[13] & 0xFF, (unsigned)counts[13],
                    pairs[14] >> 8, pairs[14] & 0xFF, (unsigned)counts[14],
                    pairs[15] >> 8, pairs[15] & 0xFF, (unsigned)counts[15]);
            }
#endif
#ifdef NES6502_STA_ABSY_PAGEFILL
            {
                uint32 batches = 0, iterations = 0;
                nes6502_pagefill_stats_snapshot(&batches, &iterations);
                pd->system->logToConsole(
                    "[pagefill] batches=%u iterations=%u",
                    (unsigned)batches, (unsigned)iterations);
            }
#endif
#ifdef NES6502_DTCM_PAGEFILL_BLOCK
            pd->system->logToConsole(
                "[dtcmblock] native_batches=%u",
                (unsigned)nes6502_dtcm_pagefill_native_batch_count());
#endif
#ifdef NES6502_DTCM_LOOKUP_BLOCK
            pd->system->logToConsole(
                "[dtcmlookup] native_calls=%u",
                (unsigned)nes6502_dtcm_lookup_native_count());
#endif
#ifdef NES6502_PAD_SERIAL_LOOP
            {
                uint32 batches = 0, reads = 0, nonzero = 0;
                nes6502_padloop_stats_snapshot(&batches, &reads, &nonzero);
                pd->system->logToConsole(
                    "[padloop] batches=%u reads=%u nonzero=%u",
                    (unsigned)batches, (unsigned)reads, (unsigned)nonzero);
            }
#endif
#ifdef NES6502_RESIDUAL_COPY_LOOPS
            {
                uint32 rb = 0, ri = 0, pb = 0, pi = 0;
                nes6502_copyloop_stats_snapshot(&rb, &ri, &pb, &pi);
                pd->system->logToConsole(
                    "[copyloops] ram_batches=%u ram_iters=%u ppu_batches=%u ppu_iters=%u",
                    (unsigned)rb, (unsigned)ri,
                    (unsigned)pb, (unsigned)pi);
            }
#endif
#ifdef NES6502_PCPROFILE
            {
                uint16 pcs[NES6502_PCPROFILE_TOP];
                uint32 counts[NES6502_PCPROFILE_TOP];
                uint32 total = 0;
                nes6502_pc_profile_snapshot_top(pcs, counts, &total);
                for (int base = 0; base < NES6502_PCPROFILE_TOP; base += 8) {
                    char line[256];
                    int used = snprintf(line, sizeof(line),
                                        "[pcprof] total=%u rank=%d",
                                        (unsigned)total, base + 1);
                    for (int i = 0; i < 8 && used > 0
                                      && used < (int)sizeof(line); i++) {
                        used += snprintf(line + used, sizeof(line) - (size_t)used,
                                         " %04X:%u", pcs[base + i],
                                         (unsigned)counts[base + i]);
                    }
                    pd->system->logToConsole("%s", line);
                }
            }
#endif
            const char *ferr = NULL;
            LCDFont *f = pd->graphics->loadFont(
                "/System/Fonts/Asheville-Sans-14-Bold.pft", &ferr);
            pd->graphics->clear(kColorWhite);
            if (f) {
                pd->graphics->setFont(f);
                const char *m = "Benchmark complete";
                int w = pd->graphics->getTextWidth(f, m, (int)strlen(m),
                                                   kUTF8Encoding, 0);
                pd->graphics->drawText(m, strlen(m), kUTF8Encoding, (400 - w) / 2,
                                       112);
            }
            pd->system->logToConsole(
                "[bench] complete -- report above; exit via the menu");
        }
        return 1;
    }
#endif

#ifdef DTCM_POOL_SCAN
    {
        static int scan_frames = 0;
        if (0 == scan_frames)
            dtcm_scan_paint();
        if (0 == (++scan_frames % 600))
            dtcm_scan_report();
    }
#endif

    int auto_mode = (frame_skip == FRAME_SKIP_AUTO);
    int draw;
#if defined(PD_PLAYBENCH_ENABLED) || defined(PD_PLAYBENCH_RECORD)
    /* Emulate every frame fully so record/replay are frame-deterministic. Frameskip
       takes the approximate sprite-0-hit path (nes_ppu.c) on skipped frames, and
       Auto skip is real-time dependent, which desyncs a replay from its recording. */
#ifdef PD_PLAYBENCH_FIXED_SKIP
    if (skip_counter <= 0) {
        draw = 1;
        skip_counter = 1;
    } else {
        draw = 0;
        skip_counter--;
    }
#else
    draw = 1;
#endif
    (void)auto_mode;
#else
    if (skip_counter <= 0) {
        draw = 1;
        skip_counter = auto_mode
                           ? (auto_boosted ? AUTO_SKIP_BOOSTED : AUTO_SKIP_BASE)
                           : frame_skip;
    } else {
        draw = 0;
        skip_counter--;
    }
#endif

    uint32_t work_start = 0;
    if (auto_mode)
        work_start = pd->system->getCurrentTimeMilliseconds();

#ifdef PD_PLAYBENCH_ENABLED
    uint32_t pb_work_start = pd->system->getCurrentTimeMilliseconds();
#endif

    diag_frame_begin();
    diag_render_begin(draw);
    nes_renderframe(draw);
    diag_render_end();
    osd_getinput();
    diag_audio_begin();
    sound_fill_buffer();
    diag_audio_end();
    diag_frame_end();

#ifdef PD_PLAYBENCH_ENABLED
    /* Report this frame's emulation work time to the benchmark (no-op until the
       script's measurement window is running). draw==0 means a skipped frame. */
    pd_playbench_report_frame(
        (float)(pd->system->getCurrentTimeMilliseconds() - pb_work_start),
        draw ? 0 : 1);
#endif

    if (auto_mode) {
        uint32_t work = pd->system->getCurrentTimeMilliseconds() - work_start;
        auto_ema_fp += ((int32_t)(work << 4) - auto_ema_fp) >> 2;

        if (auto_boosted) {
            auto_boost_age++;
            if (auto_boost_age >= auto_boost_hold &&
                auto_ema_fp < AUTO_BOOST_EXIT_FP) {
                auto_boosted = 0;
                auto_frames_since_exit = 0;
#ifdef DIAG
                pd->system->logToConsole("[autoskip] 2->1 after %d frames",
                                         auto_boost_age);
#endif
            }
        } else {
            if (auto_frames_since_exit < 0x7FFF)
                auto_frames_since_exit++;
            if (auto_ema_fp > AUTO_BOOST_ENTER_FP) {
                auto_boosted = 1;
                auto_boost_age = 0;
                auto_boost_hold = (auto_frames_since_exit < AUTO_FLAP_WINDOW)
                                      ? AUTO_BOOST_HOLD_LONG
                                      : AUTO_BOOST_MIN_FRAMES;
#ifdef DIAG
                pd->system->logToConsole("[autoskip] 1->2 ema=%d/16 ms hold=%d",
                                         (int)auto_ema_fp, auto_boost_hold);
#endif
            }
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

/* Battery SRAM lives in a shared saves folder as <romname>.sav, kept separate
   from the ROMs. Reads/writes go through the Playdate file API, the same way
   settings and ROM data do. */
#define SRAM_SAVE_DIR "/Shared/Emulation/nes/saves/"

/* Build the .sav path: SRAM_SAVE_DIR + the ROM's basename with a .sav suffix. */
static void osd_sram_path(const char *rom_path, char *out, size_t outsz) {
    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    snprintf(out, outsz, "%s%s", SRAM_SAVE_DIR, base);
    osd_newextension(out, ".sav");
}

int osd_load_sram(const char *rom_path, unsigned char *sram, int len) {
#ifdef PD_PLAYBENCH_FRESH_SRAM
    /* Deterministic Kirby record/replay starts from the cart's zero-filled
       battery RAM and must never inherit the player's persistent save. */
    (void)rom_path;
    (void)sram;
    (void)len;
    pd->system->logToConsole("[sram] fresh benchmark mode: load suppressed");
    return -1;
#else
    char fn[PATH_MAX + 1];
    osd_sram_path(rom_path, fn, sizeof(fn));

    SDFile *f = pd->file->open(fn, kFileRead | kFileReadData);
    if (!f)
        return -1; /* no save yet — normal on first play */

    int n = pd->file->read(f, sram, (unsigned int)len);
    pd->file->close(f);
    return (n == len) ? 0 : -1;
#endif
}

int osd_save_sram(const char *rom_path, const unsigned char *sram, int len) {
#ifdef PD_PLAYBENCH_FRESH_SRAM
    /* Opening the system menu to dump a recording triggers a save flush. Do
       not let a test session overwrite the user's real battery save. */
    (void)rom_path;
    (void)sram;
    (void)len;
    pd->system->logToConsole("[sram] fresh benchmark mode: save suppressed");
    return -1;
#else
    char fn[PATH_MAX + 1];
    osd_sram_path(rom_path, fn, sizeof(fn));

    pd->file->mkdir(SRAM_SAVE_DIR); /* ensure the saves folder exists */

    SDFile *f = pd->file->open(fn, kFileWrite);
    if (!f) {
        pd->system->logToConsole("[sram] cannot write %s: %s", fn,
                                 pd->file->geterr());
        return -1;
    }

    int n = pd->file->write(f, (void *)sram, (unsigned int)len);
    pd->file->close(f);
    return (n == len) ? 0 : -1;
#endif
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
