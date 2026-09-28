#!/bin/sh
# build-linux.sh — build a native Linux binary. Same sources as build.sh, but the platform layer is
# lnx.c and it links the X11 and ALSA system libraries. X11/ALSA are to Linux what Cocoa is to macOS:
# system libraries, not third-party dependencies. Produces cvertex; run ./cvertex to play.
set -e
OUT=${OUT:-cvertex}
sh tools/gen-games.sh          # scan games/*.c -> gen/games.gen.h (the cartridge roster)
# -Werror=implicit-function-declaration: clang (16+) and gcc (14+) already refuse a call to an
# undeclared function under -std=c11; gcc 13 — Ubuntu 24.04's default cc — only warns and guesses an
# `int` return. Making it an error everywhere means the CI's cc fails the same way a user's clang does.
${CC:-cc} -std=c11 -O2 -Werror=implicit-function-declaration -Igen -Isrc -o "$OUT" \
  src/core.c src/g3d.c src/shape.c src/synth.c src/text.c src/net.c src/data.c src/fx.c src/lnx.c games/*.c \
  -lX11 -lasound -lm -lpthread
echo "$OUT — ./cvertex to play"
