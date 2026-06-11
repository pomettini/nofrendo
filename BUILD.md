# Building

This branch builds the FamiCrank Playdate `.pdx` package.

## Requirements

- Playdate SDK installed, normally at `~/Developer/PlaydateSDK`
- CMake
- `arm-none-eabi-gcc` from the Playdate SDK/toolchain setup

If your SDK is elsewhere, pass `SDK=/path/to/PlaydateSDK` to `make`.

## ROM

On Playdate, the emulator opens a ROM picker at startup. Put `.nes` files in:

```sh
/Shared/Emulation/nes/games/
```

ROMs are loaded only through the picker.

## Build

```sh
git submodule update --init --recursive
make        # promoted performance build: device + simulator + FamiCrank.pdx
make perf   # alias for the promoted performance build
make device # promoted performance device binary only
make sim    # promoted performance simulator binary only
make clean  # remove build outputs
```

The default `make`, `make device`, `make sim`, and `make install` path must always use the
current promoted performance line. In the Makefile this is `FLAGS ?= $(FAST_FLAGS)`, which
currently enables batch-16 CPU slices, direct CPU memory I/O, fast absolute JMP, lazy cycle
accounting, BNE/BPL/BEQ fast paths, hot memory load/store opcode fast paths, direct audio
ring fill, fast OAM DMA, and diagnostics off.

Named diagnostic targets use `PROBE_FLAGS` plus their explicit experiment flags so they can
still measure individual variants against the neutral baseline. Useful probes:

```sh
make diag
make diag-fpslite
make diag-fastoamdma
make diag-cycletrim
make diag-jumptable
make diag-lazycycles
make diag-fastbne
make diag-fastbpl
make diag-fastbeq
make diag-fixedcycles
make diag-fastjmp
make diag-tcmhot
make diag-tcmcore
make diag-tcmstats
```

`diag-cycletrim` defaults to the conservative `CYCLEPCT=96`; override it with
`CYCLEPCT=94 make diag-cycletrim`.

`diag-tcmhot` is a proof build for Playdate fast-memory code relocation. It keeps the
production emulator path unchanged, copies only a tiny probe into the DTCM stack pool, and
prints `[tcmhot]` console lines so the relocation can be verified on device.

`diag-tcmcore` is the first functional DTCM code experiment. It routes a tiny 6502
hot-opcode core through the relocated block and falls back to the promoted interpreter for
everything it does not handle. Keep it diagnostic-only until device numbers prove it helps.

`diag-tcmstats` enables the same tiny DTCM core plus per-window attribution counters. Use it
after a `diag-tcmcore` run regresses or looks suspicious.

## Install On Device

Connect the Playdate by USB, then run:

```sh
make install
```

All install targets overwrite the single on-device `Games/FamiCrank.pdx` package. Do not
create separately named diagnostic copies.

If auto-detection fails, pass the serial port explicitly:

```sh
PORT=/dev/cu.usbmodemXXXX make install
```

If `make install` or `make _push` switches the Playdate into data-disk mode but then fails
with `Permission denied` while writing `Games/FamiCrank.pdx`, the package can be copied
manually after the disk is mounted:

```sh
diskutil mount /dev/diskXs1
cp -R FamiCrank.pdx/. /Volumes/PLAYDATE/Games/FamiCrank.pdx
dot_clean -m /Volumes/PLAYDATE/Games/FamiCrank.pdx
cmp -s FamiCrank.pdx/pdex.bin /Volumes/PLAYDATE/Games/FamiCrank.pdx/pdex.bin
cmp -s FamiCrank.pdx/pdxinfo /Volumes/PLAYDATE/Games/FamiCrank.pdx/pdxinfo
diskutil eject /Volumes/PLAYDATE
```
