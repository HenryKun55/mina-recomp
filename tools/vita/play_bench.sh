#!/bin/sh
# Gameplay benchmark in Vita3K's PS Vita speed mode: scripted Return presses
# (until gameplay starts, MINA_INPUT_UNTIL_PLAY), then the fps lines and a guest
# profile of the gameplay part only.
#
# usage: tools/vita/play_bench.sh [seconds of gameplay] [ENV=VALUE ...]   (default 40)
#   writes build-vita/play-base.txt and play-end.txt (profiles); see tools/vita/profile.py -b
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PLAY=${1:-40}
[ $# -gt 0 ] && shift
DATA="${MINA_VITA3K:-$HOME/Documents/workspace/games/vita3k-mina}/fs/ux0/data/mina"
KEYS=$(python3 -c "print(' '.join('%d:Return' % t for t in range(14, 200, 3)))")
{ echo "MINA_FPS=1"; echo "MINA_INPUT_UNTIL_PLAY=1"; echo "MINA_INPUT=$KEYS"; for kv in "$@"; do echo "$kv"; done; } > "$DATA/env.txt"
rm -f "$DATA/mina.log"
"$ROOT/tools/run_vita3k.sh" 400 > /dev/null 2>&1 &
waited=0
until grep -aq '^\[play\]' "$DATA/mina.log" 2>/dev/null; do
    sleep 2
    waited=$((waited + 2))
    [ $waited -lt 300 ] || { echo "no gameplay after 300 s" >&2; pkill -f "Vita3K -c .* -r MINA00001"; exit 1; }
done
sleep 10
cp "$ROOT/build-vita/vita3k-profile.txt" "$ROOT/build-vita/play-base.txt"
sleep "$PLAY"
cp "$ROOT/build-vita/vita3k-profile.txt" "$ROOT/build-vita/play-end.txt"
pkill -f "Vita3K -c .* -r MINA00001" || true
sleep 2
printf '\n' > "$DATA/env.txt"
grep -a '^\[fps\] [0-9]' "$DATA/mina.log" | tail -4
