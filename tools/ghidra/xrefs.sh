#!/bin/sh
# Lists the game functions that reference addresses (Ghidra, headless):
#   tools/ghidra/xrefs.sh OUT.txt 0xADDR ...   (see decompile.sh for the project)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
GHIDRA=${GHIDRA_INSTALL_DIR:-$HOME/tools/ghidra_12.1.3_PUBLIC}
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}
OUT=$1
shift
case "$OUT" in /*) ;; *) OUT=$PWD/$OUT ;; esac
"$GHIDRA/support/analyzeHeadless" "${MINA_GHIDRA_PROJECT:-$HOME/ghidra-projects/mina}" Mina \
    -process MinaTheHollower.arm64 -noanalysis -readOnly \
    -scriptPath "$HERE" -postScript XRefs.java "$OUT" "$@" > "${OUT}.log" 2>&1
echo "$OUT"
