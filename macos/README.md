# Daisy Launcher (PokeDaisy for macOS)

**Daisy Launcher** is the unofficial Mac version of [PokeDaisy](https://pokedaisy.com) by lidor30, with a game launcher
made for the Mac. It isn't made or supported by PokeDaisy's developer; the official downloads are on pokedaisy.com,
which has no Mac version. The data folder keeps PokeDaisy's name, so earlier builds' games and saves carry over.

`Daisy Launcher.app` is the desktop frontend in [`linux/`](../linux/README.md) (SDL2, Metal through SDL's renderer) built for
Apple Silicon, with the libretro cores beside it. Everything the Linux build has works the same: the companion, the HUD,
the quick menu, EXPAND, ZOOM, save states, fast-forward, rewind, the host menu and controller hot-plug. The controls are
the [Linux build's](../linux/README.md#controls-and-display).

## Build

Needs the Xcode Command Line Tools (`xcode-select --install`), CMake 3.18+ and SDL2:

```
brew install cmake sdl2
zsh macos/build.sh
```

The app is written to `build/macos/Daisy Launcher.app` (`OUT=/Applications zsh macos/build.sh` puts it elsewhere). It
has an ad hoc signature, so it runs on the Mac that built it. `SIGN_ID="Developer ID Application: ..."` signs it for
other Macs instead (hardened runtime, secure timestamp, `macos/entitlements.plist` for the DS recompiler's `MAP_JIT`);
then zip it, notarize the zip (`xcrun notarytool submit ... --wait`), staple the app (`xcrun stapler staple`) and zip it
again. SDL comes from Homebrew and is copied into the app.

## Games and saves

The data folder is `~/Library/Application Support/PokeDaisy`: put `.gb`, `.gbc`, `.gba` or `.nds` files (unzipped) in
its `roms/` folder and pick one from the game list. Saves, states and settings sit beside it, as on Linux (`saves/`,
`states/`, `host.ini`). No games are included.

## Differences from Linux

- **GBA runs on the mGBA core** (`pokedaisy_gbc_libretro.dylib`, which also runs Game Boy and Game Boy Color).
  armGBA's recompiler writes code into memory mapped read-write-execute, which macOS refuses without `MAP_JIT` and its
  write-protect toggling, so `core_for` picks mGBA for `.gba` on macOS. An Apple Silicon Mac runs it at full speed with
  room to spare.
- **DS runs on DSperate's recompiler**, made to work the Mac's way by `third_party/patches/dsperate-macos.patch`
  (applied at configure time, Apple only, along with its Apple clang and Mach-O fixes): the code arena is `MAP_JIT`
  memory written inside `pthread_jit_write_protect_np` toggles, dispatch doesn't keep the arena in x18 (which macOS
  reserves), and fastmem stays off. DSperate's `test_jit` (directed cases plus interpreter-compared fuzzing) passes on
  an M5; HeartGold's core time per frame roughly halves. Settings → DS ENGINE → SAFE runs the interpreter instead
  (`dsperate_jit` disabled), and `-DPD_DS_JIT=OFF` builds without the recompiler. A Developer ID build with the
  hardened runtime needs the `com.apple.security.cs.allow-jit` entitlement.
- The cores end in `.dylib`, and the frontend finds its own folder with `_NSGetExecutablePath` (there is no `/proc`).
- **Xbox controllers go through macOS's GameController framework** (`SDL_HINT_JOYSTICK_HIDAPI_XBOX` is 0 on Apple).
  SDL's own HIDAPI Xbox driver opens a virtual Xbox controller ([Ekipad](https://redfernsoutpost.com/ekipad/), an iPhone controller app) but gets none of its input.
- **Ekipad's Generic Gamepad route has a built-in mapping**, so that virtual controller works too (SDL has none for it).
- **In the game list, a click picks a game and a second click plays it**, as in Finder. Keys and controllers are
  unchanged, and other platforms keep one click (a Steam Deck's touch screen arrives as clicks).

## Updates

Daisy Launcher looks for a newer `mac-launcher-N` release on start. Settings → VERSION → UPDATE downloads
`DaisyLauncher-Mac.zip`, keeps it only if it is signed by the running app's own Developer ID team and passes Gatekeeper,
moves the running app to the Trash and opens the new one in its place. A self-built (ad hoc) copy, one run from
Downloads (translocated) or one in a folder it can't write opens the download page instead.

## Automatic builds

`.github/workflows/daisy-launcher-mac.yml` checks PokeDaisy's GitLab every day. When it has tagged a newer release, the
workflow merges it into `mac-launcher`, bumps `PD_MAC_RELEASE`, builds, signs and notarizes the app, pushes the code and
leaves a **draft** release `mac-launcher-N`. Publishing the draft on GitHub is what sends it to users. A merge that
conflicts, or a failed build, fails the run (GitHub emails about it) without changing anything; then merge by hand.
Run it from the Actions tab with **check_only** to build an unsigned copy of the branch as it is.

It needs these repository secrets (Settings → Secrets and variables → Actions):

| Secret | What |
|---|---|
| `MAC_CERT_P12` | The Developer ID Application certificate with its private key, exported from Keychain Access as a .p12, then `base64 -i cert.p12 \| pbcopy` |
| `MAC_CERT_PASSWORD` | The password given to that .p12 when exporting it |
| `NOTARY_APPLE_ID` | The Apple Account email of the developer account |
| `NOTARY_TEAM_ID` | The team ID (in the certificate's name, in brackets) |
| `NOTARY_PASSWORD` | An app-specific password from account.apple.com |
