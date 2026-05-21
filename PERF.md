# Performance Notes — Nofrendo Playdate Port

Running log of profiling data, findings, and optimization decisions.
Target: **60 fps** (native NES NTSC speed). Current best: **~32 fps**.

---

## Hardware

| | |
|---|---|
| CPU | ARM Cortex-M7 @ 168 MHz |
| I-cache | 4 KB (Rev A) / 16 KB (Rev B) |
| D-cache | 16 KB |
| DTCM | 64 KB (instruction-side TCM) |
| Display | 400×240, 1-bit LCD |

---

## Profiling method

`make diag` builds with `-DDIAG=1`. The update callback wraps each sub-component in
`diag_render_begin/end` and `diag_blit_begin/end`. Results are logged via
`pd->system->logToConsole` every 60 frames — visible in Playdate Mirror or
`/Data/System/eventlog.txt` on the device.

**Do not use DWT (cycle counter at 0xE0001000 / 0xE000EDFC)** — those registers
are debug-privileged and cause a HardFault on the Playdate device and a segfault
on the simulator.

Sample output:
```
[diag] frame=60    fps= 31  avg=31ms  render=28ms  blit=0μs  emu=28ms
```

---

## Confirmed bottleneck

`render` (= `nes_renderframe`) accounts for ~28 ms of the ~29 ms total frame budget.
`blit` (our Bayer-dithered Playdate scanline write) measures < 1 ms — effectively free.
`other` (input + audio fill + markUpdatedRows) ≈ 1 ms in production builds.

**All remaining headroom is inside `nes_renderframe`, i.e., the NES emulation core.**

Breakdown (confirmed via profiling):
- `blit` (our Bayer dithering, 240 scanlines): **~0 ms** — unmeasurable at 1 ms timer
  resolution, confirmed free. Per-scanline timing calls added >4 ms overhead and
  corrupted readings — do NOT measure blit per-scanline with ms-resolution timers.
- `emu` = `render` − `blit` ≈ `render` ≈ **28 ms** — all time is in the NES core.

Measured internal breakdown (derived from `FRAME_SKIP=2` experiment — see below):
- 6502 + PPU state machine (no pixel fill): **~24 ms** — dominant cost
- PPU pixel rendering (`ppu_renderbg` + `ppu_renderoam` + blit): **~8 ms**
- At 168 MHz, 24 ms = 4.0 M ARM cycles for 29,780 NES cycles → ~135 ARM cycles/NES cycle,
  consistent with severe D-cache thrashing (each miss ≈ 30–50 cycles).

Frame skip ceiling: even with every frame skipped (no pixel rendering), FPS is bounded
by `1000 / 24 ms ≈ 41 fps`. Reaching 50 fps (PAL target) requires speeding up the
6502 core itself — frame skip alone is not sufficient.

Target: 50 fps (PAL NES speed). Current best with frame skip: **~31 fps**.

---

## Optimisations applied (in order)

### Compiler flags
| Change | Effect |
|---|---|
| `-O3` override after SDK include | SDK sets `-O2` as PUBLIC; our extra `-O3` comes last on command line, wins |
| `-Os -DNES6502_SWITCH` for `nes6502.c` only | Switches from threaded interpreter (computed gotos) to `while/switch` |

### 6502 dispatch: jumptable → switch
`nes6502_execute` with computed gotos (`NES6502_JUMPTABLE`): **27 KB at -O3**.
With `while/switch` dispatch + `-Os`: **13.5 KB**.

Rev B I-cache is 16 KB. At 13.5 KB the entire execute function now fits.
Rev A (4 KB) cannot be fully cached regardless of approach.

### Custom linker script (`link_map.ld`)
`-ffunction-sections` (already enabled by SDK) puts each function in its own
`.text.<name>` section. Our script pins the two hottest functions first in `.text`:

```ld
. = ALIGN(32);  *(.text.nes6502_execute)   /* 13.5 KB */
. = ALIGN(32);  *(.text.ppu_scanline)       /*  2.3 KB */
```

Combined: 15.8 KB — **fits in the Rev B 16 KB I-cache** (with <200 bytes to spare).
Both start at 32-byte boundaries (one full Cortex-M7 cache line).

### Display dithering LUT
`white4[4][256]` — one byte per (Bayer row, palette index) pair, precomputed in
`vid_setpalette`. The inner loop per 8-pixel group:
```c
row[bx] = (w4[px[0]] & 0x80) | (w4[px[1]] & 0x40) | ... | (w4[px[7]] & 0x01);
```
8 AND + 7 OR, no branches, no comparisons. Confirmed ~0 ms overhead in profiling.

### Fix: do NOT apply `-Os` to `nes_ppu.c`
Tried `-Os` for `nes_ppu.c` to shrink `ppu_scanline` (2316 → 1884 bytes).
`ppu_scanline` at 2316 bytes already fits in cache alongside the 6502.
Result: **FPS dropped from 31 to 23** — `-Os` generated slower code with no cache benefit.
**Reverted.**

### Fix: do NOT use `-falign-functions=32` globally
Bumping from SDK default (16) to 32 increases inter-function padding for every
function in the binary, reducing code density and increasing cache pressure.
**Reverted to SDK default (16).**

### drawFPS in production
`pd->system->drawFPS` costs ~2 ms per frame. Wrapped in `#ifdef DIAG` — only
present in diagnostic builds.

### Float promotion fix (`nes_pal.c`)
`sin()`/`cos()` calls were implicitly promoting `float` arguments to `double`,
pulling in 3 KB of software double-precision FP (`__kernel_rem_pio2`,
`__adddf3`, `__muldf3`). Changed to `sinf()`/`cosf()`. Note: `__adddf3` and
`__muldf3` are still linked because `apu_create` and `pal_generate` use `double`
at startup — not a runtime hotspot.

---

## Function sizes (current build, device)

```
nes6502_execute   0x0000_0000   13 532 B   (first in .text, 32-byte aligned)
ppu_scanline      0x0000_34e0    2 316 B   (immediately after, 32-byte aligned)
ppu_scanline_blit 0x0000_cf40      212 B
nes_renderframe   0x0000_8bXX      312 B
```

---

## Optimisations tried that had no measurable effect

### OAM sprite list precomputation (2024-05)
`ppu_renderoam` previously iterated all 64 OAM sprites per scanline (262 × 64 = 16,768
checks/frame), most failing the Y-range check immediately.

Change: `ppu_build_sprite_cache()` runs once at scanline 0, building
`oam_sl_count[240]` + `oam_sl_idx[240][8]` (2.1 KB static). `ppu_renderoam` now
iterates only the pre-selected sprites: O(visible per scanline) vs O(64).

**Result: no measurable difference.** The Y-range check loop overhead was ~0.5 ms
(64 × ~5 cycles × 262 scanlines), which is invisible at ms resolution. The actual
cost is the `draw_oamtile` work for sprites that DO overlap the scanline; optimising
the miss path doesn't help when the hit path dominates.

**Why render grows 28→30 ms during active play**: those 2 ms are `draw_oamtile`
calls for the visible sprites, not the loop overhead.

---

## Root cause hypothesis: D-cache pressure

Hot data accessed per frame:
- 6502 PRG ROM: 16 KB (every instruction fetch and data read)
- CHR ROM (pattern tables): 8 KB (every background tile render)
- Nametable VRAM: 4 KB (every background tile index read)
- `ppu_t` struct: 5.2 KB (all fields accessed during rendering)

**Total: ~33 KB competing for 16 KB D-cache.** Cache thrashing between PRG ROM and
CHR ROM/PPU data is the likely dominant cost. The theoretical frame budget
(~6–7 ms for pure computation) vs measured (28 ms) implies ~4× overhead — consistent
with frequent D-cache misses where each miss costs ~30–50 ARM cycles.

---

## What has NOT been tried yet

### DTCM placement for `ppu` and `cpu` structs
The global `ppu` struct (5.2 KB) and `nes6502_context cpu` are in BSS (regular RAM).
Moving them to DTCM (tightly-coupled to the CPU, bypasses cache) could eliminate
all D-cache misses for PPU field accesses. The Playdate's stack is reportedly
already in DTCM — the "copy to stack" trick should get the benefit without linker
changes.

To try: declare hot structs `__attribute__((section(".dtcm")))` and add a DTCM
region to `link_map.ld`. Needs Playdate-specific memory map confirmation.

### Frame skip — IMPLEMENTED (`FRAME_SKIP 2` in `src/osd.c`)
`nes_renderframe(false)` skips PPU pixel fills, running only the 6502 and PPU state.
`display.c` now initialises `fb_data` unconditionally on first call so skipped frames
show the last rendered frame (no white screen).

**Result: 28 fps → 30–32 fps.** `render` drops from 32 ms to ~28 ms average (avg of
render-true=32 ms and render-false=24 ms). Game logic speed unchanged. The game
subjectively feels snappier.

**Ceiling: ~41 fps** — frame skip cannot exceed `1000 / render(false) = 1000/24 ≈ 41 fps`
regardless of skip ratio. To reach 50 fps, `render(false)` (6502 cost) must be reduced.

### PPU tile/sprite rendering optimisation
`ppu_renderbg` renders 32 background tiles per visible scanline using per-tile
pattern table lookups. Opportunities: tile caching (reuse rendered tile if CHR
data unchanged), SWAR pixel packing, or reduced palette lookups.

### Per-function ITCM relocation
Copy `nes6502_execute` and `ppu_scanline` to ITCM at startup using `memcpy` +
function pointer patching (as described in the Playdate dev forum thread
https://devforum.play.date/t/dirty-optimization-secrets-c-for-playdate/23011).
ITCM bypasses the I-cache entirely; execution is deterministic.

---

## Build commands

```sh
make                 # production build (no diagnostics)
make diag            # diagnostic build — logs fps/render/blit/emu via logToConsole
make install         # build + push to connected device
make install-diag    # diagnostic build + push
PORT=/dev/cu.XXX make install   # override auto-detected serial port
```
