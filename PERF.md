# Performance Notes — Nofrendo Playdate Port

Running log of profiling data, findings, and optimization decisions.
Target: **50 fps** (PAL NES speed). Current best full-level Mario row:
**~44-50 fps** in light windows, **~33-39 fps** in the remaining slow windows.

---

## Hardware

| | |
|---|---|
| CPU | ARM Cortex-M7 @ 168 MHz |
| I-cache | 16 KB (Rev B confirmed) |
| D-cache | 16 KB, 4-way set-associative |
| ITCM | 16 KB at 0x00000000 — **MPU write-protected by OS** (see below) |
| DTCM | 64 KB at 0x20000000 — accessible, but tested with no benefit (see below) |
| Display | 400×240, 1-bit LCD |

---

## Profiling method

`DIAG=ON` is now the **default** in `Makefile FLAGS`. The update callback measures each frame
and logs via `pd->system->logToConsole` every 60 frames. Visible in Playdate Mirror or
`/Data/System/eventlog.txt` on the device.

**Important**: `diag_render_begin(bool draw_flag)` tracks two categories separately:
- `cpu_only` = average ms for **render_false** frames (6502 + PPU state, no pixels)
- `ppu_full` = average ms for **render_true** frames (6502 + PPU + pixel rendering)

Sample output:
```
[diag] build=2026-05-22 15:08:47 audio=off bg=on sprites=on
[diag] frame=300    fps= 37  avg=26ms  cpu_only=20ms  ppu_full=28ms
```

The banner includes the diagnostic build timestamp and the active audio, background, sprite,
scanline-blit, CPU experiment, and CPU-split timing flags. Check it before comparing a
matrix row against the baseline. `cpu_split=on` means every `nes6502_execute()` batch is
timed; use it for attribution, not final speed rows.

**Do not use DWT cycle counter (0xE0001000 / 0xE000EDFC)** — debug-privileged registers,
cause HardFault on device and segfault on simulator.

---

## Current best baseline — 2026-05-22

Scene: Mario 1-1, standing still, no enemies visible. `FRAME_SKIP=2`, `AUDIO=ON`, `DIAG=ON`.

| Scenario | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Idle 1-1 (no enemies) | 37 | 26 ms | 20 ms | 28 ms |
| Goombas on screen | 27–33 | 30–35 ms | 23–27 ms | 32–38 ms |
| Hitting ? box (mushroom) | 29–30 | 32–33 ms | 24–25 ms | 34–35 ms |

**Key insight from split metrics**: The enemy slowdown is **entirely on the CPU side**.
`ppu_full − cpu_only` stays constant at ~8 ms regardless of enemies. PPU rendering is NOT
the bottleneck during enemy-heavy scenes.

**Root cause of enemy slowdown**: More objects → more diverse 6502 code paths executed per
frame → NES ROM regions that weren't recently accessed → **D-cache misses**. The NES PRG ROM
is 32 KB; the D-cache is only 16 KB. When Goomba AI and mushroom physics run, they read
from different ROM regions than the idle game loop, evicting hot cache lines.

**Path to 50 fps**: need avg ≤ 20 ms. Currently (20+28)/2 = 24 ms → 41 fps idle.
Goomba case: (27+38)/2 = 32 ms → 31 fps. The 8 ms PPU cost and D-cache thrashing
with enemies are the two remaining barriers.

---

## Confirmed cost breakdown

| Component | Cost | Notes |
|---|---:|---|
| `cpu_only` (6502 + PPU state, no pixels) | **20 ms** | baseline idle |
| PPU pixel rendering (BG + sprites + blit) | **8 ms** | ppu_full − cpu_only, constant |
| `sound_fill_buffer` | ~2 ms | outside render measurement |

At 168 MHz, 20 ms = 3.36 M ARM cycles for ~29,780 NES cycles → **113 ARM cycles/NES cycle**.
Theoretical minimum (168 MHz / 1.789 MHz) = 94 cycles/NES cycle. Overhead ≈ 20%.

The ~20% overhead comes from: D-cache misses on NES ROM reads, function call overhead for
`mem_readbyte`/`mem_writebyte`, and switch dispatch overhead.

---

## Optimisations applied (in order)

### 1. Compiler flags
- `-O3` global override (SDK sets `-O2` as PUBLIC; our `-O3` comes last, wins)
- `-O2 -DNES6502_SWITCH` for `nes6502.c` only — switch dispatch vs computed gotos

### 2. 6502 dispatch: jumptable → switch (`-DNES6502_SWITCH`)
`nes6502_execute` with computed gotos: **27 KB at -O3**. With `while/switch + -O2`: **~10 KB**.
Rev B I-cache is 16 KB — entire execute function now fits.

### 3. `-DNES6502_LEGAL_ONLY` — **saved ~8 ms**
Removed ~62 undocumented opcode cases, replacing them with `ADD_CYCLES(n)` stubs.
`nes6502_execute` shrank further; function fits comfortably in 16 KB I-cache.
Before: ~34 fps. After: ~43 fps at the time.

### 4. `FRAME_SKIP=2` (`src/osd.c`)
Alternates render_true / render_false frames. avg = (30+22)/2 = 26 ms → ~38 fps vs 29 fps
with `FRAME_SKIP=1`. Game logic runs at full NES speed; only pixel output is halved.

### 5. `.itcm` section + heap copy (`nes6502_itcm_init`)
`nes6502_execute`, `mem_readbyte`, `mem_writebyte` are placed in a `.itcm` ELF section.
At startup, `nes6502_itcm_init` copies the section to a **heap SRAM block** and sets
`nes6502_execute_ptr`. Ensures all three functions are **contiguous in SRAM**, improving
I-cache utilization and eliminating BL-out-of-range issues.

Physical ITCM at 0x00001000 is **MPU write-protected by the Playdate OS** (see failures below).
Heap SRAM is the best available destination.

### 6. Sprite pattern cache (`ppu_build_sprite_cache`)
Pre-decodes CHR bitplanes for all 64 OAM sprites × 16 rows into `oam_pat1[64][16]` /
`oam_pat2[64][16]` at scanline 0. `ppu_renderoam` reads from the cache instead of doing
`PPU_MEM()` double-dereferences in the hot scanline loop.

### 7. `mem_readbyte` ROM-first branch ordering
Reordered the three branches in `mem_readbyte`: check `address >= 0x8000` (ROM, most common)
before `address < 0x800` (RAM). Saves one comparison on every data read from ROM.

### 8. `NES6502_PC_PTR` — **saved 2 ms on cpu_only** (22 ms → 20 ms)
In switch mode the opcode dispatch becomes:
```asm
ldrb.w  r0, [pc_ptr], #1   ; *pc_ptr++ — 1 load
```
instead of `bank_readbyte(PC++)` which requires a shift + 2 loads via the page table.
`PC_REBASE()` is called at every non-sequential PC change (branches taken, JMP, JSR, RTS,
RTI, BRK, NMI, IRQ). All sites verified correct. `pc_ptr` and `pc_bank_end` are kept in
ARM registers across the switch body.

*Note: an earlier experiment listed this as "no measurable effect" — that was with `-Os`
which did not keep `pc_ptr` in a register. At `-O2` it does, and the 2 ms saving is real
and consistent across multiple test runs.*

### 9. Split diag metrics
`diag_render_begin(bool draw_flag)` tracks cpu_only and ppu_full separately.
`DIAG=ON` is now the default in `Makefile FLAGS` — always-on, minimal overhead.

---

## Optimisations tried that FAILED or REGRESSED

### ITCM physical placement (0x00001000)
**Status**: Hard crash — MemManage fault.
**Details**: CFSR=0x00000082 (DACCVIOL, MMARVALID), mmfar/bfar=0x00001000.
The Playdate OS MPU marks ITCM as execute-only; `memcpy` to that address triggers a data
access violation. There is no known API to request OS-managed ITCM placement.

### First ITCM attempt (without `mem_readbyte`/`mem_writebyte` in `.itcm`)
**Status**: Crashed silently at runtime (copy succeeded but code faulted).
**Root cause**: `mem_readbyte` and `mem_writebyte` were not in `.itcm`. A `BL` from
`nes6502_execute` at ITCM (~0x1000) to those functions in SRAM (~0x20010000) spans ~512 MB —
exceeds the ±16 MB BL instruction range. Fixed by adding `__attribute__((section(".itcm")))`
to both helpers. Verified with `objdump`: all BL targets within `.itcm`.

### DTCM placement for `cpu` struct
**Status**: No improvement; same or slightly worse fps.
**Root cause**: The cpu struct is small and already D-cache hot. The bottleneck is NES ROM
data reads (diverse, cache-missing), not cpu struct access.

### Background tile pattern cache (`bg_tile_cache[256][8]`, 4 KB)
**Status**: **Regression** — cpu_only 20→22 ms, ppu_full 28→31 ms, fps 37→33.
**Approach**: Pre-interleave `pat1`/`pat2` CHR bitplanes for all 256 tile indices × 8 rows.
Replace 2 `PPU_MEM()` double-dereferences per tile in `ppu_renderbg` with one array lookup.
**Root cause of regression**: The 4 KB static array adds D-cache pressure. After
`ppu_renderbg` warms D-cache with cache entries, subsequent `nes6502_execute` calls
(including on the **following** render_false frame, since D-cache state persists between
frames) suffer more NES ROM cache misses. The PPU_MEM dereference savings are more than
offset by increased 6502 D-cache miss cost.
**Do not retry** unless a zero-D-cache-pressure destination (DTCM or ITCM) is available
for the cache array.

### `-Os` for `nes_ppu.c`
Shrinks `ppu_scanline` but generates slower code — **fps dropped from 31 to 23**. Reverted.

### `-falign-functions=32` globally
Increases inter-function padding → reduces code density → more cache pressure. Reverted.

---

## PPU cost breakdown — first matrix pass

The 8 ms delta (ppu_full − cpu_only) is split across three components. The first device
matrix pass now has enough data to rank them:

```
make install-diag-nosprites   # ppu_full_nosprites
make install-diag-nobg        # ppu_full_nobg
make install-diag-noblit      # ppu_full_noblit
make install-diag-noaudio     # APU/audio cost row
```

Use the matching install target. `make diag-nosprites install-diag` rebuilds the normal
diagnostic build during `install-diag`, so it does not install the no-sprites PDX that was
just built.

Then:
- `renderoam_cost = ppu_full − ppu_full_nosprites`   (everything minus sprites)
- `renderbg_cost  = ppu_full − ppu_full_nobg`        (everything minus BG)
- `blit_cost      = ppu_full − ppu_full_noblit`

Current measured direction:

- The background-off boot/level-entry row cuts the steady audio-on PPU delta from about
  8 ms to about 4 ms, so background pixel rendering is a material draw-frame cost.
- The no-sprites Mario 1-1 row is within the current millisecond log noise.
- The no-blit Mario 1-1 row is also within the current millisecond log noise. The
  Playdate scanline conversion loop is not the first rendering target.

### Profiling matrix setup — 2026-05-22

The first matrix pass is ready for device logs:

- `diag-nobg`, `diag-nosprites`, `diag-noblit`, and `diag-noaudio` all build locally.
- Dedicated install targets now build the requested variant and overwrite the single
  on-device `nofrendo.pdx`. Do not create separately named PDX copies for new tests.
- The normal diagnostic build also exposes Playdate menu checkmarks for `Draw BG` and
  `Draw Sprites`. Reach the scene first, change a checkmark, and capture the following
  `[diag] runtime bg=... sprites=...` marker with the log window.
- `nofrendo-noaudio.pdx` was copied to the connected Playdate as the first handoff row.

Use runtime menu cuts for same-scene exploratory logs. Keep the compile-time variants for
clean matrix rows: they remove the render call at build time and the banner makes the row
self-identifying.

Record the first stable log window for the same gameplay scene used by the baseline:

| Build | Scene | fps | avg | cpu_only | ppu_full | Status |
|---|---|---:|---:|---:|---:|---|
| `nofrendo-noaudio.pdx` | Mario 1-1 half-level traversal | 28–50 | 20–35 ms | 8–28 ms | 18–40 ms | measured |
| `nofrendo-nosprites.pdx` | Mario 1-1 standing still | 37 | 26 ms | 19–20 ms | 28 ms | measured |
| `nofrendo-noblit.pdx` | Mario 1-1 standing still | 36–37 | 26–27 ms | 19–20 ms | 27–29 ms | measured |
| `nofrendo-nobg.pdx` | Boot into Mario 1-1 | 41–50 | 20–23 ms | 8–19 ms | 14–23 ms | measured, not playable |

### Audio-off traversal — 2026-05-22

Received device log banner:

```
[diag] build=2026-05-22 15:08:47 audio=off bg=on sprites=on
```

The first run covered about half of Mario 1-1 instead of fixed benchmark scenes, so use it
as a traversal row rather than a strict before/after comparison with the audio-on baseline:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Opening 240 frames | 41–42 | 23–24 ms | 17–18 ms | 25–28 ms |
| Light windows around frames 300, 1020, and 2220 | 47–50 | 20–21 ms | 8–11 ms | 18–20 ms |
| Busy traversal windows | 28–39 | 25–35 ms | 20–28 ms | 29–40 ms |

Findings:

- Audio off is a meaningful speed-first lever: the opening traversal windows get near the
  50 fps budget on skipped visual frames, and some light windows reach 49–50 fps overall.
- It is not enough for full-speed action. Busy windows still run at 28–39 fps with
  `cpu_only` at 20–28 ms before full pixel rendering cost is added.
- During this traversal, `ppu_full - cpu_only` is usually 9–12 ms. That is a little above
  the earlier idle baseline delta of about 8 ms and makes the BG/sprite split the next
  measurement to finish.
- `nofrendo-nobg.pdx` is the next device row. It keeps audio on and sprites on while
  disabling background rendering so `renderbg_cost` can be measured against the normal
  diagnostic build.

### Background-off boot path — 2026-05-22

Received device log banner:

```
[diag] build=2026-05-22 15:18:27 audio=on bg=off sprites=on
```

The background-off build only showed horizontal glitching lines, so this row covered booting
into Mario 1-1 rather than a level traversal:

| Segment in submitted log | fps | avg | cpu_only | ppu_full | PPU delta |
|---|---:|---:|---:|---:|---:|
| Frames 60–360 | 43–44 | 22–23 ms | 18–19 ms | 22–23 ms | 4 ms |
| Light windows at frames 420–480 | 48–50 | 20 ms | 8–9 ms | 14–15 ms | 6 ms |
| Frames 540–720 | 41 | 23 ms | 19 ms | 23 ms | 4 ms |

Findings:

- The submitted background-off build was not a playable diagnostic view: it showed stale
  horizontal glitch lines, so this row is useful for cost attribution only.
- The diagnostic BG-disabled path now clears each drawn scanline instead of reusing stale
  pixels. Verify that on device; the normal diagnostic build can also reach a scene first
  and then disable `Draw BG` from the Playdate menu.
- With audio on and sprites still enabled, the steady `ppu_full - cpu_only` cost drops to
  about 4 ms. Compared with the earlier normal audio-on idle delta of about 8 ms, this
  points to background pixel rendering costing roughly 4 ms in that comparison.
- Background rendering is material, but this row still spends 22–23 ms per frame in the
  steady boot and level-entry windows. The remaining CPU-side and non-background work still
  need measurement and reduction for a stable 50 fps target.

### Sprite-off Mario 1-1 — 2026-05-22

The sprite-off build left the Mario 1-1 background visible and removed visible sprite
drawing as expected:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Level 1-1 standing still, frames 360–660 | 37 | 26 ms | 19–20 ms | 28 ms |
| Level 1-1 moving, frames 840–1140 | 31–33 | 30–32 ms | 22–24 ms | 32–34 ms |

Findings:

- The standing-still row is effectively unchanged from the normal audio-on idle baseline:
  both report 37 fps, 26 ms average frame time, and 28 ms full draw frames.
- At the current millisecond logging resolution, visible `ppu_renderoam` work is within
  noise in this scene. Do not prioritize sprite pixel drawing for the first 50 fps push.
- This variant removes visible sprite rendering. Sprite-related state work that still runs
  for skipped frames, such as sprite-0 handling, remains part of the CPU-side budget.
- The moving row still lands around 22–24 ms on skipped visual frames and 32–34 ms on full
  draw frames. That keeps the busy-scene CPU/cache path in front of sprite drawing.

### Blit-off Mario 1-1 — 2026-05-22

The blit-off build disables `ppu_scanline_blit()` after one-time framebuffer setup. It
keeps the NES PPU scanline work and CPU work intact, so the view is not useful for normal
play but the timing row isolates Playdate scanline conversion:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Level 1-1 standing still, frames 420–660 | 36–37 | 26–27 ms | 19–20 ms | 27–29 ms |
| Level 1-1 moving before first Goomba, frames 840–900 | 33–35 | 28–30 ms | 21–22 ms | 30–32 ms |

Findings:

- Standing still is effectively unchanged from both the normal baseline and the no-sprites
  row at the current log resolution.
- Moving still costs 21–22 ms on skipped visual frames before pixel output is considered.
- Do not spend the first optimization pass on the Playdate scanline conversion loop. The
  next speed work should either reduce the CPU/emulation side of busy frames or attack
  background rendering, which is the PPU component with a measured delta so far.

### Why YOFFSET = 0 matters

NES screen is 256×240. Playdate LCD is 400×240. `YOFFSET = (240-240)/2 = 0` — the NES
fills the full Playdate height. **There is no vertical overscan to skip.** All 240 scanlines
must be rendered. Scanline-count reduction is not an option.

---

## Next experiments and open work

### Reduce ppu_renderbg register pressure (estimated 0.5–1 ms savings)

`ppu_renderbg` has ~15 live local variables (bmp_ptr, data_ptr, tile_ptr, attrib_ptr, pal,
refresh_vaddr, bg_offset, attrib_base, tile_count, tile_index, x_tile, y_tile, col_high,
attrib, attrib_shift). ARM has 13 GPRs; ~5 must spill to stack. Stack reads/writes are
D-cache accesses at ~3 cycles each. Register spilling + attribute-tracking branches add
~20 extra cycles per tile amortised.

**Approach**: pre-compute a `uint8 col_high_arr[33]` for all 33 tiles before the render loop.
The inner loop then only needs: tile_ptr, bmp_ptr, bg_offset, col_high_arr pointer. Compiler
can keep all four in registers, eliminating most spills and the attrib tracking branches.

```c
// Compute palette index for all 33 tiles upfront (outside hot loop)
uint8 col_high_arr[33];
for (int i = 0; i < 33; i++) { ... col_high_arr[i] = ... }

// Tight inner loop — only 4 live variables, no branches
for (int i = 0; i < 33; i++) {
    tile_index = *tile_ptr++;
    data_ptr   = &PPU_MEM(bg_offset + (tile_index << 4));
    draw_bgtile(bmp_ptr, data_ptr[0], data_ptr[8], pal + col_high_arr[i]);
    bmp_ptr += 8;
}
```

### Reduce D-cache thrashing in enemy-heavy scenes (cpu_only enemy overhead)

NES PRG ROM is 32 KB; D-cache is 16 KB. Goombas and mushrooms access ROM regions not used
during idle gameplay — these evict cache lines and cause ~3–7 ms cpu_only increase. Possible
approaches (none tried yet):
- Align PRG ROM base address in SRAM to a 16 KB boundary to minimise 4-way set conflicts
- Batch `nes6502_execute` calls (fewer but larger calls → less per-call cache-warmup overhead)

### Direct PC byte operand fetch experiment — no clear win

`NES6502_PC_PTR` already uses a direct host pointer for opcode dispatch and saved about
2 ms on `cpu_only`. The first follow-up experiment routed `IMMEDIATE_BYTE` through a
direct `pc_ptr` byte fetch with a rare 4 KB bank-boundary rebase guard. That covered
immediates, branch offsets, and the byte operand consumed by zero-page addressing modes.

The device row from `nofrendo-pcbyte.pdx`:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Level 1-1 standing still, frames 360–600 | 37 | 26 ms | 19 ms | 28 ms |
| Level 1-1 moving before busy enemy windows, frames 1860–2040 | 33 | 29–30 ms | 21–22 ms | 32 ms |
| Goombas appearing, frames 2100–2220 | 28–29 | 33–35 ms | 24–26 ms | 36–38 ms |

Findings:

- The standing row matches the normal baseline.
- The moving row stays in the same busy-scene range as earlier traversal logs.
- The change kept `nes6502_execute` around 12.7 KB, but it did not produce a clear device
  win. It was reverted before trying wider PC operand fetch changes.

### Skip sprite render cache on skipped frames — regression

`ppu_build_sprite_cache()` precomputes scanline sprite lists and CHR pattern rows for
visible sprite rendering. `FRAME_SKIP=2` means every other frame intentionally skips that
rendering, but the cache was still rebuilt at scanline 0 on those skipped frames. The
skipped frame only uses `ppu_fakeoam()` for the sprite-0 path, which reads raw sprite data
directly and does not consume the render cache.

The experiment rebuilt the sprite render cache only when `draw_flag` was true. The device
row from `nofrendo-skipcache.pdx` was worse:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Level 1-1 idle, frames 1200–1380 | 34 | 28 ms | 21 ms | 30 ms |
| Moving with Goombas, frames 1800–1980 | 25–29 | 34–38 ms | 25–29 ms | 36–41 ms |

Findings:

- The idle row regressed from the normal 37 fps, 19 ms `cpu_only`, 28 ms `ppu_full`
  baseline.
- Removing the cache build may leave later draw work or adjacent data colder, or the small
  edit may perturb code/data layout. Either way it is not a speed win on device.
- The change was reverted. Do not revisit skipped-frame sprite cache work without finer
  attribution than the current frame-window profiler.

### Fixed NES memory-map fast path — no clear win

The CPU read/write helpers already fast-path PRG ROM and base RAM. Mirrored RAM and the
fixed PPU/APU/input register windows still scanned handler ranges. The experiment:

- Handles all `$0000-$1FFF` RAM mirrors with the same 2 KB RAM mask.
- Routes `$2000-$4017` through the default handler prefix installed by `nes.c` before
  falling back to mapper/protect range scans.
- Keeps handler calls indirect because the CPU helpers live beside the relocated interpreter
  hot section; direct far calls from copied code would be a separate relocation hazard.

The device row from `nofrendo-memmap.pdx` was flat to slightly worse:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Level 1-1 idle, frames 300-480 | 36 | 27 ms | 20 ms | 28 ms |
| Goombas visible, frames 1080-1200 | 29-30 | 32-33 ms | 24-25 ms | 34-36 ms |

Findings:

- The fixed windows did not lower `cpu_only` in the Mario row. The extra hot helper
  branches are not worth keeping before a more targeted access-frequency measurement.
- The change was reverted before the next CPU experiment.

Build status on 2026-05-22:

- `make` succeeds for device and simulator packages.
- The build was copied to the device as `nofrendo-memmap.pdx`.

### Batch scanline CPU execution - current best CPU win

The normal frame loop returns from `nes6502_execute()` after every scanline, interleaving
the interpreter with PPU scanline work. The first speed-first batch experiment runs CPU
cycles for eight scanlines at a time when the mapper has no hblank callback. It still
flushes at the vblank/NMI line and the end of frame, but it deliberately trades scanline
timing accuracy for fewer CPU loop entries and less CPU/PPU instruction-cache ping-pong.

The device row from `nofrendo-batchcpu.pdx` is the first clear follow-up win:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Level 1-1 idle, frames 360-600 | 43 | 22 ms | 15 ms | 23-24 ms |
| Light moving windows, frames 780-1200 | 42-49 | 20-23 ms | 7-17 ms | 17-24 ms |
| Busy moving windows, frames 1440-2280 | 31-41 | 24-32 ms | 18-24 ms | 25-33 ms |
| Mushroom sequence, frames 2640-3120 | 33-38 | 25-30 ms | 19-23 ms | 27-31 ms |

Findings:

- Batch width 8 drops idle skipped-frame `cpu_only` from the normal about 20 ms row to
  15 ms and drops the idle full frame from about 28 ms to 23-24 ms.
- Busy object windows still climb back to 20-24 ms before draw-frame cost is considered,
  but the user reports this as the best and smoothest build tried so far.
- Keep the batching path. The next measurement should compare a wider batch width before
  changing another subsystem; if that scales, CPU/PPU handoff and cache churn are a larger
  lever than individual memory-helper branches.

The full Mario 1-1 run from the `-O2` `nofrendo-batchcpu16.pdx` improved the feel again.
It became the batching baseline:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-300 | 46-50 | 20-21 ms | 6-14 ms | 17-22 ms |
| First slowdown ramp, frames 420-600 | 38-42 | 23-26 ms | 17-20 ms | 24-27 ms |
| Busy mid-level windows, frames 900-1920 | 32-41 | 24-30 ms | 18-24 ms | 25-31 ms |
| Quiet plateau, frames 2040-2280 | 42-43 | 22-23 ms | 14-16 ms | 23-24 ms |
| Level tail, frames 2400-3000 | 37-45 | 22-26 ms | 14-20 ms | 23-27 ms |

Batch 16 findings:

- The best windows now touch 50 fps with draw frames at 17-22 ms and skipped visual
  frames at 6-14 ms.
- The remaining slowdowns still line up with `cpu_only` rising to 20-24 ms; `ppu_full`
  remains about 7-8 ms above it. Wider CPU batching is worth one more measurement before
  shifting focus back to background draw cost.
- The user played the whole level and reported this as the best version so far before the
  CPU `-O3` row below.

The `cpu_batch=32` packages are **not valid speed rows for Mario**. They log fast windows
similar to batch 16, but Mario feels about half speed. The first package also flushed
deferred visible-line CPU cycles inside scanline 241 after `ppu_scanline()` had already
raised vblank but before `ppu_checknmi()` delivered NMI. Flushing before vblank fixed that
ordering bug, but the retest still plays in slow motion. The wider visible-frame batch is
itself too coarse for Mario: the PPU can render far ahead of the 6502 while this core
derives sprite-zero strike timing from the CPU cycle count at scanline render time and
serves it through `PPUSTATUS`. A pending-scanline strike timestamp bias retest was worse
below. Keep batch 16 as the current timing-safe width for Mario.

### Align iNES PRG ROM start - flat

The busy Mario slowdown remains CPU-side at batch 16. One D-cache hypothesis was PRG ROM
alignment: Mario's 32 KB PRG working set is larger than the Playdate D-cache, and a poor
SRAM base could add set conflicts while enemy code comes in. The experiment kept
`cpu_batch=16` and over-allocated the loaded ROM file buffer so the iNES PRG start after
the 16-byte header landed on a 16 KB boundary for the no-trainer test ROM.

The full Mario 1-1 row from `nofrendo-batchcpu16-alignrom.pdx` is effectively flat against
the non-aligned batch 16 row:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-360 | 45-50 | 20-21 ms | 6-13 ms | 17-23 ms |
| First slowdown ramp, frames 420-660 | 39-44 | 22-25 ms | 14-19 ms | 23-26 ms |
| Busy windows, frames 960-1920 | 33-43 | 22-29 ms | 11-23 ms | 21-31 ms |
| Quiet plateau, frames 1980-2340 | 43-44 | 22-23 ms | 14-15 ms | 23-24 ms |
| Level tail, frames 2400-3000 | 36-44 | 22-27 ms | 13-21 ms | 23-28 ms |

Findings:

- Aligned PRG placement does not move the slow floor: the busy row still reaches
  21-23 ms `cpu_only` and 28-31 ms draw frames.
- Keep the probe reproducible for now, but do not spend the next iteration on more PRG
  base-address variants.

### Batched sprite-zero strike cycle bias - regression

`cpu_batch=32` still reduced the measured host-side CPU cost after the vblank-boundary
ordering fix, but Mario ran in slow motion. This core records sprite-zero hit time while
rendering the scanline as `nes6502_getcycles(false) + x/3`. During batched scanlines the
PPU can render multiple lines before the CPU executes their accumulated cycles, so the
stamp was missing those pending line cycles.

The batch 32 retest passed the current pending scanline-cycle bias into `ppu_scanline()`
and added it only when sprite-zero strike time was stamped. The user reports that it feels
worse. The half-level row from `nofrendo-batchcpu32-strikebias.pdx` also loses the earlier
fast-looking batch 32 CPU row:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early fast windows, frames 60-300 | 42-47 | 20-23 ms | 7-15 ms | 21-24 ms |
| First level slowdown, frames 360-900 | 37-42 | 23-26 ms | 13-20 ms | 24-28 ms |
| Busy half-level windows, frames 960-1920 | 31-37 | 26-32 ms | 20-25 ms | 28-33 ms |

Findings:

- A pending-line offset on only the strike timestamp is not enough to make batch 32 valid.
  The visible PPU/status timing dependency is broader than that one timestamp, or the
  bias itself is too crude once CPU execution is deferred.
- The retest is not a speed gain either: its busy windows overlap or trail batch 16.
- The code change was reverted. Do not keep widening scanline CPU batches before a
  different timing model exists.

### 6502 optimization level at batch 16 - current best

The next CPU-side probe kept the known-good `cpu_batch=16` path and made the `nes6502.c`
optimization level selectable. The preceding diagnostic CPU core used `-O2`; the first
retest switched only that file to `-O3`. This compares faster generated code against the
extra hot code size and I-cache pressure on the Playdate. The diagnostic banner reports
`cpu_opt=<level>`. The linked `-O3` `nes6502_execute` is 0x332c bytes (about 13.1 KB);
the preceding `-O2` build row was 0x2d3c bytes (about 11.6 KB).

The full Mario 1-1 row from `nofrendo-batchcpu16-cpuO3.pdx` is the new current best:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-300 | 46-50 | 20-21 ms | 5-13 ms | 14-22 ms |
| First slowdown ramp, frames 360-960 | 39-45 | 22-25 ms | 13-18 ms | 22-26 ms |
| Busy mid-level windows, frames 1020-1800 | 33-43 | 23-30 ms | 16-23 ms | 24-31 ms |
| Quiet plateau, frames 1920-2280 | 44-45 | 22 ms | 13-15 ms | 22-23 ms |
| Level tail, frames 2340-2880 | 38-45 | 21-25 ms | 13-19 ms | 22-27 ms |

Findings:

- The user played the full level and reports that, aside from a few slowdowns, it feels
  very good.
- The best windows now hit 50 fps with the lowest draw-frame row yet: 14 ms at frame 240.
- The stubborn rows remain the same shape: slowdowns are still led by `cpu_only` climbing
  to 21-23 ms, with draw frames roughly 8 ms above that.
- Promote `-O3` as the next baseline for the 6502 CPU core on the measured Rev B device.

The restored main-package retest from 2026-05-24 confirms this remains the best baseline
after rejecting spinhack:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-240 | 47-50 | 20-21 ms | 6-12 ms | 14-19 ms |
| First slowdown/ramp, frames 300-900 | 41-46 | 21-24 ms | 12-17 ms | 21-25 ms |
| Busy mid-level windows, frames 960-1980 | 34-41 | 23-29 ms | 17-22 ms | 24-30 ms |
| Quiet plateau, frames 2040-2340 | 44 | 22 ms | 14 ms | 22-23 ms |
| Level tail, frames 2400-2940 | 37-45 | 21-26 ms | 13-20 ms | 22-28 ms |

Findings from the retest:

- The user reports it feels smoother, especially around the mushroom interaction.
- Remaining slowdowns correlate with Goombas/Koopas and moving items on screen.
- The enemy-heavy rows are still CPU-led, but `cpu_only` now peaks at about 22 ms instead
  of the 23 ms peaks seen in several earlier variants.

The post-regression restore row from `build=2026-05-24 02:25:27` confirms the known-good
shape is back after reverting the sprite-cache gate:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-300 | 47-50 | 20-21 ms | 6-13 ms | 14-22 ms |
| First slowdown/ramp, frames 360-960 | 41-45 | 22-24 ms | 13-17 ms | 22-25 ms |
| Busy mid-level windows, frames 1020-1920 | 33-43 | 23-30 ms | 16-23 ms | 24-31 ms |
| Quiet plateau, frames 1980-2220 | 44 | 22-24 ms | 14-17 ms | 22-25 ms |
| Level tail, frames 2280-2760 | 37-45 | 21-26 ms | 13-20 ms | 22-27 ms |

The worst submitted window remains frame 1920 at `fps=33`, `avg=30 ms`,
`cpu_only=23 ms`, `ppu_full=31 ms`. That is the row the next attribution build should
explain.

### CPU execution split attribution - confirmed CPU bottleneck

The diagnostic build kept the same emulator behavior and added timing around each
`nes6502_execute()` chunk inside `nes_renderframe()`. The log line includes:

- `cpu_exec`: average pure CPU interpreter time on skipped visual frames.
- `cpu_misc`: `cpu_only - cpu_exec`, covering non-draw PPU state, blit no-op path,
  hblank/vblank callbacks, FIQ checks, and scanline loop overhead.
- `ppu_exec`: average pure CPU interpreter time on drawn frames.

This was instrumentation only. It adds a small timing-call cost, so the split proportions
matter more than the raw speed row.

Device row:

- Banner timestamp: `build=2026-05-24 02:33:09`.
- Settings: `cpu_batch=16`, `cpu_opt=O3`, `cpu_loop_align=off`,
  `cpu_spin=off`, `prg_align=off`.

| Segment in submitted log | fps | avg | cpu_only | cpu_exec | cpu_misc | ppu_full | ppu_exec |
|---|---:|---:|---:|---:|---:|---:|---:|
| Fast level windows, frames 60-240 | 46-49 | 20-21 ms | 6-12 ms | 4-11 ms | 1-2 ms | 16-19 ms | 5-11 ms |
| First slowdown/ramp, frames 300-840 | 38-45 | 21-26 ms | 11-19 ms | 10-17 ms | 1-2 ms | 22-27 ms | 8-16 ms |
| Busy mid-level windows, frames 900-1260 | 34-40 | 24-28 ms | 18-22 ms | 16-20 ms | 1-2 ms | 26-29 ms | 16-19 ms |
| Block/enemy-heavy windows, frames 1500-1800 | 31-40 | 24-31 ms | 17-24 ms | 15-22 ms | 1-2 ms | 26-32 ms | 15-21 ms |
| Quiet recovery, frames 1860-2160 | 43 | 22-23 ms | 14-15 ms | 12-13 ms | 1-2 ms | 24 ms | 12-13 ms |
| Tail windows, frames 2220-2640 | 37-44 | 22-26 ms | 14-19 ms | 12-17 ms | 1-2 ms | 23-28 ms | 12-17 ms |

Findings:

- The remaining Mario slowdowns are overwhelmingly pure `nes6502_execute()` time.
- `cpu_misc` is almost always only 1-2 ms, so hblank/vblank callbacks, skipped-frame PPU
  state, FIQ checks, and diagnostic frame wrapper overhead are not where the bad windows live.
- The worst submitted window is frame 1740: `fps=31`, `avg=31 ms`, `cpu_only=24 ms`,
  `cpu_exec=22 ms`, `cpu_misc=2 ms`, `ppu_full=32 ms`, `ppu_exec=21 ms`.
- Draw frames still carry about 8-11 ms of non-CPU render cost, so PPU optimization remains
  useful later. But the current slowdowns with enemies/items are CPU interpreter/opcode-mix
  problems first.

### 6502 opcode mix profile - pending device row

The next diagnostic build keeps the best behavior row (`cpu_batch=16`, `cpu_opt=O3`,
spin/align off) and adds a per-opcode counter inside the 6502 switch dispatch. Every 60-frame
window now emits a second diagnostic line:

```text
[diag] op_total=... top=AD:... D0:... ...
```

This build is expected to run slower than the plain baseline because it increments a counter
for every emulated instruction. Use it only to identify which opcodes dominate the same
slow Mario 1-1 windows. The next real optimization should specialize the hottest measured
opcodes or their addressing-mode helpers instead of guessing at broad PPU or scheduler work.

Device row:

- Banner timestamp: `build=2026-05-24 02:41:57`.
- Settings: `cpu_batch=16`, `cpu_opt=O3`, `cpu_spin=off`, `cpu_prof=opcode`.

| Segment in submitted log | fps | avg | cpu_only | cpu_exec | ppu_full | Hot opcode shape |
|---|---:|---:|---:|---:|---:|---|
| Fast/idle windows, frames 120-180 | 42-47 | 21-23 ms | 10-14 ms | 8-12 ms | 21-24 ms | `4C` dominates at 474k-544k hits |
| First slowdown, frames 300-540 | 30-34 | 29-32 ms | 22-25 ms | 21-24 ms | 31-34 ms | `4C` drops; `C8/D0/85/C9/99/4A` rise |
| Heavy enemy/block windows, frames 840-1200 | 26-28 | 35-38 ms | 27-30 ms | 25-28 ms | 36-39 ms | branches, stores, compares, zero-page loads dominate more |
| Worst profile windows, frames 1680-1740 | 24-25 | 39-40 ms | 31-32 ms | 30 ms | 41 ms | `4C` only ~177k-179k; `D0/85/C8/C9/99/A5/4A` all hot |

Findings:

- The profile build is intentionally much slower and should not be judged as a playable
  row. It adds a counter write to roughly 570k interpreted instructions per 60-frame window.
- Fast windows are mostly repeated `JMP absolute` (`4C`), likely idle/wait/control-loop code.
  The windows that feel bad have less `4C` and more real game logic: taken/not-taken branches,
  zero-page stores/loads, compares, absolute indexed stores, and shifts.
- The next measured candidate is `NES6502_FAST_PC_OPS`: fetch immediate/absolute operands
  through `pc_ptr`, avoid a full `PC_REBASE()` for same-4KB-bank taken relative branches,
  and special-case `JMP absolute` operand fetch. This directly targets the hot opcode list
  without enabling the expensive opcode counter.

### 6502 fast PC operand and branch path - mixed/flat

This diagnostic row keeps the known-good timing settings (`cpu_batch=16`, `cpu_opt=O3`,
spin/profile/align off) and enables `cpu_fastpc=on`.

Targeted costs:

- `D0/F0/10` relative branches no longer rebase the host PC pointer when the target stays in
  the same 4 KB emulated memory bank.
- `85/A5/C9` and other immediate/zero-page operands fetch operand bytes through `pc_ptr`
  instead of `bank_readbyte(PC++)`.
- `AD/BD/99/4C` and other absolute operands fetch operand words through `pc_ptr` when the
  two bytes are contiguous in the current 4 KB bank.

This is a speed row, not an attribution row. Compare it against the plain O3 baseline, not
against the opcode-profile build.

Device row:

- Banner timestamp: `build=2026-05-24 02:50:44`.
- Settings: `cpu_batch=16`, `cpu_opt=O3`, `cpu_spin=off`, `cpu_prof=off`,
  `cpu_fastpc=on`, `prg_align=off`.

| Segment in submitted log | fps | avg | cpu_only | cpu_exec | ppu_full | ppu_exec |
|---|---:|---:|---:|---:|---:|---:|
| Fast level windows, frames 60-240 | 46-50 | 20-21 ms | 6-13 ms | 4-11 ms | 15-20 ms | 4-11 ms |
| First slowdown/ramp, frames 300-840 | 39-46 | 21-25 ms | 11-19 ms | 9-17 ms | 19-26 ms | 9-16 ms |
| Busy mid-level windows, frames 900-1260 | 34-40 | 24-28 ms | 18-22 ms | 17-20 ms | 26-29 ms | 16-19 ms |
| Late busy windows, frames 1500-1740 | 31-37 | 26-31 ms | 20-24 ms | 18-23 ms | 28-33 ms | 17-22 ms |
| Recovery/tail, frames 1800-2700 | 36-44 | 22-27 ms | 13-20 ms | 12-19 ms | 23-28 ms | 12-17 ms |

Findings:

- Broad direct PC operand fetch helps some light windows, but it does not materially reduce
  the stubborn enemy/item slowdowns.
- The worst fastPC window still reaches `fps=31`, `avg=31 ms`, `cpu_only=24 ms`,
  `cpu_exec=23 ms`, `ppu_full=33 ms`, `ppu_exec=22 ms`.
- Do not promote the broad `NES6502_FAST_PC_OPS` switch yet. The likely issue is that the
  extra helper code and branch shape helps cheap/idle code but gives back the gain through
  code size and I-cache pressure during real game logic.
- The next candidate should keep the approach narrower: specialize only the opcodes that the
  profile proved are hot in Mario's slow windows.

### 6502 hot opcode specialization - mixed/inconclusive with CPU-split timing

This candidate keeps the known-good baseline (`cpu_batch=16`, `cpu_opt=O3`, spin/profile/
align/fastPC off) and enables `cpu_hotops=on`. It hand-specializes only the opcodes that
dominated the opcode profile:

- Branches: `10`/`D0`/`F0` avoid a `PC_REBASE()` when a taken branch stays in the same 4 KB
  emulated bank.
- Loads/stores: `85`/`8D`/`99`/`A5`/`AD`/`BD` fetch operands directly and use fast RAM/ROM
  accesses where the mapping is known.
- Compare/jump: `C9` and `4C` fetch operands directly.

This is a profile-guided speed row. It deliberately leaves the broad fastPC path off so the
test can answer whether targeted hot op specialization beats the larger all-opcode helper
surface.

Build/install status:

- Built and installed over the single `Games/nofrendo.pdx` on 2026-05-24.
- Local reinstall banner timestamp: `build=2026-05-24 12:41:50`; submitted log banner:
  `build=2026-05-24 12:41:37` with the same flags.
- Expected settings: `cpu_batch=16`, `cpu_opt=O3`, `cpu_spin=off`, `cpu_prof=off`,
  `cpu_fastpc=off`, `cpu_hotops=on`, `prg_align=off`.

Device row with CPU-exec split timing still enabled:

| Segment in submitted log | fps | avg | cpu_only | cpu_exec | ppu_full | ppu_exec |
|---|---:|---:|---:|---:|---:|---:|
| Fast level windows, frames 60-300 | 46-49 | 20-22 ms | 6-13 ms | 4-11 ms | 18-23 ms | 5-11 ms |
| First slowdown/ramp, frames 360-840 | 39-44 | 22-25 ms | 11-18 ms | 9-16 ms | 23-27 ms | 9-16 ms |
| Busy mid-level windows, frames 900-1440 | 35-40 | 24-28 ms | 16-21 ms | 14-19 ms | 26-29 ms | 14-18 ms |
| Worst hotops window, frames 1500-1620 | 31-34 | 28-31 ms | 21-23 ms | 20-22 ms | 30-33 ms | 18-21 ms |
| Recovery/tail, frames 1680-2520 | 35-43 | 23-27 ms | 13-20 ms | 11-18 ms | 24-29 ms | 11-17 ms |

Findings:

- This does not prove a hot-op win. The stubborn slowdown still reaches `fps=31`,
  `avg=31 ms`, `cpu_only=23 ms`, `cpu_exec=22 ms`, `ppu_full=33 ms`.
- The remaining slow frame is still pure CPU interpreter work, not draw/blit/audio.
- This row is contaminated by the attribution timer around every CPU batch. Make that split
  optional and retest the same hot-op build with `cpu_split=off` before deciding whether to
  keep or revert `NES6502_HOTOPS`.

Retest package installed:

- Built and installed over the single `Games/nofrendo.pdx` on 2026-05-24.
- Expected banner: `build=2026-05-24 18:26:36`, `cpu_hotops=on`, `cpu_split=off`.
- This row should be compared against the earlier best non-split O3 baseline and against
  the split-timed hot-op row above.

Clean device row:

- Submitted banner: `build=2026-05-24 18:26:23`, `cpu_hotops=on`, `cpu_split=off`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-300 | 44-49 | 20-22 ms | 6-13 ms | 16-22 ms |
| First slowdown/ramp, frames 360-780 | 40-45 | 22-24 ms | 12-18 ms | 22-25 ms |
| Busy mid-level windows, frames 840-1200 | 36-41 | 24-27 ms | 17-21 ms | 25-28 ms |
| Late busy windows, frames 1260-1680 | 34-42 | 23-28 ms | 15-22 ms | 24-30 ms |
| Recovery/tail, frames 1740-2640 | 36-44 | 22-27 ms | 13-20 ms | 23-28 ms |

Findings from the clean row:

- Removing CPU-split timing improves the numbers versus the split-timed hot-op row, but the
  hot-op specialization still does not beat the best plain O3 baseline.
- The worst submitted window is frame 1680: `fps=34`, `avg=28 ms`, `cpu_only=22 ms`,
  `ppu_full=30 ms`.
- Keep `NES6502_HOTOPS=OFF` for the main speed baseline. Leave the option available for
  future per-ROM experiments, but do not promote it.
- Next speed-first row: audio-off on the current baseline (`audio=off`, `cpu_batch=16`,
  `cpu_opt=O3`, `cpu_hotops=off`, `cpu_split=off`). Audio work is partly outside the render
  metric and partly in CPU memory handlers, so it is worth retesting on the current baseline.

### Audio-off current baseline - useful but not sufficient

This row disables APU/audio while keeping the current speed baseline otherwise unchanged:
`cpu_batch=16`, `cpu_opt=O3`, `cpu_loop_align=off`, `cpu_spin=off`, `cpu_hotops=off`,
`cpu_split=off`, `prg_align=off`.

Device row:

- Submitted banner: `build=2026-05-24 18:31:08`, `audio=off`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-300 | 47-49 | 20-21 ms | 5-14 ms | 14-22 ms |
| First slowdown/ramp, frames 360-840 | 42-49 | 20-23 ms | 11-19 ms | 20-26 ms |
| Busy mid-level windows, frames 900-1680 | 36-45 | 22-27 ms | 16-23 ms | 24-30 ms |
| Recovery/tail, frames 1740-2640 | 41-47 | 21-24 ms | 13-20 ms | 22-27 ms |

Findings:

- Audio removal helps the light and tail windows. Several sections sit at `46-49 fps`
  where the audio-on baseline was usually `43-45 fps`.
- It does not fix the release-blocking dips. The busy windows still hit `cpu_only=23 ms`
  and `ppu_full=30 ms`, so even a no-audio build cannot sustain a 20 ms NES frame budget
  during enemy/item-heavy sections.
- Audio is not the main blocker. The next useful test must reduce or avoid CPU interpreter
  work in those busy windows.

### Fast sprite-zero strike with wider CPU batches - regression

Batch 32 previously produced attractive host frame timings but made Mario feel like slow
motion. The most likely cause is that PPU scanline rendering was allowed to run far ahead
of CPU execution, while `PPUSTATUS` still served sprite-zero hit using the CPU cycle count
from the current batch. Games that poll sprite-zero can then wait too long even when the
host frame rate looks good.

New speed-first experiment:

- `PPU_FAST_STRIKE=ON` makes sprite-zero hit visible as soon as the renderer/fake-OAM path
  detects it, ignoring the remaining pixel-to-CPU-cycle delay.
- `diag-faststrike` combines that deliberate timing inaccuracy with `cpu_batch=32`.
- This is intentionally less accurate. It is only worth keeping if it removes the old
  batch-32 slow-motion feel while preserving or improving the high-fps windows.

Build/install status:

- Built with `make diag-faststrike` and installed over the single `Games/nofrendo.pdx` on
  2026-05-24.
- Expected device banner: `build=2026-05-24 18:37:50`, `audio=on`, `ppu_strike=early`,
  `cpu_batch=32`, `cpu_opt=O3`, `cpu_hotops=off`, `cpu_split=off`.

Result:

- The user reports that this build still runs in slow motion.
- It also causes serious graphical/gameplay corruption: Mario got stuck inside a pipe and
  softlocked.
- Do not promote `PPU_FAST_STRIKE`, and do not use wider CPU batches with this timing
  shortcut. The old batch-32 slow-motion problem is not fixed by simply returning
  sprite-zero hit earlier; the broader scanline/CPU timing model is too unstable.
- Restore the active device build to the batch-16/O3 baseline before continuing.
- Restored device build: `build=2026-05-24 18:42:51`, `audio=on`, `ppu_strike=cycle`,
  `cpu_batch=16`, `cpu_opt=O3`, `cpu_hotops=off`, `cpu_split=off`.

### 6502 RAM-mirror fast path - flat/slightly worse

This candidate keeps the stable timing row (`cpu_batch=16`, `cpu_opt=O3`, no fastPC,
no hotops) and enables `NES6502_FAST_MEMIO`.

What changes:

- `mem_readbyte()` and `mem_writebyte()` bypass the generic handler-table scan only for
  mirrored CPU RAM `$0800-$1FFF`.
- PPU registers `$2000-$3FFF`, APU/input/DMA `$4000-$4017`, mapper handlers, and expansion
  handlers still use the original handler table.

Why it is worth trying:

- The opcode profile showed slow windows heavy in absolute loads/stores and real game logic.
  Some of those touch mirrored RAM through `mem_readbyte()` / `mem_writebyte()`, where the
  old path scans the handler table before calling the RAM mirror handler.
- This should not affect CPU/PPU timing or scanline scheduling. It is a dispatch overhead
  reduction, not an emulation shortcut.
- Expected win is likely small, but it targets the CPU-only floor without reintroducing the
  batch-32 timing instability.
- A broader direct-I/O version was rejected before testing performance: it crashed the
  Playdate. Likely cause: these memory helpers live in the copied CPU execution block on
  device, and direct calls from copied code to out-of-section PPU/APU functions do not
  relocate like the simulator path. Handler-table function-pointer calls are safe, so this
  revised version keeps all I/O on the original handler path.

Build target:

- `make diag-fastmem`
- Expected settings: `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=ram`,
  `cpu_hotops=off`, `cpu_fastpc=off`, `cpu_split=off`.
- Rejected broader direct-I/O row: simulator banner `build=2026-05-24 18:48:56`,
  `ppu_strike=cycle`, `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`,
  `cpu_hotops=off`, `cpu_fastpc=off`, `cpu_split=off`, `prg_align=off`.
- Revised RAM-only row built locally on 2026-05-24. Simulator banner:
  `build=2026-05-24 19:06:03`, `ppu_strike=cycle`, `cpu_batch=16`,
  `cpu_opt=O3`, `cpu_memio=ram`, `cpu_hotops=off`, `cpu_fastpc=off`,
  `cpu_split=off`, `prg_align=off`.
- Installed over the single device `Games/nofrendo.pdx` on 2026-05-24 after replacing the
  crashing broader direct-I/O package.
- Submitted device row banner: `build=2026-05-24 19:05:51`, `cpu_memio=ram`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-240 | 46-50 | 20-21 ms | 7-13 ms | 17-19 ms |
| First slowdown/ramp, frames 300-780 | 38-45 | 22-26 ms | 12-19 ms | 22-27 ms |
| Busy mid-level windows, frames 840-1620 | 33-41 | 23-29 ms | 17-23 ms | 25-31 ms |
| Recovery/tail, frames 1680-2520 | 35-44 | 22-28 ms | 14-21 ms | 23-29 ms |

Findings:

- The RAM-only fast path is stable on device, unlike the broader direct-I/O version.
- It does not improve the release-blocking windows. Worst windows still reach
  `cpu_only=23 ms` and `ppu_full=31 ms`.
- Do not promote `NES6502_FAST_MEMIO`; leave it off for the speed baseline.

### 6502 self-JMP spin fast-forward - regression

This candidate keeps the stable row (`cpu_batch=16`, `cpu_opt=O3`, no fastPC, no hotops,
no fastmem) and enables `NES6502_JMP_SPIN`.

Why it is worth trying:

- The opcode profile showed `4C` (`JMP absolute`) dominating many windows, even in some
  busy scenes. In games like SMB, the main thread often waits for NMI in a self-`JMP`
  loop while the real frame/game work happens from NMI.
- When `JMP absolute` targets its own opcode address, the loop can only be escaped by an
  interrupt or reset. In this emulator, those events are injected between CPU slices, so
  it is safe enough for this speed-first mode to burn the rest of the current CPU slice
  instead of interpreting thousands of repeated `JMP` instructions.
- This targets idle-loop interpreter waste without widening CPU batches, so it should not
  repeat the `cpu_batch=32` slow-motion failure.

Build target:

- `make diag-jmpspin`
- Expected settings: `cpu_batch=16`, `cpu_opt=O3`, `cpu_jmpspin=on`,
  `cpu_memio=table`, `cpu_hotops=off`, `cpu_fastpc=off`, `cpu_split=off`.
- Built locally on 2026-05-24. Simulator banner: `build=2026-05-24 19:14:18`,
  `ppu_strike=cycle`, `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=table`,
  `cpu_jmpspin=on`, `cpu_hotops=off`, `cpu_fastpc=off`, `cpu_split=off`,
  `prg_align=off`.
- Installed over the single device `Games/nofrendo.pdx` on 2026-05-24.

Device result:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early light windows, frames 60-180 | 39-50 | 20-25 ms | 3-14 ms | 13-26 ms |
| Pathological spike, frame 240 | 22 | 44 ms | 55 ms | 19 ms |
| First playable stretch, frames 300-840 | 33-37 | 26-29 ms | 19-23 ms | 28-31 ms |

Findings:

- This is a real regression. The frame-240 `cpu_only=55 ms` spike means the fast-forwarded
  self-`JMP` loop can burn or misplace far more CPU slice budget than it saves.
- The later enemy/gameplay windows still sit around `cpu_only=21-23 ms` and
  `ppu_full=29-31 ms`, so it does not solve the release-blocking slowdown.
- Do not promote `NES6502_JMP_SPIN`; leave it off for the speed baseline.
- Restore the active device build to plain batch-16/O3/table-memory before continuing.

### 6502 linear PRG-ROM fast path - pending device results

This candidate keeps the stable timing row (`cpu_batch=16`, `cpu_opt=O3`, no fastPC,
no hotops, no fastmem, no spin hacks) and enables `NES6502_LINEAR_ROM`.

What changes:

- `nes6502_setcontext()` checks whether CPU pages `$8000-$FFFF` point to one contiguous
  32 KB host-memory block.
- If they do, ROM byte reads and ROM word reads use a single base pointer instead of
  indexing `cpu.mem_page[address >> 12]` and masking the address for every access.
- If a mapper has non-contiguous or bank-switched PRG pages, the fast path disables itself
  and falls back to the original page-table path.

Why it is worth trying:

- The remaining Mario slowdowns are CPU-exec heavy, and the opcode profile points at real
  game-code/data-table work rather than draw/blit/audio.
- SMB's mapper-0 PRG layout should be contiguous, so this targets both operand word fetches
  and ROM data-table reads without changing scanline timing.
- The expected win is modest but more relevant than the RAM-mirror fast path because the
  slow windows are dominated by ROM code and ROM table traffic.

Build target:

- `make diag-linearrom`
- Expected settings: `cpu_batch=16`, `cpu_opt=O3`, `cpu_rom=linear`,
  `cpu_memio=table`, `cpu_jmpspin=off`, `cpu_hotops=off`, `cpu_fastpc=off`,
  `cpu_split=off`.
- Built and installed over the single device `Games/nofrendo.pdx` on 2026-05-25.
- Expected device banner: `build=2026-05-25 01:22:16`, `cpu_rom=linear`.

### Rendered-sprite pattern cache gating - regression

This small PPU-side probe targeted the enemy-heavy dips without changing CPU timing.
`ppu_build_sprite_cache()` still scans all 64 OAM entries and still increments
`oam_sl_count` for every visible sprite on every covered scanline, so max-sprite overflow
state is preserved. The change only skips the expensive CHR bitplane predecode for sprites
that never enter one of the first eight render slots on any visible scanline.

Why it looked worth testing:

- The remaining slowdowns correlate with Goombas/Koopas/items, and this work runs at
  scanline 0 even on skipped visual frames, so it can affect the `cpu_only` number too.
- It should be low risk for drawn frames because `ppu_renderoam()` only reads cached pattern
  rows through `oam_sl_idx`, and this experiment decodes exactly those sprites.
- Skipped visual frames still use `ppu_fakeoam()` for sprite-zero handling, which reads raw
  CHR data and does not depend on the predecoded render cache.

Device row:

- Built with `make diag-cpuopt CPU_OPT=O3` and copied over the single
  `Games/nofrendo.pdx`.
- Banner timestamp: `build=2026-05-24 02:17:03`.
- Settings: `cpu_batch=16`, `cpu_opt=O3`, `cpu_loop_align=off`,
  `cpu_spin=off`, `prg_align=off`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-360 | 46-49 | 20-21 ms | 6-12 ms | 15-22 ms |
| First slowdown/ramp, frames 420-900 | 41-46 | 21-24 ms | 10-17 ms | 21-25 ms |
| Busy mid-level windows, frames 960-1860 | 33-41 | 23-29 ms | 16-23 ms | 25-31 ms |
| Quiet plateau, frames 1920-2220 | 43-44 | 22 ms | 13-14 ms | 23 ms |
| Level tail, frames 2280-2760 | 37-44 | 22-26 ms | 13-20 ms | 22-28 ms |

Findings:

- The user reports that it feels worse, especially when jumping on blocks to spawn
  mushrooms.
- The numbers are mixed rather than a clean win. Some early windows improve, but the
  block/enemy-heavy stretch still reaches `cpu_only=23 ms` and `ppu_full=31 ms`, worse
  than the restored O3 baseline in a comparable slow window.
- The branch/skip bookkeeping, changed cache locality, or scene timing side effects cost
  more than the skipped CHR predecode saves.
- The code change was reverted. Do not retry this exact gate; if sprites are attacked
  again, work inside `draw_oamtile()` or add a measured fast path instead.

### 6502 loop alignment at batch 16 - flat/slightly worse

This layout probe stayed on `cpu_batch=16`, kept the measured `-O3` CPU core, and added
`-falign-loops=32` only to `nes6502.c`. The diagnostic banner reports
`cpu_loop_align=32` for this package. The linked `nes6502_execute` grows from the O3
baseline 0x332c bytes to 0x34b4 bytes.

The full Mario 1-1 row from `nofrendo-batchcpu16-cpuO3-loopalign.pdx` does not beat the
plain `-O3` row:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-360 | 45-50 | 20-21 ms | 6-13 ms | 16-23 ms |
| First slowdown/ramp, frames 420-1020 | 41-44 | 22-24 ms | 12-17 ms | 22-26 ms |
| Busy mid-level windows, frames 1080-2040 | 34-39 | 25-28 ms | 18-22 ms | 26-30 ms |
| Quiet recovery/tail, frames 2100-2820 | 38-44 | 22-25 ms | 13-18 ms | 23-27 ms |

Findings:

- Loop alignment is flat in the fast windows and slightly worse in several busy windows.
- The larger function body does not buy back enough branch alignment to justify the extra
  I-cache pressure.
- Keep `NES6502_ALIGN_LOOPS=OFF`; the best current baseline is `cpu_batch=16`,
  `cpu_opt=O3`, `prg_align=off`.

### PPUSTATUS spin-loop fast-forward at batch 16 - miss

This CPU-side probe kept the current best baseline and added an intentionally narrow
speed hack for the classic vblank wait loop:

```asm
BIT $2002  ; or LDA $2002
BPL ...
```

When the branch is exactly the immediate backwards branch to the status read and the
vblank bit is still clear, the emulator now consumes the rest of that CPU timeslice in
one step instead of interpreting the same wait-loop body until `remaining_cycles` expires.
This should preserve the coarse timeslice accounting used by the batch-16 renderer, but it
can alter sub-scanline PC position at slice boundaries, so it is a diagnostic row first.

The diagnostic banner reports `cpu_spin=ppustatus`. The linked `nes6502_execute` grows to
0x34f8 bytes, so this row needed to visibly reduce `cpu_only` time to justify the extra
I-cache pressure.

The full Mario 1-1 row from the `cpu_spin=ppustatus` build is worse than the plain O3
baseline in several comparable windows:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Fast level windows, frames 60-240 | 44-49 | 20-22 ms | 6-14 ms | 17-23 ms |
| First slowdown/ramp, frames 300-900 | 38-43 | 22-25 ms | 13-18 ms | 23-27 ms |
| Busy mid-level windows, frames 960-1800 | 32-41 | 23-30 ms | 16-23 ms | 25-32 ms |
| Recovery/tail, frames 1860-2700 | 34-43 | 22-28 ms | 14-22 ms | 24-30 ms |

Findings:

- The user completed all of Mario 1-1. Slowdowns still appear around block/mushroom
  interactions and scenes with several Goombas/Koopas moving.
- The row does not reduce the stubborn `cpu_only` peaks; it still reaches 22-23 ms in the
  same busy areas.
- Several early and tail windows trail the plain O3 row, consistent with the added code
  size increasing I-cache pressure.
- Keep `NES6502_SPINHACK=OFF`; restore the main device build to plain `cpu_batch=16`,
  `cpu_opt=O3`, `cpu_loop_align=off`, `prg_align=off`.

Build status on 2026-05-22:

- `make diag-batchcpu` succeeds for device and simulator packages.
- The diagnostic banner reports `cpu_batch=8` for the experiment.
- The build was copied to the device as `nofrendo-batchcpu.pdx`.
- `make diag-batchcpu CPU_BATCH=16` also succeeds and was copied as
  `nofrendo-batchcpu16.pdx`.
- `make diag-batchcpu CPU_BATCH=32` also succeeds and was copied as
  `nofrendo-batchcpu32.pdx`; that first package exposed the pre-NMI flush-order bug above.
- The vblank-boundary flush fix was rebuilt at `CPU_BATCH=32` and copied as
  `nofrendo-batchcpu32-vblank.pdx`; the wider visible batch still gives Mario slow motion.
- `make diag-alignrom` succeeds for device and simulator packages and keeps
  `cpu_batch=16` while reporting `prg_align=16k`; it was copied to the device as
  `nofrendo-batchcpu16-alignrom.pdx` for the alignment row above.
- The strike-cycle bias retest was rebuilt with `make diag-batchcpu CPU_BATCH=32`
  and copied as `nofrendo-batchcpu32-strikebias.pdx`; the row above regressed and the
  strike-bias code path was removed.
- `make diag-cpuopt CPU_OPT=O3` succeeds for device and simulator packages and was copied
  as `nofrendo-batchcpu16-cpuO3.pdx`.
- `make diag-cpualign` succeeds for device and simulator packages and was copied as
  `nofrendo-batchcpu16-cpuO3-loopalign.pdx`.

Build status on 2026-05-23:

- `make diag-spinhack` succeeds for device and simulator packages. It keeps
  `cpu_batch=16`, `cpu_opt=O3`, `cpu_loop_align=off`, `prg_align=off`, and reports
  `cpu_spin=ppustatus`.

Build status on 2026-05-24:

- After the spinhack row missed, the connected Playdate was restored to the plain O3
  baseline as the single `Games/nofrendo.pdx`: `cpu_batch=16`, `cpu_opt=O3`,
  `cpu_loop_align=off`, `cpu_spin=off`, `prg_align=off`.
- The rendered-sprite pattern-cache gating probe was built and installed over the single
  `Games/nofrendo.pdx`; the user reports it feels worse and the busy rows do not improve.
  The code change was reverted.
- The plain O3 baseline was rebuilt after the revert and installed over the single
  `Games/nofrendo.pdx`. Expected banner timestamp: `build=2026-05-24 02:25:27`.

- The CPU execution split attribution build was built and installed over the single
  `Games/nofrendo.pdx`. Expected banner timestamp: `build=2026-05-24 02:33:09`.
  The submitted row confirms `cpu_exec` dominates the remaining slowdowns.
- The opcode mix profile build was installed and tested. It was intentionally much slower
  because it increments a counter for every interpreted instruction, but it identified the
  slow-window opcode mix: `D0`, `85`, `C8`, `C9`, `99`, `A5`, `4A`, plus less-dominant
  `4C` than in idle windows.
- The fast PC operand/branch candidate was installed and tested over the single
  `Games/nofrendo.pdx`. Banner timestamp: `build=2026-05-24 02:50:44`, with
  `cpu_fastpc=on` and `cpu_prof=off`. It was mixed/flat and is not promoted.
- The hot-opcode specialization candidate was built and installed over the single
  `Games/nofrendo.pdx`. Expected banner timestamp: `build=2026-05-24 12:41:50`, with
  `cpu_fastpc=off` and `cpu_hotops=on`.
- CPU-exec split timing is now optional (`DIAG_CPU_EXEC_TIMING`). Normal speed rows report
  `cpu_split=off` and omit `cpu_exec`; attribution rows can be rebuilt with
  `make diag-cpusplit`. A clean hot-op retest was installed as the single
  `Games/nofrendo.pdx` with expected banner `build=2026-05-24 18:26:36`.
- The clean hot-op retest was flat/slightly worse against the best O3 baseline. The next
  row is a current-baseline audio-off package: `cpu_batch=16`, `cpu_opt=O3`,
  `cpu_hotops=off`, `cpu_split=off`.
- The current-baseline audio-off package was built and installed over the single
  `Games/nofrendo.pdx`. Expected banner: `build=2026-05-24 18:31:20`, `audio=off`,
  `cpu_batch=16`, `cpu_hotops=off`, `cpu_split=off`.
- The submitted audio-off row improved easy windows but still reached `cpu_only=23 ms`
  and `ppu_full=30 ms` in busy windows. Audio is not the release-blocking bottleneck.
- Added `PPU_FAST_STRIKE` plus `make diag-faststrike`: a deliberately inaccurate batch-32
  retest that returns sprite-zero hit immediately after detection. This tests whether the
  old `cpu_batch=32` slow-motion problem was mostly sprite-zero `PPUSTATUS` timing.
- `PPU_FAST_STRIKE` was rejected: it still ran in slow motion and caused serious
  graphical/gameplay corruption. The active device build was restored to the batch-16/O3
  baseline.
- Added `NES6502_FAST_MEMIO` plus `make diag-fastmem`. The first broad direct-I/O version
  crashed on device because copied CPU code cannot safely direct-call out-of-section
  PPU/APU handlers. The revised version only bypasses the table for RAM mirrors.
- The revised RAM-mirror fast path is stable but flat/slightly worse. Added
  `NES6502_JMP_SPIN` plus `make diag-jmpspin`, a speed-first self-`JMP` idle-loop
  fast-forward.
- The self-`JMP` idle-loop fast-forward regressed badly, including a submitted
  `cpu_only=55 ms` spike. Keep `NES6502_JMP_SPIN=OFF`.

Build status on 2026-05-25:

- Added `NES6502_LINEAR_ROM` plus `make diag-linearrom`. This keeps the stable batch-16/O3
  row and only bypasses `cpu.mem_page[]` lookups when `$8000-$FFFF` is detected as one
  contiguous PRG block.
- `make diag-linearrom` succeeds for device and simulator packages.
- Installed the linear-ROM row over the single device `Games/nofrendo.pdx`. Expected
  banner: `build=2026-05-25 01:22:16`, `cpu_batch=16`, `cpu_opt=O3`,
  `cpu_memio=table`, `cpu_jmpspin=off`, `cpu_rom=linear`, `cpu_split=off`.

### Background tile CHR cache — only if DTCM becomes available

The 4 KB `bg_tile_cache[256][8]` approach was tried and **caused regression** (see failures
above). It would only work without D-cache pollution if placed in DTCM.

A smaller variant caching only **visible tiles** (~30–40 unique tile indices = ~640 bytes)
would reduce D-cache impact significantly. Not yet tried; expected savings small if CHR
data is already hot from per-frame use.

### FRAME_SKIP arithmetic (reference)

With cpu_only = 19 ms and ppu_full = 28 ms, average frame time by skip level:

| FRAME_SKIP | avg ms | NES fps | display fps |
|---:|---:|---:|---:|
| 2 | 23.5 | ~42 | ~21 |
| 3 | 22.0 | ~45 | ~15 |
| 4 | 21.25 | ~47 | ~12 |
| 6 | 20.5 | ~49 | ~8 |
| 8 | 20.1 | ~50 | ~6 |

**50 fps NES speed requires ≈ FRAME_SKIP=8, which gives ~6 fps visual refresh — unacceptable.**
The only viable path to 50 fps with good display quality is reducing ppu_full below 22 ms so
that FRAME_SKIP=2 gives avg ≤ 20 ms. Target: **cut 6 ms from the 8 ms PPU cost.**

---

## Build commands

```sh
make                 # production build (DIAG=ON by default)
make install         # build + push to connected device
make diag-nobg       # diagnostic build, bg rendering disabled
make diag-nosprites  # diagnostic build, sprite rendering disabled
make diag-noblit     # diagnostic build, Playdate scanline blit disabled
make diag-noaudio    # diagnostic build, audio disabled
make diag-opprofile  # diagnostic build, opcode counter enabled
make diag-fastpc     # diagnostic build, fast PC operand/branch path enabled
make diag-faststrike # diagnostic build, batch-32 with immediate sprite-zero hit
make diag-fastmem    # diagnostic build, RAM mirror fast path enabled
make diag-jmpspin    # diagnostic build, self-JMP idle-loop fast-forward enabled
make diag-linearrom  # diagnostic build, contiguous PRG-ROM fast path enabled
make install-diag-nobg       # build + push as nofrendo.pdx
make install-diag-nosprites  # build + push as nofrendo.pdx
make install-diag-noblit     # build + push as nofrendo.pdx
make install-diag-noaudio    # build + push as nofrendo.pdx
make install-diag-opprofile  # build + push as nofrendo.pdx
make install-diag-fastpc     # build + push as nofrendo.pdx
make install-diag-faststrike # build + push as nofrendo.pdx
make install-diag-fastmem    # build + push as nofrendo.pdx
make install-diag-jmpspin    # build + push as nofrendo.pdx
make install-diag-linearrom  # build + push as nofrendo.pdx
PORT=/dev/cu.XXX make install   # override auto-detected serial port
```
