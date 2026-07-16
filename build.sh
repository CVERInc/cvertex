#!/bin/sh
# cvertex — build。零依賴，只要 clang。
set -e
OUT=${OUT:-cvertex}
clang -std=c11 -Os -fno-stack-protector -fomit-frame-pointer \
  -ffunction-sections -fdata-sections \
  -o "$OUT" src/core.c src/synth.c src/mac.c \
  -framework Cocoa -framework CoreGraphics -framework AudioToolbox \
  -Wl,-dead_strip
strip -x "$OUT"

# 體積是一等公民：每次 build 都印帳。
BYTES=$(stat -f%z "$OUT")
BUDGET=1474560
printf '%s: %s bytes  (預算 %s，用掉 %s%%，剩 %s bytes)\n' \
  "$OUT" "$BYTES" "$BUDGET" \
  "$(echo "scale=2; $BYTES*100/$BUDGET" | bc)" \
  "$((BUDGET - BYTES))"
