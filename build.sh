#!/bin/sh
# cvertex — build. Zero dependencies, just clang.
set -e
OUT=${OUT:-cvertex}
clang -std=c11 -Os -fno-stack-protector -fomit-frame-pointer \
  -ffunction-sections -fdata-sections -Isrc \
  -o "$OUT" src/core.c src/g3d.c src/shape.c src/synth.c src/mac.c games/vikings.c games/title.c games/forms.c \
  -framework Cocoa -framework CoreGraphics -framework AudioToolbox \
  -Wl,-dead_strip
strip -x "$OUT"

# Size is a first-class citizen: every build prints the tally.

# Size is a first-class result, so every build reports it. Machine code is the honest
# number — the rest of the binary is container overhead and baked artwork.
BYTES=$(stat -f%z "$OUT")
TEXT=$(size -m "$OUT" 2>/dev/null | awk '/Section __text/{print $3}')
printf '%s: %s bytes  (%s of it machine code)\n' "$OUT" "$BYTES" "${TEXT:-?}"
