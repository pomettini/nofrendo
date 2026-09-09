# FamiCrank emucore for CrankBoy

FamiCrank can be built as a libcrankemu v1 core. CrankBoy discovers it as the
`nes` system and loads ROMs from `/Shared/Emulation/nes/games/`.

## Build

The Playdate SDK must be installed and configured in the usual way.

```sh
make emucore
```

The ready-to-install files are written to `build/emucore/`:

- `FamiCrank.bin` for Playdate hardware
- `FamiCrank.dylib` on macOS (`.so` on Linux or `.dll` on Windows)

To build and validate the macOS core's pdll handshake and libcrankemu metadata:

```sh
make emucore-check
```

## Install and test

For the Playdate Simulator:

```sh
make install-emucore-sim
```

For a connected Playdate in normal USB mode:

```sh
make install-emucore-device
```

The install targets copy the library to `/Shared/Emulation/cores/`. Put NES
ROMs in `/Shared/Emulation/nes/games/`, then launch CrankBoy. CrankBoy should
scan the core, associate it with the `nes` system, and list the games.

During development, CrankBoy can bypass its library using arguments relative
to its data disk:

```text
core=/Shared/Emulation/cores/FamiCrank.bin rom=/Shared/Emulation/nes/games/game.nes
```

Use `.dylib`, `.so`, or `.dll` instead of `.bin` in the Simulator. CrankBoy
2.2.1 strips the discovered extension and appends `.bin` in its hardware pdll
loader, so the device payload must use the `.bin` name.

## Implemented interface

- iNES and NES 2.0 images accepted by the existing Nofrendo loader
- CrankBoy input forwarding, with the existing crank Start/Select controls
- battery-backed 8KB SRAM managed by CrankBoy; it is conservatively persisted
  on every pause/exit because Nofrendo exposes no SRAM write barrier
- Auto and fixed frame-skip preferences
- one NES frame advanced per 50Hz CrankBoy host tick, matching standalone
  FamiCrank's PAL timing; CrankBoy's global Uncap FPS setting remains supported

Save states are not exposed because this FamiCrank port does not currently
include Nofrendo's save-state implementation.

## Current CrankBoy simulator issue

CrankBoy development commit `492170f` has an allocator mismatch in pdll's
macOS path: the library handle is created with system `calloc()` and released
through Playdate's allocator-backed `free()`. This produces a SIGBUS after a
core is scanned. Hardware uses a different loader path and is unaffected.

For local CrankBoy simulator testing, change its `libs/pdll/pdll.h` allocation
from `calloc(1, sizeof(pdll_t))` to `malloc(sizeof(pdll_t))`, followed by
`memset(lib, 0, sizeof(*lib))`. The end-to-end test described above passes
after that frontend-side fix.
