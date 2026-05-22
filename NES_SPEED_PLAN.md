# Native NES Speed Plan for Playdate

Goal: get the Playdate port to full-speed gameplay first. The immediate target is
50 fps PAL-like NES speed; frame skipping, visual artifacts, and emulation
inaccuracies are acceptable while establishing that speed. This document orders
the work by a mix of feasibility, expected performance boost, and risk.

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
device with the same ROM, same `FRAME_SKIP`, same audio setting, and a clean
production/diagnostic distinction. A 1 ms measurement error is large on this
hardware, so compare averages over many frames.

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

During performance work, the current Makefile intentionally keeps `DIAG=ON` so
device logs are available. Treat those numbers as diagnostic measurements and
keep every row labeled with its active flags. Before release claims or
production comparisons, build an explicit `DIAG=OFF` package too.

Do this first:

- Keep diagnostic banners self-identifying.
- Keep production and diagnostic build targets explicit.
- After changing diagnostic flags, run a clean configure or force the option.
- Log matrix rows in `PERF.md` with their audio, background, and sprite settings.

Measure:

- Production `FRAME_SKIP=1`, `2`, and `3`.
- Diagnostic `FRAME_SKIP=1`, `2`, and `3`.
- Confirm `drawFPS` is never present in production.

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

- `FRAME_SKIP=1`, `AUDIO=ON`, normal rendering.
- `FRAME_SKIP=1`, `AUDIO=OFF`, normal rendering.
- `FRAME_SKIP=1`, sprites off.
- `FRAME_SKIP=1`, background off.
- `FRAME_SKIP=999`, draw never true, if that path still runs correctly.
- Same matrix on at least one NROM ROM and one scrolling/action ROM.

Measure:

- Average frame time.
- Average `nes6502_execute` time if you can isolate it.
- Average PPU scanline/render time.
- Separate draw frames from skipped frames.

Output should be a table in `PERF.md`. This tells you which branch of the plan
is actually worth the next week.

Current repo status on 2026-05-22: the audio-off, background-off,
sprites-off, and blit-off diagnostic builds have matching install targets, and
their log banner reports the active flags. The normal diagnostic build also has
`Draw BG` and `Draw Sprites` system-menu checkmarks so a scene can be reached
before a visual path is cut; the runtime marker belongs in the captured log
window. Keep the compile-time variants for clean matrix rows. The first
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
worse in the Mario device row. The current speed-first CPU experiment batches
scanline CPU execution for simple no-hblank mappers to test whether interpreter
re-entry and CPU/PPU cache churn are a larger lever than individual access paths.

## 3. I-Cache: Shrink and Isolate Hot Interpreter Code

Feasibility: medium-high
Expected boost: medium-high, likely 2-10 ms depending on device revision
Risk: medium

This should happen before large CPU rewrites. The forum thread's strongest
lesson is that Playdate performance can be dominated by whether the hot loop
fits in instruction cache. The current build already improved this by compiling
`nes6502.c` with `-Os -DNES6502_SWITCH`, shrinking `nes6502_execute` to about
13.5 KB. That is useful on Rev B, but still far above the Rev A 4 KB cache.

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
- Try `-falign-loops=32` only in a measured branch; keep it if it helps.

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

- `FRAME_SKIP=999` or equivalent no-draw path.
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

- Full render `FRAME_SKIP=1`.
- Draw-frame-only cost with `FRAME_SKIP=2` or `3`.
- CHR ROM game and CHR RAM game separately.

This does not raise the no-draw ceiling, so it comes after CPU work.

## 9. PPU Sprite Row Cache and Sprite Draw Rewrite

Feasibility: medium
Expected boost: low-medium, likely 1-3 ms in sprite-heavy scenes
Risk: medium

The OAM miss-path optimization already did not move the needle. The expensive
part is drawing visible sprite rows in `draw_oamtile()`.

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

1. Fix `DIAG=OFF` production builds.
2. Add the profiling matrix and commit fresh baseline numbers.
3. Save `nm` symbol maps and confirm hot-code addresses/sizes.
4. Shrink and isolate the hot interpreter path.
5. Prototype PC host-pointer opcode fetch.
6. Specialize CPU memory access by address class.
7. Add a fast compatibility build profile.
8. Try DTCM data placement with canaries.
9. Try ITCM relocation only after a tiny proof works.
10. Add PPU background row caching.
11. Add sprite row caching and sprite fast paths.
12. Audit tight-loop arithmetic and prefetch candidates.
13. Consider a monochrome-native PPU path only after CPU work lands.

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
- `AUDIO`, `DIAG`, and `FRAME_SKIP` settings.
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
