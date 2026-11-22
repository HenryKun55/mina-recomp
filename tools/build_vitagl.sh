#!/bin/sh
# Builds the vitaGL the Vita build links (deps/vitaGL/libvitaGL.a), with:
#   STORE_DEPTH_STENCIL  depth/stencil survive a scene change (the engine's
#                        stencil masks are written and used in different passes)
#   LOG_ERRORS           GL errors on the console (Vita3K shows sceClibPrintf)
#   HAVE_SHARK_LOG       shader compiler messages
#   NO_SPLASHSCREEN      no vitaGL logo at boot (the bubble's pic0 shows instead)
#   HAVE_SHADER_CACHE    each shader compiled once (ux0:data/shader_cache): a
#                        compile holds the renderer for 1-3 s (the opening froze)
# The SDK's libvitaGL.a skips error handling and crashes on this game's draws.
# tools/vitagl/*.patch: depth/stencil renderbuffers shared by the framebuffers
# they are attached to (the engine writes a stencil mask with one color target
# and tests it with another), and the SDK's older vitaShaRK.
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
# our changes (tools/vitagl/*.patch, in order), applied once: when the last
# one is in, all are (a later patch can change an earlier one's context)
last=$(ls "$ROOT"/tools/vitagl/*.patch | tail -1)
if ! git apply --reverse --check "$last" > /dev/null 2>&1; then
    for p in "$ROOT"/tools/vitagl/*.patch; do
        git apply "$p" || { echo "cannot apply $p" >&2; exit 1; }
    done
fi
make clean > /dev/null 2>&1 || true
PATH="$VITASDK/bin:$PATH" VITASDK="$VITASDK" make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
    STORE_DEPTH_STENCIL=1 LOG_ERRORS=1 HAVE_SHARK_LOG=1 NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 ${VITAGL_FLAGS:-} > build-mina.log 2>&1 \
    || { tail -20 build-mina.log >&2; exit 1; }
echo "built $DEST/libvitaGL.a"
