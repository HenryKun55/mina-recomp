# mina-recomp

A static recompilation of **Mina the Hollower** (Yacht Club Games) from the
arm64 slice of its macOS executable to portable C, running natively on the
**PlayStation Vita**.

No game data is included, and nothing derived from it goes into the repo. You
need your own copy of the game: the build recompiles *your* executable, and the
game reads its data from your installation (`gamedata/app` → `Mina the Hollower.app`).

## Status

- The whole game runs on a real PS Vita: title, menus, overworld, dungeons,
  bosses, audio, saves.
- Rendering holds 60 fps. The game logic steps 60 times a second where it can
  and drops to 30 in the heaviest rooms (an adaptive rate controller); the
  current work is making the world step cheap enough to stay at 60 everywhere.
- Long play sessions on the console are the stability test; crash dumps
  (`ux0:data/psp2core-*.psp2dmp`) are read back to the recompiled functions.

See [docs/PLAN.md](docs/PLAN.md) for the architecture.

## How it works

- `tools/recomp/gen.py` translates every arm64 function of the Mach-O
  (27,900 of them) to C (`build*/gen/funcs_*.c`), with the image's data and
  relocations in `tables.c`.
- `runtime/` is the guest's world: memory (`memory.c`), libc/pthreads/libdispatch
  (`libc.c`, `threads.c`), a small Objective-C runtime (`objc.c`), SDL, audio, and
  Metal translated to OpenGL ES on vitaGL (`metal*.c`; shaders from MSL by
  `tools/shaders/`).
- `runtime/natives.txt` lists game functions replaced by native C in
  `runtime/src/game.c` (hot paths and fixes); `MINA_NO_NATIVES=name` turns one
  off, `MINA_VERIFY_*` switches compare a native with the recompiled original.

## Building

Needs the [VitaSDK](https://vitasdk.org), CMake, Python 3 and your copy of the game.

```sh
ln -s "/path/to/Mina the Hollower.app" gamedata/app
tools/build_vitagl.sh                     # vitaGL with tools/vitagl/*.patch, into deps/
cmake -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita -j8              # build-vita/mina.vpk, eboot.bin
```

`-DMINA_PROFILE=ON` adds the sampling profiler's function marks (`MINA_PROFILE=1`
in `ux0:data/mina/env.txt` turns it on).

## Running

- Console: `tools/vita/deploy_ftp.sh <vita-ip>` (VitaShell's FTP, SELECT) sends the
  VPK and your game data to `ux0:data/mina`; after a rebuild, `... eboot` sends
  only the executable. Install the VPK once with VitaShell.
- Emulator: `tools/run_vita3k.sh` runs it in a Vita3K fork with a PS Vita speed
  mode (CPU clock, memory budgets, a GPU model calibrated on a Vita 1000).
- `ux0:data/mina/env.txt` holds runtime switches (one `NAME=value` a line), e.g.
  `MINA_FPS=1` for a frame-time log in `ux0:data/mina/mina.log`.
