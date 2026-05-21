# Native NES Speed Plan for Playdate

Goal: get the Playdate port close to native NES speed, ideally 60 Hz NTSC game
logic with acceptable audio and video. This document orders the work by a mix of
feasibility, expected performance boost, and risk.

## Current Baseline

Native NTSC timing means one emulated NES frame every 16.67 ms. Current profiling
in `PERF.md` puts this port roughly here:

| Mode | Approx cost | Approx ceiling |
|---|---:|---:|
| Full frame render | 28-32 ms | 31-36 fps |
| Skipped visual frame | 24 ms | 41 fps |
| PPU pixel drawing portion | 8 ms | N/A |
| Playdate framebuffer blit/dither | <1 ms | N/A |

The important conclusion is that the Playdate display path is no longer the
main bottleneck. Even if pixel rendering is skipped forever, the emulator core
still cannot reach NTSC speed. The first serious target is therefore reducing
the 6502 plus PPU state-machine cost from about 24 ms to below 16.67 ms.

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

## 1. Fix Build Hygiene and Measurement

Feasibility: very high
Expected boost: 0-2 ms, mostly from avoiding accidental diagnostic overhead
Risk: very low

Normal builds should force `DIAG=OFF`. Right now `make diag` can leave
`DIAG=ON` in the CMake cache, and a later `make` can continue building with
`pd->system->drawFPS`, logging, and diagnostic state. That can make production
numbers look worse than they are.

Do this first:

- Change the Makefile so production config passes `-DDIAG=OFF`.
- Keep `diag` as the only target that passes `-DDIAG=ON`.
- Add a small note to `PERF.md`: after changing diagnostic flags, run a clean
  configure or force the option.
- Log the final compile defines in the diagnostic banner if practical.

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

## 4. CPU Core: Replace PC Address Fetch With a Host Pointer Fast Path

Feasibility: medium
Expected boost: high, likely 3-8 ms if done well
Risk: medium-high

The current interpreter repeatedly fetches opcodes through:

```c
cpu.mem_page[address >> NES6502_BANKSHIFT][address & NES6502_BANKMASK]
```

That is flexible, but expensive. Most CPU time is spent fetching sequential
opcodes from the same 4 KB bank. A fast interpreter should keep:

- `pc_addr`: the emulated 16-bit PC.
- `pc_ptr`: direct host pointer into the current PRG bank.
- `pc_bank_end`: pointer limit for the current mapped bank.

Then the common opcode fetch becomes:

```c
opcode = *pc_ptr++;
pc_addr++;
```

Only slow paths need to rebase `pc_ptr`:

- Branch taken.
- Jump, JSR, RTS, RTI, BRK, IRQ, NMI.
- Page/bank crossing.
- Mapper bank switch.
- Any path that writes to PRG mapping.

Why this is still a top CPU optimization:

- It attacks the 24 ms no-draw ceiling.
- It reduces memory indirection and D-cache pressure in the hottest loop.
- It can be done while preserving Nofrendo's basic interpreter structure.

Implementation strategy:

- Prototype in a branch, not mixed with PPU changes.
- Add a helper to rebase `pc_ptr` from `pc_addr`.
- Keep the existing `bank_readbyte()` path for data reads at first.
- Convert only opcode and immediate operand fetches initially.
- Run a known-good ROM and compare input responsiveness, frame count, and basic
  game behavior before broadening compatibility.

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
