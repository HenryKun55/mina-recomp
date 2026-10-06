#!/bin/sh
# Builds the vitaGL the Vita build links (deps/vitaGL/libvitaGL.a), with:
#   STORE_DEPTH_STENCIL  depth/stencil survive a scene change (the engine's
#                        stencil masks are written and used in different passes)
#   LOG_ERRORS           GL errors on the console (Vita3K shows sceClibPrintf)
#   HAVE_SHARK_LOG       shader compiler messages
# The SDK's libvitaGL.a skips error handling and crashes on this game's draws.
#
# usage: tools/build_vitagl.sh            (clones vitaGL into deps/ the first time)
# env:   VITAGL_SRC   a vitaGL source tree to copy instead of cloning
#        VITAGL_REV   the commit to build (default 464876a)
#        VITAGL_FLAGS extra make variables (e.g. "LOG_ERRORS=0")
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=$ROOT/deps/vitaGL
REV=${VITAGL_REV:-464876a}
VITASDK=${VITASDK:-$HOME/vitasdk}

if [ ! -d "$DEST" ]; then
    mkdir -p "$ROOT/deps"
    if [ -n "${VITAGL_SRC:-}" ]; then
        cp -R "$VITAGL_SRC" "$DEST"
    else
        git clone https://github.com/Rinnegatamante/vitaGL "$DEST"
        git -C "$DEST" checkout "$REV"
    fi
fi

cd "$DEST"
make clean > /dev/null 2>&1 || true
PATH="$VITASDK/bin:$PATH" VITASDK="$VITASDK" make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
    STORE_DEPTH_STENCIL=1 LOG_ERRORS=1 HAVE_SHARK_LOG=1 ${VITAGL_FLAGS:-} > build-mina.log 2>&1 \
    || { tail -20 build-mina.log >&2; exit 1; }
echo "built $DEST/libvitaGL.a"
