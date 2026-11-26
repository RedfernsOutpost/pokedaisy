#!/bin/zsh
# Builds Daisy Launcher (the unofficial Mac version of PokeDaisy, Apple Silicon) from the SDL2 desktop frontend in
# linux/ and packs Daisy Launcher.app. Needs the Xcode Command Line Tools, CMake 3.18+ and SDL2 (Homebrew: brew install
# cmake sdl2). See macos/README.md.
#   zsh macos/build.sh              # -> build/macos/Daisy Launcher.app, ad hoc signed
#   OUT=/Applications zsh macos/build.sh
#   SIGN_ID="Developer ID Application: ..." zsh macos/build.sh   # hardened runtime + macos/entitlements.plist
set -e
ROOT="${0:A:h:h}"
cd "$ROOT"
B="$ROOT/build/macos"
OUT="${OUT:-$B}"
J=$(sysctl -n hw.ncpu)
PREFIX=$(brew --prefix 2>/dev/null || echo /opt/homebrew)

git submodule update --init third_party/mgba third_party/zlib
git submodule update --init --recursive third_party/dsperate-lr

# mGBA's core runs .gb, .gbc and .gba here (armGBA's recompiler maps RWX memory, which macOS refuses without MAP_JIT).
cmake -S libretro -B "$B/gbc" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$B/gbc" --target pokedaisy_gbc_libretro -j$J
cmake -S linux/libretro-ds -B "$B/ds" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$B/ds" --target pokedaisy_ds_libretro -j$J
VER=$(sed -n 's/^ *versionName = "\(.*\)"/\1/p' app/build.gradle.kts)
cmake -S linux -B "$B/host" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" -DBUILD_TESTING=OFF \
  -DPOCKET_VERSION="$VER" >/dev/null
cmake --build "$B/host" --target pokedaisy-linux -j$J

APP="$OUT/Daisy Launcher.app"
M="$APP/Contents/MacOS"
rm -rf "$APP"; mkdir -p "$M" "$APP/Contents/Resources"
# The frontend is the bundle's executable (it finds its data folder itself; see main.c), so the hardened runtime and
# the JIT entitlement apply to it directly.
cp "$B/host/pokedaisy-linux" "$M/DaisyLauncher"
cp "$B/gbc/pokedaisy_gbc_libretro.dylib" "$B/ds/pokedaisy_ds_libretro.dylib" "$M/"
sed "s/@VERSION@/$VER/g" macos/Info.plist > "$APP/Contents/Info.plist"
IS="$B/AppIcon.iconset"; rm -rf "$IS"; mkdir -p "$IS"
for s in 16 32 128 256 512; do
  sips -z $s $s linux/icon.png --out "$IS/icon_${s}x${s}.png" >/dev/null
  sips -z $((s*2)) $((s*2)) linux/icon.png --out "$IS/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$IS" -o "$APP/Contents/Resources/AppIcon.icns"
# SDL inside the app, so it runs on a Mac without Homebrew: sdl2-compat (what the host links) beside SDL3, which
# sdl2-compat loads from its own folder (@loader_path/libSDL3.dylib).
F="$APP/Contents/Frameworks"; mkdir -p "$F"
SDL2=$(otool -L "$M/DaisyLauncher" | awk '/libSDL2-2\.0\.0\.dylib/{print $1; exit}')
if [[ "$SDL2" == /* ]]; then
  SDL2_REAL=$(realpath "$SDL2"); SDL2_DIR="${SDL2_REAL:h}"
  cp "$SDL2_REAL" "$F/libSDL2-2.0.0.dylib"
  SDL3_REAL=$(realpath "$(brew --prefix sdl3 2>/dev/null || echo $PREFIX)/lib/libSDL3.0.dylib")
  cp "$SDL3_REAL" "$F/libSDL3.dylib"
  chmod u+w "$F"/*.dylib
  install_name_tool -id @rpath/libSDL2-2.0.0.dylib "$F/libSDL2-2.0.0.dylib" 2>/dev/null
  install_name_tool -id @rpath/libSDL3.dylib "$F/libSDL3.dylib" 2>/dev/null
  install_name_tool -change "$SDL2" @executable_path/../Frameworks/libSDL2-2.0.0.dylib "$M/DaisyLauncher"
fi

# Licenses and where the source is (the same notices linux/package.py ships).
R="$APP/Contents/Resources"; L="$R/licenses"; mkdir -p "$L"
cp LICENSE NOTICE "$R/"; cp macos/README.md "$R/README.md"
cp third_party/mgba/LICENSE "$L/mGBA-MPL-2.0.txt"
cp third_party/mgba/src/third-party/blip_buf/license.txt "$L/blip_buf-LGPL-2.1.txt"
cp third_party/mgba/src/third-party/inih/LICENSE.txt "$L/inih-BSD.txt"
cp third_party/zlib/LICENSE "$L/zlib.txt"
echo "Pixel Operator by Jayvee Enaguas. CC0. See NOTICE." > "$L/PixelOperator.txt"
( cd third_party/dsperate-lr && find . -type f \( -iname 'license*' -o -iname 'copying*' -o -iname 'notice*' \) ) |
  while IFS= read -r f; do mkdir -p "$L/dsperate-lr/${f:h}"; cp "third_party/dsperate-lr/$f" "$L/dsperate-lr/$f"; done
if [[ -n "${SDL2_DIR:-}" ]]; then
  cp "${SDL2_DIR:h}/LICENSE.txt" "$L/sdl2-compat-zlib.txt"
  cp "${SDL3_REAL:h:h}/LICENSE.txt" "$L/SDL3-zlib.txt"
fi
FORK_URL=$(git remote get-url fork 2>/dev/null || true)
{ echo "Daisy Launcher: the unofficial Mac version of PokeDaisy by lidor30 (https://pokedaisy.com; its source was"
  echo "  https://github.com/lidor30/pokedaisy)"; [[ -n "$FORK_URL" ]] && echo "This build: ${FORK_URL%.git}"
  echo "Commit: $(git rev-parse HEAD)"; echo "mGBA: $(git rev-parse HEAD:third_party/mgba)"
  echo "DSperate: $(git rev-parse HEAD:third_party/dsperate-lr)"; echo "Build: macos/README.md at this commit"; } > "$R/SOURCE.txt"

# Ad hoc signature by default (Apple Silicon runs nothing unsigned). SIGN_ID signs for release: hardened runtime, a
# secure timestamp, and the JIT entitlement on the executable; notarize the zip afterwards.
if [[ -n "${SIGN_ID:-}" ]]; then
  for f in "$F"/*.dylib(N) "$M"/*.dylib; do codesign --force --options runtime --timestamp -s "$SIGN_ID" "$f"; done
  codesign --force --options runtime --timestamp --entitlements macos/entitlements.plist -s "$SIGN_ID" "$M/DaisyLauncher"
  codesign --force --options runtime --timestamp --entitlements macos/entitlements.plist -s "$SIGN_ID" "$APP"
  codesign --verify --strict --deep "$APP"
else
  for f in "$F"/*.dylib(N) "$M"/*.dylib "$M/DaisyLauncher"; do codesign --force -s - "$f"; done
  codesign --force -s - "$APP"
  codesign --verify "$APP"
fi
echo ">> $APP ($VER)"
