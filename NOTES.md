# NOTES.md

FamiCrank is an NES emulator for Playdate built on the Nofrendo core. Current performance
target: full-speed 50 fps PAL-like gameplay first; visual quality and accuracy can trail
while speed is being established.

Status as of 2026-06-06: the core Playdate port roadmap is complete. The app builds as a
Playdate `FamiCrank.pdx`, boots into the ROM picker, launches games cleanly, supports audio,
runtime frameskip/FPS settings, and has a promoted fast default build. Remaining work is
optional polish or compatibility work, not required for the main roadmap.

Status as of 2026-07-09 (0.3): post-launch fixes shipped after user reports — ROM picker
cap raised 256→1024, on-screen load-failure message (shows the core's reason, e.g. an
unsupported mapper), battery (SRAM) saves to `/Shared/Emulation/nes/saves/` written on
return-to-picker / system-menu / terminate (save states not supported), and motion-based
crank Start/Select. The crank change is the notable one: position-based mapping fired a
phantom Start from whatever angle the crank rested at when undocked, so it was replaced
with `getCrankChange` motion detection. See `FINDINGS.md` for details.

## Target hardware

| | |
|---|---|
| CPU | Cortex-M7 @ 168 MHz |
| RAM | 16 MB |
| Display | 400×240, 1-bit LCD |
| Audio | Mono speaker + headphone jack |
| Controls | D-pad, A, B, Menu, Lock, Crank |

## Device install policy

Always install the current test build as `FamiCrank.pdx` on the Playdate. Do not push
separately named diagnostic copies such as `FamiCrank-batchcpu16-...pdx`; keep only one
on-device copy and let new experiments overwrite it.

When sending a test build to the device, copy the package and eject the Playdate. Do not
launch the app from the toolchain; the tester will launch it and provide serial output.

The `make _push` helper copies `FamiCrank.pdx/.` into the existing device package so it does
not create a nested `FamiCrank.pdx/FamiCrank.pdx` folder.

Diagnostic performance builds should be log-only unless the on-screen FPS HUD is explicitly
needed. With frame skipping enabled, skipped visual frames must not mark the whole LCD dirty;
otherwise the Playdate still performs a display update on frames where the emulator did not
draw anything.

The runtime Playdate menu should not expose background/sprite render toggles. The old
`Draw BG` and `Draw Sprites` diagnostic checkmarks made real gameplay testing too easy to
skew. Use compile-time targets such as `diag-nobg` or `diag-nosprites` only when measuring
those rows.

Current promoted speed line: `FAST_FLAGS` is the default Makefile path via
`FLAGS ?= $(FAST_FLAGS)`. It starts from `BASE_FLAGS` and adds only the currently promoted
speed flags, so plain `make`, `make device`, `make sim`, and `make install` stay on the
fastest known safe build.

Promoted flags and why they are on:

- `CMAKE_BUILD_TYPE=Release` and `NES6502_OPT_LEVEL=O3`: keep the hot emulator code on the
  measured fast compiler profile.
- `AUDIO=ON`: ship with APU audio enabled; audio-off is diagnostic only.
- `AUDIO_DIRECT_RING=ON`: fill the Playdate audio ring directly from the emulator thread,
  avoiding an extra temporary-buffer copy.
- `DIAG=OFF`: keep the default package on the fastest clean path. Timing logs are available
  through explicit diagnostic targets such as `diag`, `diag-fpslite`, and `diag-fastbeq`.
- `PPU_BG=ON`, `PPU_SPRITES=ON`, and `PPU_BLIT=ON`: keep normal rendering enabled;
  `diag-nobg`, `diag-nosprites`, and `diag-noblit` are measurement targets only.
- `PPU_FAST_OAMDMA=ON`: copy OAM DMA directly from the mapped CPU page while preserving
  the DMA cycle burn; safe in Mario 1-1 and cheap enough to keep in the promoted stack.
- `NES_CPU_BATCH_SCANLINES=16`: run the CPU in 16-scanline slices for fewer interpreter
  entries and less CPU/PPU cache ping-pong. Wider batches broke timing or felt worse.
- `NES_CPU_CYCLE_PERCENT=100`: keep normal CPU timing. Cycle-trim probes were mixed and
  caused visual issues at more aggressive settings.
- `NES6502_DIRECT_MEMIO=ON`: fast-path common CPU memory/I/O access instead of routing
  everything through the generic handler scan.
- `NES6502_FAST_JMP_ABS=ON`: optimize hot absolute-JMP operand fetches, useful for idle
  and wait-loop-heavy NES code.
- `NES6502_LAZY_CYCLES=ON`: avoid writing `cpu.total_cycles` on every opcode; derive live
  totals only when the renderer/timing path asks for them.
- `NES6502_FAST_BNE=ON`, `NES6502_FAST_BPL=ON`, and `NES6502_FAST_BEQ=ON`: narrow,
  timing-safe branch fast paths promoted after the lazy-cycle baseline.
- `NES6502_FAST_MEMOPS=ON`: hand-specialize the measured hot memory load/store opcodes on
  the promoted lazy/branch baseline; the current 1-1 row narrows the bad band to one brief
  dip and recovers quickly.

Rejected or diagnostic-only probes remain off for a reason:

- `NES6502_JUMPTABLE_DISPATCH=ON` was visually safe but not faster than the switch
  dispatcher in busy Mario windows.
- `NES_CPU_BATCH_SCANLINES=24/32`, `PPU_FAST_STRIKE=ON`, and pending-line strike-bias
  experiments caused slow motion, timing risk, or mixed results.
- `NES_CPU_CYCLE_PERCENT` below 100 was mixed; 92% had visible glitches, 94% was not
  better, and 96% was visually safer but not a clean speed win.
- Broad `NES6502_FAST_BRANCHES`, broad operand-byte fetches, `NES6502_FAST_PC_OPS`,
  broad hot-op specializations, and contiguous/linear PRG-ROM probes were flat, mixed, or
  regressed important windows.
- `PPU_SPRITE_CACHE_DRAW_ONLY=ON` and other sprite-cache gating ideas regressed feel or
  busy-band timing.
- `NES_FIXED_SCANLINE_CYCLES=ON`, loop alignment, spin hacks, physical ITCM placement,
  CPU-struct DTCM placement, and global alignment/size experiments did not produce a safe
  net win.
- `NES6502_TCMHOT_PROBE=ON` is a diagnostic-only fast-memory proof. It verifies the
  relocation mechanism with tiny probes before any real emulator hot path is moved.
- `NES6502_TCMHOT_CORE=ON` is diagnostic-only and currently rejected as a speed row. The
  first tiny relocated 6502 core activated on device but still dipped to 43 fps in Mario
  1-1 and added a worse 39 fps window. `diag-tcmstats` showed that after level entry this
  wrapper handles zero cycles because it is only tried at CPU-slice entry; do not extend
  this exact shape.

Runtime defaults: `Frameskip` defaults to `1`, where the user-facing value means "skip
this many frames between draws." Thus `0` draws every frame, `1` draws every other frame,
and `2` draws one frame then skips two. `Show FPS` defaults on and uses Playdate's native
`drawFPS(0, 0)` counter. On skipped visual frames with FPS disabled, the update callback
returns `0`; with FPS enabled, only the top rows used by the native FPS counter are marked
dirty.

## What's here

`src/nofrendo/` — upstream Nofrendo emulator core, unchanged. Keep it that way; all platform work goes in `src/`.

`src/2048.nes` — bundled test ROM.

## Key challenges

**1-bit display.** The NES outputs 256×240 at 64 colors. The Playdate screen is 400×240 monochrome. The image fits centered (72px padding on each side). Color-to-1-bit conversion via ordered (Bayer) dithering is the most practical approach at this CPU budget — threshold dithering per scanline with a precomputed luminance LUT.

**Audio.** The APU is enabled by the current Makefile and can be compiled out with
`AUDIO=OFF` for profiling. Playdate uses a pull-model audio callback
(`pd->sound->addSource`). The APU fills a buffer; the callback drains it. Sample rate:
22050 Hz mono, 8-bit (matches the existing `osd_getsoundinfo` stub).

**ROM loading.** Startup uses `pd-rom-picker` to browse `.nes` files from `/Shared/Emulation/nes/games/`, then passes the selected path into the existing Nofrendo loader. ROMs are loaded only through the picker.

**Input.** D-pad maps to NES D-pad, A maps to NES A, and B maps to NES B. Start and Select
are no longer menu items: with the crank undocked, angle less than 60 degrees holds Select
and angle greater than 180 degrees holds Start.

**Save states and battery saves.** Save states are removed from scope. SRAM battery saves
for games such as Zelda are not supported for now.

## Roadmap

### Phase 1 — Build system ✓
- [x] Write `CMakeLists.txt` using the Playdate SDK cmake toolchain (`PlaydateSDK/C_API/buildsupport/setup.cmake`)
- [x] Wire `src/nofrendo/` sources, include paths, and audio flag (cmake `-DAUDIO=ON`)
- [x] Verify it links and produces `FamiCrank.pdx`

### Phase 2 — Minimal platform layer ✓
New files created in `src/`:

| File | Responsibility |
|---|---|
| `main.c` | `eventHandler` entry point; start ROM picker; launch `nofrendo_main` with selected ROM |
| `osd.c` | `osd_init`, `osd_shutdown`, `osd_main` (calls `main_loop(selected_rom, system_nes)`) |
| `timing.c` | `osd_installtimer`, `osd_nofrendo_ticks` via `pd->system->getCurrentTimeMilliseconds()` |
| `sound.c` | Playdate pull-audio source plus emulator audio ring-buffer plumbing |
| `stubs.c` | GUI and state stubs for features outside the Playdate scope |
| `keyboard.c` | Playdate button and crank polling mapped to NES controller events |

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
- [x] Start / Select mapped to crank zones: `< 60°` holds Select, `> 180°` holds Start
- [x] Remove Start / Select Playdate menu items

### Phase 5 — Audio ✓
- [x] `-DAUDIO=ON` in Makefile, `sndhrdw/*.c` + `src/nofrendo/sndhrdw` include path added
- [x] `pd->sound->addSource` callback registered in `osd_setsound`
- [x] Lock-free SPSC ring buffer (4096 × int16); APU output at 22050 Hz, upsampled 2:1 to 44100 Hz
- [x] `sound_fill_buffer` fills based on elapsed ms so audio tracks wall-clock time regardless of FPS
- [x] `AUDIO_DIRECT_RING` fills the SPSC ring directly, avoiding an extra temp-buffer copy on the game thread

### Phase 6 — ROM loading from SD card ✓
- [x] `pd-rom-picker` scans `/Shared/Emulation/nes/games/` for `.nes` files
- [x] `osd_getromdata` loads the selected ROM path via `pd->file->stat` + `pd->file->open`
- [x] `osd_unloadromdata` frees the heap buffer

### Phase 7 — Performance ✓ (first pass)
- [x] Fixed `-O3` being overridden by SDK's `-O2`: re-append `-O3` after `include(playdate_game.cmake)`
- [x] Dithering replaced with branch-free `white4[4][256]` LUT: 8 AND+OR ops per 8 pixels, no comparisons
- [x] Promoted `FAST_FLAGS` as the default build path
- [x] Added dirty-on-draw display updates so skipped visual frames do not refresh the full LCD
- [x] Added runtime `Frameskip` options `0`, `1`, `2`; default `1`
- [x] Added native Playdate `Show FPS` toggle; default on

Early testing was 26 FPS on device with `2048.nes`; later Mario 1-1 performance work is
tracked in `PERF.md`. Further gains are now perf-backlog work rather than roadmap blockers.

### Phase 8 — Save states (removed)
Save states removed from scope. `nesstate.c` and `libsnss/` are excluded from the build;
`state_save`/`state_load` are stubbed in `stubs.c`. SRAM battery saves are also out of
scope for now.

### Phase 9 — Polish ✓ for core UX
- [x] `pdxinfo` metadata file (name, author, bundle ID, version)
- [x] Startup ROM picker via `pd-rom-picker`
- [x] In-game `ROM Picker` menu item
- [x] No redundant `ROM Picker` item while already in the picker
- [x] Menu order: `ROM Picker`, `Frameskip`, `Show FPS` in game; settings only in picker
- [x] Clear the screen/framebuffer to black before launching a ROM so the picker does not bleed into gameplay
- [x] Updated `pd-rom-picker` submodule to the latest available version used by this port
- [x] Card art (`pdxinfo` launcher image)
- [x] Reset command intentionally left out of scope
- [x] SRAM battery saves intentionally left unsupported for now
