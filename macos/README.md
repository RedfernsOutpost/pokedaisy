# PokeDaisy for macOS

`PokeDaisy.app` is the desktop frontend in [`linux/`](../linux/README.md) (SDL2, Metal through SDL's renderer) built for
Apple Silicon, with the libretro cores beside it. Everything the Linux build has works the same: the companion, the HUD,
the quick menu, EXPAND, ZOOM, save states, fast-forward, rewind, the host menu and controller hot-plug. The controls are
the [Linux build's](../linux/README.md#controls-and-display).

## Build

Needs the Xcode Command Line Tools (`xcode-select --install`), CMake 3.18+ and SDL2:

```
brew install cmake sdl2
zsh macos/build.sh
```

The app is written to `build/macos/PokeDaisy.app` (`OUT=/Applications zsh macos/build.sh` puts it elsewhere). It has an
ad hoc signature, so it runs on the Mac that built it; a copy for other Macs would need a Developer ID signature and
notarization. It links Homebrew's SDL2.

## Games and saves

The data folder is `~/Library/Application Support/PokeDaisy`: put `.gb`, `.gbc`, `.gba` or `.nds` files (unzipped) in
its `roms/` folder and pick one from the game list. Saves, states and settings sit beside it, as on Linux (`saves/`,
`states/`, `host.ini`). No games are included.

## Differences from Linux

- **GBA runs on the mGBA core** (`pokedaisy_gbc_libretro.dylib`, which also runs Game Boy and Game Boy Color).
  armGBA's recompiler writes code into memory mapped read-write-execute, which macOS refuses without `MAP_JIT` and its
  write-protect toggling, so `core_for` picks mGBA for `.gba` on macOS. An Apple Silicon Mac runs it at full speed with
  room to spare.
- **DS runs on DSperate's interpreter.** Its recompiler is Linux's (the signal handler reads Linux's `ucontext`, and it
  also needs RWX memory), so `linux/libretro-ds` builds with `DSPERATE_JIT=OFF` on macOS, and
  `third_party/patches/dsperate-macos.patch` (applied at configure time, Apple only) covers Apple clang and Mach-O.
- The cores end in `.dylib`, and the frontend finds its own folder with `_NSGetExecutablePath` (there is no `/proc`).
