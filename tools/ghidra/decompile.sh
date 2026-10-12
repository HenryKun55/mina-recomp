#!/bin/sh
# Decompiles game functions with Ghidra (headless) from the analyzed project
# in ~/ghidra-projects/mina (the arm64 slice, imported and analyzed once with
#   lipo -thin arm64 .../MacOS/MinaTheHollower -output MinaTheHollower.arm64
#   analyzeHeadless ~/ghidra-projects/mina Mina -import MinaTheHollower.arm64)
#   tools/ghidra/decompile.sh OUT.c NAME|0xADDR ...   (OUT.c.log: Ghidra's log)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
GHIDRA=${GHIDRA_INSTALL_DIR:-$HOME/tools/ghidra_12.1.3_PUBLIC}
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}
OUT=$1
shift
case "$OUT" in /*) ;; *) OUT=$PWD/$OUT ;; esac
"$GHIDRA/support/analyzeHeadless" "${MINA_GHIDRA_PROJECT:-$HOME/ghidra-projects/mina}" Mina \
    -process MinaTheHollower.arm64 -noanalysis -readOnly \
    -scriptPath "$HERE" -postScript DecompileFns.java "$OUT" "$@" > "${OUT}.log" 2>&1
echo "$OUT"
