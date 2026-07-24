#!/bin/sh
# cvertex — build. Zero dependencies, just clang.
set -e
OUT=${OUT:-cvertex}
sh tools/gen-games.sh          # scan games/*.c -> gen/games.gen.h (the cartridge roster)
# gen/ holds the generated roster, OUT of src/ so `#include "games.gen.h"` (a quoted include, which
# searches mac.c's own dir first) can never shadow a consumer's roster with this repo's own.
clang -std=c11 -Os -fno-stack-protector -fomit-frame-pointer \
  -ffunction-sections -fdata-sections -Igen -Isrc \
  -o "$OUT" src/core.c src/g3d.c src/shape.c src/synth.c src/text.c src/net.c src/fx.c src/mac.c games/*.c \
  -framework Cocoa -framework CoreGraphics -framework AudioToolbox \
  -Wl,-dead_strip
strip -x "$OUT"

# Size is a first-class citizen: every build prints the tally.

# Size is a first-class result, so every build reports it. Machine code is the honest
# number — the rest of the binary is container overhead and baked artwork.
BYTES=$(stat -f%z "$OUT")
TEXT=$(size -m "$OUT" 2>/dev/null | awk '/Section __text/{print $3}')
printf '%s: %s bytes  (%s of it machine code)\n' "$OUT" "$BYTES" "${TEXT:-?}"
