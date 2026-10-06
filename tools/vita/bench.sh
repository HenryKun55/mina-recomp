#!/bin/sh
# Runs the Vita build in Vita3K's PS Vita speed mode through the same stretch
# of the intro (from boot) and reports the averages: fps (MINA_FPS), and the
# GPU model's host time and Vita estimate per frame (the GPU side).
#
# usage: tools/vita/bench.sh [seconds] [ENV=VALUE ...]   (default 60 s)
#        the ENV=VALUE pairs go to the game's env.txt (e.g. MINA_NO_SKIP=1)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SECS=${1:-60}
[ $# -gt 0 ] && shift
DATA="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0/data/mina"
{ echo "MINA_FPS=1"; for kv in "$@"; do echo "$kv"; done; } > "$DATA/env.txt"
while pgrep -f "run_vita3k.sh" > /dev/null; do sleep 2; done
"$ROOT/tools/run_vita3k.sh" "$SECS" > /dev/null 2>&1
grep -a '^\[fps\]' "$DATA/mina.log" | sed 1d |
    awk '{s += $2; n++; if (min == "" || $2 < min) min = $2} END {printf "fps %.1f avg, %.1f min (%d samples)\n", s / n, min, n}'
grep -a 'gpu_model_frame' "$ROOT/build-vita/vita3k.log" | grep -o 'host [0-9.]* ms -> Vita estimate [0-9.]* ms' |
    awk '{h += $2; e += $7; n++} END {printf "gpu host %.2f ms, Vita estimate %.1f ms per frame (%d samples)\n", h / n, e / n, n}'
printf '\n' > "$DATA/env.txt"
