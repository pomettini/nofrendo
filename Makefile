SDK      ?= $(HOME)/Developer/PlaydateSDK
TOOLCHAIN = $(SDK)/C_API/buildsupport/arm.cmake
FLAGS     = -DCMAKE_BUILD_TYPE=Release -DAUDIO=ON -DDIAG=ON -DPPU_BG=ON -DPPU_SPRITES=ON -DPPU_BLIT=ON -DALIGN_PRG_ROM=OFF -DNES_CPU_BATCH_SCANLINES=1 -DNES6502_OPT_LEVEL=O3 -DNES6502_ALIGN_LOOPS=OFF -DNES6502_SPINHACK=OFF -DNES6502_OPPROFILE=OFF -DNES6502_FAST_PC_OPS=OFF -DNES6502_HOTOPS=OFF
PDUTIL    = $(SDK)/bin/pdutil
PORT      ?= $(shell ls /dev/cu.usbmodem* 2>/dev/null | head -1)
VOLUME    ?= /Volumes/PLAYDATE
PDX_DEST  ?= nofrendo.pdx
CPU_BATCH ?= 8
CPU_OPT   ?= O3

.PHONY: all device sim clean rebuild diag diag-batchcpu diag-alignrom diag-cpuopt diag-cpualign diag-spinhack diag-opprofile diag-fastpc diag-hotops diag-nobg diag-nosprites diag-noblit diag-noaudio \
	install install-diag install-diag-nobg install-diag-nosprites install-diag-noblit \
	install-diag-noaudio install-diag-batchcpu install-diag-alignrom install-diag-cpuopt install-diag-cpualign install-diag-spinhack install-diag-opprofile install-diag-fastpc install-diag-hotops

# Build device first so pdex.elf lands in Source/ before sim runs pdc
all: device sim

device:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS)
	cmake --build build/device

sim:
	cmake -B build/sim $(FLAGS)
	cmake --build build/sim

clean:
	rm -rf build nofrendo.pdx Source/pdex.elf Source/pdex.dylib

rebuild:
	touch src/*.c && $(MAKE) all

diag:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON
	cmake --build build/sim

# Speed-first experiment: trade scanline CPU timing for fewer interpreter entries.
diag-batchcpu:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=$(CPU_BATCH)
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=$(CPU_BATCH)
	cmake --build build/sim

# Keep the timing-safe batch width while testing PRG ROM D-cache alignment.
diag-alignrom:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DALIGN_PRG_ROM=ON
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DALIGN_PRG_ROM=ON
	cmake --build build/sim

# Keep the timing-safe batch width while comparing 6502 interpreter code shape.
diag-cpuopt:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=$(CPU_OPT)
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=$(CPU_OPT)
	cmake --build build/sim

# Keep the measured O3 batch16 row while testing CPU loop layout.
diag-cpualign:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_ALIGN_LOOPS=ON
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_ALIGN_LOOPS=ON
	cmake --build build/sim

# Fast-forward exact PPUSTATUS wait loops on top of the current best CPU baseline.
diag-spinhack:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_SPINHACK=ON
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_SPINHACK=ON
	cmake --build build/sim

# Count the opcode mix in each 60-frame diagnostic window.
diag-opprofile:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_OPPROFILE=ON
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_OPPROFILE=ON
	cmake --build build/sim

# Use pc_ptr for hot operand fetches and avoid rebasing same-bank taken branches.
diag-fastpc:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_FAST_PC_OPS=ON
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_FAST_PC_OPS=ON
	cmake --build build/sim

# Narrow profile-guided probe: specialize only the opcodes that dominate SMB windows.
diag-hotops:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_HOTOPS=ON
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DNES_CPU_BATCH_SCANLINES=16 -DNES6502_OPT_LEVEL=O3 -DNES6502_HOTOPS=ON
	cmake --build build/sim

# Profiling matrix targets — build diag with one subsystem disabled at a time.
diag-nobg:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DPPU_BG=OFF
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DPPU_BG=OFF
	cmake --build build/sim

diag-nosprites:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DPPU_SPRITES=OFF
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DPPU_SPRITES=OFF
	cmake --build build/sim

diag-noblit:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DPPU_BLIT=OFF
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DPPU_BLIT=OFF
	cmake --build build/sim

diag-noaudio:
	cmake -B build/device -DTOOLCHAIN=armgcc -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(FLAGS) -DDIAG=ON -DAUDIO=OFF
	cmake --build build/device
	cmake -B build/sim $(FLAGS) -DDIAG=ON -DAUDIO=OFF
	cmake --build build/sim

_push:
	@test -n "$(PORT)" || (echo "No Playdate device found on /dev/cu.usbmodem*"; exit 1)
	@echo "Mounting $(PORT)..."
	$(PDUTIL) $(PORT) datadisk
	@sleep 3
	@echo "Copying nofrendo.pdx to $(PDX_DEST)..."
	cp -R nofrendo.pdx $(VOLUME)/Games/$(PDX_DEST)
	diskutil eject $(VOLUME)
	@echo "Done. $(PDX_DEST) installed on device."

install: all _push

# Keep the device tidy: every diagnostic install overwrites the single on-device
# nofrendo.pdx instead of creating separately named test copies.
install-diag: diag _push
install-diag-nobg: diag-nobg _push
install-diag-nosprites: diag-nosprites _push
install-diag-noblit: diag-noblit _push
install-diag-noaudio: diag-noaudio _push
install-diag-batchcpu: diag-batchcpu _push
install-diag-alignrom: diag-alignrom _push
install-diag-cpuopt: diag-cpuopt _push
install-diag-cpualign: diag-cpualign _push
install-diag-spinhack: diag-spinhack _push
install-diag-opprofile: diag-opprofile _push
install-diag-fastpc: diag-fastpc _push
install-diag-hotops: diag-hotops _push
