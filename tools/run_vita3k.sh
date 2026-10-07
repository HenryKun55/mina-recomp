#!/bin/sh
# Runs the Vita build in Vita3K: installs build-vita/mina.vpk as MINA00001
# (eboot.bin and sce_sys), links the game data into ux0:data/mina (once),
# starts Vita3K in its PS Vita speed mode (the patched fork) and follows
# ux0:data/mina/mina.log. Speed mode: CPU at the Vita's clock, 3 cores, the
# user/cdram/phycont memory budgets, microSD reads, the GPU model calibrated
# on a Vita 1000 (scale 37), and the guest sampling profiler.
#
# usage: tools/run_vita3k.sh [seconds]     (default 120; then this Vita3K is closed)
# env:   VITA3K   Vita3K binary (default the local source build)
#        MHZ      emulated CPU clock (default 444; 0 = full host speed, no speed mode)
#        GPU_SCALE  GPU model scale (default 37; 0 = off)
#        GPU_LOG  1 = GPU model per 60 frames, 2 = also each scene of a frame
#        GPU_MEDIAN  a scene costs the median of its last n timings (default 31; 0 = each its own)
#        PROFILE  guest profile output (default $BUILD/vita3k-profile.txt)
#        BUILD    Vita build directory (default build-vita)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECS=${1:-120}
BUILD=${BUILD:-$ROOT/build-vita}
VITA3K=${VITA3K:-$HOME/Documents/workspace/games/Vita3K/build/macos-ninja/bin/Release/Vita3K.app/Contents/MacOS/Vita3K}
UX0="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0"
APP="$UX0/app/MINA00001"
DATA="$UX0/data/mina"

[ -x "$VITA3K" ] || { echo "no Vita3K at $VITA3K" >&2; exit 1; }
[ -f "$BUILD/mina.vpk" ] || { echo "no $BUILD/mina.vpk" >&2; exit 1; }
[ "$BUILD/mina.vpk" -nt "$BUILD/mina" ] || { echo "$BUILD/mina.vpk is older than the binary (did the vpk step fail?)" >&2; exit 1; }

mkdir -p "$APP" "$DATA"
(cd "$APP" && rm -rf sce_sys && unzip -oq "$BUILD/mina.vpk" eboot.bin 'sce_sys/*')
if [ ! -e "$DATA/Contents" ]; then
    ln -s "$ROOT/gamedata/app/Contents" "$DATA/Contents"
fi
rm -f "$DATA/mina.log"

pkill -9 -f "Vita3K -r MINA00001" 2>/dev/null
rm -f "${PROFILE:-$BUILD/vita3k-profile.txt}"
(VITA3K_CPU_MHZ=${MHZ:-444} VITA3K_GPU_SCALE=${GPU_SCALE:-37} VITA3K_GPU_LOG=${GPU_LOG:-1} VITA3K_GPU_MEDIAN=${GPU_MEDIAN:-31} \
 VITA3K_PROFILE="${PROFILE:-$BUILD/vita3k-profile.txt}" "$VITA3K" -r MINA00001 > "$BUILD/vita3k.log" 2>&1 &)
echo "Vita3K started (log: $BUILD/vita3k.log, game log: ux0:data/mina/mina.log)"

waited=0
while [ $waited -lt "$SECS" ]; do
    sleep 5
    waited=$((waited + 5))
    if ! pgrep -f "Vita3K -r MINA00001" > /dev/null; then
        echo "Vita3K exited after ${waited}s"
        break
    fi
done
pkill -9 -f "Vita3K -r MINA00001" 2>/dev/null
echo "---- mina.log (tail) ----"
tail -40 "$DATA/mina.log" 2>/dev/null
