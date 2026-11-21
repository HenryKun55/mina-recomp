#!/bin/sh
# Runs the Vita build in Vita3K for you to play until it is closed (by
# hand, or after MAX seconds), then keeps its logs in build-vita/runs/<time>/ and prints a
# summary: gameplay reached, dead objects, the first bad memory access with
# the recompiled function it was in. Nothing is left running.
#
# usage: tools/vita/crash_run.sh [MAX seconds] [ENV=VALUE ...]   (default 600)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MAX=${1:-600}
[ $# -gt 0 ] && shift
DATA="${MINA_VITA3K:-$HOME/Documents/workspace/games/vita3k-mina}/fs/ux0/data/mina"
# you play; KEYS=1 presses Return until gameplay instead
{
    echo "MINA_FPS=1"
    if [ "${KEYS:-0}" = 1 ]; then
        echo "MINA_INPUT_UNTIL_PLAY=1"
        echo "MINA_INPUT=$(python3 -c "print(' '.join('%d:Return' % t for t in range(14, 200, 3)))")"
    fi
    for kv in "$@"; do echo "$kv"; done
} > "$DATA/env.txt"
OUT="$ROOT/build-vita/runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
printf '%s\n' "$@" > "$OUT/env.txt"

finish() {
    pkill -f "MacOS/Vita3K -c .* -r MINA00001$" 2>/dev/null
    sleep 1
    cp "$DATA/mina.log" "$OUT/mina.log" 2>/dev/null
    cp "${BUILD:-$ROOT/build-vita}/vita3k.log" "$OUT/vita3k.log" 2>/dev/null
    printf '\n' > "$DATA/env.txt"
    L="$OUT/mina.log"
    E="$OUT/vita3k.log"
    echo "run kept in $OUT"
    echo "gameplay: $(grep -ac '^\[play\]' "$L")  dead objects: $(grep -ac 'not a live\|unknown class\|non-heap' "$L")  bad accesses: $(grep -ac 'Invalid read\|Invalid write' "$E")"
    grep -a '^\[fps\] [0-9]' "$L" | tail -2
    n=$(grep -an 'Invalid read\|Invalid write' "$E" | head -1 | cut -d: -f1)
    if [ -n "$n" ]; then
        sed -n "${n}p" "$E" | cut -c1-160
        pc=$(sed -n "$((n + 1))p" "$E" | sed -n 's/.*PC: \(0x[0-9a-fA-F]*\).*/\1/p')
        ras=$(sed -n "$((n - 12)),$((n + 12))p" "$E" | grep -a 'LogStackReturnAddresses' | head -1 | sed 's/.*candidates: //')
        for a in $pc $ras; do
            f=$("$HOME/vitasdk/bin/arm-vita-eabi-addr2line" -f -e "$ROOT/build-vita/mina" "$a" | head -1)
            name=$(grep -h -B1 "^void $f(" "$ROOT"/build-vita/gen/funcs_*.c 2>/dev/null | head -1)
            echo "  $a $f $name"
        done
    fi
    exit 0
}
trap finish INT TERM

rm -f "$DATA/mina.log"
"$ROOT/tools/run_vita3k.sh" "$MAX" > /dev/null 2>&1 &
sleep 5
t=5
while pgrep -f "MacOS/Vita3K -c .* -r MINA00001$" > /dev/null && [ "$t" -lt "$MAX" ]; do
    sleep 2
    t=$((t + 2))
done
finish
