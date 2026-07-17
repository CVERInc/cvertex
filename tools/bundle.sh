#!/bin/sh
# bundle — wrap the binary in a .app so it can be double-clicked.
#
# A .app is a directory with a plist in it. That's the whole secret, and it's why this is a
# shell script and not a dependency: no Xcode project, no signing identity, no build system
# that wants to own the repo.
#
#   tools/bundle.sh [dest]        # default: ./cvertex.app
#
# Ad-hoc signed, because an unsigned binary makes Gatekeeper mutter. That's enough to run it
# yourself; shipping it to anyone else needs a real identity and notarisation.
set -e
DEST=${1:-cvertex.app}

./build.sh >/dev/null
rm -rf "$DEST"
mkdir -p "$DEST/Contents/MacOS"

cat > "$DEST/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>              <string>cvertex</string>
  <key>CFBundleDisplayName</key>       <string>cvertex</string>
  <key>CFBundleIdentifier</key>        <string>net.cver.cvertex</string>
  <key>CFBundleExecutable</key>        <string>cvertex</string>
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>LSMinimumSystemVersion</key>    <string>11.0</string>
  <key>NSHighResolutionCapable</key>   <true/>
</dict>
</plist>
PLIST

cp cvertex "$DEST/Contents/MacOS/cvertex"
codesign --force --sign - "$DEST" 2>/dev/null || true
echo "$DEST — double-click it, or: open $DEST"
