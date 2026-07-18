#!/bin/sh
# build-win.sh — cross-compile a Windows .exe from macOS with zig. Same sources as build.sh, but
# the platform layer is win.c and it links the Win32 libraries. Produces cvertex.exe; copy it to a
# Windows machine and double-click. (zig: brew install zig)
set -e
OUT=${OUT:-cvertex.exe}
sh tools/gen-games.sh          # scan games/*.c -> src/games.gen.h (the cartridge roster)
zig cc -target x86_64-windows-gnu -std=c11 -O2 -Isrc \
  -o "$OUT" src/core.c src/g3d.c src/shape.c src/synth.c src/text.c src/win.c games/*.c \
  -lgdi32 -lwinmm -luser32
echo "$OUT — copy to a Windows machine and run it"
