#!/bin/sh
# Copies the shaders vitaGL compiled in the Vita3K session (ux0:data/mina_shader_cache)
# into deps/shader_cache, which the VPK carries (the first boot puts them on the card).
# Play or tour the game in Vita3K first (tools/vita/level_tour.sh), then run this
# and rebuild. usage: tools/vita/pull_shader_cache.sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
V3=${MINA_VITA3K:-$HOME/Documents/workspace/games/vita3k-mina}
SRC="$V3/fs/ux0/data/mina_shader_cache"
[ -d "$SRC" ] || { echo "no $SRC (run the game in Vita3K first)" >&2; exit 1; }
mkdir -p "$ROOT/deps/shader_cache"
cp -R "$SRC/." "$ROOT/deps/shader_cache/"
echo "$(find "$ROOT/deps/shader_cache" -name '*.gxp' | wc -l | tr -d ' ') shaders in deps/shader_cache"
