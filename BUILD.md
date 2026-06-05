# Building

This branch builds a Playdate `.pdx` package.

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
make        # promoted performance build: device + simulator + nofrendo.pdx
make device # promoted performance device binary only
make sim    # promoted performance simulator binary only
make clean  # remove build outputs
```

The default `make`, `make device`, `make sim`, and `make install` path must always use the
current promoted performance line. In the Makefile this is `FLAGS ?= $(FAST_FLAGS)`, which
currently enables batch-16 CPU slices, direct CPU memory I/O, fast absolute JMP, lazy cycle
accounting, BNE/BPL/BEQ fast paths, direct audio ring fill, and fast OAM DMA.

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
```

`diag-cycletrim` defaults to the conservative `CYCLEPCT=96`; override it with
`CYCLEPCT=94 make diag-cycletrim`.

## Install On Device

Connect the Playdate by USB, then run:

```sh
make install
```

All install targets overwrite the single on-device `Games/nofrendo.pdx` package. Do not
create separately named diagnostic copies.

If auto-detection fails, pass the serial port explicitly:

```sh
PORT=/dev/cu.usbmodemXXXX make install
```
