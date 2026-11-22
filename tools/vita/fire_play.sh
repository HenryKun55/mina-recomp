#!/bin/sh
# New game on the deck, the fire started, then KEYS2 (default: walk and act):
# reproduces the scene change after the fire. Screenshots in OUT.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-/tmp}
V3=${MINA_VITA3K:-$HOME/Documents/workspace/games/vita3k-mina}
for p in $(pgrep -f "tools/run_vita3k[.]sh"); do kill "$p" 2>/dev/null; done
"$ROOT/tools/vita/stop.sh"
rm -f "$V3/fs/ux0/data/Yacht Club Games/Mina the Hollower (recomp)/saveData.yc"
{ echo "MINA_FPS=1"; for kv in "$@"; do echo "$kv"; done; } > "$V3/fs/ux0/data/mina/env.txt"
(MHZ=${MHZ:-240} GPU_SCALE=0 "$ROOT/tools/run_vita3k.sh" 20000 > /dev/null 2>&1 &)
sleep 5
"$ROOT/tools/vita/drive.py" 900 | tail -1
"$ROOT/tools/vita/vkey.sh" "$OUT/fp0.png" up:0.6 right:0.3 x wait:2 x wait:1 x wait:1 x wait:1 x wait:20
i=1
for k in ${KEYS2:-"left:1" "x" "z" "c" "v" "wait:5"}; do
    "$ROOT/tools/vita/vkey.sh" "$OUT/fp$i.png" $k || break
    i=$((i + 1))
done
