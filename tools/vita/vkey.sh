#!/bin/sh
# Sends keys to the running Vita3K (its keyboard mapping: arrows = d-pad,
# X = cross, C = circle, Z = square, V = triangle, Enter = start, WASD = left stick)
# and saves a screenshot of its game window.
# usage: tools/vita/vkey.sh OUT.png [key[:seconds] ...]   keys: up down left right x c z v enter w a s d
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; shift
pid=$(pgrep -f "MacOS/Vita3K -c .* -r MINA00001" | head -1)
[ -n "$pid" ] || { echo "no Vita3K running" >&2; exit 1; }
for k in "$@"; do
    name=${k%%:*}; hold=0.15
    case "$k" in *:*) hold=${k#*:};; esac
    case "$name" in
        up) code=126;; down) code=125;; left) code=123;; right) code=124;;
        x) code=7;; c) code=8;; z) code=6;; v) code=9;; enter) code=36;;
        w) code=13;; a) code=0;; s) code=1;; d) code=2;; q) code=12;; e) code=14;;
        wait) sleep "$hold"; continue;;
        *) echo "unknown key $name" >&2; continue;;
    esac
    osascript -e "tell application \"System Events\"
        set frontmost of (first process whose unix id is $pid) to true
        key down $code
        delay $hold
        key up $code
    end tell" > /dev/null
    sleep 0.25
done
id=$("$ROOT/build/winid" | /usr/bin/grep "MINA00001" | head -1 | cut -d' ' -f1)
[ -n "$id" ] && [ -n "$OUT" ] && screencapture -x -o -l "$id" "$OUT"
