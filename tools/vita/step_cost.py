#!/usr/bin/env python3
"""Guest instructions of a thread per fixed update (world step) in a bench window:
the profile's weight between two copies (-b base end) over the steps the [fps]
lines of that window count (fps x 2 s x fixed updates per frame). Independent of
the host's load, unlike the fps.
usage: tools/vita/step_cost.py base.txt end.txt bench_output.txt [thread]"""
import re, sys
base, end, out = sys.argv[1:4]
thr = sys.argv[4] if len(sys.argv) > 4 else "220"
def load(p):
    w = 0
    for l in open(p):
        t = l.split()
        if len(t) >= 4 and t[0] == thr:
            w += int(t[3])
    return w
w = load(end) - load(base)
steps = frames = 0.0
for l in open(out, errors="replace"):
    m = re.match(r"\[fps\] ([0-9.]+) fps, .* ([0-9.]+) fixed updates", l)
    if m:
        frames += float(m.group(1)) * 2
        steps += float(m.group(1)) * 2 * float(m.group(2))
print("thread %s: %.2f M instructions per step (%d steps), %.2f M per frame" % (thr, w / steps / 1e6, steps, w / frames / 1e6))
