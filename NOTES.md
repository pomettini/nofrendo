# NOTES.md

NES emulator (Nofrendo core) port to Playdate. Current performance target: full-speed
50 fps PAL-like gameplay first; visual quality and accuracy can trail while speed is
being established.

## Target hardware

| | |
|---|---|
| CPU | Cortex-M7 @ 168 MHz |
| RAM | 16 MB |
| Display | 400×240, 1-bit LCD |
| Audio | Mono speaker + headphone jack |
| Controls | D-pad, A, B, Menu, Lock, Crank |

## Device install policy

Always install the current test build as `nofrendo.pdx` on the Playdate. Do not push
separately named diagnostic copies such as `nofrendo-batchcpu16-...pdx`; keep only one
on-device copy and let new experiments overwrite it.

The `make _push` helper copies `nofrendo.pdx/.` into the existing device package so it does
not create a nested `nofrendo.pdx/nofrendo.pdx` folder.

Diagnostic performance builds should be log-only unless the on-screen FPS HUD is explicitly
needed. With frame skipping enabled, skipped visual frames must not mark the whole LCD dirty;
otherwise the Playdate still performs a display update on frames where the emulator did not
draw anything.

The runtime Playdate menu should not expose background/sprite render toggles. The old
`Draw BG` and `Draw Sprites` diagnostic checkmarks made real gameplay testing too easy to
skew. Use compile-time targets such as `diag-nobg` or `diag-nosprites` only when measuring
those rows.

Current promoted speed line after the 2026-05-25 Mario 1-1 tests is direct audio fill,
display dirty-on-draw, direct CPU memory I/O, and safe fast absolute-JMP fetch. Fast branch
and broad one-byte operand fetches were safe but mixed, so keep them off unless retesting.
The narrow `diag-fastmemops` load/store specialization was also mixed and is not promoted.
The `diag-fastbatch` batch-24 timing probe was also mixed and is not promoted. The
`diag-skipcache` sprite-cache-on-draw-only probe regressed the busy band and is not
promoted. `diag-fixedcycles` was safe but effectively flat, so do not stack future tests on
it. `diag-fastoamdma` was safe in Mario 1-1, with no softlocks or enemy weirdness, but did
not move the worst `cpu_only=25 ms` band enough to promote as the main answer by itself.
`diag-fpslite` proved the remaining slowdowns are not diagnostic-timer overhead.
`diag-fastbne` was mixed and is not promoted: it improved some old dips, but regressed
others. `diag-cycletrim` at 92% was also mixed and is not promoted: it improved some windows
but added annoying visual glitches and shifted a bad dip later in the level. The gentler
96% cycle-trim probe had no meaningful visual glitches, but the speed result was still
mixed: several windows improved, while some earlier busy windows regressed. The middle
94% cycle-trim probe was not better and had a few minor graphical glitches, so keep
cycle-trim experiments unpromoted. The next probe is `diag-jumptable`, which restores
normal CPU timing (`cyclepct=100`) and tests computed-goto CPU opcode dispatch against the
stable switch dispatcher. It has been installed as the single main `nofrendo.pdx` device
package; expected banner is `build=2026-05-26 02:28:48` with `cpu_dispatch=jump`.

## What's here

`src/nofrendo/` — upstream Nofrendo emulator core, unchanged. Keep it that way; all platform work goes in `src/`.

`src/2048.nes` — bundled test ROM.

## Key challenges

**1-bit display.** The NES outputs 256×240 at 64 colors. The Playdate screen is 400×240 monochrome. The image fits centered (72px padding on each side). Color-to-1-bit conversion via ordered (Bayer) dithering is the most practical approach at this CPU budget — threshold dithering per scanline with a precomputed luminance LUT.

**Audio.** The APU is enabled by the current Makefile and can be compiled out with
`AUDIO=OFF` for profiling. Playdate uses a pull-model audio callback
(`pd->sound->addSource`). The APU fills a buffer; the callback drains it. Sample rate:
22050 Hz mono, 8-bit (matches the existing `osd_getsoundinfo` stub).

**ROM loading.** Playdate reads files from the data folder on the SD card via `pd->file->open`. The ROM path should be hardcoded as `"cartridge.nes"` initially, matching the NumWorks convention.

**Save states.** The Nofrendo statefile API already exists (`nesstate.c`). `statefile_wrapper` stubs it out on NumWorks; on Playdate we can implement it for real using `pd->file->open` with write access.

## Roadmap

### Phase 1 — Build system ✓
- [x] Write `CMakeLists.txt` using the Playdate SDK cmake toolchain (`PlaydateSDK/C_API/buildsupport/setup.cmake`)
- [x] Wire `src/nofrendo/` sources, include paths, and audio flag (cmake `-DAUDIO=ON`)
- [ ] Verify it links (even if it crashes at runtime) — blocked on Phase 2

### Phase 2 — Minimal platform layer ✓
New files created in `src/`:

| File | Responsibility |
|---|---|
| `main.c` | `eventHandler` entry point; register update callback; call `nofrendo_main` |
| `osd.c` | `osd_init`, `osd_shutdown`, `osd_main` (calls `main_loop("cartridge.nes", system_nes)`) |
| `timing.c` | `osd_installtimer`, `osd_nofrendo_ticks` via `pd->system->getCurrentTimeMilliseconds()` |
| `sound.c` | `osd_setsound`, `osd_getsoundinfo` stubs (silence until Phase 5) |
| `stubs.c` | GUI stubs, `vid_*` stubs — same as NumWorks version |
| `statefile_wrapper.c` | Stub initially (`return NULL` / `return -1`) |

Goal: boots to a black screen without crashing.

### Phase 3 — Display ✓
- [x] Implement `display.c`: register `viddriver_t`, implement `ppu_scanline_blit`
- [x] Precompute luminance LUT using Rec. 601 integer approximation (`77r + 150g + 29b >> 8`)
- [x] Bayer 4×4 dithering per pixel via `pd->graphics->getFrame()` + `markUpdatedRows`
- [x] Center 256×240 within 400×240 (72px left offset, byte-aligned)

Note: use `getFrame()` not `getDisplayBufferBitmap()` — the latter is a read-only copy.

### Phase 4 — Input ✓
- [x] Implement `keyboard.c`: poll `pd->system->getButtonState()` each frame
- [x] Mapping: D-pad → arrows, A → NES A, B → NES B
- [x] Start / Select wired as Playdate system menu items (`pd->system->addMenuItem`)

### Phase 5 — Audio ✓
- [x] `-DAUDIO=ON` in Makefile, `sndhrdw/*.c` + `src/nofrendo/sndhrdw` include path added
- [x] `pd->sound->addSource` callback registered in `osd_setsound`
- [x] Lock-free SPSC ring buffer (4096 × int16); APU output at 22050 Hz, upsampled 2:1 to 44100 Hz
- [x] `sound_fill_buffer` fills based on elapsed ms so audio tracks wall-clock time regardless of FPS
- [x] `AUDIO_DIRECT_RING` fills the SPSC ring directly, avoiding an extra temp-buffer copy on the game thread

### Phase 6 — ROM loading from SD card ✓
- [x] `osd_getromdata` loads `"cartridge.nes"` from the bundle via `pd->file->stat` + `pd->file->open`
- [x] `osd_unloadromdata` frees the heap buffer

### Phase 7 — Performance ✓ (first pass)
- [x] Fixed `-O3` being overridden by SDK's `-O2`: re-append `-O3` after `include(playdate_game.cmake)`
- [x] Dithering replaced with branch-free `white4[4][256]` LUT: 8 AND+OR ops per 8 pixels, no comparisons

Tested at 26 FPS on device (2048.nes). Further gains if needed:
- Frame-skip: call `nes_renderframe(false)` on frames where `draw_flag` isn't needed (PPU skips bg/sprite fill)
- Per-file `-Ofast` for `nes6502.c` and `nes_ppu.c` (safe — no FP in those files)

### Phase 8 — Save states (removed)
Save states removed from scope. `nesstate.c` and `libsnss/` are excluded from the build;
`state_save`/`state_load` are stubbed in `stubs.c`. SRAM battery saves (games like Zelda)
may be added in a post-polish pass using `pd->file->open`.

### Phase 9 — Polish
- [ ] Card art (`pdxinfo` launcher image)
- [ ] `pdxinfo` metadata file (name, author, bundle ID, version)
- [ ] Menu item for reset / ROM select if multiple ROMs are present
