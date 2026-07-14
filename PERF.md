# Performance Notes — FamiCrank Playdate Port

Running log of profiling data, findings, and optimization decisions.
Current package name: `FamiCrank.pdx`. Older rows may mention historical diagnostic package
names from before the app rename.
Target: **50 fps** (PAL NES speed). Current best full-level Mario row:
**~49-50 fps** in light windows, **~37-45 fps** in the remaining slow windows.
(All F746 figures below. See the Rev B / STM32H7 entry immediately after this line.)

---

## Deterministic benchmark baseline + dynarec feasibility gate — 2026-07-14

Current unmodified `make bench` baseline, measured on the developer's Rev B (STM32H7):

| metric | result |
|---|---:|
| `total_frames` / `measured_frames` | 2879 / 2879 |
| `elapsed_ms` | 45411.00 |
| `avg_frame_ms` | **15.77** |
| `best_frame_ms` | 2.00 |
| `worst_frame_ms` | 20.00 |
| `slow_frames` / `skipped_frames` | 0 / 0 |
| `estimated_fps` | 63.40 |

Correctness guard: the deterministic `nes_smb1_world_1_1` replay completed all 2879
frames with no skipped or slow frames.

**Dynarec feasibility gate: REJECTED — hard fault.** The opt-in probe wrote two Thumb
instructions into a 32-byte-aligned writable-SRAM buffer at `0x9001D9A0`, then attempted
the mandatory D-cache clean before calling the SDK `clearICache()` and jumping to it.
Device output stopped after:

```text
[dynarec-ram] begin addr=9001d9a0 bytes=32 region=writable-sram
[dynarec-ram] pass=1 code-written
```

The crash record identifies the cache-maintenance write exactly:

```text
pc=9000d5c8 r2=e000ef68 cfsr=00000400 hfsr=40000000
```

`0xE000EF68` is Cortex-M7 `DCCMVAC`; `CFSR=0x400` is an imprecise bus fault from that
write. The Playdate sandbox therefore blocks the required D-cache range clean. The probe
never reached the SDK I-cache call or generated-code jump, but cached writable SRAM cannot
be used safely as an updatable code cache without the rejected coherency operation. Under
the mandatory hard-fault gate, the dynarec is **not viable**: stop here and do not build a
translator. The failed probe implementation and build target were reverted; this result
remains logged so the privileged cache-maintenance experiment is not repeated.

---

## Two-tile background renderer probe — 2026-07-14

Hypothesis: for the common mapper path where `ppu.latchfunc == NULL`, background
attribute palettes can be advanced once per aligned pair of tiles. Rendering two tiles
per loop iteration removes 16 loop-control operations and all 33 per-tile latch checks
per drawn scanline. The exact existing renderer remains active for MMC2/MMC4 latch games.

Build flag: `PPU_BG_PAIR_FAST`, default **OFF**. Test command:
`make install-bench-bgpair`.

Keep gate against the 15.77 ms Rev B baseline: all 2879 deterministic SMB frames must
complete, `slow_frames=0`, `skipped_frames=0`, `worst_frame_ms <= 20`, and average frame
time must improve by at least approximately 0.2 ms.

First device run (`build=0.3-bench-bgpair`, before correcting the package version to 0.4):

| metric | baseline | pair probe | delta |
|---|---:|---:|---:|
| `elapsed_ms` | 45411.00 | 44257.00 | -1154.00 |
| `avg_frame_ms` | 15.77 | **15.37** | **-0.40 (-2.5%)** |
| `worst_frame_ms` | 20.00 | 21.00 | +1.00 |
| `slow_frames` | 0 | 2 | +2 |
| `skipped_frames` | 0 | 0 | 0 |
| `estimated_fps` | 63.40 | 65.05 | +1.65 |

All 2879 frames completed, so the correctness guard passed and the average-time win is
well above the keep threshold. This run is **provisional**, however: two 21 ms frames miss
the tail-latency gate. Repeat the identical probe as `build=0.4-bench-bgpair`; promote only
if the confirmation run clears the full gate.

Confirmation device run (`build=0.4-bench-bgpair`):

| metric | baseline | confirmation | delta |
|---|---:|---:|---:|
| `elapsed_ms` | 45411.00 | 44628.00 | -783.00 |
| `avg_frame_ms` | 15.77 | **15.50** | **-0.27 (-1.7%)** |
| `worst_frame_ms` | 20.00 | 20.00 | 0.00 |
| `slow_frames` | 0 | 0 | 0 |
| `skipped_frames` | 0 | 0 | 0 |
| `estimated_fps` | 63.40 | 64.51 | +1.11 |

**Decision: KEEP for broader game validation.** The confirmation passes every gate,
including all 2879 frames and the approximately 0.2 ms minimum-win threshold. The option
remains default OFF and is not added to the production `FAST_FLAGS` yet. Validate Kirby
with the normal ROM picker and diagnostic timing via `make install-diag-bgpair`; its banner
must report `bg_pair=on`.

### Kirby deterministic smoke test — 2026-07-15

Fresh-SRAM replay of Kirby's Adventure from title/file creation through the first 2,010
frames of 1-1. The original 4,782-frame capture was truncated immediately before a door
whose entry did not replay reliably; the retained prefix is deterministic and long enough
to cover the heavy MMC3 gameplay window. Battery SRAM loading and saving are suppressed
only in the dedicated Kirby record/replay builds, so benchmark runs start fresh without
touching the user's real save.

First device row, with `PPU_BG_PAIR_FAST=ON`:

| metric | Kirby bg-pair |
|---|---:|
| `total_frames` / `measured_frames` | 2010 / 2010 |
| `elapsed_ms` | 38638 |
| `avg_frame_ms` | 19.22 |
| `best_frame_ms` | 3.00 |
| `worst_frame_ms` | 26.00 |
| `slow_frames` | 605 |
| `skipped_frames` | 0 |
| `estimated_fps` | 52.02 |

Correctness result: **graphical glitches appear after a repeatable point in the frame.**
Do not begin further performance work or promote the pair renderer until isolated. First
A/B control: `make install-bench-kirby-base`, identical replay and FAST_FLAGS with only
`PPU_BG_PAIR_FAST=OFF`. If the glitch disappears, reject/fix the pair renderer. If it
remains, test the existing MMC3 IRQ batching next; its original validation explicitly left
the status-bar/split position as an open visual check.

Background-pair OFF control result:

| metric | bg-pair ON | bg-pair OFF | Pair delta |
|---|---:|---:|---:|
| `total_frames` / `measured_frames` | 2010 / 2010 | 2010 / 2010 | exact |
| `elapsed_ms` | 38638 | 40122 | -1484 ms |
| `avg_frame_ms` | 19.22 | 19.96 | **-0.74 ms** |
| `worst_frame_ms` | 26.00 | 27.00 | -1.00 ms |
| `slow_frames` | 605 | 828 | -223 |
| `skipped_frames` | 0 | 0 | 0 |
| `estimated_fps` | 52.02 | 50.10 | +1.92 |

Visual result: glitches are unchanged with the pair renderer OFF, especially in Kirby's
save-selection/menu window graphics. **The pair renderer is not the cause.** It produces a
clear performance improvement on this exact heavy MMC3 replay, but remains unpromoted while
the underlying correctness issue is isolated.

Next single-variable control: `make install-bench-kirby-noirqbatch`, pair renderer OFF and
`NES_IRQ_MAPPER_BATCH=OFF`. This restores the old per-scanline MMC3 execution path. A clean
window/menu would isolate the artifact to delayed IRQ-handler/PPU writes in the batcher;
unchanged artifacts would move investigation into baseline PPU register semantics.

---

## Rev B is a different CPU (STM32H7) — measured 2026-07-09

The whole log below is the **original Playdate, STM32F746** (16KB I-cache + 16KB D-cache).
A newer Playdate revision uses an **STM32H7** (boot log `target=h7d1`, `pcbver=0x13`),
higher clock + bigger caches. Same shipped 0.3 diag-fast build, Mario 1-1 idle:

| metric | F746 (original) | H7 (Rev B) |
|---|---:|---:|
| `cpu_only` | ~20 ms | ~4 ms (3–5) |
| `ppu_full` | ~28 ms | ~8 ms (6–9) |
| `fps` | ~37 | 50 (locked to PAL target) |
| `avg` | ~26 ms | ~20 ms |

Raw H7 sample: `fps=50 avg=19-20ms cpu_only=3-5ms ppu_full=6-9ms` steady across 2400+ frames.

**Interpretation:** `cpu_only` down ~5x confirms the bottleneck was D-cache misses on the
32KB PRG ROM against a 16KB D-cache — the H7's bigger cache erases it, and it hits the 50fps
PAL target with ~60% of each frame idle. **No Rev-B optimization is warranted** (nothing to
fix). Latent-only option: 60fps NTSC on H7, blocked by compile-time `NES_REFRESH_RATE`
(50 PAL) + one-binary-for-all-devices. Not pursued. Full write-up: FINDINGS.md.

---

## Current Promoted Runtime — 2026-06-06

The default `make`, `make device`, `make sim`, and `make install` path uses
`FAST_FLAGS` via `FLAGS ?= $(FAST_FLAGS)`. The promoted line enables audio, direct audio
ring fill, fast OAM DMA, batch-16 CPU slices, direct CPU memory I/O, fast absolute-JMP
fetch, lazy cycle accounting, the narrow BNE/BPL/BEQ fast paths, and `DIAG=OFF`.

Runtime menu:

- ROM picker menu: `Frameskip`, `Show FPS`
- In-game menu: `ROM Picker`, `Frameskip`, `Show FPS`

`Frameskip` is now user-facing and means "how many frames to skip between draws":

| Menu value | Meaning |
|---:|---|
| 0 | draw every frame |
| 1 | skip one frame between draws; default |
| 2 | skip two frames between draws |

Historical notes below often use the old compile-time `FRAME_SKIP=N` terminology, where
`FRAME_SKIP=2` meant a draw interval of two frames. That old `FRAME_SKIP=2` behavior maps
to the current menu value `Frameskip=1`.

`Show FPS` defaults on and uses Playdate's native `drawFPS(0, 0)`. For clean performance
measurements, turn it off unless the native HUD is the thing being checked. When FPS is
off, skipped visual frames return `0` from the update callback and avoid a full LCD update.
When FPS is on, only the top rows needed by the native counter are marked dirty on skipped
visual frames.

Start and Select are crank controls now, not menu items: crank undocked below 60 degrees
holds Select; crank undocked above 180 degrees holds Start.

---

## TCM Relocation Probe — 2026-06-06

The Vecx Playdate TCM guide suggests that small hot-code blocks can be copied into the
unused DTCM stack pool and executed from there, provided the block stays tiny and its input
section is collected inside the normal `.text` output section so Playdate loader
relocations land in `.rel.text`.

FamiCrank now has a diagnostic-only proof target:

```sh
make diag-tcmhot
make install-diag-tcmhot
```

The proof is gated behind `NES6502_TCMHOT_PROBE=ON`, which is off in the default
performance build. It copies a tiny `.tcmhot` probe block into DTCM with a manual
word-copy loop, clears the I-cache, preserves the Thumb bit on the relocated function
pointer, and checks three capabilities separately: entry execution, global data access,
and an in-block function call.

Build-time ELF checks on the device binary:

- `__tcmhot_start=0x2970`, `__tcmhot_end=0x29c0`: 80-byte proof block.
- `readelf -S` shows `.text` and `.rel.text`, with no separate `.tcmhot` or
  `.rel.tcmhot` output section.
- `NES6502_TCMHOT_MAX_BYTES` is capped at 1328 bytes to match the conservative ceiling
  from the Vecx guide.

Device validation passed on PDU1-Y024621, SDK 3.0.6:

```text
[tcmhot] status=0 ready=1 size=80 max=1328 src=60002970 dest=20007970 frame=20009b48
[tcmhot] entry got=6502c001 expected=6502c001 ok=1
[tcmhot] global got=13579bdf expected=13579bdf ok=1
[tcmhot] call got=6502c0de expected=6502c0de ok=1
```

The mechanism is viable for a future compact 6502 hot core. It is not a speed win by
itself and should not be promoted until a real hot-path row beats the current default.

The next diagnostic target is:

```sh
make diag-tcmcore
make install-diag-tcmcore
```

`diag-tcmcore` keeps the same promoted interpreter as its fallback, but first tries a
relocated DTCM mini-core for only the safest high-frequency opcodes seen in profiling:
`INY`, `LSR A`, `CMP #`, `LDA #`, `LDA zp`, `STA zp`, `BNE`, `BEQ`, and `BPL`. The
diagnostic banner reports `cpu_tcmcore=on`, and startup logs one `[tcmcore]` status line
with the copied byte count and DTCM destination. The first device ELF check puts the copied
core at `__tcmhot_start=0x2970`, `__tcmhot_end=0x2bd0`, or 608 bytes, with no calls inside
the relocated block. This should stay opt-in until a clean Mario 1-1 run shows a real
improvement over the current frameskip-1 baseline.

Device result from Mario 1-1: `diag-tcmcore` is valid but **not a speed win**. The startup
status was `status=0 ready=1 size=608 max=1328 src=60002970 dest=20007770`, but the busy
windows still dipped to `43 fps` at frames 900 and 1560, `44 fps` at frame 1440, and a new
bad `39 fps` / `avg=25ms` / `cpu_only=22ms` window at frame 1620. Keep it unpromoted.

The next attribution target is:

```sh
make diag-tcmstats
make install-diag-tcmstats
```

`diag-tcmstats` logs the DTCM-core call count, hit/miss count, handled cycles, fallback
cycles, handled-cycle percentage, and maximum DTCM run for each 60-frame diagnostic window.
Use it to decide whether the mini-core is rarely reached, exits too quickly, or simply costs
more than it saves.

Device result from Mario 1-1 with the usual settings answered that question: after the
first window, every 60-frame window reported `calls=1200 hit=0 miss=1200 core=0 pct=0`.
Only startup/level-entry had `13` hits for `38` total core cycles. This means the current
wrapper shape is effectively pure overhead: the normal interpreter handles all real work
because the relocated mini-core is only tried at CPU-slice entry.

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

Diagnostic targets force `DIAG=ON`. In those builds the update callback measures each frame
and logs via `pd->system->logToConsole` every 60 frames. Logs are visible in Playdate Mirror
or `/Data/System/eventlog.txt` on the device.

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

## Historical baseline — 2026-05-22

Scene: Mario 1-1, standing still, no enemies visible. Old hardcoded `FRAME_SKIP=2`
(current menu `Frameskip=1` equivalent), `AUDIO=ON`, `DIAG=ON`.

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

### 4. Old hardcoded `FRAME_SKIP=2` (`src/osd.c`)
Alternates render_true / render_false frames. avg = (30+22)/2 = 26 ms → ~38 fps vs 29 fps
with old `FRAME_SKIP=1`. Game logic runs at full NES speed; only pixel output is halved.
This is equivalent to the current runtime menu value `Frameskip=1`.

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
Diagnostic targets use `DIAG=ON`; the default build now keeps diagnostics off for the
clean performance package.

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
  on-device `FamiCrank.pdx`. Do not create separately named PDX copies for new tests.
- The normal diagnostic build no longer exposes runtime BG/sprite render checkmarks. The
  old `Draw BG` and `Draw Sprites` toggles were removed on 2026-05-25 because they made
  gameplay tests too easy to skew. Use `diag-nobg` / `diag-nosprites` only for explicit
  compile-time measurement rows.
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
  pixels. Verify BG-off rows with the compile-time `diag-nobg` build; the runtime `Draw BG`
  menu toggle was later removed because it polluted real gameplay tests.
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
visible sprite rendering. In the old compile-time terminology, `FRAME_SKIP=2` meant every
other frame intentionally skips that rendering; today that maps to runtime `Frameskip=1`.
The skipped frame only uses `ppu_fakeoam()` for the sprite-0 path, which reads raw sprite
data directly and does not consume the render cache.

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

### 6502 opcode mix profile - measured diagnostic row

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

### 6502 linear PRG-ROM fast path - flat/slightly worse

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
- Device banner: `build=2026-05-25 01:22:16`, `cpu_rom=linear`.

Device result:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-900 | 40-50 | 19-24 ms | 6-18 ms | 16-26 ms |
| Busy mid-level windows, frames 1020-1980 | 32-39 | 25-30 ms | 18-24 ms | 27-31 ms |
| Recovery/tail, frames 2040-2880 | 35-44 | 22-27 ms | 13-21 ms | 23-29 ms |

Findings:

- The contiguous-PRG fast path did not improve the release-blocking windows.
- Worst submitted windows still reach `cpu_only=24 ms` and `ppu_full=31 ms`, which matches
  the ordinary page-table path closely.
- Do not promote `NES6502_LINEAR_ROM`; leave it off for the speed baseline.

### Playdate display update skip - promoted current baseline

This candidate keeps the stable CPU/PPU timing row and removes a platform-layer leak in the
frame-skip path.

What changes:

- Diagnostic builds no longer draw Playdate's on-screen FPS HUD by default. Console diag
  logging remains enabled.
- On skipped visual frames (old `FRAME_SKIP=2`, current runtime `Frameskip=1`,
  `draw=false`), `playdate_update()` no longer marks the whole LCD dirty and returns `0`
  so Playdate can skip the display update.
- On rendered frames, it still marks the full LCD dirty and returns `1`.

Why it is worth trying:

- The emulator intentionally renders only every other NES frame, but the old platform loop
  still requested a full Playdate display update every frame.
- This should reduce time outside `cpu_only`/`ppu_full`; those split numbers may stay similar,
  but the logged `avg`/`fps` and the felt smoothness should improve.
- It does not change CPU timing, PPU timing, mapper behavior, or memory mapping, so it should
  avoid the slow-motion and softlock failures from previous timing shortcuts.

Build target:

- `make diag-cpuopt CPU_OPT=O3`
- Expected settings: `hudfps=off`, `lcd_dirty=draw`, `cpu_batch=16`, `cpu_opt=O3`,
  `cpu_rom=page`, `cpu_memio=table`, `cpu_jmpspin=off`, `cpu_split=off`.

Device result:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-840 | 47-50 | 19-20 ms | 5-18 ms | 10-19 ms |
| Busy mid-level windows, frames 900-1740 | 37-49 | 20-26 ms | 16-23 ms | 18-25 ms |
| Recovery/tail, frames 1800-2640 | 43-50 | 19-23 ms | 13-20 ms | 16-21 ms |

Findings:

- This is the largest single win since the CPU core shrink. Most of 1-1 now sits at
  `49-50 fps` with `avg=19-20 ms`.
- The old diagnostic HUD and full-LCD dirty marking were forcing display work even on
  skipped visual frames. Removing that platform tax also reduced measured draw-frame time,
  likely by keeping the framebuffer/display path out of the emulator's hot-cache window.
- Remaining slowdowns are now narrow and CPU-side: worst submitted window is
  `fps=37`, `avg=26 ms`, `cpu_only=23 ms`, `ppu_full=25 ms`.
- Promote this as the new baseline: `hudfps=off`, `lcd_dirty=draw`, `cpu_batch=16`,
  `cpu_opt=O3`, all CPU timing hacks off.

### Direct CPU memory-I/O fast path - stable, subjective win

This candidate keeps the new display-update baseline and enables `NES6502_DIRECT_MEMIO`.

What changes:

- `nes6502_setcontext()` caches the function pointers for the fixed handler ranges:
  mirrored PPU registers (`$2000-$3FFF`), APU/null reads (`$4000-$4015`), controller reads
  (`$4016-$4017`), APU writes (`$4000-$4013`, `$4015`), and OAM/input writes
  (`$4014-$4017`).
- `mem_readbyte()` / `mem_writebyte()` bypass the generic handler-table scan for those
  ranges and for NES RAM mirrors (`$0800-$1FFF`).
- It still calls handlers through function pointers, so copied CPU code does not direct-`BL`
  out to PPU/APU code. Unusual mapper/protected ranges still fall back to the original scan.

Why it is worth trying:

- The remaining slow windows peak at `cpu_only=22-23 ms`, so small CPU-memory wins matter now.
- This attacks a known interpreter overhead without widening CPU batches or changing
  PPUSTATUS timing, which should avoid previous slow-motion failures.

Build target:

- `make diag-directmem`
- Expected settings: `hudfps=off`, `lcd_dirty=draw`, `cpu_batch=16`, `cpu_opt=O3`,
  `cpu_memio=direct`, `cpu_rom=page`, `cpu_jmpspin=off`, `cpu_split=off`.

Device result:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-780 | 46-50 | 19-21 ms | 6-19 ms | 11-19 ms |
| Busy mid-level windows, frames 840-1680 | 37-48 | 20-26 ms | 17-24 ms | 18-25 ms |
| Recovery/tail, frames 1740-2640 | 42-50 | 19-23 ms | 14-20 ms | 16-21 ms |

Findings:

- Stable on hardware; the user reports this plays exceptionally well and close to native NES.
- Numerically it is not a clean CPU win over the display-update baseline: the worst submitted
  window is still `fps=37`, `avg=26 ms`, `cpu_only=24 ms`, `ppu_full=25 ms`.
- Keep it active for the next test because subjective smoothness is best so far, but treat the
  remaining dips as CPU-interpreter-bound, not display-bound.

### Audio direct ring fill - promoted current baseline

This candidate keeps the directmem/display-update baseline and removes one main-thread audio
copy in `sound_fill_buffer()`.

What changes:

- The old path asked the APU to fill a stack `tmp[MAX_FILL]` buffer, then copied each sample
  into the Playdate audio ring one by one.
- The new path calls the APU directly on the ring's writable contiguous span, with a second
  APU call only if the write wraps around the end of the ring.
- `ring_write` is advanced only after each span is filled, so the audio callback never sees
  partially generated samples.

Why it is worth trying:

- This should reduce non-emulation main-thread work without changing CPU timing, PPU timing,
  memory mapping, display skipping, or audio wall-clock pacing.
- It probably will not lower `cpu_only`; if it helps, expect smaller `avg`/felt-smoothness
  gains rather than a dramatic CPU-side drop.

Build target:

- `make diag-directmem`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_rom=page`.

Device result:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-840 | 47-50 | 19-21 ms | 6-18 ms | 10-19 ms |
| Busy mid-level windows, frames 900-1860 | 37-49 | 20-26 ms | 12-24 ms | 15-25 ms |
| Recovery/tail, frames 1920-2760 | 43-50 | 19-22 ms | 14-20 ms | 16-20 ms |

Findings:

- Stable on hardware and preserves the current best-feeling directmem/display baseline.
- The light and recovery windows are extremely close to native-speed budget, with many
  `49-50 fps` windows at `avg=19-20 ms`.
- The worst submitted window remains CPU-bound: `frame=1860`, `fps=37`, `avg=26 ms`,
  `cpu_only=24 ms`, `ppu_full=25 ms`.
- Promote direct ring fill as the current baseline because it removes unnecessary work and
  does not change emulator timing. The next useful probe should stay inside `nes6502_execute`.

### Fast absolute JMP fetch - safe, very small / mixed win

This candidate keeps the directmem/display/audio baseline and enables
`NES6502_FAST_JMP_ABS`.

What changes:

- Only opcode `4C` (`JMP $nnnn`) changes.
- Instead of calling `bank_readword(PC)` for the jump target, it reads the target word from
  the already-rebased `pc_ptr` when the two operand bytes are contiguous in the current CPU
  memory bank.
- If the operand would cross the current 4 KB bank boundary, it falls back to `bank_readword`.
- It does not fast-forward self-jump loops, widen CPU batches, or change cycle accounting.

Why it is worth trying:

- Mario's opcode profile showed `4C` dominating many diagnostic windows by a very large
  margin, including the busy windows.
- The earlier broad hot-op build grew too much code and did not beat the baseline. This keeps
  the specialization to one tiny path with the best profile signal.

Build target:

- `make diag-fastjmp`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_rom=page`.

Device row:

- Build: `2026-05-25 01:57:26`
- Settings: `audio_fill=direct`, `lcd_dirty=draw`, `cpu_memio=direct`,
  `cpu_fastjmp=on`, `cpu_rom=page`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-840 | 48-50 | 19-20 ms | 6-18 ms | 10-19 ms |
| Busy mid-level windows, frames 900-1800 | 36-48 | 20-27 ms | 12-24 ms | 14-25 ms |
| Recovery/tail, frames 1860-2700 | 42-50 | 19-23 ms | 14-21 ms | 15-21 ms |

Finding:

- No obvious correctness regression in the submitted log.
- Performance is close to the directmem/audio-direct baseline. Some windows move a little
  better, but the worst busy windows still hit `cpu_only=21-24 ms`.
- Keep the option available, but do not treat this alone as the breakthrough.

### Fast taken relative branches - mixed / not promoted

This candidate keeps the directmem/display/audio baseline, keeps the safe `JMP $nnnn`
operand fast path enabled, and adds `NES6502_FAST_BRANCHES`.

What changes:

- All relative branches (`BNE`, `BEQ`, `BPL`, etc.) use `pc_ptr` for the offset byte.
- Taken branches avoid `PC_REBASE()` when the target stays inside the same 4 KB CPU
  memory bank; they just adjust `pc_ptr` by the signed branch offset.
- Cycle accounting stays unchanged, including the existing 6502 page-cross penalty.

Why it is worth trying:

- The opcode profile showed `D0` (`BNE`) and `F0` (`BEQ`) among the dominant non-`JMP`
  opcodes in busy Mario windows.
- This is narrower than `NES6502_FAST_PC_OPS` and much narrower than `NES6502_HOTOPS`,
  so it should avoid most of the code-size/cache regression risk from the earlier broad
  opcode-specialization build.

Build target:

- `make diag-fastbranch`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_fastbranch=on`, `cpu_rom=page`.

Device row:

- Build: `2026-05-25 12:38:56`
- Settings: `audio_fill=direct`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_fastbranch=on`, `cpu_rom=page`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-840 | 47-50 | 19-21 ms | 6-19 ms | 11-19 ms |
| Busy mid-level windows, frames 900-1740 | 38-48 | 20-26 ms | 17-24 ms | 18-24 ms |
| Recovery/tail, frames 1800-2640 | 42-50 | 19-23 ms | 15-21 ms | 16-21 ms |

Finding:

- Safe in the submitted log, but not a clear win over the directmem/audio-direct baseline.
- The remaining bad windows are still CPU-bound, with `cpu_only=21-24 ms`.
- Do not promote `NES6502_FAST_BRANCHES` yet; the next probe keeps it off and targets only
  one-byte operand fetches.

### Fast one-byte operand fetch - mixed / not promoted

This candidate keeps the directmem/display/audio baseline, keeps the safe absolute-JMP
operand fast path enabled, removes the runtime BG menu toggle, and adds
`NES6502_FAST_OPERAND_BYTES`.

What changes:

- Immediate and one-byte addressing operands use `pc_ptr` via `FETCH_PC_BYTE_DIRECT`.
- This covers immediate ops and zero-page addressing modes, so it targets hot Mario opcodes
  such as `C9` (`CMP #nn`), `A5` (`LDA $nn`), `85` (`STA $nn`), plus relative branch
  offset fetches.
- Unlike `NES6502_FAST_BRANCHES`, taken branches still use the normal target-rebase path.
- Cycle accounting stays unchanged.

Why it is worth trying:

- The opcode profile showed many busy-window cycles in short-operand instructions after
  `4C`.
- This tests one narrow piece of the broader `NES6502_FAST_PC_OPS` idea without also
  changing absolute-word operands or same-bank branch behavior.

Build target:

- `make diag-fastopbyte`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_fastbranch=off`, `cpu_fastopbyte=on`, `cpu_rom=page`.

Device row:

- Build: `2026-05-25 12:55:54`
- Settings: `audio_fill=direct`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_fastbranch=off`, `cpu_fastopbyte=on`, `cpu_rom=page`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-780 | 47-50 | 19-20 ms | 6-18 ms | 10-19 ms |
| Busy mid-level windows, frames 840-1680 | 36-49 | 20-27 ms | 16-24 ms | 17-25 ms |
| Recovery/tail, frames 1740-2460 | 43-50 | 19-22 ms | 13-20 ms | 16-21 ms |

Finding:

- Safe in the submitted log, but it does not clearly beat the directmem/audio-direct
  baseline. Several mid-level windows remain in the `cpu_only=21-24 ms` range.
- Do not promote `NES6502_FAST_OPERAND_BYTES`.
- The next clean baseline should disable `cpu_fastbranch` and `cpu_fastopbyte`, keep
  direct memory I/O and the safe fast absolute-JMP path, and remove runtime render toggles.

### Clean directmem/fastjmp baseline, render toggles removed - current promoted line

This is the clean row after removing the runtime `Draw BG` and `Draw Sprites` menu
checkmarks. It keeps direct audio fill, display-update skip, direct CPU memory I/O, and
the safe absolute-JMP operand fetch.

Device row:

- Build: `2026-05-25 13:02:33`
- Settings: `audio_fill=direct`, `lcd_dirty=draw`, `cpu_memio=direct`,
  `cpu_fastjmp=on`, `cpu_fastbranch=off`, `cpu_fastopbyte=off`, `cpu_rom=page`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-840 | 47-50 | 19-21 ms | 6-19 ms | 10-18 ms |
| Busy mid-level windows, frames 900-1740 | 35-49 | 20-27 ms | 17-25 ms | 17-25 ms |
| Recovery/tail, frames 1800-2580 | 41-50 | 20-23 ms | 14-22 ms | 15-21 ms |

Finding:

- This remains the best-behaved line: most Mario 1-1 windows now sit at `47-50 fps`.
- Removing the render toggles did not change the bottleneck shape; the worst sections are
  still CPU-bound, with a peak `cpu_only=25 ms` around frame 1680.
- Next probe: keep branch behavior and broad operand fetching unchanged, and only inline
  the measured hot memory load/store opcodes on top of this line.

### Fast hot memory opcodes - mixed / not promoted

This candidate adds `NES6502_FAST_MEMOPS` on top of the clean directmem/fastjmp baseline.
It is narrower than the earlier `NES6502_HOTOPS` row.

What changes:

- Inline `STA $nn`, `STA $nnnn`, `STA $nnnn,Y`, `LDA $nn`, `LDA $nnnn`, and
  `LDA $nnnn,X`.
- These opcodes use direct `pc_ptr` operand fetches and inline RAM/ROM fast paths when the
  address class is known.
- It deliberately leaves `cpu_fastbranch=off` and `cpu_fastopbyte=off`, because both were
  safe but mixed.

Build target:

- `make diag-fastmemops`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_fastbranch=off`, `cpu_fastopbyte=off`, `cpu_fastmemops=on`, `cpu_rom=page`.

Device row:

- Build: `2026-05-25 13:22:17`
- Settings: `audio_fill=direct`, `lcd_dirty=draw`, `cpu_memio=direct`,
  `cpu_fastjmp=on`, `cpu_fastmemops=on`, `cpu_rom=page`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-840 | 45-50 | 19-21 ms | 6-19 ms | 10-20 ms |
| Busy mid-level windows, frames 900-1620 | 35-49 | 20-28 ms | 16-25 ms | 18-26 ms |
| Recovery/tail, frames 1680-2520 | 41-50 | 19-24 ms | 14-22 ms | 16-22 ms |

Finding:

- Safe in the submitted Mario row, but not a net win over the clean directmem/fastjmp
  baseline.
- The stubborn busy stretch still reaches `cpu_only=25 ms`, and several comparable
  windows are worse than the `cpu_fastmemops=off` row.
- Do not promote `NES6502_FAST_MEMOPS`; keep it as an available probe only.

### Batch 24 on directmem/fastjmp - mixed / not promoted

This timing probe keeps the current promoted CPU/data-path line and raises the no-hblank
CPU batch width from 16 to 24 scanlines.

Why it is worth one test:

- The remaining bad windows are CPU-bound, and opcode-level specialization is now mostly
  trading I-cache/code-shape costs against small local wins.
- `cpu_batch=32` was rejected because Mario played in slow motion, but `cpu_batch=16`
  is very close to the 20 ms budget. `24` is a middle value that may reduce interpreter
  entry overhead without pushing PPU/CPU timing as far apart as 32.

Risk:

- Treat this as speed-first and provisional. If Mario feels slow-motion again, reject it
  even if the numeric fps row looks better.

Build target:

- `make diag-fastbatch`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `cpu_batch=24`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_fastmemops=off`, `cpu_rom=page`.
- Built and installed over the single device `Games/nofrendo.pdx` on 2026-05-25.
  Expected device banner timestamp: `build=2026-05-25 13:29:41`.

Device row:

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-840 | 44-50 | 19-22 ms | 6-20 ms | 10-19 ms |
| Busy mid-level windows, frames 900-1620 | 36-48 | 20-27 ms | 17-25 ms | 17-25 ms |
| Recovery/tail, frames 1680-2400 | 43-50 | 19-22 ms | 14-20 ms | 15-20 ms |

Finding:

- Not a breakthrough. It remains safe-looking in the submitted log, but the same busy
  stretch still reaches `cpu_only=25 ms`.
- Do not promote `cpu_batch=24`; the timing risk is not buying enough over the cleaner
  batch-16 row.

### Sprite render cache only on drawn frames - mixed/regression

This probe keeps the clean `cpu_batch=16` directmem/fastjmp baseline and avoids rebuilding
the sprite render cache on skipped visual frames.

What changes:

- In the old compile-time terminology, `FRAME_SKIP=2` already means every other NES frame
  runs with `draw_flag=false`; today that is runtime `Frameskip=1`.
- Skipped frames still need `ppu_fakeoam()` for sprite-zero status, but they never call
  `ppu_renderoam()` and therefore never read `oam_sl_count`, `oam_sl_idx`, `oam_pat1`, or
  `oam_pat2`.
- With `PPU_SPRITE_CACHE_DRAW_ONLY=ON`, `ppu_build_sprite_cache()` runs at scanline 0 only
  for drawn frames. The next drawn frame rebuilds the cache before rendering, so there is
  no stale sprite-cache dependency.

Why it is worth testing:

- This removes pure PPU bookkeeping from the `cpu_only` frames that define the remaining
  slowdowns.
- Unlike the earlier sprite-cache gating regression, this does not add per-sprite
  conditionals or change cache layout on drawn frames; it simply skips work that cannot be
  consumed on skipped frames.

Build target:

- `make diag-skipcache`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `ppu_strike=cycle`, `sprcache=draw`, `cpu_batch=16`, `cpu_opt=O3`,
  `cpu_memio=direct`, `cpu_fastjmp=on`, `cpu_rom=page`.
- Built and installed over the single device `Games/nofrendo.pdx` on 2026-05-25.
  Expected device banner timestamp: `build=2026-05-25 13:40:05`.

Device result:

- Submitted banner: `build=2026-05-25 13:40:05`, `sprcache=draw`, `cpu_batch=16`,
  `cpu_memio=direct`, `cpu_fastjmp=on`.
- The early/light windows were fine, but the busy band was worse than the clean baseline:
  frames 1500-1620 fell to `38`, `37`, then `35 fps`, with `cpu_only` rising to `26 ms`.
- Do not promote this. The likely explanation is that rebuilding sprite pattern cache even
  on skipped frames was warming CHR/OAM cache lines for the following drawn frame, or the
  reduced work simply changed D-cache shape in the wrong direction.

### Fixed scanline cycle accumulator - neutral/mixed

This probe keeps the current promoted rendering and CPU flags but removes float math from
the per-scanline CPU cycle accumulator.

What changes:

- The old code accumulates `1364.0 / 12` CPU cycles per scanline as a `float`, casts it to
  `int` before `nes6502_execute()`, then subtracts the actual elapsed cycles.
- `NES_FIXED_SCANLINE_CYCLES=ON` stores the same value as integer thirds: each scanline adds
  `341`, the executor receives `scanline_cycles / 3`, and elapsed cycles subtract
  `elapsed * 3`.
- This preserves the same integer truncation cadence as the float path while avoiding
  floating-point add/subtract/cast work in the 262-line frame loop.

Build target:

- `make diag-fixedcycles`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `ppu_strike=cycle`, `sprcache=all`, `cycleacc=fixed3`, `cpu_batch=16`,
  `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`, `cpu_rom=page`.
- Built successfully for device and simulator on 2026-05-25, then installed over the single
  device `Games/nofrendo.pdx`. Banner timestamp: `build=2026-05-25 20:43:59`.

Device result:

- Submitted banner: `build=2026-05-25 20:43:59`, `cycleacc=fixed3`, `cpu_batch=16`,
  `cpu_memio=direct`, `cpu_fastjmp=on`.
- The row is safe-looking but flat/slightly worse in the slow band: frames 1500-1620 were
  `38`, `38`, then `35 fps`, with `cpu_only=24-25 ms`.
- Do not promote this for now. The float operations were not the remaining bottleneck, and
  future probes should return to the clean `cycleacc=float` baseline.

### Fast OAM DMA page copy - safe but flat

This probe targets a per-frame sprite path that still used byte-at-a-time CPU memory reads.
NES games commonly write `$4014` every frame to DMA a 256-byte CPU page into OAM; Mario uses
this path heavily because enemy/item slowdowns correlate with sprite activity.

What changes:

- `PPU_FAST_OAMDMA=ON` replaces the 256 `nes6502_getbyte()` calls in `ppu_oamdma()` with
  direct copies from the mapped CPU memory page.
- The existing Nofrendo OAM fixup bytes are preserved, also using direct copies.
- The emulated timing stays the same: `nes6502_burn(513)` and `nes6502_release()` are
  unchanged.
- This should be safe for the current mapper path because `nes6502_getbyte()` itself reads
  directly from `cpu.mem_page[]`; it does not invoke read handlers.

Build target:

- `make diag-fastoamdma`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `ppu_strike=cycle`, `sprcache=all`, `oamdma=fast`, `cycleacc=float`, `cpu_batch=16`,
  `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`, `cpu_rom=page`.
- Built successfully for device and simulator on 2026-05-25, then installed over the single
  device `Games/nofrendo.pdx`. Banner timestamp: `build=2026-05-25 21:52:06`.

Device result:

- Submitted banner: `build=2026-05-25 21:52:06`, `oamdma=fast`, `cycleacc=float`,
  `cpu_batch=16`, `cpu_memio=direct`, `cpu_fastjmp=on`.
- The row is safe in Mario 1-1: no softlocks, no weird enemy behavior, and only minor
  pre-existing 1-2 frame visual glitches.
- It does not solve the remaining slow band. Busy windows still hit `39-43 fps` around
  frames 840-1200 and `35 fps` at frame 1620, with `cpu_only` still reaching `25 ms`.
- Keep the code as a safe optional micro-optimization, but do not treat it as the main path
  to stable 50 fps.

### FPS-only diagnostics - no material gain

This probe tests whether the diagnostic hooks themselves are stealing enough time to matter.
The normal diagnostic row calls `getCurrentTimeMilliseconds()` around every
`nes_renderframe()` so it can split `cpu_only` and `ppu_full`. That is useful for
attribution, but it is not release-like.

What changes:

- `DIAG_FPS_ONLY=ON` leaves the 60-frame FPS window and build banner intact.
- `diag_render_begin()` and `diag_render_end()` become no-ops, removing two time reads per
  emulated frame.
- Logs intentionally drop `cpu_only` and `ppu_full`; use this row to judge real gameplay
  smoothness and overall `fps/avg`, not subsystem attribution.

Build target:

- `make diag-fpslite`
- Expected settings: `diag=fps`, `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `ppu_strike=cycle`, `sprcache=all`, `oamdma=fast`, `cycleacc=float`, `cpu_batch=16`,
  `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`, `cpu_rom=page`.
- Built successfully for device and simulator on 2026-05-26, then installed over the single
  device `Games/nofrendo.pdx`. Expected device banner timestamp:
  `build=2026-05-26 00:14:04`.

Device result:

- Submitted banner: `build=2026-05-26 00:14:04`, `diag=fps`, `oamdma=fast`,
  `cycleacc=float`, `cpu_batch=16`, `cpu_memio=direct`, `cpu_fastjmp=on`.
- The row still has the same slow bands: `40 fps` at frames 840-900, `38-39 fps` at
  frames 1500-1560, and `35 fps` at frame 1620.
- Conclusion: per-frame diagnostic render timing is not the hidden remaining cost. Keep
  `diag=fps` around for release-like subjective tests, but CPU hot-path work is still needed.

### BNE-only fast branch - mixed, not promoted

The broad `NES6502_FAST_BRANCHES` probe was mixed, likely because changing every branch
inflated the already tight interpreter. This narrower probe targets only opcode `D0`
(`BNE`), which was repeatedly near the top of the Mario opcode profile.

What changes:

- `NES6502_FAST_BNE=ON` makes only `BNE` use the `pc_ptr` branch path: direct operand fetch,
  no `PC_REBASE()` when the branch target stays in the same 4 KB CPU page, and the same page
  crossing cycle accounting.
- Other branch opcodes stay on the current safe path.

Build target:

- `make diag-fastbne`
- Expected settings: `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `ppu_strike=cycle`, `sprcache=all`, `oamdma=fast`, `cycleacc=float`, `cpu_batch=16`,
  `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`, `cpu_fastbne=on`,
  `cpu_fastbranch=off`, `cpu_rom=page`.

Build/install status:

- Built successfully for device and simulator on 2026-05-26, then installed over the single
  device `Games/nofrendo.pdx`. Expected device banner timestamp:
  `build=2026-05-26 00:20:26`.

Device result:

- Submitted banner: `build=2026-05-26 00:20:26`, `cpu_fastbne=on`,
  `cpu_fastbranch=off`, `oamdma=fast`, `cycleacc=float`, `cpu_batch=16`.
- Mixed result: old 1500-1620 area improved compared with the fast-OAM row, but the 840
  window stayed bad (`39 fps`, `cpu_only=23 ms`) and a new/shifted 1680-1740 band hit
  `37-38 fps`, `cpu_only=23-24 ms`.
- Do not promote. This looks like another code-layout/cache tradeoff, not a robust win.

### 92% CPU cycle budget - mixed, not promoted

This is a deliberate speed-first accuracy tradeoff. The emulator runs only 92% of the usual
CPU cycles per PPU scanline. In games that spend spare frame time in wait loops, this should
remove wasted interpreter work while still letting the game update finish before NMI. If a
game relies on the exact CPU budget, it can cause timing bugs or slow-motion behavior.

What changes:

- `NES_CPU_CYCLE_PERCENT=92` scales the per-scanline CPU cycle accumulator.
- The current stable line is kept otherwise: batch 16, direct memory I/O, fast absolute-JMP
  operand fetch, fast OAM DMA, normal sprite-zero timing, and no BNE fast path.

Build target:

- `make diag-cycletrim`
- Expected settings: `cyclepct=92`, `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `ppu_strike=cycle`, `sprcache=all`, `oamdma=fast`, `cycleacc=float`, `cpu_batch=16`,
  `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`, `cpu_fastbne=off`, `cpu_rom=page`.
- Built successfully for device and simulator on 2026-05-26, then installed over the single
  device `Games/nofrendo.pdx`. Expected device banner timestamp:
  `build=2026-05-26 00:48:23`.

Device result:

- Submitted banner: `build=2026-05-26 00:48:23`, `cyclepct=92`, `oamdma=fast`,
  `cpu_batch=16`, `cpu_memio=direct`, `cpu_fastjmp=on`.
- It helped some old bands, especially around frames 840 and 1500-1560, but it did not
  stabilize the level. The run still hit `38 fps` at frame 900, `41-43 fps` around
  960-1200, and a worse shifted dip at frame 1800: `34 fps`, `avg=28 ms`,
  `cpu_only=26 ms`.
- Subjectively there were more visual glitches than normal. Do not promote 92%; it is too
  blunt.

### 96% CPU cycle budget - safe visually, mixed speed

This is the same speed-first idea as the 92% row, but gentler. The goal is to see whether a
small CPU-budget trim reduces wait-loop overhead without creating the extra visual glitches
and shifted slow band seen at 92%.

Build target:

- `make diag-cycletrim`
- Submitted banner: `build=2026-05-26 01:53:32`, `cyclepct=96`, `audio_fill=direct`,
  `hudfps=off`, `lcd_dirty=draw`, `ppu_strike=cycle`, `sprcache=all`, `oamdma=fast`,
  `cycleacc=float`, `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_fastbne=off`, `cpu_rom=page`.
- Installed as the single main device package at `/Volumes/PLAYDATE/Games/nofrendo.pdx`;
  verified no nested `nofrendo.pdx` directory before ejecting.
- Device result: no meaningful visual glitches. There was one barely noticeable bad frame
  mid-level, but this also happened in previous builds and is not a blocker right now.
- Speed was mixed. The run improved some known windows (`900`, `960`, `1020`, `1200`,
  `1500-1560`, and the late `2160-2280` band), but regressed/shifted others (`420-840`,
  `1080-1140`, `1260`, `1380`, and `1620`). Worst submitted window was still `36 fps`,
  `avg=27 ms`, `cpu_only=24 ms` at frame 1620.
- Do not promote 96% as a clear win yet, but it proves that a moderate cycle trim can be
  visually acceptable. Next probe: split the difference with `cyclepct=94`.

### 94% CPU cycle budget - mixed, not promoted

This is a calibration build between the glitchy 92% row and the visually safe 96% row. The
goal is to see whether the project can get a little more CPU relief than 96% without
bringing back the annoying visual glitches seen at 92%.

Build target:

- `make diag-cycletrim`
- Submitted banner: `build=2026-05-26 02:19:47`, `cyclepct=94`, `audio_fill=direct`,
  `hudfps=off`, `lcd_dirty=draw`, `ppu_strike=cycle`, `sprcache=all`, `oamdma=fast`,
  `cycleacc=float`, `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_fastbne=off`, `cpu_rom=page`.
- Installed as the single main device package at `/Volumes/PLAYDATE/Games/nofrendo.pdx`;
  verified no nested `nofrendo.pdx` directory before ejecting.
- Device result: still hits the same CPU-bound troughs. Worst submitted window was
  `35 fps`, `avg=28 ms`, `cpu_only=25 ms` at frame 1620; other busy windows hit
  `38-43 fps` with `cpu_only=20-23 ms`.
- Subjective result: a few minor graphical glitches. Nothing serious, but worse than the
  clean 96% visual result.
- Do not promote. The cycle-trim line is useful calibration, but it mostly shifts slowdowns
  and starts buying speed with visual risk before it reaches stable 50 fps.

### Computed-goto CPU dispatch - safe but not promoted

This probe goes back to the timing-safe CPU budget (`cyclepct=100`) and tests the original
Nofrendo GCC computed-goto opcode dispatcher instead of the current C `switch` dispatcher.
If the Playdate branch predictor and I-cache like it, this could reduce per-instruction
dispatch overhead in the remaining CPU-bound windows without changing emulated timing.

What changes:

- `NES6502_JUMPTABLE_DISPATCH=ON` stops forcing `NES6502_SWITCH` for `nes6502.c`.
- The computed-goto fetch path was updated to keep the existing `pc_ptr` cache in sync, so
  it can be tested on top of the current directmem/fastjmp line.
- Normal timing is restored: `cyclepct=100`, `cpu_batch=16`.

Build target:

- `make diag-jumptable`
- Expected settings: `cpu_dispatch=jump`, `cyclepct=100`, `audio_fill=direct`,
  `hudfps=off`, `lcd_dirty=draw`, `ppu_strike=cycle`, `sprcache=all`, `oamdma=fast`,
  `cycleacc=float`, `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_fastbne=off`, `cpu_rom=page`.
- Built successfully for device and simulator on 2026-05-26. Expected device banner:
  `build=2026-05-26 02:28:48`.
- Installed as the single main device package at `/Volumes/PLAYDATE/Games/nofrendo.pdx`
  and verified there is no nested `nofrendo.pdx` directory.
- Device result: no visual glitches, but no net speed win. It improved or held some
  windows, but regressed important busy stretches: frames 1560-1800 sat at `34-38 fps`
  with `cpu_only=23-25 ms`, and frames 960-1260 were also mostly `36-43 fps`.
- Do not promote. Keep the option available, but the stable switch dispatcher remains the
  better release line.

### Lazy total-cycle accounting - promoted current best

This probe keeps the switch dispatcher and normal timing, but stops writing
`cpu.total_cycles` on every interpreted opcode. `ADD_CYCLES()` still subtracts from the
global `remaining_cycles`, so `nes6502_release()` can still stop the slice immediately, but
the total cycle counter is committed once at the end of `nes6502_execute()`.

Why this is worth testing:

- The remaining bad windows are CPU interpreter-bound, so a per-opcode global write is
  exactly the kind of overhead that can matter on Playdate.
- PPU sprite-zero timing still asks `nes6502_getcycles(false)` during `$2002` reads, so the
  getter derives the live total from `cpu.total_cycles + slice_elapsed` while a CPU slice is
  active.
- `nes6502_release()` records unspent released cycles so OAM DMA release does not count the
  rest of the slice as executed CPU time.

Build target:

- `make diag-lazycycles`
- Expected settings: `cpu_dispatch=switch`, `cpu_cycles=lazy`, `cyclepct=100`,
  `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`, `ppu_strike=cycle`,
  `sprcache=all`, `oamdma=fast`, `cycleacc=float`, `cpu_batch=16`, `cpu_opt=O3`,
  `cpu_memio=direct`, `cpu_fastjmp=on`, `cpu_rom=page`.
- Built successfully for device and simulator on 2026-05-26. Expected device banner:
  `build=2026-05-26 02:39:29`.
- Installed as the single main device package at `/Volumes/PLAYDATE/Games/nofrendo.pdx`
  and verified there is no nested `nofrendo.pdx` directory.
- Device result: strongest Mario 1-1 row so far, with no visual glitches and the best
  subjective smoothness reported. Most windows are `49-50 fps`.
- Remaining slow windows are brief and CPU-bound: frames 840-900 are `43-44 fps` with
  `cpu_only=20 ms`, frames 1560-1680 are `39-42 fps` with `cpu_only=21-23 ms`, and most
  later windows recover to `49-50 fps`.
- Promote as the new baseline. Keep normal timing (`cyclepct=100`) and switch dispatch.

### BNE-only fast branch on lazy-cycle baseline - promoted current best

The old `NES6502_FAST_BNE` probe was mixed before lazy cycle accounting. This retest keeps
the new promoted baseline and adds only the BNE fast path, because opcode `D0` remained one
of the measured hot opcodes in real-gameplay windows.

Why this is worth retesting:

- The lazy-cycle change substantially altered interpreter cost, so the earlier BNE result is
  no longer definitive.
- It is timing-safe: it changes how the BNE operand/target is fetched, not how many CPU
  cycles the branch consumes.
- It is narrower than the rejected all-branch and broad fast-PC paths.

Build target:

- `make diag-fastbne`
- Expected settings: `cpu_cycles=lazy`, `cpu_fastbne=on`, `cpu_dispatch=switch`,
  `cyclepct=100`, `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`,
  `ppu_strike=cycle`, `sprcache=all`, `oamdma=fast`, `cycleacc=float`,
  `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`, `cpu_fastjmp=on`,
  `cpu_rom=page`.
- Built successfully for device and simulator. Expected device banner timestamp:
  `build=2026-05-26 02:47:10`.
- Installed as the single main device package at `/Volumes/PLAYDATE/Games/nofrendo.pdx`
  and verified there is no nested `nofrendo.pdx` directory.
- Device results: two Mario 1-1 runs, no visual glitches. User notes the lowest-fps spots
  included a slightly more stressful route than the prior comparison run.
- Run 1: mostly `49-50 fps`; the first busy section improved to frames 840-960 at
  `45/47/49 fps`; the later stressful section bottomed at frame 1620 `42 fps` and frame
  1680 `44 fps`.
- Run 2: mostly `49-50 fps`; frames 840-960 were `45/47/49 fps`; later route-dependent
  dips were frames 1620-1920 at `46/47/46/46/42/42 fps`, then recovery to `49-50 fps`.
- Compared with the pure lazy-cycle baseline, this improves the previously weak windows
  without adding visual glitches. Promote `cpu_fastbne=on` as the new baseline.

### BPL-only fast branch on lazy/BNE baseline - promoted current best

The next probe keeps the promoted lazy/BNE baseline and adds only a `BPL` fast path. The
old opcode profiles repeatedly showed opcode `10` (`BPL`) near the top in real gameplay,
but the all-branch fast path was too broad and not worth keeping.

Why this is worth testing:

- It is the same shape as the now-promoted `BNE` optimization: direct branch operand fetch
  and same-bank target update, with unchanged branch timing.
- It is narrower than the rejected `NES6502_FAST_BRANCHES` probe, so code-size/layout risk
  is much lower.
- If it helps, it should lift the remaining CPU-bound enemy/item windows without touching
  PPU timing or renderer behavior.

Build target:

- `make diag-fastbpl`
- Expected settings: `cpu_cycles=lazy`, `cpu_fastbne=on`, `cpu_fastbpl=on`,
  `cpu_fastbranch=off`, `cpu_dispatch=switch`, `cyclepct=100`, `audio_fill=direct`,
  `hudfps=off`, `lcd_dirty=draw`, `ppu_strike=cycle`, `sprcache=all`, `oamdma=fast`,
  `cycleacc=float`, `cpu_batch=16`, `cpu_opt=O3`, `cpu_memio=direct`,
  `cpu_fastjmp=on`, `cpu_rom=page`.
- Built successfully for device and simulator. Expected device banner timestamp:
  `build=2026-05-26 02:58:19`.
- Installed as the single main device package at `/Volumes/PLAYDATE/Games/nofrendo.pdx`
  and verified there is no nested `nofrendo.pdx` directory.
- Device result: one Mario 1-1 run, no visual glitches.
- Most windows are `49-50 fps`; the busy frame 840-960 band is `49/45/47 fps`, and the
  later busy band is frames 1620-1800 at `49/44/44/45 fps` before recovering to `50 fps`.
- Compared with the BNE-only baseline, this improves the worst observed floor in the
  submitted logs from `42 fps` to `44 fps` without adding visual issues. Promote
  `cpu_fastbpl=on` as the current best baseline.

### BEQ-only fast branch on lazy/BNE/BPL baseline - promoted in default build

This probe keeps the promoted lazy/BNE/BPL baseline and adds only a `BEQ` fast path. Opcode
`F0` (`BEQ`) was also frequently present in the historical real-gameplay opcode profiles.

Why this is worth testing:

- It follows the same timing-safe pattern as the promoted BNE/BPL fast paths.
- It remains much narrower than the rejected all-branch path.
- The remaining dips are still CPU-bound, so one more hot branch may help without touching
  PPU behavior.

Build target:

- `make diag-fastbeq`
- Expected settings: `cpu_cycles=lazy`, `cpu_fastbne=on`, `cpu_fastbpl=on`,
  `cpu_fastbeq=on`, `cpu_fastbranch=off`, `cpu_dispatch=switch`, `cyclepct=100`,
  `audio_fill=direct`, `hudfps=off`, `lcd_dirty=draw`, `ppu_strike=cycle`,
  `sprcache=all`, `oamdma=fast`, `cycleacc=float`, `cpu_batch=16`, `cpu_opt=O3`,
  `cpu_memio=direct`, `cpu_fastjmp=on`, `cpu_rom=page`.
- Built successfully for device and simulator. Expected device banner timestamp:
  `build=2026-05-26 12:46:08`.
- Installed over the single main device package at `/Volumes/PLAYDATE/Games/nofrendo.pdx`
  on 2026-05-27 after `make _push` hit the mounted-volume permission issue.
- Verified `pdex.bin` matches the local package and that there is no nested
  `nofrendo.pdx` directory.
- Follow-up default-build work promoted `cpu_fastbeq=on` into `FAST_FLAGS`; the current
  normal `make` path includes it alongside lazy cycles, BNE, and BPL.

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

- `NES6502_LINEAR_ROM` was tested and rejected: the full-level Mario row stayed effectively
  flat, with busy windows still reaching `cpu_only=24 ms` and `ppu_full=31 ms`.
- The platform-layer display-update skip is the new best baseline. It makes diagnostic
  builds log-only by default (`hudfps=off`) and avoids marking skipped visual frames dirty.
  Mario 1-1 now spends most windows at `49-50 fps`, with remaining dips at `37-45 fps`.
- The direct CPU memory-I/O fast path is stable and subjectively the best device build so far,
  but the worst busy windows still reach `cpu_only=24 ms`.
- Direct audio ring fill is stable and promoted as the current baseline; the worst busy
  window still reaches `cpu_only=24 ms`.
- Next build to test: fast absolute `JMP` operand fetch on top of the current baseline,
  expected banner fields `audio_fill=direct`, `cpu_memio=direct`, and `cpu_fastjmp=on`.
- Fast absolute `JMP` stayed safe and was kept. Fast branch and fast one-byte operand
  fetches were tested but not promoted. The clean promoted line is now directmem +
  fastjmp with runtime BG/sprite menu toggles removed.
- Latest clean row (`build=2026-05-25 13:02:33`) is near-native for most of Mario 1-1,
  but still peaks at `cpu_only=25 ms` in the busy mid-level band. The next probe is
  `NES6502_FAST_MEMOPS`, a narrow hot load/store specialization on top of that line.
- `NES6502_FAST_MEMOPS` was tested and rejected as mixed: it stayed safe, but the busy
  mid-level band still reached `cpu_only=25 ms` and several comparable windows got worse.
  The next probe is `diag-fastbatch`, a middle-ground `cpu_batch=24` timing row on the
  clean directmem/fastjmp baseline. Reject it immediately if Mario feels slow-motion.
- `diag-fastbatch` was tested and also rejected as mixed. It still reached `cpu_only=25 ms`
  in the busy mid-level band. The next probe is `diag-skipcache`, which keeps batch 16 and
  skips render-only sprite-cache construction on skipped visual frames.
- `diag-skipcache` was tested and rejected as mixed/regression. It reached `cpu_only=26 ms`
  in the busy band. The next probe is `diag-fixedcycles`, which keeps batch 16,
  `cpu_memio=direct`, and `cpu_fastjmp=on`, but replaces the float scanline cycle
  accumulator with exact integer thirds.
- `diag-fixedcycles` was tested and left unpromoted: safe, but flat/slightly worse in the
  slow band, still reaching `cpu_only=25 ms`. The next probe is `diag-fastoamdma`, a direct
  CPU-page copy for `$4014` sprite DMA with the same emulated 513-cycle burn.
- `diag-fastoamdma` was tested and is safe, but flat for the remaining problem: Mario 1-1
  still reaches `cpu_only=25 ms` and `35 fps` in the busy band. The next probe is
  `diag-fpslite`, which removes per-frame render timing while keeping 60-frame FPS logs so
  we can compare against a more release-like build.
- `diag-fpslite` was tested and did not materially improve the slow bands. The next probe is
  `diag-fastbne`, a narrow version of the rejected broad branch fast path that touches only
  the hot `BNE` opcode.
- `diag-fastbne` was tested and rejected as mixed: some dips moved/improved, but other busy
  bands regressed.
- `diag-cycletrim` at 92% was tested and rejected as mixed. It improved some old bands but
  added more visual glitches and shifted a bad dip to frame 1800. `diag-cycletrim` at 96%
  was visually safe but mixed on speed. `diag-cycletrim` at 94% was mixed and had minor
  visual glitches, so leave cycle-trim unpromoted.
- `diag-jumptable` was tested and rejected as safe but not faster: no visual glitches, but
  it regressed the mid-level busy stretch. The next probe is `diag-lazycycles`, which keeps
  `cyclepct=100` and switch dispatch while removing the per-opcode `cpu.total_cycles`
  write.
- `diag-tcmhot` was added as a relocation-mechanism proof, not as a speed row. Test it on
  device first; only then consider a compact hot-core experiment.
- `diag-tcmhot` passed on device: entry execution, relocated global access, and an in-block
  call all returned the expected values from DTCM.
- `diag-tcmcore` is now the next opt-in measurement build. It handles a deliberately tiny
  no-callback opcode set in DTCM, then falls back to the normal promoted interpreter for
  all other opcodes and remaining cycles.
- `diag-tcmcore` was tested through Mario 1-1 and rejected as a speed row: startup
  relocation succeeded, but the run still hit `43 fps` busy dips and added a worse
  `39 fps` window at frame 1620.
- `diag-tcmstats` was added as the follow-up attribution build to measure how often the
  tiny DTCM core actually handles cycles before falling back.
- `diag-tcmstats` confirmed the shape is a dead end: after the first window it handled zero
  cycles in every Mario 1-1 window (`hit=0`, `core=0`, `pct=0`), so do not extend this
  slice-entry-only wrapper.
- `diag-cpusplit` now measures the current promoted fast line plus CPU-exec timing, rather
  than the older neutral probe line.

### Current promoted-line CPU split retest

The 2026-06-06 `diag-cpusplit` retest used the current promoted line, not the older O3-only
profile row: direct audio ring fill, dirty-on-draw display updates, fast OAM DMA, direct
memory I/O, fast absolute `JMP`, lazy cycle accounting, and the promoted narrow
`BNE`/`BPL`/`BEQ` branch paths. TCM was off.

Device row:

- Submitted banner: `build=2026-06-06 20:58:21`, `cpu_split=on`, `cpu_tcmcore=off`.
- The row confirms the remaining Mario 1-1 dips are still interpreter-bound. In the worst
  submitted window, frame 1680 hit `fps=39`, `avg=25 ms`, `cpu_only=22 ms`,
  `cpu_exec=20 ms`, `cpu_misc=2 ms`, `ppu_full=23 ms`, and `ppu_exec=13 ms`.

| Segment in submitted log | fps | avg | cpu_only | cpu_exec | cpu_misc | ppu_full | ppu_exec |
|---|---:|---:|---:|---:|---:|---:|---:|
| Early/light windows, frames 60-840 | 49-50 | 19-20 ms | 6-17 ms | 3-15 ms | 2-3 ms | 10-18 ms | 4-10 ms |
| First busy band, frames 900-1260 | 43-48 | 20-23 ms | 17-21 ms | 15-19 ms | 1-2 ms | 17-21 ms | 10-13 ms |
| Late busy band, frames 1380-1680 | 39-46 | 21-25 ms | 18-22 ms | 16-20 ms | 2 ms | 18-23 ms | 10-13 ms |
| Recovery/tail, frames 1740-2520 | 44-50 | 19-22 ms | 12-19 ms | 10-17 ms | 1-3 ms | 15-20 ms | 7-11 ms |

Next measurement: retarget `diag-opprofile` to `FAST_FLAGS` so the opcode profile reflects
the current promoted interpreter rather than the stale O3-only line. Use that to decide
whether another narrow opcode path is worth trying, instead of extending the rejected TCM
slice-entry wrapper.

### Current promoted-line opcode profile retest

The 2026-06-06 `diag-opprofile` retest used the current promoted line plus opcode counters:
direct audio ring fill, dirty-on-draw display updates, fast OAM DMA, direct memory I/O,
fast absolute `JMP`, lazy cycle accounting, and the promoted narrow `BNE`/`BPL`/`BEQ`
branch paths. CPU split timing and TCM were off.

Device row:

- Submitted banner: `build=2026-06-06 21:05:27`, `cpu_prof=opcode`, `cpu_split=off`,
  `cpu_tcmcore=off`.
- As expected, the opcode counter slows the build and should not be judged as a playable
  speed row. It is an attribution row.
- Worst submitted window: frame 1680 at `fps=30`, `avg=32 ms`, `cpu_only=28 ms`,
  `ppu_full=30 ms`.

| Window | fps | Top opcode shape |
|---|---:|---|
| Fast/idle, frames 180-240 | 50 | `4C` dominates at 499k-544k; real-gameplay opcodes are small |
| First busy band, frames 420-600 | 39-40 | `4C` drops to 298k-314k; `C8/D0/85/C9/4A/99/A5` rise |
| Mid busy band, frames 900-1200 | 33-37 | `4C` drops to 219k-264k; `D0/85/C8/C9/4A/A5/F0/99` dominate the rest |
| Worst late band, frames 1560-1680 | 30-33 | `4C` bottoms at 142k-199k; `85/D0/C8/C9/99/4A/A5` are all hot |

Findings:

- The promoted `4C`, `D0`, `10`, and `F0` paths are still among the highest-count opcodes,
  but the worst windows now clearly shift toward real gameplay stores, compares, shifts,
  and zero-page loads.
- `C8` (`INY`) and `4A` (`LSR A`) are already tiny register-only cases, so there is little
  obvious safe work there.
- The largest remaining unpromoted cluster is the memory load/store group: `85`, `99`, and
  `A5`, with `AD/BD/8D` also present in lighter windows.

Next measurement: retarget `diag-fastmemops` to `FAST_FLAGS` and retest the existing hot
load/store opcode specialization on top of the current promoted line. The old row was
measured before lazy cycles and the promoted branch fast paths, so it is stale.

### Fast hot memory opcodes on promoted line - promoted current best

The stale `NES6502_FAST_MEMOPS` row was retested on the current promoted line instead of
the old directmem/fastjmp-only baseline. This keeps direct audio ring fill, dirty-on-draw
display updates, fast OAM DMA, direct memory I/O, fast absolute `JMP`, lazy cycle
accounting, and the promoted narrow `BNE`/`BPL`/`BEQ` branch paths, then adds the hot
load/store opcode specializations.

Device row:

- Submitted banner: `build=2026-06-06 21:12:05`, `cpu_fastmemops=on`,
  `cpu_prof=off`, `cpu_split=off`, `cpu_tcmcore=off`.
- Worst submitted window: frame 1560 at `fps=39`, `avg=25 ms`, `cpu_only=22 ms`,
  `ppu_full=23 ms`.

| Segment in submitted log | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Early/light windows, frames 60-720 | 49-50 | 19-20 ms | 6-16 ms | 10-18 ms |
| First busy band, frames 780-1260 | 44-49 | 20-22 ms | 15-19 ms | 17-20 ms |
| Late busy band, frames 1380-1620 | 39-46 | 21-25 ms | 18-22 ms | 19-23 ms |
| Recovery/tail, frames 1680-2400 | 45-50 | 19-21 ms | 12-18 ms | 15-20 ms |

Findings:

- This is a clear improvement over the stale pre-lazy fastmemops row and a better shape
  than the current promoted-line attribution/profile rows: most of Mario 1-1 sits at
  `49-50 fps`, and the serious late dip is now a brief frame-1560 trough rather than a
  long frame-1560-to-1680 band.
- Promote `NES6502_FAST_MEMOPS=ON` into `FAST_FLAGS`.
- Keep the older warning about broad hot-op specializations: the promoted bit is only the
  measured memory load/store subset, not the broader `NES6502_HOTOPS` path.

### Adaptive frameskip (Auto) - implemented 2026-06-11, awaiting device row

The remaining Mario 1-1 dips (39-44 fps in enemy/item windows) are interpreter-bound and
have resisted every narrow CPU optimization since FAST_MEMOPS. Adaptive frameskip attacks
the ship-blocker directly: it keeps game logic at native speed through the busy windows by
temporarily reducing visual refresh instead of letting the whole emulation slow down.

What changes (`src/osd.c`, `src/main.c`):

- The Frameskip menu now offers `Auto / 0 / 1 / 2`, with `Auto` as the new default.
  Menu index 0 maps to `FRAME_SKIP_AUTO (-1)` in osd.c; fixed values behave exactly as
  before (no extra timing calls in fixed mode, so old measurement rows stay comparable).
- Auto mode runs at the proven base skip 1 and boosts to skip 2 while overloaded.
- Load detection: EMA (alpha 1/4, 1/16-ms fixed point) of per-frame emulation work
  (`nes_renderframe` + input + audio fill), measured with two
  `getCurrentTimeMilliseconds()` calls per frame (shown immaterial by the fpslite row).
- Boost enters when EMA > 20 ms (PAL budget), exits when EMA < 18 ms AND the boost has
  held >= 50 frames (~1 s). The hysteresis gap plus minimum hold prevents the visual
  refresh flapping between 25 and 16 fps on borderline scenes.
- The draw cadence is a countdown counter reloaded on each drawn frame, so skip-level
  changes take effect cleanly without double-draw or long-gap artifacts.
- Diagnostic builds log `[autoskip] 1->2` / `[autoskip] 2->1` transitions to the console.

Expected behavior: light windows unchanged (49-50 fps at skip 1, 25 fps visual). In the
busy bands the boost converts the former 39-44 fps slowdown into native-speed gameplay at
~16 fps visual refresh for the duration of the dip.

**Validated on device 2026-06-11 — promoted.** Two rows were measured:

1. First push accidentally used plain `make install-diag`, which builds `PROBE_FLAGS`
   (everything off, `cpu_batch=1`, table memory I/O). The user immediately reported it
   playing much worse — banner check caught it. Lesson recorded below; `diag-fast` /
   `install-diag-fast` targets added for promoted-line validation builds.
2. The real row (`build=2026-06-11 16:49:05`, full FAST_FLAGS banner + DIAG):

| Segment | fps | avg | cpu_only | ppu_full |
|---|---:|---:|---:|---:|
| Whole Mario 1-1 run, all windows | **45-50** | 19-22 ms | 5-17 ms | 10-21 ms |
| Worst window (frame 1620-1680) | 45 | 21-22 ms | 16-17 ms | 20-21 ms |

Findings:

- Worst-case fps across the entire level rose from 39-44 (fixed skip 1) to **45**. Most
  windows sit at 48-50. User verdict: "Smoothest game I've ever tried on any emulator on
  the Playdate" — promoted.
- The controller behaved correctly: boosts entered under load and exited at/near the
  minimum hold once load cleared.
- Tuning flaw found in the row: nearly all boosts fired at EMA 321-328 (20.06-20.5 ms,
  barely over the 20 ms enter threshold) and exited right at the 50-frame minimum hold —
  marginal windows flickering into boost. Un-boosted, those windows run 47-49 fps
  (imperceptible game slowdown); boosted, they cost a second of visible 16 fps refresh.
  Only the frame-1620 window (EMA 338 = 21.1 ms) genuinely needed boost. The user
  reported the boosted stretches as visually choppy.
- Fix applied: `AUTO_BOOST_ENTER_FP` raised from 20 ms to 21 ms (exit stays 18 ms, so
  the hysteresis gap widened from 2 to 3 ms). Marginal windows no longer boost; real
  dips still do.
- Confirmation row (`build=2026-06-11 16:54:57`): boost count dropped from ~13 to ~8
  per level, and every remaining boost fired at EMA 337-345 (21.1-21.6 ms — genuinely
  over budget) instead of the marginal 320s. Floor 44-45 fps (one long-boost window at
  frame 1800), everything else 46-50 fps. Long quiet stretches show zero transitions.
  User verdict: better than before. **21 ms enter / 18 ms exit / 50-frame hold is the
  promoted tuning.**

### Makefile target pitfall: `diag` vs `diag-fast`

Plain `make diag` / `make install-diag` builds **`PROBE_FLAGS`** — the neutral
everything-off configuration for attribution probes (`cpu_batch=1`, `cpu_memio=table`,
no fast paths, eager cycles). It is roughly 25% slower than the promoted line and must
never be used to judge release behavior or playability. For release-candidate validation
with `[diag]`/`[autoskip]` logs, use **`make install-diag-fast`** (FAST_FLAGS + DIAG=ON).
Always check the banner line before reading a row: the promoted line shows `oamdma=fast
cpu_batch=16 cpu_cycles=lazy cpu_memio=direct cpu_fastjmp=on cpu_fastbne=on
cpu_fastbpl=on cpu_fastbeq=on cpu_fastmemops=on`.

### NES work RAM in DTCM - probe built 2026-06-11, awaiting device row

The remaining dips are interpreter-bound, and the opcode profile of the worst windows is
dominated by zero-page loads/stores, stack traffic, and compares (`85/A5/D0/C8/C9/99/4A`)
— exactly the opcodes that hit the 2 KB NES work RAM. This probe moves that RAM from a
heap `malloc` into the DTCM stack pool: every zero-page/stack access becomes zero-wait-state
and deterministic, and up to 64 D-cache lines are freed for the thrashing 32 KB PRG ROM.

Distinct from two earlier failures: the rejected DTCM row moved the *cpu struct* (already
cache-hot); the rejected 4 KB bg-tile cache *added* new D-cache pressure. This probe
relocates existing hot data *out* of the D-cache pool. Data-only, so none of the BL-range /
relocation hazards that limited code relocation apply.

Mechanism (`osd_dtcm_ram_alloc` in `src/osd.c`, used by `nes_create`):

- Same frame-derived scheme as the validated tcmhot probe: pool top = current stack frame
  minus 0x2180, block placed at the pool top, 32-byte aligned.
- Known risk: 2 KB exceeds the Vecx guide's conservative 1328-byte ceiling for that pool.
  Collision with OS data or deep stack growth would corrupt NES RAM (game state) and show
  up as immediate, obvious gameplay chaos — making this a cheap pass/fail probe.
- Falls back to heap (and logs it) when the derived address is outside DTCM; this also
  covers the simulator. Startup logs `[dtcmram] dest=... size=2048 frame=...`.
- `nes_destroy` skips `free()` when the RAM is the DTCM block.

Build: `make install-diag-dtcmram` (FAST_FLAGS + DIAG + NES_RAM_DTCM).

**Device result: REJECTED — crash.** Startup logged
`[dtcmram] dest=0x200071a0 size=2048 frame=0x20009b30` (allocation succeeded), then the
device hung on a black screen during Mario ROM load until the watchdog killed it.

Findings:

- The 2 KB block spanned 0x200071a0-0x200079a0 — roughly 700 bytes deeper into the pool
  than the validated 80-byte tcmhot probe (0x20007970-0x200079c0). Something in that
  deeper region is live during ROM load.
- `NES_RAM_DTCM` stays available as an opt-in probe flag but must remain OFF until a
  corrected placement is validated. The device was restored to the fast-line diag build.

**CORRECTION (2026-06-12, from the full Vecx PLAYDATE_ITCM_GUIDE.md):** the original
"1328-byte hard ceiling" conclusion above is wrong. The guide's bisection on the same
hardware found the ceiling was a *placement* artifact:

- DTCM layout: firmware data below a floor of ~**0x200074d0**; live stack above a ceiling
  that stayed over ~0x20008a00 for vecx. The gap between is a **~5 KB writable pool**.
- Our crashed block (0x200071a0-0x200079a0) had its bottom ~0x330 *below the firmware
  floor* — it corrupted firmware data. The crash is fully explained by placement, not size.
- Firmware holes exist at 64-byte granularity below the floor (e.g. 0x20006980); dense
  writes there fault.
- The guide's vecx data-relocation caveat ("don't move data that fits D-cache": their 1 KB
  RAM in DTCM lost 4.5%) does **not** transfer to FamiCrank: their regression came from
  converting a fixed-address array into pointer indirection, and FamiCrank's NES RAM is
  already pointer-accessed (`ram = cpu.mem_page[0]`), so relocation adds zero indirection.
- Also from the guide, for any future hot-core attempt: the old tcmcore failure was the
  *wrapper shape* (slice-entry only, hit=0), not the mechanism — vecx's +13-15% core is
  entered per instruction with per-instruction fallback. Outbound calls from relocated
  code need `-mlong-calls -fno-lto` on that translation unit. Vecx also measured an
  I-cache packing fragility effect (±3 fps from unrelated edits) that matches our long
  history of flat/mixed layout probes.

**DTCM pool scan — MEASURED 2026-06-12 (`make install-diag-dtcmscan`).** Paint-and-
watermark run: painted 0x200074d0..0x200099c0 (9,456 bytes) at first update, full Mario
1-1 with audio and busy scenes. The clean run was **identical in all four reports**:

```
[dtcmscan] clean run 0x200074d0..0x200095a8 (8408 bytes)
```

- **FamiCrank's measured safe DTCM pool: 0x200074d0-0x200095a8 (8,408 bytes).**
- The clean run starts exactly at the paint floor — vecx's firmware floor (0x200074d0)
  holds for this SDK/app, and no firmware activity occurred above it.
- The deepest stack excursion over the full level (incl. audio interrupts and autoskip
  transitions) reached only 0x200095a8 — startup frame was 0x20009bf0.
- Budget: enough for the 2 KB NES RAM **and** a ~4-5 KB per-instruction hot core with a
  generous stack margin. Caveat: measured on one game/level; deeper-stacking ROMs or
  menus could lower the ceiling. Keep relocated blocks at the pool bottom.

**NES RAM retry — STABLE, PROMOTED 2026-06-12.** `osd_dtcm_ram_alloc` places the 2 KB
block at fixed **0x20007500-0x20007D00** — bottom of the measured pool, 64-byte aligned,
6.3 KB below the observed stack watermark.

Device row (`build=2026-06-12 00:50:09`, `[dtcmram] dest=0x20007500 size=2048`):

- **Stability: full Mario 1-1, zero corruption, zero glitches.** First successful TCM
  relocation of the campaign; validates the guide's pool map end to end on this device.
- Performance: flat to slightly positive, ~1 ms territory. The stubborn late band
  narrowed (one 44 fps window vs 3-4 consecutive 44-45 windows in the two prior runs),
  boosts mostly at minimum hold (longest 73 frames vs 103-144), tail windows 11-12 ms
  `cpu_only` vs 12-13 ms. Within noise individually; direction consistently positive.
- Promoted into `FAST_FLAGS` despite the modest measured win because the change has no
  plausible cost: the RAM was already pointer-accessed (no new indirection — the vecx
  data-relocation caveat does not apply), DTCM access is single-cycle (equal to a D-cache
  hit, never a miss), and 64 D-cache lines are freed for PRG ROM. Measured non-negative +
  mechanically can't-lose.
- Ship caveat: pool watermark measured on one game/level. Soak-test a few other ROMs
  from the picker before release. Remaining pool above the RAM block:
  0x20007D00-0x200095a8 (~6.3 KB incl. margin) — available for a future per-instruction
  DTCM hot core (the vecx-shaped one, +13-15% in their project).

### Hot-opcode case clustering - probe built 2026-06-12, awaiting device row

Layout probe on the promoted fast line: the 16 measured-hot opcode case bodies are
emitted first in the dispatch switch so they pack into adjacent I-cache lines, instead
of being scattered across the ~10-13 KB function in numeric opcode order.

- Cluster, hottest first: `4C D0 F0 10 85 A5 C8 C9 99 4A A9 20 60 8D AD BD`
  (from the 2026-06-06 promoted-line opcode profile plus the always-hot JSR/RTS/LDA#).
- Mechanism: `NES6502_HOT_CLUSTER` emits verbatim copies of those case blocks right
  after the switch opens; the originals are `#ifndef`-guarded out. Duplicate-case
  compile errors guarantee each opcode exists exactly once, so semantics are unchanged.
- All 16 relocated cases were verified `break`-terminated with no fall-through
  dependencies on their original neighbors.
- Banner field: `cpu_hotcluster=on`. Build: `make install-diag-hotcluster`.
- **cmake cache contamination recurrence**: the first hot-cluster push crashed with a
  `[dtcmram]` startup line — the rejected `NES_RAM_DTCM=ON` had survived in the cmake
  cache because the two new options were missing from the `BASE_FLAGS` explicit-OFF
  list. Rule reaffirmed: **every new cmake option must be added to `BASE_FLAGS` with
  an explicit OFF** the moment it is created, or it leaks into every later build.
  Both options are pinned now.
- Expectation is low: prior layout probes (`-falign-loops`, jumptable dispatch) came
  back flat or worse. Judge against the 2026-06-11 16:54 Auto-frameskip row; the
  signal to watch is `cpu_only` in busy windows and the number of `[autoskip]` boosts.

**Device result (`build=2026-06-12 00:19:13`): FLAT — not promoted.**

| Comparison vs 2026-06-11 16:54 baseline | Baseline | Hot cluster |
|---|---|---|
| Worst window | 44 fps, avg=22 ms, cpu_only=17 ms | 45 fps, avg=22 ms, cpu_only=17 ms |
| Busy-window cpu_only | 16-17 ms | 15-17 ms |
| [autoskip] boosts per level | ~8 | ~9 (same sections, same EMA levels) |
| Light windows | 49-50 fps | 49-50 fps |

Findings:

- Differences are within the established ±1 fps run-to-run noise. Keep
  `NES6502_HOT_CLUSTER=OFF`; the flag stays available as a probe.
- Diagnostic value: this rules out I-cache placement of hot case bodies as the busy-window
  cost. Together with the DTCM RAM rejection, the remaining 16-17 ms busy-window
  `cpu_only` is the practical floor of this interpreter on this cache hierarchy.
- **Release candidate**: promoted fast line + Auto frameskip (21 ms enter / 18 ms exit /
  50-frame hold). `make install` ships it with DIAG=OFF.

### Multi-ROM soak round - 2026-06-12 (DTCM RAM build 00:50:09)

Five games, five mappers, full sessions each. **DTCM RAM stability: PASS in all five —
zero crashes, zero state corruption.** The promoted `NES_RAM_DTCM` placement is soaked.

| Game (mapper) | fps | cpu_only | ppu_full | Verdict |
|---|---:|---:|---:|---|
| Zelda 1 (MMC1) | 43-50 | 6-18 ms | 10-23 ms | Plays decently; dips only in busy scroll scenes |
| Mega Man 2 (MMC1) | 38-50 | 8-21 ms | 12-27 ms | Plays decently |
| Punch-Out!! (MMC2) | 43-50 | 8-12 ms | 14-25 ms | OK but autoskip flapped (see below) |
| Kirby's Adventure (MMC3) | 26-43 | 17-31 ms | 22-44 ms | **Plays badly — batching cliff** |
| Batman | 28-50 | 11-27 ms | 14-40 ms | Plays badly; possible sprite glitch (unconfirmed vs dithering) |

**Finding 1 — the MMC3/IRQ-mapper batching cliff.** `cpu_batch=16` only engages when the
mapper has no hblank callback. MMC3 (Kirby, SMB3) and Batman's mapper clock per-scanline
IRQ counters, so those games silently run per-scanline interpreter entry — the
pre-batching configuration that measured 29-34 fps in May. The Kirby/Batman numbers match
that era exactly. This is now the single largest per-game performance gap. Possible
future work: batch CPU through the 22 vblank scanlines (no IRQ counter clocking while
rendering is off) for IRQ mappers — a safe subset worth ~8% of the frame; full
visible-line batching for MMC3 would need careful IRQ-timing treatment (the batch-32
history shows how that fails).

**Finding 2 — autoskip flap pathology (Punch-Out).** Load shape: draw frames 24-25 ms
(huge sprites) but skip frames only 11-12 ms. At skip 1 the EMA sits right at the enter
threshold; boosting collapses the average under the exit line, so the controller flapped
1->2->1 once per second for the entire session (every exit at the 50-52-frame minimum,
every re-entry within a few frames). Visual result: refresh toggling 25<->16 fps
continuously.

**Fix applied (anti-flap escalation):** if a boost re-enters within 120 frames of the
last exit, the hold escalates from 50 to 300 frames. Sustained at-threshold loads now
switch visual modes ~5x less often; spaced re-entries (Mario's pattern) keep the short
hold. Diag line now logs the chosen hold: `[autoskip] 1->2 ema=... hold=...`.
Validation: re-run Punch-Out; expect far fewer `[autoskip]` transitions, `hold=300`
after the first flap, and steadier visuals.

**Finding 3 — Batman sprite glitch: CONFIRMED real (2026-06-12 retest).** Not
quantization. Prime suspect: the sprite pattern cache's documented blind spot — it
pre-decodes all CHR at scanline 0 and assumes the mapping stays valid for the frame.
Batman's Sunsoft mapper animates sprites by bank-switching CHR mid-frame, so the cache
serves stale pre-switch patterns every frame. Supporting evidence: Punch-Out's 8x16
sprites render correctly (rules out the 8x16 decode path), and Batman is on the same
IRQ-mapper class that does mid-frame raster work.

**Probe result (`make install-diag-livechr`, build 01:50:22): sprite FIXED with live CHR
reads — root cause confirmed.** The cache's scanline-0 snapshot goes stale when the game
bank-switches CHR mid-frame.

**Fix shipped (2026-06-12): CHR-dirty auto-fallback.** `ppu_setpage()` sets
`sprite_chr_dirty` whenever a CHR page (0-7) is remapped; `ppu_renderoam()` checks the
flag per sprite — clean cache uses the fast pre-decoded path, dirty falls back to live
CHR reads for the rest of the frame; `ppu_build_sprite_cache()` clears the flag at
scanline 0. Games that never switch CHR mid-frame (Mario etc.) keep full cache speed
(one predictable branch added per sprite-scanline); mid-frame switchers (Batman) get
correctness at live-read cost. `PPU_SPRITE_LIVE_CHR` remains as a force-live probe flag.
This was a **correctness bug affecting all mid-frame CHR-switching games**, not only
Batman — MMC3 titles that swap sprite banks per-frame were silently at risk too.

**Validated 2026-06-12 (build 04:57:55, single Batman-then-Mario log):** Batman sprites
render correctly through the runtime fallback (banner `sprcache=all`, not the forced-live
probe). Mario from frame 3600 holds 49-50 fps with `cpu_only` 12-17 ms — the per-sprite
dirty-flag branch costs nothing on the clean fast path. Correctness + zero fast-path
regression confirmed in one run. **Promoted; the fix is default runtime behavior.**

**Anti-flap validation (2026-06-12, build 01:14:29):**

- Punch-Out: first boost `hold=50`, immediate re-entry escalates to `hold=300`;
  transitions ~6x less frequent (every ~6 s vs every ~1 s); steady 44-46 fps; user
  confirms it plays better. **Promoted.**
- Mario 1-1 regression check: clean — same 44-50 shape, short holds for spaced load,
  escalated holds engage only in sustained sections and behave correctly.

### IRQ-mapper CPU batching - probe built 2026-06-12, awaiting device row

The MMC3/IRQ-mapper batching cliff (Kirby/SMB3/Batman at 28-36 fps) is the last perf
item. Root cause: `cpu_batch_lines = mapintf->hblank ? 1 : 16` — any scanline-IRQ mapper
runs 262 separate `execute_cpu()` calls/frame (interpreter evicted from I-cache by PPU
code between each), the pre-batching configuration.

**Why the May batch-32 failure does NOT apply here.** batch-32 batched blindly and
delivered the split IRQ up to 16+ lines late → slow-motion/softlock. This design is
*timing-exact*: it clamps the batch to the mapper's own IRQ countdown so no IRQ can ever
fire mid-batch.

Design:
- New optional `mapintf_t.irq_countdown()` (trailing field → auto-NULL for all 40+
  existing mappers via positional init). MMC3 (`map4_irq_countdown`) returns scanlines
  until the counter underflows; conservative (returns 1 on reset/just-fired, 0x7FFF when
  IRQ disabled) so it always errs toward less batching, never across an IRQ.
- `nes_renderframe`: hblank mappers WITH a countdown batch at width 16; before each
  hblank, if `irq_countdown() <= 1` (this hblank will fire), flush the pending CPU batch
  first. CPU then sits exactly at the previous scanline, IRQ injects with correct timing,
  taken at the next batch start — identical to batch=1 but paid only at the ~1 split line
  per frame instead of all 240. hblank mappers WITHOUT a countdown still get batch=1.
- Non-IRQ mappers (Mario etc.) are completely unaffected.

Build: `make install-diag-irqbatch` (banner `irq_batch=on`). Mappers other than MMC3
keep `irq_countdown=NULL` → unchanged, so only MMC3 titles change behavior in this probe.

**Device result (build 2026-06-13 21:28:54, `irq_batch=on`): BIG WIN — promoted.**

| Kirby's Adventure | batch=1 (04:57:55) | irq_batch | Delta |
|---|---|---|---|
| Busy windows fps | 28-36 | 34-44 | +~8 fps |
| Busy windows cpu_only | 22-28 ms | 17-24 ms | -~5 ms |
| Menu / lighter play fps | ~40 | 46-49 | +~7 fps |

- Mario 1-1: 44-50 fps, identical shape — non-IRQ path untouched, **zero regression**
  (confirms the change is isolated to hblank mappers with a countdown).
- No slow-motion, no softlock observed in menu + level 1-1.
- The timing-exact countdown clamp delivered the IRQ-free-stretch batching benefit
  without the batch-32 timing failure. Promoted into `FAST_FLAGS`.
- Benefits every MMC3 title (SMB3, Kirby, Crystalis, many more) — the largest single
  per-game-class win since the original batching. Other IRQ mappers (MMC5, VRC, FME-7)
  still get batch=1 until they implement `irq_countdown` — a clean future extension.
- **Open visual check**: status-bar split position on Kirby/SMB3 (timing correctness
  proof) still needs an explicit eyeball; if a split ever jitters by a line, tune
  `map4_irq_countdown`. No artifact reported in the validation session.

### DTCM hot core, per-instruction-fallback redesign - probe built 2026-06-13

The original `NES6502_TCMHOT_CORE` (diag-tcmcore, May) relocated a 9-opcode core to DTCM
but measured `hit=0` — it handled zero cycles in real gameplay. Root cause diagnosis this
round (two bugs):

1. **Hot set too small / missing control flow.** It lacked `4C` (JMP absolute) — the single
   most frequent opcode — so any loop hit an uncovered op within a few instructions.
2. **Catastrophic fallback shape.** On any uncovered op it `break`-ed and handed the *entire
   rest of the slice* (a 16-scanline batch ≈ 1900 instructions) to the slow interpreter. One
   early cold op ⇒ core did nothing ⇒ `hit=0`.

Redesign (this probe):

- **Placement fixed to the measured pool.** Core copied to a fixed `0x20007D00` (above the
  DTCM NES-RAM block at 0x7500-0x7D00), not the old frame-derived `frame-0x2180` that
  straddled the firmware floor. Bounds-checked against the 0x200095a8 stack watermark.
  Section measured 992 bytes (cap 4096, pool room 6312).
- **Per-instruction fallback (the key fix).** On an uncovered or IO-touching opcode the core
  executes exactly ONE instruction via the full interpreter (`fallback(1)`), then RESUMES its
  DTCM loop — instead of bailing the whole slice. The hot path stays resident in fast memory.
- **No timing hazard.** All IO/PPU-touching ops (incl. `$2002` sprite-0 reads) go through the
  fallback, which keeps the lazy-cycle live-timing path intact. The DTCM core only ever runs
  ops that touch RAM/ROM/registers — no `nes6502_getcycles` dependency inline.
- **Relocation-safe.** objdump confirms the only outbound transfer from the DTCM section is
  `blx r3` (indirect fallback pointer, valid at any distance). No direct `bl`/`b.w` leaves
  the section; globals resolve via `.rel.text` (the `.tcmhot`-inside-`.text` linker fix).
- Cycle accounting verified: core adds only its inline cycles to `total_cycles`; fallback
  commits its own; no double-count. Return = total consumed.

This iteration adds only `4C` to the inline set (the dominant idle/control opcode) on top of
the existing tested ops {INY,LSR,CMP#,LDA#,LDA zp,STA zp,BNE,BEQ,BPL} — minimal correctness
surface. The architectural fix is the per-instruction fallback.

**Device result (build 2026-06-13 22:03:45, FAST_FLAGS + tcmstats): REJECTED — principled
negative. The DTCM hot-core lever does not transfer to FamiCrank.**

The redesign WORKS (the per-instruction fallback fixed the `hit=0` failure), but it does not
pay off:

| | Mario 1-1 | Kirby (MMC3) |
|---|---|---|
| Coverage (`pct`) | 0% (core never engages; plays perfectly) | **71%** inline in DTCM |
| Speed vs prior baseline | unchanged 44-50 fps | **REGRESSED** 25-37 vs 34-44 fps (irq_batch) |

The `[tcmcore]` Kirby line, e.g. `core=1333296 fallback=453546 pct=74 max=1821` — the core
genuinely ran 71-74% of cycles inline in DTCM and was *still slower*. That is the decisive
data point, and it explains why this whole technique is wrong for us:

1. **Our interpreter already fits the 16 KB I-cache** (~13 KB with LEGAL_ONLY). vecx's 6809
   core was ~20 KB and could NOT fit — they had constant I-cache eviction that relocation
   relieved. **We have no such eviction to relieve, so the core benefit vecx measured simply
   doesn't exist for us.** This is the root reason the +13-15% does not transfer.
2. **The naive if-else DTCM core is slower per opcode than the main `switch`** — which carries
   FAST_MEMOPS, FAST_JMP_ABS, the branch fast paths, and jump-table-class dispatch the small
   core lacks. Even "covered" ops run slower relocated.
3. **Per-instruction fallback double-marshals** the register file on every cold op (TCM_FLUSH
   + the fallback's own GET/STORE + TCM_RELOAD): ~40 cyc × ~8700 cold ops/frame ≈ 2 ms/frame
   on Kirby — directly the measured regression.

Correctness note: Mario played identically and the redesign is sound (no desync) — the
per-instruction fallback is a *correct* design, just not a faster one here. The reshaped
core + measured-pool placement are kept behind `NES6502_TCMHOT_CORE` (OFF in BASE/FAST) as
reference infrastructure; **do not promote**. This closes the DTCM-hot-core line: the
prerequisite (an interpreter that overflows I-cache) is absent in this project.

**Net for IRQ-mapper games (Kirby/Batman): the IRQ-mapper batching (promoted) remains the
win; the hot core does not add to it.** The remaining gap on those heavy MMC3 engines is
diffuse D-cache + real 6502 work — the same floor as Mario, just hit harder.

### PRG-page execution profile + hot-page DTCM relocation - 2026-06-14

Reframing after the hot-core failure: every TCM attempt so far relocated *code* (I-cache).
The actual bottleneck is *data* — the NES program is read by our interpreter via
`mem_page[...]` from cache-backed SRAM, so opcode/operand fetches are **D-cache** traffic
(32 KB PRG vs 16 KB D-cache). New profiler `NES6502_PRGPROFILE` (`make diag-prgprof`)
histograms executed instructions per 4 KB PRG page.

**Kirby full level 1 result (per-mille of executed instructions):**

| Window type | p8 | p9 | pA | pB | **pC** | pD | pE | pF |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| Light/fast (45-49 fps) | ~60 | ~5 | ~15 | ~5 | **720-840** | ~100 | ~10 | ~15 |
| Slow (30-35 fps) | ~120 | ~55 | ~15 | ~8 | **400-550** | ~130 | ~85 | ~120 |

- **Page C ($C000-$CFFF) dominates: 72-84% in calm scenes, ~40-55% in the slow ones.**
- The slow windows show the working set *spreading* (p8/p9/pD/pE/pF all rise as pC falls) —
  the D-cache thrash mechanism captured directly: busy gameplay touches more code pages,
  overflowing the 16 KB D-cache.
- Strong GO signal for relocating page C (and maybe pC+pD) to DTCM. Caveat: the slow-window
  spread means even pC+pD (~52%) leaves ~48% still thrashing, so expect partial relief.

**Relocation probe built (`NES_PRG_DTCM`, `make diag-prgdtcm`):** mirror page C (4 KB) into a
fixed DTCM buffer at 0x20007D00 (above the NES-RAM block, within the measured pool).
**Relocate-on-map**: `nes6502_setcontext` (the single chokepoint every bank switch flows
through) copies whatever ROM bank is mapped to page C into the DTCM buffer and repoints
`cpu.mem_page[12]` at it — correct whether the page is a fixed or swappable MMC3 bank (a swap
just re-copies). Manual word-copy (memcpy's LDM/STM hard-faults writing DTCM); source const,
dest volatile. `[prgdtcm] copies=` logs the count: **~1 = page C is a fixed bank (ideal);
growing fast = the mapper swaps it and re-copy cost may eat the win.**

**Device results (2026-06-14):**

1. First mirror build: Mario perfect (`copies=1`), but **Kirby white-screened entering the
   level**. Hypothesised a stack collision; **disproven by a Kirby stack scan** — Kirby's
   watermark is 0x20009438, 1848 bytes ABOVE the buffer top (0x8D00). The buffer was never
   touched. Real cause: relocating page 12 alone left it non-contiguous with page 13 ($D000,
   still ROM), so a 16-bit read straddling the $CFFF->$D000 seam got a garbage high byte.
   Mario never lands on that seam; Kirby (72-84% from this bank) does → corrupted jump → white
   screen. **Fix: mirror page 12 + 32 bytes of page 13's start** (`NES_PRG_DTCM_OVERLAP`) so
   seam word-reads resolve correctly.

2. Seam-fixed build: **Kirby runs the full level, no white screen, `copies=1`, no glitches.**
   Correctness solved. **But the speed is FLAT** — no measurable gain:
   - Mario: `cpu_only` 12-18 ms busy, 49-50 fps — identical to baseline (already fps-capped).
   - Kirby: busy windows 33-39 fps / `cpu_only` 21-25 ms vs the irq_batch baseline's 34-44 /
     17-24 — within noise, if anything a hair higher (the per-bank-switch relocation check).

**REJECTED — principled negative, same wall as the hot core.** The hottest page is already
D-cache-resident by definition (page C is so constantly accessed that LRU keeps it cached, so
its reads were already ~1-cycle hits). Relocating an already-cached page to DTCM buys nearly
nothing. The D-cache *misses* are on the **diffuse spread** of the other pages (p8/p9/pD/pE/pF)
that light up in busy scenes — 45-55% of busy-window fetches, too many and too spread to fit
in the ~6 KB DTCM pool. This is the third fast-memory data/code relocation to come back flat
for the same reason (after the DTCM cpu-struct and the hot core): **the hot working set is
already cached; the cold misses are diffuse and don't fit fast memory.** The fast-memory
avenue for the CPU bottleneck is exhausted. `NES_PRG_DTCM` stays an OFF probe flag; the
profiler (`NES6502_PRGPROFILE`) and the seam-overlap technique are kept as reference.

### Turbo toggle (runtime CPU cycle-trim) - added 2026-06-14

After the fast-memory avenue was exhausted (cpu-struct DTCM, hot core, PRG mirror all flat),
the user opted for an explicit speed-over-accuracy lever. Cycle-trim was tried compile-time
before (diag-cycletrim: 92% glitchy, 96% safe-but-mixed) and rejected as a forced default —
but as an **optional, off-by-default runtime toggle** it's a legitimate power-user feature.

Implementation:
- `nes.c`: the float-path per-scanline cycle step is now a runtime global
  `nes_scanline_cycle_step` (was the compile-time `NES_SCANLINE_CYCLES` macro), with
  `nes_set_cpu_cycle_percent(int)` to scale it (clamped 50-100%).
- `osd.c`: `osd_set_turbo(level)` maps menu levels to percentages —
  Off=100, Light=95, Med=90, High=85. Default Off (fully accurate).
- `main.c`: "Turbo" options menu item. **Playdate caps custom menu items at 3** (ROM Picker
  + Frameskip + Turbo), so Show FPS was dropped from the menu to make room (its `drawFPS`
  overlay stays on by default; the osd machinery is retained).

Mechanism/expectation: trimming runs fewer 6502 cycles per emulated frame → less work →
higher fps, but the game gets fewer cycles/frame so CPU-bound games run slightly slow / more
glitch-prone. Games with spare frame time (idle waits) get near-free fps; heavy games
(Kirby/Batman) trade visible glitches for speed. Default-off keeps the accurate experience
intact.

**Device result: REJECTED — reverted 2026-06-14.** Kirby at High was too glitchy to play
with no noticeable speedup; Batman not glitchy but no perceptible gain; user estimate 1-2 fps
at best, not worth the artifacts. Confirms the reason: cycle-trim only reclaims idle/wait-loop
cycles, but the heavy games are CPU-bound on **real work**, so trimming cuts into that work
(glitches) rather than buying speed. Turbo menu + runtime cycle-step reverted; Show FPS
restored.

### Optimization campaign — practical ceiling reached (2026-06-14)

After ~30 measured experiments, every lever that worked is promoted (LEGAL_ONLY, switch
dispatch, FRAME_SKIP, sprite cache, PC_PTR, fast branches/memops, lazy cycles, direct memio,
fast OAM DMA, fast JMP, IRQ-mapper batching, Auto frameskip + anti-flap, DTCM RAM, CHR sprite
fix). Everything else came back flat or rejected for one well-understood reason:

- **CPU-side fast-memory relocation** (cpu-struct DTCM, hot core, PRG-page mirror): all flat —
  the hot working set is already cached; the cold D-cache misses are diffuse across the 32 KB
  PRG ROM and don't fit the ~6 KB DTCM pool. The interpreter already fits the 16 KB I-cache,
  so there's no eviction to relieve (unlike the vecx 6809 case the technique came from).
- **Dispatch/layout** (hot-cluster, jumptable, loop-align, batch 24/32, spinhack, fastpc,
  linear-rom, fixedcycles): flat.
- **Accuracy trades** (cycle-trim / Turbo): glitches without meaningful speed.
- **Audio**: `apu_process` (~730 samples/frame x 5-channel DSP) is in `sound_fill_buffer`
  ("other", ~2 ms), not the cpu_only floor — not a useful lever.

The remaining gap on heavy MMC3 games (Kirby/Batman busy windows ~33-45 fps, cpu_only
~20-24 ms) is the 6502 interpreter doing genuine work, bottlenecked by D-cache misses on a
32 KB program that cannot fit fast memory. The only lever that structurally breaks this is a
**dynamic recompiler** (6502->ARM JIT) — a multi-week rewrite, not a probe. Clean micro-opts
that remain (e.g. `ppu_renderbg` register-pressure tidy) are sub-1 ms and imperceptible.
Recommendation: the current promoted build is the release.

### Background tile CHR cache — only if DTCM becomes available

The 4 KB `bg_tile_cache[256][8]` approach was tried and **caused regression** (see failures
above). It would only work without D-cache pollution if placed in DTCM.

A smaller variant caching only **visible tiles** (~30–40 unique tile indices = ~640 bytes)
would reduce D-cache impact significantly. Not yet tried; expected savings small if CHR
data is already hot from per-frame use.

### FRAME_SKIP arithmetic (historical reference)

This section uses the old compile-time render interval, not the current user-facing menu
value. Old `FRAME_SKIP=2` maps to current `Frameskip=1`; old `FRAME_SKIP=3` maps to
current `Frameskip=2`. It was useful before the `lcd_dirty=draw` fix and now overestimates
the present display cost because skipped visual frames no longer force a Playdate LCD
update.

With cpu_only = 19 ms and ppu_full = 28 ms, average frame time by skip level:

| FRAME_SKIP | avg ms | NES fps | display fps |
|---:|---:|---:|---:|
| 2 | 23.5 | ~42 | ~21 |
| 3 | 22.0 | ~45 | ~15 |
| 4 | 21.25 | ~47 | ~12 |
| 6 | 20.5 | ~49 | ~8 |
| 8 | 20.1 | ~50 | ~6 |

The old conclusion was that 50 fps required unacceptable visual skipping. The display-update
skip fix invalidated that conclusion: old `FRAME_SKIP=2` / current `Frameskip=1` is now
viable in light windows, and the remaining slowdowns are CPU-side.

---

## Build commands

```sh
make                 # promoted fast build (DIAG=OFF by default)
make perf            # alias for the promoted fast build
make install         # build + push to connected device
make diag-nobg       # diagnostic build, bg rendering disabled
make diag-nosprites  # diagnostic build, sprite rendering disabled
make diag-noblit     # diagnostic build, Playdate scanline blit disabled
make diag-noaudio    # diagnostic build, audio disabled
make diag-opprofile  # diagnostic build, opcode counter enabled
make diag-fastpc     # diagnostic build, fast PC operand/branch path enabled
make diag-faststrike # diagnostic build, batch-32 with immediate sprite-zero hit
make diag-fastmem    # diagnostic build, RAM mirror fast path enabled
make diag-directmem  # diagnostic build, direct common memory-I/O fast path enabled
make diag-fastjmp    # diagnostic build, fast absolute JMP operand fetch enabled
make diag-fastbranch # diagnostic build, fast taken relative branches enabled
make diag-fastopbyte # diagnostic build, fast one-byte operand fetch enabled
make diag-fastmemops # diagnostic build, hot memory load/store opcodes specialized
make diag-fastbatch  # diagnostic build, directmem/fastjmp with FASTBATCH scanline batches
make diag-skipcache  # diagnostic build, skip sprite render-cache rebuilds on skipped frames
make diag-fastoamdma # diagnostic build, direct CPU-page copy for OAM DMA
make diag-fpslite   # diagnostic build, FPS-only logs without render timing hooks
make diag-cycletrim # diagnostic build, conservative 96% CPU-cycle budget per scanline
make diag-jumptable # diagnostic build, computed-goto CPU opcode dispatch
make diag-lazycycles # diagnostic build, lazy total-cycle accounting
make diag-fastbne   # diagnostic build, fast-path only hot BNE branches
make diag-fastbpl   # diagnostic build, fast-path only hot BPL branch on lazy/BNE baseline
make diag-fastbeq   # diagnostic build, fast-path only hot BEQ branch on lazy/BNE/BPL baseline
make diag-fixedcycles # diagnostic build, fixed-point scanline cycle accumulator
make diag-jmpspin    # diagnostic build, self-JMP idle-loop fast-forward enabled
make diag-linearrom  # diagnostic build, contiguous PRG-ROM fast path enabled
make diag-tcmhot     # diagnostic build, tiny DTCM relocated-code proof enabled
make diag-tcmcore    # diagnostic build, tiny DTCM hot-opcode CPU core enabled
make diag-tcmstats   # diagnostic build, DTCM hot-core attribution counters enabled
make install-diag-nobg       # build + push as FamiCrank.pdx
make install-diag-nosprites  # build + push as FamiCrank.pdx
make install-diag-noblit     # build + push as FamiCrank.pdx
make install-diag-noaudio    # build + push as FamiCrank.pdx
make install-diag-opprofile  # build + push as FamiCrank.pdx
make install-diag-fastpc     # build + push as FamiCrank.pdx
make install-diag-faststrike # build + push as FamiCrank.pdx
make install-diag-fastmem    # build + push as FamiCrank.pdx
make install-diag-directmem  # build + push as FamiCrank.pdx
make install-diag-fastjmp    # build + push as FamiCrank.pdx
make install-diag-fastbpl    # build + push as FamiCrank.pdx
make install-diag-fastbeq    # build + push as FamiCrank.pdx
make install-diag-fastbranch # build + push as FamiCrank.pdx
make install-diag-fastopbyte # build + push as FamiCrank.pdx
make install-diag-fastmemops # build + push as FamiCrank.pdx
make install-diag-fastbatch  # build + push as FamiCrank.pdx
make install-diag-skipcache  # build + push as FamiCrank.pdx
make install-diag-fastoamdma # build + push as FamiCrank.pdx
make install-diag-fpslite    # build + push as FamiCrank.pdx
make install-diag-cycletrim  # build + push as FamiCrank.pdx
make install-diag-jumptable  # build + push as FamiCrank.pdx
make install-diag-lazycycles # build + push as FamiCrank.pdx
make install-diag-fastbne    # build + push as FamiCrank.pdx
make install-diag-fixedcycles # build + push as FamiCrank.pdx
make install-diag-jmpspin    # build + push as FamiCrank.pdx
make install-diag-linearrom  # build + push as FamiCrank.pdx
make install-diag-tcmhot     # build + push as FamiCrank.pdx
make install-diag-tcmcore    # build + push as FamiCrank.pdx
make install-diag-tcmstats   # build + push as FamiCrank.pdx
PORT=/dev/cu.XXX make install   # override auto-detected serial port
```
