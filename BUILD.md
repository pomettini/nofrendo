# Building

This branch builds a Playdate `.pdx` package.

## Requirements

- Playdate SDK installed, normally at `~/Developer/PlaydateSDK`
- CMake
- `arm-none-eabi-gcc` from the Playdate SDK/toolchain setup

If your SDK is elsewhere, pass `SDK=/path/to/PlaydateSDK` to `make`.

## ROM

The emulator loads `cartridge.nes` from the app bundle. Put your ROM at:

```sh
Source/cartridge.nes
```

## Build

```sh
make        # build device + simulator and create nofrendo.pdx
make device # device binary only
make sim    # simulator binary only
make clean  # remove build outputs
```

Diagnostic builds are the default during performance work. Useful probes:

```sh
make diag
make diag-fpslite
make diag-fastoamdma
make diag-cycletrim
make diag-jumptable
make diag-lazycycles
make diag-fastbne
make diag-fastbpl
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
