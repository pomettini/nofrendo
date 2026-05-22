SDK      ?= $(HOME)/Developer/PlaydateSDK
TOOLCHAIN = $(SDK)/C_API/buildsupport/arm.cmake
FLAGS     = -DCMAKE_BUILD_TYPE=Release -DAUDIO=ON -DDIAG=ON -DPPU_BG=ON -DPPU_SPRITES=ON -DPPU_BLIT=ON -DALIGN_PRG_ROM=OFF -DNES_CPU_BATCH_SCANLINES=1 -DNES6502_OPT_LEVEL=O3 -DNES6502_ALIGN_LOOPS=OFF
PDUTIL    = $(SDK)/bin/pdutil
PORT      ?= $(shell ls /dev/cu.usbmodem* 2>/dev/null | head -1)
VOLUME    ?= /Volumes/PLAYDATE
PDX_DEST  ?= nofrendo.pdx
CPU_BATCH ?= 8
CPU_OPT   ?= O3

.PHONY: all device sim clean rebuild diag diag-batchcpu diag-alignrom diag-cpuopt diag-cpualign diag-nobg diag-nosprites diag-noblit diag-noaudio \
	install install-diag install-diag-nobg install-diag-nosprites install-diag-noblit \
	install-diag-noaudio install-diag-batchcpu install-diag-alignrom install-diag-cpuopt install-diag-cpualign

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

install-diag: diag _push
install-diag-nobg: PDX_DEST = nofrendo-nobg.pdx
install-diag-nobg: diag-nobg _push
install-diag-nosprites: PDX_DEST = nofrendo-nosprites.pdx
install-diag-nosprites: diag-nosprites _push
install-diag-noblit: PDX_DEST = nofrendo-noblit.pdx
install-diag-noblit: diag-noblit _push
install-diag-noaudio: PDX_DEST = nofrendo-noaudio.pdx
install-diag-noaudio: diag-noaudio _push
install-diag-batchcpu: PDX_DEST = nofrendo-batchcpu$(CPU_BATCH).pdx
install-diag-batchcpu: diag-batchcpu _push
install-diag-alignrom: PDX_DEST = nofrendo-batchcpu16-alignrom.pdx
install-diag-alignrom: diag-alignrom _push
install-diag-cpuopt: PDX_DEST = nofrendo-batchcpu16-cpu$(CPU_OPT).pdx
install-diag-cpuopt: diag-cpuopt _push
install-diag-cpualign: PDX_DEST = nofrendo-batchcpu16-cpuO3-loopalign.pdx
install-diag-cpualign: diag-cpualign _push
