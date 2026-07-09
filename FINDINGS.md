# FamiCrank 0.2 — Performance Findings & Wrap-Up

A NES emulator (Nofrendo core) for the Playdate. This note summarizes the
optimization campaign that produced the 0.2 release: what shipped, why it
works, what didn't, and where the ceiling is. The blow-by-blow log lives in
`PERF.md`; this is the executive summary.

## 0.3 — Post-launch fixes & features (2026-07)

Follow-up release addressing user reports from the 0.2 launch. No changes to the
emulation core or its performance; these are compatibility, UX, and feature work.

- **ROM picker cap raised 256 → 1024** (pd-rom-picker submodule). A user with a
  large collection saw only part of it: the picker stopped collecting at 256
  files, and non-`.nes` files in the folder counted toward that cap too. The
  list is heap-allocated and freed before emulation, so this has zero runtime
  cost (only a one-time `qsort` at init; drawing only ever renders 10 rows).

- **On-screen load-failure message.** Failed loads used to drop back to the
  picker silently. The core already reports a precise reason via
  `gui_sendmsg(GUI_RED, …)` (unsupported mapper, truncated/invalid image, out of
  memory) — that was a no-op stub. It now captures the message and `main.c`
  shows it ("Could not load this ROM" + reason, e.g. "Mapper 71 not yet
  implemented", press A to go back). Turns every failed ROM into a self-report.

- **Battery (SRAM) saves.** `rom_savesram`/`rom_loadsram` were `#if 0` stubs
  using desktop `fopen`. Reimplemented over `pd->file` as `osd_load_sram`/
  `osd_save_sram`, writing `<romname>.sav` to `/Shared/Emulation/nes/saves/`.
  Saved on three paths, because returning to the picker never frees the ROM (so
  `rom_free`'s existing save call never fires): return-to-picker, `kEventPause`
  (system menu opens), and `kEventTerminate`. Only `ROM_FLAG_BATTERY` carts get
  a `.sav`. Save states are intentionally **not** supported.

- **Crank Start/Select is now motion-based, not position-based.** *Discovery:* a
  position-based mapping (crank angle inside a zone = button held) can never
  avoid a phantom press, because the angle the crank rests at when undocked is
  unpredictable — whatever zone it lands in fires immediately. Deadzone +
  hysteresis did **not** fix it (the resting angle simply sat inside the Start
  zone, which spanned half the circle). The fix keys off `getCrankChange`
  (motion): turning the crank one way = Start, the other = Select, held while
  turning and released ~4 frames after stopping (a flick reads as a tap). An
  idle or newly-undocked crank sends nothing at any angle. Safe because holding
  Start/Select is essentially never required anywhere in the NES library.

## Hardware revisions: two different Playdate CPUs (measured 2026-07)

All the optimization in this document was done on the **original Playdate, an
STM32F746** (Cortex-M7, 16KB I-cache + 16KB D-cache). Panic later shipped a
newer revision using an **STM32H7** (its boot log reports `target=h7d1`,
`pcbver=0x13`) — a faster clock with larger caches. The *same shipped 0.3 binary*
behaves very differently on the two:

| Mario 1-1 idle (diag-fast build) | F746 (original) | H7 (Rev B) |
|---|---:|---:|
| `cpu_only` | ~20 ms | **~4 ms** |
| `ppu_full` | ~28 ms | **~8 ms** |
| `fps` | ~37 | **50** (locked to PAL target) |
| frame budget used | over budget | **~40%** |

`cpu_only` fell ~5x and `ppu_full` ~3.5x. This is a clean confirmation of the
D-cache theory: the wall was the 32KB PRG ROM missing a 16KB D-cache, and the
H7's larger cache plus higher clock erases it. On the H7 the emulator hits the
50fps PAL target with ~60% of every frame sitting idle (the F746 couldn't reach
50fps even idle).

**Implications:**
- **No Rev B-specific optimization is worth doing** — there is nothing to fix; the
  H7 runs the current binary better than the F746 ever could. The value flipped
  from "can't help" to "doesn't need help."
- The **F746 remains the constraint**, and it is already taken as far as it
  sensibly goes (only a dynarec would move it further, which is not worth it).
- The one latent option is **60fps NTSC on the H7** (it has the headroom), but
  `NES_REFRESH_RATE` is a compile-time 50(PAL)/60(NTSC) switch that also drives
  audio/timing, and itch ships one binary to a userbase that still includes many
  F746 units. So 60fps would need either a separate H7 build or a refactor to a
  runtime-selectable rate with hardware detection. Filed as "interesting, not
  worth it now."

Note: the "Hardware reality" section below predates this and labels the F746 as
"rev B" — that label was loose; treat that section as describing the **F746**.

## Target & result

- **Goal:** 50 fps (PAL NES speed). Starting point: ~30 fps, unshippable.
- **Result:** native game speed everywhere via adaptive frameskip, with the
  visual refresh at:
  - **Mario / most non-IRQ-mapper games:** locked ~50 fps.
  - **Heavy MMC3 games (Kirby, Batman, SMB3):** 45–50 fps in normal play,
    33–40 fps in the busiest scenes.
- Correct rendering verified across five mappers (NROM, MMC1, MMC2, MMC3, +).

## Hardware reality (Playdate rev B, STM32F746 @ 168 MHz)

- 16 KB I-cache, 16 KB D-cache (4-way).
- ITCM (16 KB, fast instruction memory): **MPU write-protected by the OS** —
  unusable for code relocation (confirmed by a hard fault).
- DTCM (64 KB, fast data memory): usable, but the OS/stack leave only a
  **~8 KB contiguous free pool** (0x200074d0–0x20009438, measured per-game).
- **The bottleneck is the D-cache**, not the CPU clock. Our emulator reads each
  NES opcode/operand from the 32 KB PRG ROM as *data*; 32 KB vs 16 KB D-cache
  means constant misses, and busy scenes spread the working set wider.

## What shipped (the wins, in rough order of impact)

1. **`NES6502_LEGAL_ONLY`** — drop ~62 undocumented opcodes; shrinks the
   interpreter to fit the 16 KB I-cache. The single biggest gain (~+8 fps).
2. **Switch dispatch + `-O2`** for the core; `-O3` elsewhere.
3. **Adaptive frameskip ("Auto")** — runs the 6502 at native speed always,
   dropping only *visual* frames under load. With anti-flap hysteresis so the
   refresh doesn't oscillate. This is what makes game speed correct everywhere.
4. **IRQ-mapper CPU batching** — the big win for MMC3 games. Batches CPU across
   IRQ-free scanlines, clamped to the mapper's own IRQ countdown so timing
   stays exact (the status-bar split lands on the right line). +~8 fps on
   Kirby/Batman, zero effect on non-IRQ games.
5. **Interpreter fast paths** — `NES6502_PC_PTR` (direct opcode fetch),
   fast `JMP`/`BNE`/`BPL`/`BEQ`, hot load/store specialization, lazy cycle
   accounting, direct memory I/O, fast OAM DMA.
6. **Sprite pattern cache** + **mid-frame CHR-bank-switch fix** — pre-decode
   sprite CHR; fall back to live reads only when a game (e.g. Batman) swaps CHR
   mid-frame. Fixes a real rendering bug affecting all such games.
7. **NES work RAM in DTCM** — the 2 KB zero-page/stack RAM is zero-wait.
   Marginal but free.
8. Platform: direct audio-ring fill, display-update skip on skipped frames.

## What was tried and rejected — and the one lesson

Roughly 30 experiments. Nearly all the *rejected* ones failed for **one reason**:

> **The hot working set is already cached; the cold misses are diffuse and
> don't fit fast memory.**

- **Code relocation to fast memory** (whole interpreter, a "hot core" of the
  most-used opcodes): flat. Our interpreter already fits the I-cache, so there's
  no eviction to relieve — unlike the GB/vecx emulators this trick comes from,
  whose interpreters overflowed cache.
- **Data relocation to fast memory** (cpu struct, the hottest PRG page): flat.
  The hottest page is so heavily used that the D-cache keeps it resident anyway;
  relocating it buys nothing. The actual misses are spread across the *other*
  pages, which are too many to fit the ~8 KB DTCM pool.
- **Dispatch/layout tweaks** (computed-goto, loop alignment, opcode clustering,
  wider CPU batches): flat or timing-unsafe.
- **Accuracy trades** (running the 6502 below 100% cycle budget — the "Turbo"
  toggle): glitches without meaningful speed, because heavy games spend their
  cycles on real work, not idle loops.

These weren't bad ideas — they're aimed at a wall this specific
hardware/workload puts where none of them can move it.

## The ceiling, honestly

The remaining gap on heavy MMC3 games is the 6502 interpreter doing genuine
work, bottlenecked by D-cache misses on a program that cannot fit fast memory.
The only thing that structurally beats interpreter overhead is a **dynamic
recompiler (6502 → ARM JIT)** — a multi-week rewrite with its own cache risks,
a deliberate project rather than an incremental probe. The clean micro-
optimizations that remain are sub-millisecond and imperceptible.

**0.2 is the release.** It went from "can't ship" to a genuinely good 1-bit
NES experience: native speed on the common games, a real structural gain on the
hard ones, correct rendering, and a clean menu (ROM Picker / Frameskip / Show
FPS). Further speed is a JIT project, scoped separately.

## Build

```sh
make            # production build (DIAG off, full FAST_FLAGS optimizations)
make install    # build + push to a connected Playdate
make diag-fast  # same build + on-device [diag]/[autoskip] console logging
```

The experimental flags from the campaign remain in the source, guarded off and
documented in `PERF.md`, as reference for any future JIT work — they are
compiled out of the shipping binary.
