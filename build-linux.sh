#!/bin/sh
# build-linux.sh — build a native Linux binary. Same sources as build.sh, but the platform layer is
# lnx.c and it links the X11 and ALSA system libraries. X11/ALSA are to Linux what Cocoa is to macOS:
# system libraries, not third-party dependencies. Produces cvertex; run ./cvertex to play.
set -e
OUT=${OUT:-cvertex}
sh tools/gen-games.sh          # scan games/*.c -> gen/games.gen.h (the cartridge roster)
${CC:-cc} -std=c11 -O2 -Igen -Isrc -o "$OUT" \
  src/core.c src/g3d.c src/shape.c src/synth.c src/text.c src/net.c src/lnx.c games/*.c \
  -lX11 -lasound -lm -lpthread
echo "$OUT — ./cvertex to play"
