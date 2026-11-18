#!/bin/sh
# Builds the Three's Company CLAP plugin on a Mac (with its window), tests it, and installs it.
#   sh build-mac.sh
# The plugin lands in ~/Library/Audio/Plug-Ins/CLAP, where Bitwig Studio looks for it,
# and a copy is kept in build/mac inside this folder.
set -e
cd "$(dirname "$0")"
HERE="$(pwd)"
[ -f libs/clap-main/include/clap/clap.h ] || { echo "libs/clap-main is missing (the CLAP headers)."; exit 1; }

# the window's page is built into the plugin
if command -v python3 >/dev/null 2>&1; then python3 tools/embed_gui.py; fi

# Build on the internal disk: signing a bundle on an external drive can fail.
WORK="$(mktemp -d)"
B="$WORK/Threes Company.clap"
mkdir -p "$B/Contents/MacOS"
INC="-Isrc -Ilibs/sst-filters-include -Ilibs/sst-basic-blocks-include -Ilibs/simde -Ilibs/clap-main/include"
COMMON="-O2 -mmacosx-version-min=11.0 -fvisibility=hidden -fvisibility-inlines-hidden"
# a per-build tag on the window's Objective-C class names, so two builds can be loaded side by side
TAG="$(date +%s)"

build() { # $1: architecture flags
  clang++ -std=c++20 $COMMON $1 -c src/tc_clap.cpp -o "$WORK/tc_clap.o" $INC &&
  clang++ -std=c++17 $COMMON -fno-objc-arc -DTC_OBJC_SUFFIX="$TAG" $1 -c src/tc_gui_mac.mm -o "$WORK/tc_gui_mac.o" -Isrc &&
  clang++ $COMMON $1 -bundle -o "$B/Contents/MacOS/Threes Company" "$WORK/tc_clap.o" "$WORK/tc_gui_mac.o" -framework Cocoa -framework WebKit
}

echo "Building (a minute or two)..."
if build "-arch arm64 -arch x86_64 -Xarch_x86_64 -msse4.2" 2>"$WORK/universal.log"; then
  echo "Built for Apple Silicon and Intel."
else
  echo "Universal build did not work; building for this Mac only."
  tail -20 "$WORK/universal.log"
  build ""
fi

cat > "$B/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>Threes Company</string>
  <key>CFBundleIdentifier</key><string>com.otiskanno.threes-company</string>
  <key>CFBundleName</key><string>Threes Company</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleSignature</key><string>????</string>
  <key>CFBundleVersion</key><string>0.18.0</string>
  <key>CFBundleShortVersionString</key><string>0.18.0</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
</dict>
</plist>
PLIST
codesign --force --sign - "$B" >/dev/null 2>&1 || echo "(could not sign; it may still load)"

echo "Testing the built plugin..."
clang++ -std=c++20 -O2 -o "$WORK/clap_host_test" tests/clap_host_test.cpp -Ilibs/clap-main/include
"$WORK/clap_host_test" "$B/Contents/MacOS/Threes Company"

DEST="$HOME/Library/Audio/Plug-Ins/CLAP"
mkdir -p "$DEST" "$HERE/build/mac"
rm -rf "$DEST/Threes Company.clap" "$HERE/build/mac/Threes Company.clap"
cp -R "$B" "$DEST/"
cp -R "$B" "$HERE/build/mac/"
echo ""
echo "Installed: $DEST/Threes Company.clap"
echo "In Bitwig: rescan plugins if needed, then open Three's Company (by Otis Kanno) and click its window button."
