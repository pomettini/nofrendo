SDK      ?= $(HOME)/Developer/PlaydateSDK
TOOLCHAIN = $(SDK)/C_API/buildsupport/arm.cmake

# BASE_FLAGS pins every optional feature explicitly so a stale cmake cache can
# never leak a setting between builds (a real footgun this project hit). The
# shipping configuration is FAST_FLAGS: the full set of promoted optimizations.
# Every option here is validated on device; see PERF.md for the full history.
BASE_FLAGS = -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF -DENABLE_LTO_NO_IPA_CLONE=OFF -DAUDIO=ON -DAUDIO_DIRECT_RING=ON -DDIAG=OFF -DDIAG_FPS_ONLY=OFF -DPPU_BG=ON -DPPU_SPRITES=ON -DPPU_BLIT=ON -DPPU_BG_PAIR_FAST=OFF -DPPU_FAST_STRIKE=OFF -DPPU_SPRITE_CACHE_DRAW_ONLY=OFF -DPPU_FAST_OAMDMA=OFF -DNES_FIXED_SCANLINE_CYCLES=OFF -DALIGN_PRG_ROM=OFF -DDIAG_CPU_EXEC_TIMING=OFF -DNES_CPU_BATCH_SCANLINES=1 -DNES_CPU_CYCLE_PERCENT=100 -DNES6502_OPT_LEVEL=O3 -DNES6502_JUMPTABLE_DISPATCH=OFF -DNES6502_LAZY_CYCLES=OFF -DNES6502_ALIGN_LOOPS=OFF -DNES6502_SPINHACK=OFF -DNES6502_OPPROFILE=OFF -DNES6502_FAST_PC_OPS=OFF -DNES6502_HOTOPS=OFF -DNES6502_FAST_MEMIO=OFF -DNES6502_DIRECT_MEMIO=OFF -DNES6502_FAST_JMP_ABS=OFF -DNES6502_FAST_BNE=OFF -DNES6502_FAST_BPL=OFF -DNES6502_FAST_BEQ=OFF -DNES6502_FAST_BRANCHES=OFF -DNES6502_FAST_OPERAND_BYTES=OFF -DNES6502_FAST_MEMOPS=OFF -DNES6502_JMP_SPIN=OFF -DNES6502_LINEAR_ROM=OFF -DNES6502_LINKED_CORE=OFF -DNES6502_FUSE_INCDEC_BNE=OFF -DNES6502_PAIRPROFILE=OFF -DNES6502_CHAIN_F0_A5=OFF -DNES6502_ZP_BEQ_SPIN=OFF -DNES6502_TCMHOT_PROBE=OFF -DNES6502_TCMHOT_CORE=OFF -DNES6502_TCMHOT_CORE_STATS=OFF -DNES_RAM_DTCM=OFF -DNES6502_HOT_CLUSTER=OFF -DDTCM_POOL_SCAN=OFF -DPPU_SPRITE_LIVE_CHR=OFF -DNES_IRQ_MAPPER_BATCH=OFF -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=OFF -DNES6502_PRGPROFILE=OFF -DNES_PRG_DTCM=OFF -DPD_PLAYBENCH=OFF -DPD_PLAYBENCH_KIRBY=OFF -DPD_PLAYBENCH_RECORD=OFF -DPD_PLAYBENCH_RECORD_KIRBY=OFF
FAST_FLAGS = $(BASE_FLAGS) -DPPU_FAST_OAMDMA=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_DIRECT_MEMIO=ON -DNES6502_FAST_JMP_ABS=ON -DNES6502_LAZY_CYCLES=ON -DNES6502_FAST_BNE=ON -DNES6502_FAST_BPL=ON -DNES6502_FAST_BEQ=ON -DNES6502_FAST_MEMOPS=ON -DNES_RAM_DTCM=ON -DNES_IRQ_MAPPER_BATCH=ON -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=ON -DENABLE_LTO=ON -DNES6502_LINKED_CORE=ON -DNES6502_ZP_BEQ_SPIN=ON
FLAGS ?= $(FAST_FLAGS)

PDUTIL    = $(SDK)/bin/pdutil
PORT      ?= $(shell ls /dev/cu.usbmodem* 2>/dev/null | head -1)
VOLUME    ?= /Volumes/PLAYDATE
PDX_NAME  ?= FamiCrank.pdx
PDX_DEST  ?= $(PDX_NAME)

.PHONY: all perf device sim clean rebuild install diag-fast install-diag-fast diag-bgpair install-diag-bgpair bench install-bench bench-bgpair install-bench-bgpair bench-kirby-noirqbatch install-bench-kirby-noirqbatch bench-kirby-irqonly install-bench-kirby-irqonly bench-kirby-irqpair install-bench-kirby-irqpair bench-kirby-lto install-bench-kirby-lto bench-kirby-lto-linked install-bench-kirby-lto-linked bench-kirby-lto-fuse install-bench-kirby-lto-fuse bench-kirby-pairprof install-bench-kirby-pairprof bench-kirby-lto-f0a5 install-bench-kirby-lto-f0a5 bench-kirby-lto-zpspin install-bench-kirby-lto-zpspin bench-kirby-lto-noclone install-bench-kirby-lto-noclone bench-kirby-profile install-bench-kirby-profile bench-kirby-base install-bench-kirby-base bench-kirby install-bench-kirby bench-record install-bench-record record-kirby install-record-kirby

# Build device first so pdex.elf lands in Source/ before sim runs pdc.
all: device sim

perf: all

device:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS)
	cmake --build build/device

sim:
	cmake -B build/sim $(FLAGS)
	cmake --build build/sim

clean:
	rm -rf build FamiCrank.pdx nofrendo.pdx Source/pdex.elf Source/pdex.dylib

rebuild:
	touch src/*.c && $(MAKE) all

# Diagnostics on the shipping line: same FAST_FLAGS build plus the DIAG console
# logging ([diag] fps/cpu_only/ppu_full, [autoskip] transitions). Use this for
# any future on-device profiling; the production build keeps DIAG off.
diag-fast:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DDIAG=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DDIAG=ON
	cmake --build build/sim

# Manual-ROM validation for the accepted background-pair probe. This keeps the
# production FAST_FLAGS unchanged while exposing the normal picker and metrics.
diag-bgpair:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPPU_BG_PAIR_FAST=ON -DDIAG=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPPU_BG_PAIR_FAST=ON -DDIAG=ON
	cmake --build build/sim

# Scripted-input benchmark harness (dev only): shipping FAST config plus
# pd-playbench. Auto-loads the Mario ROM and replays the built-in 1-1 script.
bench:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPD_PLAYBENCH=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPD_PLAYBENCH=ON
	cmake --build build/sim

# Single-variable PPU experiment: identical to bench, with only the common
# no-latch two-tile background renderer enabled.
bench-bgpair:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPPU_BG_PAIR_FAST=ON -DPD_PLAYBENCH=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPPU_BG_PAIR_FAST=ON -DPD_PLAYBENCH=ON
	cmake --build build/sim

# Timing reference for the Kirby smoke test: original background renderer and
# per-scanline MMC3 CPU execution. This differs from bench-kirby-base only by
# disabling the promoted IRQ-mapper batcher.
bench-kirby-noirqbatch:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH=OFF -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=OFF -DPPU_BG_PAIR_FAST=OFF -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH=OFF -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=OFF -DPPU_BG_PAIR_FAST=OFF -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/sim

# Candidate correctness repair for IRQ batching. Keep 16-line batches outside
# hardware IRQ handlers and use per-scanline execution from IRQ entry through
# RTI. NMI/BRK handlers stay batched.
bench-kirby-irqonly:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=OFF -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=OFF -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/sim

# Combine the validated IRQ-only correctness repair with the independently
# measured two-tile background renderer. This adds one flag to irqonly.
bench-kirby-irqpair:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=ON -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=ON -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/sim

# Compiler-only experiment on the corrected stack. LTO is default-off and is
# the sole variable relative to bench-kirby-irqpair.
bench-kirby-lto:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=ON -DPD_PLAYBENCH_KIRBY=ON -DENABLE_LTO=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=ON -DPD_PLAYBENCH_KIRBY=ON -DENABLE_LTO=ON
	cmake --build build/sim

# Rev-A memory-placement discriminator: run the linker's contiguous .itcm
# section in place instead of copying the complete interpreter to heap SRAM.
bench-kirby-lto-linked:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_LINKED_CORE=ON -DNES6502_ZP_BEQ_SPIN=OFF
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_LINKED_CORE=ON -DNES6502_ZP_BEQ_SPIN=OFF
	cmake --build build/sim

# Compact interpreter superinstruction probe: fold the common DEY/INY/DEX/INX
# followed by BNE pairs without changing cycle or interrupt-slice boundaries.
bench-kirby-lto-fuse:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_FUSE_INCDEC_BNE=ON -DNES6502_ZP_BEQ_SPIN=OFF
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_FUSE_INCDEC_BNE=ON -DNES6502_ZP_BEQ_SPIN=OFF
	cmake --build build/sim

# Diagnostic-only opcode-transition profile on the accepted linked-core line.
# Its wall timing is intentionally invalid; use the [pairprof] ranking only.
bench-kirby-pairprof:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_PAIRPROFILE=ON -DNES6502_ZP_BEQ_SPIN=OFF
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_PAIRPROFILE=ON -DNES6502_ZP_BEQ_SPIN=OFF
	cmake --build build/sim

# Profile-directed structural candidate: F0->A5 and A5->F0 comprise 46.7% of
# Kirby's executed opcode transitions, so chain those existing case bodies.
bench-kirby-lto-f0a5:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_CHAIN_F0_A5=ON -DNES6502_ZP_BEQ_SPIN=OFF
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_CHAIN_F0_A5=ON -DNES6502_ZP_BEQ_SPIN=OFF
	cmake --build build/sim

# Collapse stable LDA-zp/BEQ-back polling loops to the same execute-slice edge.
# Kirby's dominant F0/A5 profile is its unique A5 39 F0 FC loop at CPU $C070.
bench-kirby-lto-zpspin:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_ZP_BEQ_SPIN=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPD_PLAYBENCH_KIRBY=ON -DNES6502_ZP_BEQ_SPIN=ON
	cmake --build build/sim

# Keep LTO but disable interprocedural constant-propagation cloning, limiting
# one source of code growth while retaining whole-program optimization.
bench-kirby-lto-noclone:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=ON -DPD_PLAYBENCH_KIRBY=ON -DENABLE_LTO_NO_IPA_CLONE=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=ON -DPD_PLAYBENCH_KIRBY=ON -DENABLE_LTO_NO_IPA_CLONE=ON
	cmake --build build/sim

# Diagnostic-only split timing on the corrected combined stack. Its timing
# calls add overhead, so use the phase breakdown rather than the final FPS.
bench-kirby-profile:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=ON -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON -DDIAG=ON -DDIAG_CPU_EXEC_TIMING=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=ON -DPPU_BG_PAIR_FAST=ON -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON -DDIAG=ON -DDIAG_CPU_EXEC_TIMING=ON
	cmake --build build/sim

# Correctness control for the background-pair experiment: promoted shipping
# flags, original one-tile renderer, and the exact same recorded input.
bench-kirby-base:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=OFF -DPPU_BG_PAIR_FAST=OFF -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=OFF -DPPU_BG_PAIR_FAST=OFF -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/sim

# Same Kirby replay with only the background-pair experiment enabled. Kept
# separate from the canonical Mario replay and the correctness control above.
bench-kirby:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=OFF -DPPU_BG_PAIR_FAST=ON -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DNES_IRQ_MAPPER_BATCH_IRQ_SCOPE=OFF -DPPU_BG_PAIR_FAST=ON -DENABLE_LTO=OFF -DPD_PLAYBENCH_KIRBY=ON
	cmake --build build/sim

# Input recorder (dev only): auto-loads the Mario ROM, you play it live, then the
# "Dump script" menu item prints a pd-playbench script of your inputs.
bench-record:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPD_PLAYBENCH_RECORD=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPD_PLAYBENCH_RECORD=ON
	cmake --build build/sim

# Kirby recorder: normal ROM picker, pair renderer enabled, and a dedicated
# script path with virtual NES Start so D-pad Up remains replayable.
record-kirby:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FAST_FLAGS) -DPPU_BG_PAIR_FAST=ON -DPD_PLAYBENCH_RECORD_KIRBY=ON
	cmake --build build/device
	cmake -B build/sim $(FAST_FLAGS) -DPPU_BG_PAIR_FAST=ON -DPD_PLAYBENCH_RECORD_KIRBY=ON
	cmake --build build/sim

_push:
	@test -n "$(PORT)" || (echo "No Playdate device found on /dev/cu.usbmodem*"; exit 1)
	@echo "Mounting $(PORT)..."
	$(PDUTIL) $(PORT) datadisk
	@sleep 3
	@echo "Copying $(PDX_NAME) to $(PDX_DEST)..."
	mkdir -p $(VOLUME)/Games/$(PDX_DEST)
	cp -R $(PDX_NAME)/. $(VOLUME)/Games/$(PDX_DEST)
	diskutil eject $(VOLUME)
	@echo "Done. $(PDX_DEST) installed on device."

install: all _push
install-diag-fast: diag-fast _push
install-diag-bgpair: diag-bgpair _push
install-bench: bench _push
install-bench-bgpair: bench-bgpair _push
install-bench-kirby-noirqbatch: bench-kirby-noirqbatch _push
install-bench-kirby-irqonly: bench-kirby-irqonly _push
install-bench-kirby-irqpair: bench-kirby-irqpair _push
install-bench-kirby-lto: bench-kirby-lto _push
install-bench-kirby-lto-linked: bench-kirby-lto-linked _push
install-bench-kirby-lto-fuse: bench-kirby-lto-fuse _push
install-bench-kirby-pairprof: bench-kirby-pairprof _push
install-bench-kirby-lto-f0a5: bench-kirby-lto-f0a5 _push
install-bench-kirby-lto-zpspin: bench-kirby-lto-zpspin _push
install-bench-kirby-lto-noclone: bench-kirby-lto-noclone _push
install-bench-kirby-profile: bench-kirby-profile _push
install-bench-kirby-base: bench-kirby-base _push
install-bench-kirby: bench-kirby _push
install-bench-record: bench-record _push
install-record-kirby: record-kirby _push
