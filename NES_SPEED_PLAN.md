# Native NES Speed Plan for FamiCrank on Playdate

Goal: get the Playdate port to full-speed gameplay first. The immediate target is
50 fps PAL-like NES speed; frame skipping, visual artifacts, and emulation
inaccuracies are acceptable while establishing that speed. This document orders
the work by a mix of feasibility, expected performance boost, and risk.

Status as of 2026-06-06: the main FamiCrank Playdate-port roadmap is complete and the
default Makefile path now uses the promoted fast profile. This file remains useful as the
historical speed plan and performance backlog, not as an active product roadmap.

## Current Baseline

The 50 fps target means one emulated NES frame every 20 ms. Current profiling in
`PERF.md` puts this port roughly here:

| Mode | Approx cost | Approx ceiling |
|---|---:|---:|
| Idle full render frame | 28 ms | 35 fps |
| Idle skipped visual frame | 20 ms | 50 fps boundary |
| Enemy-heavy full render frame | 32-38 ms | 26-31 fps |
| Enemy-heavy skipped visual frame | 23-27 ms | 37-43 fps |
| PPU pixel drawing portion | about 8 ms | N/A |
| `sound_fill_buffer` | about 2 ms | outside render metric |

The important conclusion is narrower now. Skipping pixel output can put the idle
scene on the 50 fps boundary, but action scenes still miss it before a full
visual frame is drawn. The first pass should therefore measure the easy
subsystem cuts and then attack the CPU-side enemy-scene cost and the remaining
about 8 ms of PPU pixel work.

## Guiding Rule

Do not optimize from intuition anymore. Every item below should be measured on
device with the same ROM, same runtime `Frameskip` value, same audio setting, same
`Show FPS` setting, and a clean production/diagnostic distinction. A 1 ms measurement
error is large on this hardware, so compare averages over many frames.

## Playdate-Specific Findings

The Playdate developer forum thread "Dirty Optimization Secrets (C for
Playdate)" is directly relevant to this port. The main takeaways to apply here:

- Treat the CPU as fast and uncached memory as expensive. Register-heavy code can
  be fine even when it has branches; pointer-heavy code can be unexpectedly slow.
- The instruction cache is the core constraint: Rev A is around 4 KB, Rev B
  around 16 KB. The current `nes6502_execute` is about 13.5 KB, which is a Rev B
  strategy, not a Rev A strategy.
- Smaller code can beat more aggressively optimized code. Keep testing `-Os`,
  `-O2`, and `-O3` per hot file or even per hot function.
- Linker placement matters. Use explicit sections, `nm`, and 32-byte alignment
  to keep hot code contiguous and cache-line friendly.
- Build-to-build performance can change because of address alignment and branch
  predictor effects. Preserve symbol maps from fast builds and treat layout as
  an optimization variable.
- DTCM and ITCM can be useful, but they are crash-prone experiments. Start with
  tiny proofs, use canaries for stack-based DTCM pools, and flush I-cache after
  copying executable code.

## 1. Keep Build Hygiene and Measurement Honest

Feasibility: very high
Expected boost: 0-2 ms, mostly from avoiding accidental diagnostic overhead
Risk: very low

During performance work, plain `make` is the clean `DIAG=OFF` package and explicit
diagnostic targets force `DIAG=ON` when device logs are needed. Treat diagnostic numbers as
diagnostic measurements and keep every row labeled with its active flags. Compare release
claims against the default `DIAG=OFF` build.

Do this first:

- Keep diagnostic banners self-identifying.
- Keep production and diagnostic build targets explicit.
- After changing diagnostic flags, run a clean configure or force the option.
- Log matrix rows in `PERF.md` with their audio, background, and sprite settings.

Measure:

- Production runtime `Frameskip=0`, `1`, and `2`.
- Diagnostic runtime `Frameskip=0`, `1`, and `2`.
- `Show FPS` off for clean perf rows; on only when checking the native Playdate FPS HUD.

This does not solve native speed, but it prevents chasing fake regressions.

## 2. Build a Repeatable Profiling Matrix

Feasibility: very high
Expected boost: 0 ms directly, high leverage
Risk: low

The next work needs better attribution than "render is slow." Add toggles that
can be compiled or selected at runtime:

| Toggle | Question answered |
|---|---|
| `AUDIO=OFF` | How much does APU generation and APU register traffic cost? |
| `PPU_BG=OFF` | What is background tile rendering cost? |
| `PPU_SPRITES=OFF` | What is sprite rendering cost? |
| `PPU_BLIT=OFF` | Verify Playdate framebuffer conversion stays negligible. |
| `LEGAL_OPS_ONLY` | Estimate cost of supporting undocumented 6502 opcodes. |
| `MAPPER_SET=minimal` | Estimate cost of compiling/linking only needed mappers. |

Recommended test rows:

- `Frameskip=0`, `AUDIO=ON`, normal rendering.
- `Frameskip=0`, `AUDIO=OFF`, normal rendering.
- `Frameskip=0`, sprites off.
- `Frameskip=0`, background off.
- Forced no-draw path, if that path still runs correctly.
- Same matrix on at least one NROM ROM and one scrolling/action ROM.

Measure:

- Average frame time.
- Average `nes6502_execute` time if you can isolate it.
- Average PPU scanline/render time.
- Separate draw frames from skipped frames.

Output should be a table in `PERF.md`. This tells you which branch of the plan
is actually worth the next week.

Current repo status update: the audio-off, background-off, sprites-off, and blit-off
diagnostic builds have matching install targets, and their log banner reports the active
flags. The normal runtime menu no longer exposes `Draw BG` or `Draw Sprites`; those
checkmarks were removed because they polluted real gameplay testing. Keep the compile-time
variants for clean matrix rows. The first
audio-off Mario 1-1 traversal reached 49-50 fps in light windows but still fell
to 28-39 fps in busy windows, so audio off alone does not reach the speed target.
The no-background boot row drops the steady audio-on PPU delta to about 4 ms;
compared with the earlier roughly 8 ms normal idle delta, that points to
background rendering as a material cost. The BG-disabled diagnostic draw path
now fills blank scanlines instead of leaving stale scanline noise, but that needs
device verification. The no-sprites Mario 1-1 standing row is effectively
unchanged from the normal idle row at millisecond log resolution, and its moving
row still misses the budget. The no-blit Mario 1-1 row is also effectively
unchanged: standing still still reports 36-37 fps and moving still spends
21-22 ms on skipped visual frames. That makes visible sprite pixel drawing and
Playdate scanline conversion low priorities for the first 50 fps push.
Background rendering is the PPU cost with a measured delta so far, but the next
immediate work should improve the busy-scene CPU/emulation path because it
already misses the 20 ms budget before full pixel drawing. The first follow-up
CPU experiment extended the existing `pc_ptr` fast path to byte operands, but the
same-scene device row was flat and that change was dropped. Removing skipped-frame
sprite render-cache rebuild work regressed the next device row too, so that change
was also dropped. Fast-pathing fixed RAM mirrors and the standard PPU/APU/input
register windows before the generic memory-handler scan was flat to slightly
worse in the Mario device row. Batching scanline CPU execution for simple
no-hblank mappers is the first clear follow-up win: the first eight-scanline row
drops Mario idle `cpu_only` from about 20 ms to 15 ms and full draw frames from
about 28 ms to 23-24 ms. The sixteen-scanline full-level Mario row feels better
again and reaches 46-50 fps in its fastest windows, but object-heavy windows
still raise skipped visual frames to 20-24 ms. The next measurement tries a
32-scanline batch before changing another subsystem. The first 32-scanline
package was an invalid fast-looking row because pending visible-line CPU work
ran after vblank was raised and before NMI delivery; retest it only with that
vblank-boundary flush ordering fixed. The fixed retest still gives Mario
slow-motion gameplay: this core's sprite-zero/status timing cannot tolerate the
PPU running that far ahead of the 6502. Treat batch 16 as the Mario ceiling for
now and move to the busy-scene CPU/cache path. The first low-risk probe there
keeps `cpu_batch=16` and aligns the iNES PRG start on a 16 KB boundary to test
whether ROM buffer placement is contributing to the enemy-scene slowdown. The
full-level row is flat against the non-aligned batch 16 row, so ROM base
alignment is not the next busy-scene lever.

The next batch experiment targeted sprite-zero strike timing: batched rendering
lets the PPU get ahead of CPU execution, so the first batch 32 slow-motion row
could have been missing pending scanline cycles in its strike timestamp. The
pending-cycle bias retest regressed feel and the busy half-level row, so that
change was removed. Close the wide visible-frame batch branch for now and
continue on batch 16.

## 3. I-Cache: Shrink and Isolate Hot Interpreter Code

Feasibility: medium-high
Expected boost: medium-high, likely 2-10 ms depending on device revision
Risk: medium

This should happen before large CPU rewrites. The forum thread's strongest
lesson is that Playdate performance can be dominated by whether the hot loop
fits in instruction cache. The current build already improved this with switch
dispatch, legal-opcode stubs, and a direct PC pointer. The full-level Mario row
improves again when `nes6502.c` moves from `-O2` to `-O3`, despite
`nes6502_execute` growing from about 11.6 KB to about 13.1 KB. Keep `-O3` as
the Rev B baseline now, but still compare it with layout rows rather than
assuming denser code wins. Rev B benefits while the hot path stays near the
16 KB I-cache; Rev A still has only a 4 KB cache.

The next target is not necessarily "make the whole interpreter 4 KB." The target
is to make the common interpreter path much smaller and push rare paths out of
line.

Do this:

- Use `arm-none-eabi-nm -S --size-sort Source/pdex.elf` and keep symbol maps for
  every measured build.
- Mark hot functions with explicit sections such as
  `__attribute__((section(".text.cpu_hot")))`.
- In `link_map.ld`, place hot CPU sections together, then align to 32 bytes
  before the next section.
- Split rare opcodes, undocumented opcodes, decimal-mode code, disassembly, JAM,
  and edge cases into cold functions or a cold compatibility build.
- Try a legal-opcode-only fast profile for tested ROMs.
- Try `-Os`, `-O2`, and `-O3` on the CPU core after each structural change.
- Keep the measured `-falign-loops=32` row off for now. On the current
  `cpu_batch=16`/`-O3` baseline it grew `nes6502_execute` from about 13.1 KB to
  about 13.5 KB and was flat to slightly worse in Mario 1-1 busy windows.

Performance-lottery controls:

- Preserve `nm` output from the fastest build.
- If a harmless edit causes a large speed change, compare hot symbol addresses
  modulo 32 and modulo 1024.
- Consider linker-script padding experiments such as `. = ALIGN(32);` before
  hot sections. Use larger 1024-byte alignment or offsets sparingly because it
  wastes code space.

Measure:

- Function size of `nes6502_execute` and any new hot/cold helpers.
- No-draw frame cost on the same ROM.
- Full render after no-draw improves.
- Rev A and Rev B separately if possible.

This is now the first hard optimization because it can improve speed without
changing emulator semantics.

## 4. CPU Core: Finish the Host Pointer PC Fetch Path

Feasibility: medium
Expected boost: medium, likely 1-4 ms after the opcode-fetch win
Risk: medium-high

The current interpreter already uses a host `pc_ptr` for opcode dispatch. That
removed one bank-table lookup from the hottest sequential fetch path and the
measured `NES6502_PC_PTR` build saved about 2 ms on `cpu_only`.

The addressing macros still fetch many byte and word operands through:

```c
cpu.mem_page[address >> NES6502_BANKSHIFT][address & NES6502_BANKMASK]
```

That is flexible, but expensive for sequential instruction operands that are
usually beside the opcode in the same 4 KB bank. Keep using:

- `pc_addr`: the emulated 16-bit PC.
- `pc_ptr`: direct host pointer into the current PRG bank.
- `pc_bank_end`: pointer limit for the current mapped bank.

Then a common byte operand fetch can become:

```c
if (pc_ptr == pc_bank_end)
    rebase_pc_ptr();
operand = *pc_ptr++;
pc_addr++;
```

Only slow paths need to rebase `pc_ptr`:

- Branch taken.
- Jump, JSR, RTS, RTI, BRK, IRQ, NMI.
- Page/bank crossing.
- Mapper bank switch.
- Any path that writes to PRG mapping.

Why this is still a top CPU optimization:

- It attacks the 21-24 ms busy no-draw ceiling after the opcode fetch win.
- It reduces memory indirection and D-cache pressure in the hottest loop.
- It can be done while preserving Nofrendo's basic interpreter structure.

Implementation strategy:

- Keep the existing `bank_readbyte()` path for data reads at first.
- Convert byte operands first; they cover immediates, branch offsets, and the
  one-byte addresses used by zero-page addressing modes.
- Measure code size as well as frame time. A wider direct-fetch macro is a loss
  if it pushes the interpreter out of the useful instruction-cache footprint.
- Consider word operands next only if the byte form is a real device win and
  the rare bank-boundary case stays correct.

Measure:

- Forced no-draw path.
- Full render path after the CPU gain is confirmed.

This is probably the most important optimization in the whole project.

## 5. CPU Core: Specialize the Common Memory Map

Feasibility: medium
Expected boost: medium-high, likely 2-5 ms
Risk: medium

`mem_readbyte()` and `mem_writebyte()` currently scan handler ranges for many
addresses. This is general but costly. Most common NES accesses are predictable:

- `$0000-$07FF`: internal RAM.
- `$0800-$1FFF`: mirrored RAM.
- `$2000-$3FFF`: mirrored PPU registers.
- `$4000-$4017`: APU/input/OAM DMA.
- `$6000-$7FFF`: SRAM or mapper area.
- `$8000-$FFFF`: PRG ROM.

Do this:

- Replace handler scanning on the hot path with a direct address class switch.
- Keep mapper callbacks for mapper-owned ranges.
- Special-case RAM mirrors with `address & 0x7FF`.
- Special-case PPU register mirrors with `0x2000 + (address & 7)`.
- For simple mappers, directly write to mapper handlers instead of scanning
  generic arrays.

Measure:

- No-draw frame cost.
- A ROM with lots of RAM/PPU polling.
- A ROM with mapper writes, if supported.

This should be done after or alongside the PC pointer fast path, because both
touch the CPU memory access model.

Related measured experiment: add a diagnostic-only fast-forward for exact
PPUSTATUS vblank wait loops, specifically `BIT $2002` or `LDA $2002` followed
by `BPL` back to the read. This is less general than the address-class rewrite,
but cheaper to test. Device results say to keep it off: it did not reduce the
busy-scene `cpu_only` peaks, and the larger `nes6502_execute` body likely added
I-cache pressure.

## 6. Compile a Fast Compatibility Profile

Feasibility: high
Expected boost: low-medium, likely 1-4 ms depending on ROM/build
Risk: medium, mostly compatibility

The current build includes many mappers, expansion audio, and undocumented CPU
opcodes. For a Playdate product, it is reasonable to have a fast profile that
targets common games and a compatibility profile that keeps broader support.

Fast profile ideas:

- Build only the mappers needed by bundled/tested ROMs at first.
- Disable expansion audio unless a loaded mapper requires it.
- Consider a `LEGAL_OPS_ONLY` CPU build for known-good games.
- Keep NROM, MMC1, UxROM, CNROM, and MMC3 as the practical first mapper set.

Expected benefit is not only smaller binary size. Smaller hot and warm code
improves I-cache behavior on Rev B and is especially important on Rev A.

Measure:

- Function sizes from `arm-none-eabi-nm -S --size-sort`.
- Full frame time on Rev B if possible.
- No-draw frame time.

Do not remove support permanently until the performance difference is measured.

## 7. TCM Experiments for Hot Code and Data

Feasibility: medium-low
Expected boost: medium-high, potentially 2-8 ms
Risk: high

The Playdate's Cortex-M7 has tiny caches. The current hot working set is larger
than D-cache:

- PRG ROM instruction/data fetches.
- CHR ROM pattern table reads.
- PPU nametable and OAM.
- `ppu_t`.
- CPU context and NES RAM.

Experiments to try:

- Before TCM placement, vary PRG ROM buffer alignment on the timing-safe
  `cpu_batch=16` path and keep it only if the busy Mario row improves.
- Use stack-local copies for hot structs during long operations, then copy back.
- Create a small persistent DTCM pool near the low end of the stack, discovered
  from `__builtin_frame_address(0)` during `kEventInit`.
- Protect that DTCM pool with canaries and check them every update.
- Put only a measured working set there at first: CPU context, NES RAM, the
  active PPU fields, OAM, or scanline scratch buffers.
- Keep the current linker-script hot-code placement as the baseline before
  testing ITCM.

DTCM caution:

- Do not assume a universal safe size. Start with a tiny pool, then grow it.
- The current `ppu_t` is about 5 KB; CPU RAM is 2 KB; the audio ring is 8 KB.
  These sizes matter.
- The framebuffer may also be fast memory, but using visible framebuffer memory
  as hidden scratch is risky and should not be part of the main plan.

ITCM experiment:

- Define a marker such as
  `#define ITCM_FN __attribute__((section(".itcm"))) __attribute__((short_call))`.
- Add `__itcm_start` and `__itcm_end` around `*(.itcm)` in the linker script.
- Copy that section to a TCM-backed buffer at startup.
- Call the relocated copy through a function pointer, not the original symbol.
- Preserve Thumb address parity when computing function pointers.
- Mark any direct callees outside `.itcm` with `__attribute__((long_call))`.
- Flush the instruction cache after copying code.
- Start with a trivial one-function proof before moving any emulator function.

Order of experiments:

1. Stack-local copy of one hot struct or scratch buffer.
2. Persistent DTCM pool with canaries.
3. Tiny ITCM proof function.
4. ITCM copy of a small hot helper.
5. ITCM copy of the reduced CPU hot loop, only after section 3 shrinks it.

Risks:

- Wrong memory map assumptions can crash on device.
- Function-pointer relocation can break if calls are not routed consistently.
- TCM size is small; moving too much can make things worse.

Measure:

- No-draw frame cost first.
- Full render second.
- Rev A and Rev B separately if both devices are available.

This is a strong candidate, but it is less feasible than CPU-structure changes
because the Playdate loader/runtime details matter.

## 8. PPU Background Tile Row Cache

Feasibility: medium
Expected boost: medium, likely 2-5 ms on draw frames
Risk: medium

`ppu_renderbg()` decodes 33 tiles per scanline. That is roughly 792 background
tile-row decodes per frame. Many games reuse the same CHR rows for many frames.

Add a cache keyed by:

- Pattern table address.
- Low pattern byte.
- High pattern byte.
- Palette high bits.
- Horizontal fine scroll case if needed.

A cached entry should store either:

- The 8 palette-index pixels expected by the current renderer, or
- A Playdate-ready packed/dither-friendly representation if you later bypass
  the 8-bit intermediate scanline.

Invalidation:

- CHR ROM games: almost no invalidation except bank switches.
- CHR RAM games: invalidate on pattern memory writes or use generation counters.
- Mapper latch games: keep the old path until proven safe.

Measure:

- Full render with runtime `Frameskip=0`.
- Draw-frame-only cost with runtime `Frameskip=1` or `2`.
- CHR ROM game and CHR RAM game separately.

This does not raise the no-draw ceiling, so it comes after CPU work.

## 9. PPU Sprite Row Cache and Sprite Draw Rewrite

Feasibility: medium
Expected boost: low-medium, likely 1-3 ms in sprite-heavy scenes
Risk: medium

The OAM miss-path optimization already did not move the needle. The expensive
part is drawing visible sprite rows in `draw_oamtile()`.

Measured miss before the larger rewrite: keeping the scanline OAM list and
max-sprite counts exact while only predecoding CHR pattern rows for sprites that
enter the first eight render slots did not help. The user reported worse feel
around block/mushroom interactions, and the busy rows still reached
`cpu_only=23 ms`. Do not retry that exact cache gate.

Do this:

- Cache decoded sprite pattern rows for normal and h-flipped variants.
- Split priority cases into separate tight functions instead of branching per
  pixel.
- Fast path common case: sprite in front, no sprite-zero check.
- Run sprite-zero check only for sprite 0 and only until the strike is found.

Measure:

- Sprite-heavy scrolling/action scene.
- `PPU_SPRITES=OFF` delta before and after.

This is worthwhile, but it should not distract from the CPU ceiling.

## 10. Bypass the 8-Bit Intermediate Scanline Eventually

Feasibility: low-medium
Expected boost: low-medium, likely 1-4 ms on draw frames
Risk: high

The current renderer draws an 8-bit NES scanline, then `display.c` converts it
to Playdate 1-bit. The blit is already very fast, so bypassing this buffer is
not the first priority. Later, however, a monochrome-native PPU path could:

- Decode background tile rows directly into 1-bit Playdate bytes.
- Apply ordered dither from palette indices directly.
- Composite sprites into a small per-scanline priority/opacity buffer.

This is a larger PPU rewrite. It may only pay off after CPU cost is reduced
enough that draw frames become the remaining bottleneck.

## 11. Audio Cost Control

Feasibility: high
Expected boost: unknown until measured, likely 0.5-3 ms
Risk: medium

Audio may not dominate, but under-running audio can make the emulator feel worse
even when FPS improves.

Do this:

- Measure `AUDIO=OFF` vs `AUDIO=ON`.
- Generate audio in larger chunks if callback/ring overhead is meaningful.
- Avoid expansion audio unless required by the loaded mapper.
- Consider a lower-cost APU quality mode if it produces a measurable gain.

Keep audio synchronized to emulated time, not wall-clock time, if the emulator
starts catching up to native speed. The current wall-clock fill behavior is good
for a slow emulator, but native-speed emulation wants stable emulated timing.

## 12. Tight-Loop Arithmetic and Prefetch Audit

Feasibility: high
Expected boost: low, possibly 0.5-2 ms if a bad loop is found
Risk: low

The forum thread mentions two smaller tactics:

- Integer division can be surprisingly expensive in tight loops, or can create
  register-pressure side effects. Audit hot loops for `/`, `%`, and repeated
  address division.
- Prefetching has mixed reports. Treat it as a narrow experiment for pointer
  chasing, not a general solution.

Do this:

- Search hot files for division and modulo.
- Replace divisions by shifts, masks, increments, or lookup tables where natural.
- Do not replace simple integer work with float unless a measured loop benefits.
- Try `__builtin_prefetch()` only where the next pointer is known early, such as
  CHR row reads or mapper/page pointer walks.

Measure this only after bigger CPU changes; otherwise it will be lost in noise.

## Recommended Execution Order

1. Keep `DIAG=OFF` production builds verified.
2. Add the profiling matrix and commit fresh baseline numbers.
3. Save `nm` symbol maps and confirm hot-code addresses/sizes.
4. Shrink and isolate the hot interpreter path.
5. Prototype PC host-pointer opcode fetch.
6. Specialize CPU memory access by address class.
7. Test exact PPUSTATUS wait-loop fast-forward as a diagnostic row.
8. Add a fast compatibility build profile.
9. Try DTCM data placement with canaries.
10. Try ITCM relocation only after a tiny proof works.
11. Add PPU background row caching.
12. Add sprite row caching and sprite fast paths.
13. Audit tight-loop arithmetic and prefetch candidates.
14. Consider a monochrome-native PPU path only after CPU work lands.

## Expected Path to 60 Hz

A plausible route looks like this:

| Step | Frame cost target |
|---|---:|
| Current no-draw | 24 ms |
| Hot-core shrink and layout control | 20-22 ms |
| CPU PC pointer + memory specialization | 15-19 ms |
| Fast profile + DTCM/ITCM wins | 13-17 ms |
| PPU row caches on draw frames | Full frames near 16-20 ms |
| Selective frame skip | Stable perceived native game speed |

The honest expectation is that "all games, full accuracy, full audio, full
60 rendered frames" is unlikely with this codebase on this hardware. A strong
Playdate emulator target is more realistic:

- Native or near-native game logic for a tested game set.
- 30 or 60 visual updates depending on scene cost.
- Fast mapper/profile defaults.
- Compatibility mode for slower but broader support.

## Do Not Spend Much Time On

- More Playdate framebuffer conversion work: it is already below measurement
  resolution in the current profiler.
- More frame skip as the main strategy: skipped frames still cost about 24 ms.
- OAM miss-path optimization: already tested and did not matter.
- Global `-Ofast`, `-Os`, or alignment changes without per-file measurement.
- ITCM relocation of a 13 KB interpreter before proving a tiny function works.
- Large accuracy rewrites before establishing the fast profile target.

## Definition of Done

For each optimization, update `PERF.md` with:

- Git commit or patch name.
- ROM used.
- Device revision if known.
- `AUDIO`, `DIAG`, runtime `Frameskip`, and `Show FPS` settings.
- `nm` output or at least hot symbol addresses and sizes.
- Full-frame average.
- Skipped-frame average.
- Notes on visible or audio regressions.

The project reaches the main performance milestone when a production build can
run a representative NROM or MMC3 action game with native-speed game logic,
stable input response, acceptable audio, and no sustained frame-time average
above 16.67 ms for the emulation core.

## References

- Playdate Developer Forum: <https://devforum.play.date/t/dirty-optimization-secrets-c-for-playdate/23011>
