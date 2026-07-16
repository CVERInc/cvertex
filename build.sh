#!/bin/sh
# cvertex — build. Zero dependencies, just clang.
set -e
OUT=${OUT:-cvertex}
clang -std=c11 -Os -fno-stack-protector -fomit-frame-pointer \
  -ffunction-sections -fdata-sections \
  -o "$OUT" src/core.c src/g3d.c src/shape.c src/synth.c src/mac.c \
  -framework Cocoa -framework CoreGraphics -framework AudioToolbox \
  -Wl,-dead_strip
strip -x "$OUT"

# Size is a first-class citizen: every build prints the tally.
BYTES=$(stat -f%z "$OUT")
BUDGET=1474560
printf '%s: %s bytes  (budget %s, %s%% used, %s bytes left)\n' \
  "$OUT" "$BYTES" "$BUDGET" \
  "$(echo "scale=2; $BYTES*100/$BUDGET" | bc)" \
  "$((BUDGET - BYTES))"
