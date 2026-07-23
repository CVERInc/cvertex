#!/bin/sh
# build-win.sh — cross-compile a Windows .exe from macOS with zig. Same sources as build.sh, but
# the platform layer is win.c and it links the Win32 libraries. Produces cvertex.exe; copy it to a
# Windows machine and double-click. (zig: brew install zig)
set -e
OUT=${OUT:-cvertex.exe}
sh tools/gen-games.sh          # scan games/*.c -> gen/games.gen.h (the cartridge roster)
zig cc -target x86_64-windows-gnu -std=c11 -O2 -Igen -Isrc -Wl,--subsystem,windows \
  -o "$OUT" src/core.c src/g3d.c src/shape.c src/synth.c src/text.c src/net.c src/win.c games/*.c \
  -lgdi32 -lwinmm -luser32 -lws2_32
# -mwindows = the GUI subsystem: double-clicking opens just the game window, no black console.
# (win.c still AttachConsole's to a terminal if it was launched from one, so --headless prints.)
echo "$OUT — double-click to play"
