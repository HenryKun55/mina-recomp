#!/bin/sh
# Benchmark of the heaviest spot known: the ship's deck at the start of a new
# game (fire, the boss's arm). Vita3K at the console's speed (MHZ=240, the
# calibration of the sotc port; GPU model off unless GPU_SCALE is set),
# driven to gameplay by tools/vita/drive.py, then SECS seconds standing still.
# Writes build-vita/deck-{base,end}.txt (profiles) and prints the fps lines
# and the profile of that window.
# usage: tools/vita/deck_bench.sh [SECS] [ENV=VALUE ...]     (default 30)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SECS=${1:-30}
[ $# -gt 0 ] && shift
V3=${MINA_VITA3K:-$HOME/Documents/workspace/games/vita3k-mina}
DATA="$V3/fs/ux0/data/mina"
{ echo "MINA_FPS=1"; for kv in "$@"; do echo "$kv"; done; } > "$DATA/env.txt"
pkill -9 -f "Vita3K -c .* -r MINA00001" 2>/dev/null
(MHZ=${MHZ:-240} GPU_SCALE=${GPU_SCALE:-0} "$ROOT/tools/run_vita3k.sh" 3000 > /dev/null 2>&1 &)
sleep 5
"$ROOT/tools/vita/drive.py" 900 | tail -1 || exit 1
sleep 3
cp "$ROOT/build-vita/vita3k-profile.txt" "$ROOT/build-vita/deck-base.txt"
n0=$(/usr/bin/grep -ac "" "$DATA/mina.log")
sleep "$SECS"
cp "$ROOT/build-vita/vita3k-profile.txt" "$ROOT/build-vita/deck-end.txt"
"$ROOT/tools/vita/vkey.sh" "$ROOT/build-vita/deck.png"
pkill -9 -f "Vita3K -c .* -r MINA00001"
sleep 1
tail -n +"$n0" "$DATA/mina.log" | /usr/bin/grep -a "^\[fps\] [0-9]\|^\[fps\]   render\|^\[mem\] guest\|^\[mtl\]"
python3 "$ROOT/tools/vita/profile.py" -b "$ROOT/build-vita/deck-base.txt" "$ROOT/build-vita/deck-end.txt" 30 | sed -n '/== threads/,/top 30 function <-/p'
printf 'MINA_FPS=1\n' > "$DATA/env.txt"
