#!/bin/sh
# Builds Three's Company as CLAP, VST3 and AU on this Mac (Apple Silicon and Intel) and installs them:
#   sh build-all-mac.sh
# Needs CMake (brew install cmake) and an internet connection the first time (it downloads clap,
# clap-wrapper, the VST3 SDK and Apple's AudioUnitSDK into the build folder).
# The quick CLAP-only build is still: sh build-mac.sh
set -e
cd "$(dirname "$0")"
HERE="$(pwd)"
command -v cmake >/dev/null 2>&1 || { echo "CMake is missing. Install it with: brew install cmake"; exit 1; }

# Build on the internal disk: signing a bundle on an external drive can fail.
WORK="$HOME/Library/Caches/ThreesCompanyBuild"
mkdir -p "$WORK"
# Unix Makefiles: needs only Apple's command line tools (xcode-select --install), not the full Xcode app.
# A build folder made earlier with another generator is cleared first (CMake can't switch generators).
if [ -f "$WORK/CMakeCache.txt" ] && ! grep -q "CMAKE_GENERATOR:INTERNAL=Unix Makefiles" "$WORK/CMakeCache.txt"; then
  rm -rf "$WORK/CMakeCache.txt" "$WORK/CMakeFiles"
fi
echo "Configuring (the first time downloads the SDKs)..."
cmake -S "$HERE" -B "$WORK" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >"$WORK/configure.log" 2>&1 || { tail -30 "$WORK/configure.log"; exit 1; }
echo "Building CLAP, VST3 and AU (a few minutes)..."
cmake --build "$WORK" --parallel "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >"$WORK/build.log" 2>&1 || { grep -E "error" "$WORK/build.log" | head -30; exit 1; }

OUT="$HERE/build/mac-all"
rm -rf "$OUT"; mkdir -p "$OUT"
for ext in clap vst3 component; do
  b=$(find "$WORK" -maxdepth 4 -type d -name "Threes Company.$ext" -not -path "*/_deps/*" | head -1)
  [ -n "$b" ] && cp -R "$b" "$OUT/" || echo "Warning: no Threes Company.$ext was built"
done
for b in "$OUT"/*; do codesign --force --deep --sign - "$b" >/dev/null 2>&1 || true; done

install_one() { # $1 bundle, $2 folder
  mkdir -p "$2"; rm -rf "$2/$(basename "$1")"; cp -R "$1" "$2/"
}
install_one "$OUT/Threes Company.clap" "$HOME/Library/Audio/Plug-Ins/CLAP"
install_one "$OUT/Threes Company.vst3" "$HOME/Library/Audio/Plug-Ins/VST3"
install_one "$OUT/Threes Company.component" "$HOME/Library/Audio/Plug-Ins/Components"
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true

echo ""
echo "Checking the AU with Apple's validator..."
auval -v aufx ThCo OtKa | tail -5 || true
echo ""
echo "Installed CLAP, VST3 and AU. A copy of each is in build/mac-all."
echo "Bitwig: CLAP and VST3. Ableton Live: VST3 and AU (rescan plug-ins in Live's settings)."
