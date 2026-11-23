#!/usr/bin/env python3
"""
Drives a running Vita3K session of the game to gameplay by looking at its
window: each screenshot is matched against reference thumbnails (48x27 gray,
in build/vita-screens/<state>.png: made from screenshots of the game, never
committed) and the state's keys are sent (tools/vita/vkey.sh: arrows = d-pad,
x = cross, enter = start). Unknown screens (intro, letter, loading) get
"cross, hold start" (shows the skip prompt, then skips). Ends at "[play]" in
the game log.

usage: tools/vita/drive.py [timeout seconds] [--until state]
       (--until title: stops at the title screen, e.g. to warp from there)
"""
import os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
V3 = os.environ.get("MINA_VITA3K", os.path.expanduser("~/Documents/workspace/games/vita3k-mina"))
LOG = os.path.join(V3, "fs/ux0/data/mina/mina.log")
REF = os.path.join(ROOT, "build/vita-screens")
VKEY = os.path.join(ROOT, "tools/vita/vkey.sh")
TMP = os.path.join(ROOT, "build/drive-shot.png")
ACTIONS = {
    "title": ["x", "wait:3"],
    "name": ["x", "down", "down", "down", "down", "right", "x", "wait:3"],
    "profile": ["x", "wait:2"],
    "confirm": ["x", "wait:6"],
}

def gray(path):
    crop = "+0+64"                     # (the window's title bar)
    out = subprocess.run(["magick", path, "-crop", crop, "+repage", "-resize", "48x27!", "-colorspace", "Gray",
                          "-depth", "8", "gray:-"], capture_output=True).stdout
    return out

def in_play():
    try:
        with open(LOG, "rb") as f:
            return b"\n[play]" in f.read()
    except OSError:
        return False

def main():
    args = sys.argv[1:]
    until = None
    if "--until" in args:
        k = args.index("--until")
        until = args[k + 1]
        del args[k:k + 2]
    limit = float(args[0]) if args else 900
    refs = {}
    for name in ACTIONS:
        p = os.path.join(REF, name + ".png")
        if os.path.exists(p):
            refs[name] = subprocess.run(["magick", p, "-depth", "8", "gray:-"], capture_output=True).stdout
    start = time.time()
    while time.time() - start < limit:
        if subprocess.run(["pgrep", "-f", "Vita3K -c .* -r MINA00001"], capture_output=True).returncode:
            print("no Vita3K running", file=sys.stderr)
            return 2
        if in_play():
            print("gameplay after %d s" % (time.time() - start))
            return 0
        subprocess.run([VKEY, TMP])
        if not os.path.exists(TMP):
            time.sleep(2)
            continue
        g = gray(TMP)
        best, dist = None, 1e9
        for name, r in refs.items():
            if len(r) == len(g):
                d = sum(abs(a - b) for a, b in zip(r, g)) / len(g)
                if d < dist:
                    best, dist = name, d
        state = best if dist < 15 else None
        print("[drive] %4ds %s (%.1f)" % (time.time() - start, state or "other", dist), flush=True)
        if until and state == until:
            print("%s after %d s" % (until, time.time() - start))
            return 0
        if state:
            subprocess.run([VKEY, ""] + ACTIONS[state])
        else:
            subprocess.run([VKEY, "", "x", "wait:0.5", "enter:3", "wait:2"])
    print("no gameplay after %d s" % limit, file=sys.stderr)
    return 1

if __name__ == "__main__":
    sys.exit(main())
