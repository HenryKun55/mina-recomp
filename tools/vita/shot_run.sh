#!/bin/sh
# Runs the Vita build in Vita3K for SECS seconds with the given env lines,
# saves screenshots of the Vita3K game window (only that window) at the
# given times, then closes it.
# usage: tools/vita/shot_run.sh OUTDIR "t1 t2 ..." [ENV=VALUE ...]
# env:   MHZ, GPU_SCALE (as tools/run_vita3k.sh)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; TIMES=$2; shift 2
V3=${MINA_VITA3K:-$HOME/Documents/workspace/games/vita3k-mina}
DATA="$V3/fs/ux0/data/mina"
mkdir -p "$OUT"
{ echo "MINA_FPS=1"; for kv in "$@"; do echo "$kv"; done; } > "$DATA/env.txt"
WINID="$ROOT/build/winid"
[ -x "$WINID" ] || swiftc -O "$ROOT/tools/winid.swift" -o "$WINID"
last=0; for t in $TIMES; do last=$t; done
"$ROOT/tools/run_vita3k.sh" $((last + 10)) > /dev/null 2>&1 &
start=$(date +%s)
for t in $TIMES; do
    while [ $(( $(date +%s) - start )) -lt "$t" ]; do sleep 1; done
    id=$("$WINID" | /usr/bin/grep "MINA00001" | head -1 | cut -d' ' -f1)
    [ -n "$id" ] && screencapture -x -o -l "$id" "$OUT/t$t.png"
done
pkill -9 -f "Vita3K -c .* -r MINA00001"
wait
cp "$DATA/mina.log" "$OUT/mina.log"
printf 'MINA_FPS=1\n' > "$DATA/env.txt"
