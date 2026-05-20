# NOTES.md

NES emulator (Nofrendo core) port to Playdate, targeting full-speed 60 fps NTSC.

## Target hardware

| | |
|---|---|
| CPU | Cortex-M7 @ 168 MHz |
| RAM | 16 MB |
| Display | 400×240, 1-bit LCD |
| Audio | Mono speaker + headphone jack |
| Controls | D-pad, A, B, Menu, Lock, Crank |

## What's here

`src/nofrendo/` — upstream Nofrendo emulator core, unchanged. Keep it that way; all platform work goes in `src/`.

`src/2048.nes` — bundled test ROM.

## Key challenges

**1-bit display.** The NES outputs 256×240 at 64 colors. The Playdate screen is 400×240 monochrome. The image fits centered (72px padding on each side). Color-to-1-bit conversion via ordered (Bayer) dithering is the most practical approach at this CPU budget — threshold dithering per scanline with a precomputed luminance LUT.

**Audio.** The APU is compiled out by default (`AUDIO=1` to enable). Playdate uses a pull-model audio callback (`pd->sound->addSource`). The APU fills a buffer; the callback drains it. Sample rate: 22050 Hz mono, 8-bit (matches the existing `osd_getsoundinfo` stub).

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

### Phase 5 — Audio
- [ ] Enable APU: add `-DAUDIO=1` to CMake, add `sndhrdw/*.c` sources
- [ ] Register a `pd->sound->addSource` callback
- [ ] Implement ring buffer between APU fill and audio callback drain
- [ ] Tune buffer size to avoid underruns without adding latency

### Phase 6 — ROM loading from SD card
- [ ] Implement `osd_getromdata` using `pd->file->open` + `pd->file->read` to load `"cartridge.nes"` into a heap buffer
- [ ] Implement `osd_unloadromdata` to free it

### Phase 7 — Performance
Profile on hardware before optimizing. Likely bottlenecks in order:
1. Dithering loop — consider SWAR tricks or a 4-pixel-wide LUT
2. 6502 core — `nes6502.c` is already reasonably tight
3. PPU rendering — scanline blit path (`ppu_scanline_blit`) avoids a full framebuffer copy; make sure it's the active path

If full 60 fps isn't achievable: implement frame-skip (render every Nth frame, still run CPU/APU every frame).

### Phase 8 — Save states
- [ ] Implement `statefile_wrapper.c` using `pd->file->open`/`read`/`write`/`close`
- [ ] Trigger save/load on crank dock/undock or a menu option

### Phase 9 — Polish
- [ ] Card art (`pdxinfo` launcher image)
- [ ] `pdxinfo` metadata file (name, author, bundle ID, version)
- [ ] Menu item for reset / ROM select if multiple ROMs are present
