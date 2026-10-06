#!/bin/sh
# Sends the Vita build to a PS Vita over VitaShell's FTP server (SELECT in
# VitaShell turns it on; port 1337).
#
# usage: tools/vita/deploy_ftp.sh IP [vpk] [eboot] [data] [log]   (default: vpk data)
#   vpk    build-vita/mina.vpk -> ux0:mina.vpk (install it once with VitaShell:
#          that creates the bubble)
#   eboot  build-vita/eboot.bin -> ux0:app/MINA00001/eboot.bin (an installed game,
#          after each build)
#   data   the game's Contents (your copy, ~870 MiB) -> ux0:data/mina/Contents;
#          files already there with the same size are skipped, so it resumes
#   log    ux0:data/mina/mina.log -> build-vita/vita-mina.log
# env:   BUILD  Vita build directory (default build-vita)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build-vita}
[ $# -ge 1 ] || { sed -n 2,15p "$0" >&2; exit 1; }
IP=$1
shift
URL="ftp://$IP:1337"
[ $# -gt 0 ] || set -- vpk data

put() {     # put LOCAL REMOTE
    curl --disable-epsv -sS --ftp-create-dirs --connect-timeout 10 -T "$1" "$URL/$2"
}
remote_size() {
    curl --disable-epsv -sSI --connect-timeout 10 "$URL/$1" 2>/dev/null |
        tr -d '\r' | awk '/^Content-Length:/ {print $2}'
}

for what in "$@"; do
    case $what in
    vpk)
        echo "mina.vpk -> ux0:mina.vpk"
        put "$BUILD/mina.vpk" "ux0:/mina.vpk"
        echo "  install it with VitaShell (ux0:mina.vpk, X)"
        ;;
    eboot)
        echo "eboot.bin -> ux0:app/MINA00001/eboot.bin"
        put "$BUILD/eboot.bin" "ux0:/app/MINA00001/eboot.bin"
        ;;
    data)
        SRC=$ROOT/gamedata/app/Contents
        total=$(find "$SRC" -type f | wc -l | tr -d ' ')
        n=0
        find "$SRC" -type f | sort | while read -r f; do
            n=$((n + 1))
            rel=${f#"$SRC"/}
            size=$(stat -f %z "$f")
            if [ "$(remote_size "ux0:/data/mina/Contents/$rel")" = "$size" ]; then
                continue
            fi
            printf '[%s/%s] %s (%s MiB)\n' "$n" "$total" "$rel" "$((size / 1048576))"
            put "$f" "ux0:/data/mina/Contents/$rel"
        done
        ;;
    log)
        curl --disable-epsv -sS --connect-timeout 10 "$URL/ux0:/data/mina/mina.log" -o "$BUILD/vita-mina.log"
        echo "ux0:data/mina/mina.log -> $BUILD/vita-mina.log"
        tail -20 "$BUILD/vita-mina.log"
        ;;
    *)
        echo "unknown: $what" >&2
        exit 1
        ;;
    esac
done
