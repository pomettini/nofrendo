SDK      ?= $(HOME)/Developer/PlaydateSDK
TOOLCHAIN = $(SDK)/C_API/buildsupport/arm.cmake
FLAGS     = -DCMAKE_BUILD_TYPE=Release -DAUDIO=ON
PDUTIL    = $(SDK)/bin/pdutil
PORT      ?= $(shell ls /dev/cu.usbmodem* 2>/dev/null | head -1)
VOLUME    ?= /Volumes/PLAYDATE

.PHONY: all device sim clean rebuild diag install install-diag

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

_push:
	@test -n "$(PORT)" || (echo "No Playdate device found on /dev/cu.usbmodem*"; exit 1)
	@echo "Mounting $(PORT)..."
	$(PDUTIL) $(PORT) datadisk
	@sleep 3
	@echo "Copying nofrendo.pdx..."
	cp -r nofrendo.pdx $(VOLUME)/Games/
	diskutil eject $(VOLUME)
	@echo "Done. nofrendo.pdx installed on device."

install: all _push

install-diag: diag _push
