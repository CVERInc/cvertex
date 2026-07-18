#!/bin/sh
# test.sh — the engine's regression harness. It knows no game's rules; it checks the two properties
# every cartridge must hold: it runs without crashing, and it's deterministic (same input, same sim).
set -e
pass=0; fail=0
ok()  { printf '\033[32mok\033[0m   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '\033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }
./build.sh >/dev/null 2>&1 && ok "builds" || bad "builds"
GAMES="menu $(./cvertex --help 2>/dev/null | sed -n 's/.*available: //p')"
for g in $GAMES; do
    out=$(./cvertex --game "$g" --headless 300 2>&1) || { bad "$g runs"; continue; }
    echo "$out" | grep -q 'sim_checksum=' && ok "$g runs 300 frames" || bad "$g runs"
done
K="rrjllaauddrrjjllaarrddjjllaauurr"
for g in $GAMES; do
    a=$(./cvertex --game "$g" --keys "$K" --headless 300 2>/dev/null | sed -n 's/.*sim_checksum=\([0-9]*\).*/\1/p')
    b=$(./cvertex --game "$g" --keys "$K" --headless 300 2>/dev/null | sed -n 's/.*sim_checksum=\([0-9]*\).*/\1/p')
    [ -n "$a" ] && [ "$a" = "$b" ] && ok "$g deterministic" || bad "$g deterministic ($a vs $b)"
done
echo
[ "$fail" -eq 0 ] && printf '\033[32mALL GREEN\033[0m (%d checks)\n' "$pass" || { printf '\033[31m%d FAILED\033[0m\n' "$fail"; exit 1; }
