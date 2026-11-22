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
# an older session's runner would close this one when its own Vita3K goes
for p in $(pgrep -f "tools/run_vita3k[.]sh"); do kill "$p" 2>/dev/null; done
"$ROOT/tools/vita/stop.sh"
# a new game every time (the save moves on as it is played)
# (and its backup: the game restores a missing save from it)
SAVE="$V3/fs/ux0/data/Yacht Club Games/Mina the Hollower (recomp)"
rm -f "$DATA/warp.txt" "$DATA/mina.log" "$SAVE/saveData.yc" "$SAVE/saveData.bak.yc"
(MHZ=${MHZ:-240} GPU_SCALE=${GPU_SCALE:-0} "$ROOT/tools/run_vita3k.sh" 3000 > /dev/null 2>&1 &)
sleep 5
alive() { pgrep -f "Vita3K -c .* -r MINA00001" > /dev/null || { echo "Vita3K is gone" >&2; exit 1; }; }
if [ -n "${UNTIL:-}" ]; then
    # UNTIL=confirm: driven to that screen only (no cutscene skipped after it), then KEYS
    "$ROOT/tools/vita/drive.py" 300 --until "$UNTIL" | tail -1 || exit 1
elif [ -n "${WARP:-}" ]; then
    # WARP=levels/core/astralOrrery: that level instead of the deck, warped to
    # from the title screen (no new game to drive through); SETTLE seconds to load.
    # Not before the title: a warp during the opening logos is lost.
    "$ROOT/tools/vita/drive.py" 300 --until title | tail -1 || exit 1
    echo "$WARP" > "$DATA/warp.txt"
    t=0
    until /usr/bin/grep -aq "^\[warp\]" "$DATA/mina.log" 2>/dev/null; do
        alive; sleep 2; t=$((t + 2)); [ $t -gt 300 ] && { echo "no warp after 300 s" >&2; exit 1; }
    done
    sleep ${SETTLE:-25}
else
    "$ROOT/tools/vita/drive.py" 900 | tail -1 || exit 1
    sleep 3
fi
alive
# KEYS="left:1 x ...": played first (tools/vita/vkey.sh), e.g. to start the fire on the deck
[ -n "${KEYS:-}" ] && "$ROOT/tools/vita/vkey.sh" "" $KEYS
cp "$ROOT/build-vita/vita3k-profile.txt" "$ROOT/build-vita/deck-base.txt"
n0=$(/usr/bin/grep -ac "" "$DATA/mina.log")
if [ -n "${SHOTS:-}" ]; then
    # SHOTS=5: a screenshot every 5 s of the window (build-vita/shot-<s>.png)
    t=0
    while [ $t -lt "$SECS" ]; do
        sleep "$SHOTS"; t=$((t + SHOTS)); alive
        "$ROOT/tools/vita/vkey.sh" "$ROOT/build-vita/shot-$t.png"
    done
else
    sleep "$SECS"
fi
alive
cp "$ROOT/build-vita/vita3k-profile.txt" "$ROOT/build-vita/deck-end.txt"
"$ROOT/tools/vita/vkey.sh" "$ROOT/build-vita/deck.png"
pkill -9 -f "Vita3K -c .* -r MINA00001"
sleep 1
tail -n +"$n0" "$DATA/mina.log" | /usr/bin/grep -a "^\[fps\] [0-9]\|^\[fps\]   render\|^\[mem\] guest\|^\[mtl\]"
# a level is up when its fixed updates do real work (a menu's or a logo's take ~1 ms)
[ -n "${UNTIL:-}" ] || tail -n +"$n0" "$DATA/mina.log" | /usr/bin/grep -a "^\[fps\] [0-9]" | tail -1 \
    | awk '{ for (i = 1; i < NF; i++) if ($(i+1) == "fixed") { gsub(/\(/, "", $(i+3)); ms = $(i+3) } }
           END { if (ms + 0 < 3) { print "NOT IN A LEVEL (fixed updates " ms " ms)" > "/dev/stderr"; exit 1 } }' || st=1
python3 "$ROOT/tools/vita/profile.py" -b "$ROOT/build-vita/deck-base.txt" "$ROOT/build-vita/deck-end.txt" 30 | sed -n '/== threads/,/top 30 function <-/p'
printf 'MINA_FPS=1\n' > "$DATA/env.txt"
exit ${st:-0}
