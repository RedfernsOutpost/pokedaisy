#!/bin/zsh
# Builds PokeDaisy for macOS (Apple Silicon) from the SDL2 desktop frontend in linux/ and packs PokeDaisy.app.
# Needs the Xcode Command Line Tools, CMake 3.18+ and SDL2 (Homebrew: brew install cmake sdl2). See macos/README.md.
#   zsh macos/build.sh              # -> build/macos/PokeDaisy.app
#   OUT=/Applications zsh macos/build.sh
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

APP="$OUT/PokeDaisy.app"
M="$APP/Contents/MacOS"
rm -rf "$APP"; mkdir -p "$M" "$APP/Contents/Resources"
# The binary isn't named "pokedaisy": a case-insensitive disk would make it the same file as the PokeDaisy launcher.
cp "$B/host/pokedaisy-linux" "$M/pokedaisy-host"
cp "$B/gbc/pokedaisy_gbc_libretro.dylib" "$B/ds/pokedaisy_ds_libretro.dylib" "$M/"
cp macos/PokeDaisy.sh "$M/PokeDaisy"; chmod +x "$M/PokeDaisy"
sed "s/@VERSION@/$VER/g" macos/Info.plist > "$APP/Contents/Info.plist"
IS="$B/AppIcon.iconset"; rm -rf "$IS"; mkdir -p "$IS"
for s in 16 32 128 256 512; do
  sips -z $s $s linux/icon.png --out "$IS/icon_${s}x${s}.png" >/dev/null
  sips -z $((s*2)) $((s*2)) linux/icon.png --out "$IS/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$IS" -o "$APP/Contents/Resources/AppIcon.icns"
# Ad hoc signature (Apple Silicon runs nothing unsigned). A release would sign with a Developer ID and notarize.
for f in "$M"/*.dylib "$M/pokedaisy-host"; do codesign --force -s - "$f"; done
codesign --force -s - "$APP"
codesign --verify "$APP"
echo ">> $APP ($VER)"
