#!/bin/sh
# Runs the Vita build in Vita3K's speed mode into gameplay (scripted Return
# presses until it starts) and prints the MINA_FPS log of the gameplay part:
# fps, render timings and the functions of runtime/timed.txt.
#
# usage: tools/vita/play_log.sh [seconds of gameplay] [ENV=VALUE ...]   (default 20)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PLAY=${1:-20}
[ $# -gt 0 ] && shift
DATA="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0/data/mina"
KEYS=$(python3 -c "print(' '.join('%d:Return' % t for t in range(14, 200, 3)))")
{ echo "MINA_FPS=1"; echo "MINA_INPUT_UNTIL_PLAY=1"; echo "MINA_INPUT=$KEYS"; for kv in "$@"; do echo "$kv"; done; } > "$DATA/env.txt"
rm -f "$DATA/mina.log"
"$ROOT/tools/run_vita3k.sh" 400 > /dev/null 2>&1 &
waited=0
until grep -aq '^\[play\]' "$DATA/mina.log" 2>/dev/null; do
    sleep 2
    waited=$((waited + 2))
    [ $waited -lt 300 ] || { echo "no gameplay after 300 s" >&2; break; }
done
sleep "$PLAY"
pkill -f "Vita3K -r MINA00001" || true
sleep 2
printf '\n' > "$DATA/env.txt"
sed -n '/^\[play\]/,$p' "$DATA/mina.log" | grep -a '^\[fps\]' | tail -40
