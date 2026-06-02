# Nofrendo

[![Build](https://github.com/nwagyu/nofrendo/actions/workflows/build.yml/badge.svg)](https://github.com/nwagyu/nofrendo/actions/workflows/build.yml)

This app is a [NES](https://en.wikipedia.org/wiki/Nintendo_Entertainment_System) emulator that runs on the [NumWorks calculator](https://www.numworks.com).

## Install the app

To install this app, you'll need to:
1. Download the latest `nofrendo.nwa` file from the [Releases](https://github.com/nwagyu/nofrendo/releases) page
2. Put `.nes` ROM dumps in `/Shared/Emulation/nes/games/` and select them from the Playdate ROM picker.
2. Head to [my.numworks.com/apps](https://my.numworks.com/apps) to send the `nwa` file on your calculator along the `nes` file.

## How to use the app

The controls are pretty obvious because the NES gamepad looks a lot like the NumWorks' keyboard:

|NES controls|NumWorks|
|-|-|
|Arrow|Arrows|
|B|Back|
|A|OK|
|Select|Shift|
|Start|Backspace|

## Build the app

To build this sample app, you will need to install the [embedded ARM toolchain](https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain) and [nwlink](https://www.npmjs.com/package/nwlink).

```shell
brew install numworks/tap/arm-none-eabi-gcc node # Or equivalent on your OS
npm install -g nwlink
make clean && make build
```
