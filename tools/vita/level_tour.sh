#!/bin/sh
# Warps through levels in a running Vita3K session (after drive.py): for each
# level, warp.txt, SETTLE seconds, a walk (KEYS), a screenshot and the fps lines
# of the last WINDOW seconds. Output: OUT/<n>.png and OUT/tour.txt
# usage: tools/vita/level_tour.sh OUT level...
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; shift
V3=${MINA_VITA3K:-$HOME/Documents/workspace/games/vita3k-mina}
L="$V3/fs/ux0/data/mina/mina.log"
mkdir -p "$OUT"
i=0
for lv in "$@"; do
    i=$((i + 1))
    echo "$lv" > "$V3/fs/ux0/data/mina/warp.txt"
    sleep ${SETTLE:-25}
    n0=$(wc -l < "$L" | tr -d " ")
    "$ROOT/tools/vita/vkey.sh" "" ${KEYS:-left:1 right:2 left:1 up:1 down:1}
    sleep ${WINDOW:-10}
    "$ROOT/tools/vita/vkey.sh" "$OUT/$i.png" || break
    echo "== $i $lv" >> "$OUT/tour.txt"
    tail -n +"$n0" "$L" | /usr/bin/grep -a "^\[fps\] [0-9]\|render:\|\[warp\]" | tail -6 >> "$OUT/tour.txt"
done
